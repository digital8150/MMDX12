# MMDX12: MikuMikuDance DX12 Player

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

Screens: **library** (character / stage / song cards with rendered thumbnails, scene preview,
graphics quality and lighting) → **play**, and **benchmark** (lobby + online leaderboard →
fixed-workload run → score/submit). Light theme, Pretendard + Phosphor (assets/fonts, OFL/MIT).
Thumbnails are rendered once and cached in `%LOCALAPPDATA%/MMDX12/thumbs`.

Rendering features:

- **DXR 1.1 ray tracing**: RT shadows, RTAO, RT reflections (렌더링 → 레이 트레이싱).
- **Path tracing**: 1–4 samples per pixel with a temporal + à-trous denoiser (패스 트레이싱).
- **Upscalers**: DLSS, FSR and XeSS (품질/균형/성능/울트라); the DLLs are fetched with
  `tools/fetch_sdks.ps1`.
- **Benchmark categories**: `dx12-rt-fhd`, `dx12-rt-4k`, `dx12-pt-fhd`, `dx12-pt-4k` alongside the
  raster ones.

Physics: the character's PMX rigid bodies and joints run in Bullet (hair, skirts, accessories),
stepped by the motion clock at 120 Hz. Seeks, loops and teleports in the motion reset the bodies to
the animated pose. Toggle it under 세부 설정 → 물리 연산, or with `--no-physics`. The benchmark always
simulates.

Play controls: Space (play/pause), ←/→ (seek ±5 s), C (motion/free camera), L (lighting preset),
F1 (hide UI), mouse drag (orbit), right drag (pan), wheel (zoom), Esc (back).

Command line (also used for automated checks):
```
--library <dir>  --character <s> --stage <s|none> --song <s>  --autoplay  --seek <sec>
--benchmark dx12-raster-fhd|dx12-raster-4k|dx12-rt-fhd|dx12-rt-4k|dx12-pt-fhd|dx12-pt-4k  --bench-frames <n>
--frames <n>  --capture <out.png>  --width <w> --height <h>  --debug
--free-camera  --camera tx,ty,tz,yaw,pitch,dist  --paused  --lighting 0..3  --quality 0..3  --no-physics
--render raster|rt|pt  --upscaler none|dlss|fsr|xess  --upscale-quality native|quality|balanced|performance|ultra
--screen select|stages|songs|settings|bench   (UI capture testing)
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
            PhysicsWorld (Bullet rigid bodies + 6DOF spring joints: hair, skirts), BoundMotion /
            CameraMotion (VMD bezier evaluation)
src/render  Dx12Context (device, swap chain, frame pacing, descriptor heaps, uploads, capture)
            Renderer = ordered IRenderPass list (Passes.h): Shadow (3 cascades) -> Scene (MSAA MRT:
            HDR colour, view normal + reflectivity, velocity) -> Resolve -> SSAO -> SSR -> Composite
            (AO, reflections, haze) -> TAA (optional) -> Bloom -> Post (PBR Neutral tonemap, grade,
            vignette) -> UI backdrop blur -> Present
            GpuModel (GPU skinning via StructuredBuffer, morph delta stream), IUpscaler seam
src/app     App state machine + ImGui screens, scene loader (worker thread), benchmark, leaderboard (WinHTTP)
src/audio   miniaudio playback; audio cursor is the master clock
shaders     mmd.hlsl (MMD toon + shadows through the toon ramp, punctual lights, rim, sky, studio
            floor, shadow map), resolve/ssao/ssr/composite/taa/bloom/post/present.hlsl
tools       asset_probe, anim_probe, render_smoke (headless-ish self tests)
```

Conventions: MMD native space (left-handed, +Y up) everywhere. It matches D3D, so nothing is
axis-flipped. DirectXMath row vectors (`v * M`); HLSL uses `pack_matrix(row_major)`.

Leaderboard: `https://home.codingbot.kr/api/benchmark`,
categories `dx12-raster-fhd` / `dx12-raster-4k`. The score formula is the same as the web MikuMark.

## Roadmap (beyond the MVP)

- Material morphs, SDEF skinning, PMD support, VMD light track.
- DoF, motion blur, contact shadows.
- DXR (reflections/shadows/GI), and DLSS / FSR / XeSS behind `IUpscaler` (motion vectors and
  jitter already exist); needs the Agility SDK with DXC (SM 6.x).
- Library index cache (the scan currently re-probes all files on every launch).
