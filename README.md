# MMDX12 — MikuMikuDance DX12 Player (MVP)

A Windows Direct3D 12 MikuMikuDance (MMD) client. Drop MMD characters (PMX), stages (PMX) and
dance sets (VMD motion + camera + music) into the library folder in any layout; the client
classifies them by content.

## Build

Requirements: Visual Studio 2026 (v18, MSVC 14.5x) with the Windows SDK, and CMake + Ninja on PATH
(`pip install cmake ninja`). All third-party code is vendored in `external/`.

```
build.cmd                 # -> build\bin\MMDX12.exe (RelWithDebInfo)
build.cmd build --target asset_probe
```

## Run

`build\bin\MMDX12.exe`. The library folder is resolved as follows: `--library`, then `mmdx12.ini`,
then `<exe>\library`, then the first `library\` folder found walking up from the exe.
The demo assets came from the the server server (`<server>/assets`) into `library/`, which is git-ignored.

Screens: **select** (character / stage / song) → **play**, and **benchmark** (lobby + online
leaderboard → fixed-workload run → score/submit).

Play controls: Space (play/pause), ←/→ (seek ±5 s), C (motion/free camera), F1 (hide UI),
mouse drag (orbit), right drag (pan), wheel (zoom), Esc (back).

Command line (also used for automated checks):
```
--library <dir>  --character <s> --stage <s|none> --song <s>  --autoplay  --seek <sec>
--benchmark dx12-raster-fhd|dx12-raster-4k  --bench-frames <n>
--frames <n>  --capture <out.png>  --width <w> --height <h>  --debug
--free-camera  --screen select|bench   (UI capture testing)
```

## Library classification (src/asset/AssetLibrary.cpp)

- **Character**: a PMX with humanoid bones (`頭`, `左腕`, `左足`/`左ひざ`), not under a `stage` folder.
- **Stage**: any other PMX. All PMX files in one folder form one stage (multi-part stages).
  Byte-identical duplicates are dropped. A non-humanoid PMX next to a character is treated as an accessory and skipped.
- **Song**: a folder with a VMD containing bone keys. The camera is the VMD with camera keys.
  Morph-only VMDs are facial layers. The audio is a wav/mp3/flac/ogg in the same or the parent folder.
  When there are several dance VMDs, the one whose length best matches the audio wins.

## Architecture

```
src/core    Log, text encodings (UTF-8 / UTF-16 / Shift-JIS)
src/asset   PMX 2.0/2.1 + VMD parsers, image decode (stb + WIC), library scanner
src/anim    ModelInstance (bone hierarchy, append bones, CCD IK with limits, vertex/bone/group morphs),
            BoundMotion / CameraMotion (VMD bezier evaluation)
src/render  Dx12Context (device, swap chain, frame pacing, descriptor heaps, uploads, capture)
            Renderer = ordered IRenderPass list: ScenePass -> ResolvePass -> PresentPass
            GpuModel (GPU skinning via StructuredBuffer, morph delta stream), IUpscaler seam
src/app     App state machine + ImGui screens, scene loader (worker thread), benchmark, leaderboard (WinHTTP)
src/audio   miniaudio playback; audio cursor is the master clock
shaders     mmd.hlsl (MMD toon shading + inverted-hull outline), present.hlsl
tools       asset_probe, anim_probe, render_smoke (headless-ish self tests)
```

Conventions: MMD native space (left-handed, +Y up) everywhere. It matches D3D, so nothing is
axis-flipped. DirectXMath row vectors (`v * M`); HLSL uses `pack_matrix(row_major)`.

Leaderboard: `https://home.codingbot.kr/api/benchmark`,
categories `dx12-raster-fhd` / `dx12-raster-4k`. The score formula is the same as the web MikuMark.

## Roadmap (beyond the MVP)

- Physics (Bullet rigid bodies/joints): hair and skirts are currently rigid.
- Material morphs, SDEF skinning, PMD support, VMD light track.
- Shadow map pass, HDR + tone mapping, post FX (bloom/DoF) as new `IRenderPass`es.
- DXR (reflections/shadows/GI), and DLSS / FSR / XeSS behind `IUpscaler`. These need motion vectors
  and jitter, plus the Agility SDK with DXC (SM 6.x).
- Library index cache (the scan currently re-probes all files on every launch).
