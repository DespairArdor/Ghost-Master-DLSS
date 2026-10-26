NVIDIA DLSS Super Resolution and DLSS Frame Generation (2x) for **Ghost Master: Resurrection** (Steam build 25511106, DirectX 12), as a ReShade add-on.

**Download:** `GhostMasterDLSS-1.0.0.zip` below. Extract it into the game folder (the one containing `GhostMasterRemake`); full instructions in `README.txt` inside the archive.

**Requirements**
- ReShade 6.8.0+ **with full add-on support** (https://reshade.me)
- NVIDIA RTX GPU: DLSS SR on RTX 20+, Frame Generation on RTX 40/50 only (enable Hardware-accelerated GPU scheduling)
- Microsoft Visual C++ 2015-2022 Redistributable (x64)

**Features**
- DLSS Super Resolution in place of the game's TAAU
- DLSS Frame Generation 2x through NVIDIA Streamline 2.14.16
- "DLSS SR" tab in the ReShade overlay (Home): DLSS on/off, render resolution presets and slider (applied live and from launch), DLSS model preset, Frame Generation switch
- Hotkeys: F6 DLSS on/off, F7 status line

Tested on RTX 4090 at 4K: ~105-115 FPS without and ~175-210 FPS shown with Frame Generation (58 % render scale, in a level).

The archive contains NVIDIA runtime files (DLSS 310.9.16, Streamline 2.14.16) redistributed under their licenses (texts included in `streamline\`). Not affiliated with or endorsed by NVIDIA or the game's developers/publishers.

SHA256 (GhostMasterDLSS-1.0.0.zip): see `SHA256SUMS.txt` in the release assets.
