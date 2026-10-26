# Ghost Master DLSS — start here

ReShade add-on adding NVIDIA DLSS Super Resolution and DLSS Frame Generation (2x, Streamline) to Ghost Master: Resurrection
(UE 5.5.1, Steam app 3289090, build 25511106, exe SHA256 0e5e8471...4e9). The user speaks Russian; answer in Russian.

## State (2026-10-26)
- Release 1.0.0 built (`tools/make-release.ps1` -> `release/GhostMasterDLSS-1.0.0.zip`, 34.8 MB) and verified in game by the user:
  DLSS SR, live render scale, Frame Generation 2x, UI without artifacts, clean exit. Not yet uploaded: the game has no Nexus section,
  it must be requested via "ADD NEW GAME" in the upload flow. Page texts: `release/nexus/` (sources in `release-src/`).
- The user's game folder has the release binaries installed (`tools/install-addon.ps1`), their own ini kept. DLSS 5 addons were moved
  out to `disabled-addons/dlss5/` at the user's request. ReShade 6.8.0 (API 20) is installed in the game folder.
- This folder ("C:\dev\GhostMaster-DLSS — копия") is the working copy and a git repo (public on GitHub, no license chosen by the
  user, GitHub Release v1.0.0 carries the release zip). `.gitignore` keeps out SDKs, game dumps (captures), logs, backups and
  other mods. The original `C:\dev\GhostMaster-DLSS` is older; work here.
- Full evidence and history: `PROGRESS.md` (append new entries at the end). User-facing overview: `README.md`.

## Map
- `src/probe.cpp` — the add-on: lock-free descriptor mirror, TAAU interception (shader hashes fc434178cfb8e489, 0f1634b465b59707),
  View/constant reads, menu ("DLSS SR" overlay tab), ini, r.ScreenPercentage control, FG hookup, perf logging.
- `src/live_bridge.cpp` — input preparation shader, DLSS evaluate, copy to TAAU output, command-list restore, resize handling.
- `src/dlss_backend.cpp` — NGX DLSS SR. `src/frame_generation.cpp` -> `ghostmaster_fg.dll` (pinned) — Streamline manual hooking.
- `src/version.rc` — release version. `tools/` — build, install/uninstall, launcher, make-release. Tests: `bridge_smoke`, `sl_smoke`.

## Facts that cost time to find (do not rediscover)
- r.ScreenPercentage: TAutoConsoleVariable<float> static at RVA 0x912d8c0 (+0 vtable RVA 0x6f2e780 — the base-class vtable 0x8187ef8
  is overwritten, +8 IConsoleVariable*, +0x10 TConsoleVariableData<float>*); registration code at RVA 0xdf7310 and 0xdf73cf is
  byte-compared before any write.
- UE View buffer (float4 registers): [20] ViewToTranslatedWorld, [32] ViewToClipNoAA, [36] ClipToView, [136] ClipToPrevClip,
  [144] TemporalAAJitter, [148] ViewSizeAndInvSize, [155].z PreExposure. TAAU b0: [17] view size, [9] padded size, 74/75 jitter px.
- ReShade unloads/reloads add-ons per probe D3D12 device -> anything hooked must live in the pinned ghostmaster_fg.dll.
- ReShade proxies the DXGI factory here, so patching the real factory v-table slot 15 puts Streamline under ReShade.
- Streamline initializes NGX first; NGX then only searches the `streamline` folder -> nvngx_dlss.dll must be there too.
  Never call NVSDK_NGX_D3D12_Shutdown1 while FG runs (it stops DLSS-G). SL needs both projectId and engineVersion.
- DLSS-G does not interpolate for an unfocused window. NVIDIA Control Panel forces VSync ON on this PC.
- The vanilla game ships no nvngx_dlss.dll (the user's copy came from the DLSS 5 mod).
- Global mutex per descriptor write cost ~118 ms CPU/frame; keep descriptor tracking lock-free.

## Working rules
- Game originals: back up before changing anything in the game folder; the installer does this (`backups/before-install-*`).
- Build: `tools/build.ps1`. NGX/Streamline tests must run unsandboxed (NVIDIA telemetry hangs in the sandbox).
- In-game checks need the user: ask them to launch via Steam, then read `Win64/ghostmaster-dlss/` logs
  (`capture.jsonl` perf events with fg_* fields, `streamline/sl.log`, `streamline/frame_generation.jsonl`).
- Long Python edits through bash heredocs have corrupted backslashes (\b, \f, \v, \n) several times — write such scripts
  to a file or use the Edit tool, and scan for control characters afterwards.
- Do not look into other games' folders (the user asked not to).
