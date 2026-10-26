# Ghost Master: Resurrection — DLSS SR + Frame Generation (ReShade add-on)

> Русская версия: [README.ru.md](README.ru.md)

A C++/DirectX 12 ReShade add-on for Ghost Master: Resurrection (UE 5.5.1, Steam app 3289090, build 25511106, executable SHA256 `0e5e8471808be706d119f37da90c84ecf291c9e91613e5a5f4e9bd4caa8ae4e9`). It replaces the game's temporal upscaling pass (TAAU) with NVIDIA DLSS Super Resolution and adds DLSS Frame Generation 2x through NVIDIA Streamline. No game source code is needed.

**Status: release 1.0.0, verified in game.** Download it from [Releases](https://github.com/DespairArdor/Ghost-Master-DLSS/releases/tag/v1.0.0). The full development log with evidence is in `PROGRESS.md`; a short start-here summary for development sessions is in `CLAUDE.md`.

## Verified in game (RTX 4090, 4K output)

- DLSS SR in place of TAAU: thousands of replaced frames, correct image and UI (confirmed by playing).
- Resolution and render-scale changes are handled live: DLSS is recreated for the new size without a restart.
- The add-on's render-scale setting applies immediately and from the first frame of every launch (it writes `r.ScreenPercentage`).
- DLSS Frame Generation: 2 presented frames per rendered frame; in a level ~175-210 FPS shown vs ~105-115 without it; no UI artifacts noticed.
- Add-on CPU cost ~2-3 ms per frame summed over render threads (down from ~118 ms before descriptor tracking was made lock-free).
- The packaged 1.0.0 release: loads as "Ghost Master DLSS" v1.0.0.0, DLSS and Frame Generation work, clean exit.

## Installation

Players: use the release archive, see `release-src/README.txt` (the same file ships in the archive). In short: install ReShade 6.8+ **with full add-on support** for the game, extract the archive into the game folder, press Home in game and open the **DLSS SR** tab.

From this working folder, with the game closed:

```powershell
.\tools\install-addon.ps1      # add-on, ghostmaster_fg.dll, ini (if missing) and the signed NVIDIA runtime in Win64\streamline
.\tools\uninstall-addon.ps1    # removes the add-on, ghostmaster_fg.dll and the streamline folder (-RemoveSettings also removes ini and logs)
```

The installer checks the executable hash, ReShade (`dxgi.dll`) and NVIDIA signatures, and backs up the game folder configuration plus a SHA256 file list to `backups\before-install-*`.

## Menu and controls

Home → **DLSS SR** tab (the same panel is also under the add-on's entry in the Add-ons tab):

- DLSS on/off (F6), status line (F7, saved);
- render resolution: "Game slider" (the game's Resolution Quality) or "Set here" — Quality 66.7 / Balanced 58 / Performance 50 / Ultra Performance 33.3 % and a 33.3-99 % slider;
- Frame Generation on/off and status;
- DLSS model preset (default, J, K, L, M), applied immediately;
- current input/output size and FPS.

Settings file `ghostmaster_dlss.ini` (next to the add-on): `Mode` (replace/off/shadow), `ShowStatus`, `Preset`, `ScaleSource` (addon/game), `ScreenPercentage`, `FrameGeneration` (0 = Streamline is not loaded at all), `Diagnostics` (shader dumps and dispatch capture, default 0), `RuntimeDir`, `StreamlineDir`, `LogDir` (empty = defaults; relative paths are relative to the add-on folder). DLSS SR and Frame Generation share one signed `nvngx_dlss.dll` 310.9.16 in `Win64\streamline` — the game itself does not ship `nvngx_dlss.dll`.

Logs: `Win64\ghostmaster-dlss\capture.jsonl` (events; every 2 s a `perf` event with FPS and `fg_*` fields), `ghostmaster-dlss\streamline\frame_generation.jsonl`, `ghostmaster-dlss\streamline\sl.log`.

## How it works (short)

- `src/probe.cpp` — the ReShade add-on: lock-free descriptor tracking, interception of two exact TAAU shader hashes (`fc434178cfb8e489`, `0f1634b465b59707`), constant reads, menu, `r.ScreenPercentage` control, Frame Generation hookup.
- `src/live_bridge.cpp` — input preparation (color, depth, motion vectors), DLSS evaluation, copy into the TAAU output, command-list state restore, resolution changes.
- `src/dlss_backend.cpp` — NGX DLSS SR.
- `src/frame_generation.cpp` → `ghostmaster_fg.dll` — Streamline in manual-hooking mode: the real `IDXGIFactory2::CreateSwapChainForHwnd` v-table slot is redirected so the chain is game → ReShade → Streamline proxy → DXGI. The DLL pins itself because ReShade unloads add-ons between the game's probe devices.
- Key addresses and layouts (`r.ScreenPercentage`, the UE `View` buffer) are documented in `PROGRESS.md` and in comments in `probe.cpp`.

## Test launcher (without installing into the game)

`Start-DLSS.cmd` / `tools\launch-live.ps1` start the game with an isolated ReShade profile in `run\` (only this add-on, Streamline from `external\Streamline\bin\x64`); the render scale is passed through `run\addons\ghostmaster_dlss.ini`. `Start-Baseline.cmd` is the same launch with the add-on only measuring FPS (for comparisons). Launcher options: `-Mode`, `-ScreenPercentage`, `-Width/-Height`, `-Fullscreen`, `-Diagnostics`, `-Baseline`. Captures go to `captures\live-*`.

## Building, tests, release

Requires VS 2022 Build Tools (C++), the Windows SDK, CMake (from VS) and these dependencies in `external\` (not part of the repository):

| Folder | Source |
|---|---|
| `external\reshade` | `git clone https://github.com/crosire/reshade`, commit `7bf9de8b33bcc76c3177007e65d73c72dd0f34c0` (needs `include` and `deps\imgui`) |
| `external\DLSS` | [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) v310.9.16 (`include`, `lib\Windows_x86_64`, `LICENSE.txt`) |
| `external\Streamline` | `streamline-sdk-v2.14.16.zip` from the [NVIDIA-RTX/Streamline releases](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16), extracted (`include`, `bin\x64`, `license.txt`) |

```powershell
.\tools\build.ps1
.\build\RelWithDebInfo\bridge_smoke.exe .\external\DLSS\lib\Windows_x86_64\rel .\reports\bridge-smoke   # DLSS bridge incl. a resolution change
.\build\RelWithDebInfo\sl_smoke.exe .\external\Streamline\bin\x64 .\reports\sl-smoke                     # DLSS-G through the FG module (opens a window)
.\tools\make-release.ps1                                                                                  # archive and texts in release\
```

Run the NGX/Streamline tests outside a sandbox (NVIDIA telemetry hangs on exit inside one). `sl_smoke` needs its window focused — DLSS-G does not interpolate an unfocused window. The release version lives in `src\version.rc`; release texts and ini in `release-src\`; `make-release.ps1` checks the version and NVIDIA signatures and writes `release\SHA256SUMS.txt`.

## Limitations

- Tied to one game build; after a game update the add-on finds no TAAU pass and DLSS stays inactive.
- Only the two studied TAAU shader variants; a different anti-aliasing method in the game settings can select another variant.
- 100 % (DLAA) is not offered — the game then uses a different anti-aliasing pass.
- Tested on an RTX 4090 only; HDR, multi-monitor and other GPU generations are untested.
- Frame Generation works only while DLSS is active and the window is focused; toggling DLSS off with F6 logs harmless "Failed to find global tag" errors in `sl.log`.
- A V-Sync forced in the NVIDIA Control Panel caps the shown FPS at the refresh rate; the game's own `MaxFPS` limit (Settings.sav) only limits rendered frames.

## Dependencies and licenses

- [ReShade](https://github.com/crosire/reshade), commit `7bf9de8b33bcc76c3177007e65d73c72dd0f34c0` (API headers and ImGui); not included in the release.
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) 310.9.16, commit `9981c6350dc58c0ce21f024fd65e31e851c31689`; NVIDIA RTX SDK license (`external\DLSS\LICENSE.txt`), which allows redistributing the runtime as part of an application with the required notice.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) 2.14.16 (zip SHA256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`); MIT.
- [Dear ImGui](https://github.com/ocornut/imgui), commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`; MIT.

No license is granted for this repository's own code; all rights reserved by the author. NVIDIA files keep their own licenses.

This software contains source code provided by NVIDIA Corporation. Not affiliated with or endorsed by NVIDIA or by the game's developers/publishers. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
