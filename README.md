<div align="center">
  <h1>MMDX12</h1>
  <p><b>English</b> · <a href="README.ko.md">한국어</a></p>
  <p>A native Direct3D 12 MikuMikuDance player — real-time ray tracing, path tracing, and an offline GI renderer for Blender-class MMD videos.</p>
  <p>
    <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-39C5BB" alt="Platform">
    <img src="https://img.shields.io/badge/Graphics-Direct3D%2012%20%2B%20DXR%201.1-39C5BB" alt="Graphics">
    <img src="https://img.shields.io/badge/Language-C%2B%2B20-39C5BB" alt="Language">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-39C5BB" alt="License"></a>
    <a href="https://youtu.be/7S7D670HbIM"><img src="https://img.shields.io/badge/YouTube-Introduction-39C5BB?logo=youtube&logoColor=white" alt="YouTube"></a>
  </p>
  <a href="https://youtu.be/7S7D670HbIM">
    <img src="docs/media/promo-preview.webp" width="100%" alt="MMDX12 preview">
  </a>
  <br>
  <a href="https://youtu.be/7S7D670HbIM">▶ Watch the introduction on YouTube</a>
  · <a href="https://github.com/digital8150/MMDX12/releases/latest/download/MMDX12-intro-1080p60.mp4">Download the 58 s promo (1080p60 MP4)</a>
  <p>
    <a href="#gallery">Gallery</a> ·
    <a href="#highlights">Highlights</a> ·
    <a href="#bring-your-own-assets">Assets</a> ·
    <a href="#getting-started">Getting started</a> ·
    <a href="#benchmark-for-fun">Benchmark</a>
  </p>
</div>

## Gallery

Unedited stills from the built-in offline GI renderer (3840×2160, shown downscaled).

<table>
  <tr>
    <td>
      <img src="docs/media/gi-open-arms.jpg" width="100%">
      <br><i>Soft studio light, thin-lens depth of field</i>
    </td>
    <td>
      <img src="docs/media/gi-point.jpg" width="100%">
      <br><i>Close-up with a bokeh foreground</i>
    </td>
  </tr>
  <tr>
    <td>
      <img src="docs/media/gi-stage.jpg" width="100%">
      <br><i>Night stage, depth of field and bokeh</i>
    </td>
    <td>
      <img src="docs/media/gi-render-bench.jpg" width="100%">
      <br><i>GI render benchmark: refraction and dispersion through a glass cube</i>
    </td>
  </tr>
</table>

## Highlights

- **Drop-in library**: any folder layout; characters, stages and dance sets are classified by content.
- **Real-time raster**: MMD toon shading, cascaded shadows, SSAO, SSR and TAA.
- **DXR 1.1 ray tracing**: RT shadows, RTAO and RT reflections.
- **Path tracing**: 1–4 spp with a temporal + à-trous denoiser.
- **Upscalers**: DLSS, FSR and XeSS.
- **Post effects**: depth of field, volumetric light, FFT convolution bloom, colour LUTs (built-in looks or any `.cube`).
- **Physics**: PMX rigid bodies and joints in Bullet (hair, skirts, accessories).
- **Offline GI renderer**: irradiance cache + path tracing with depth of field, motion blur and skin subsurface scattering, for 4K stills and MP4 videos. The render dialog measures its own time estimate.
- **Benchmark**: real-time and GI render categories with an online leaderboard.

## Interface

<img src="docs/media/ui-library.jpg" width="100%" alt="Library UI">
<br><sub>The library screen. The UI is in Korean.</sub>

## Bring your own assets

> [!IMPORTANT]
> MMDX12 ships NO models, motions, stages, or music. MMD assets are licensed by their authors and cannot be redistributed, so you point MMDX12 at your own library folder and must follow each asset's terms (e.g., many models forbid commercial use or require credit when you publish videos). The images and film in this README were made with the author's local library.

The library folder is resolved as follows: `--library`, then `mmdx12.ini`, then `<exe>\library`, then the first `library\` folder found walking up from the executable.

**Library classification**
- **Character**: a PMX with humanoid bones (`頭`, `左腕`, `左足`/`左ひざ`), not under a `stage` folder.
- **Stage**: any other PMX. All PMX files in one folder form one stage (multi-part stages). Byte-identical duplicates are dropped. A non-humanoid PMX next to a character is treated as an accessory and skipped.
- **Song**: a folder with a VMD containing bone keys. The camera is the VMD with camera keys. Morph-only VMDs are facial layers. The audio is a wav/mp3/flac/ogg in the same or the parent folder. When there are several dance VMDs, the one whose length best matches the audio wins.

## Getting started

**Requirements:**
- Windows 10/11, a D3D12 GPU.
- DXR 1.1 required for RT/PT/GI.
- Visual Studio 2026 v18 with MSVC 14.5x + Windows SDK.
- CMake + Ninja (via `pip install cmake ninja`).

**Build commands:**
```text
build.cmd
build.cmd build --target asset_probe
```

**Optional upscalers:**
Fetch the optional upscaler DLLs, then rebuild (missing DLLs only disable that upscaler):
```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_sdks.ps1
```

**Run:**
`build\bin\MMDX12.exe`

## Using it

- **Screens**: Library → Play, 영상 렌더 render dialog, Benchmark.
- **Play controls**: Space (play/pause), ←/→ (seek ±5 s), C (motion/free camera), L (lighting preset), F1 (hide UI), mouse drag (orbit), right drag (pan), wheel (zoom), Esc (back).
- **Offline renders**:
  - P key / camera button = 4K PNG still in `Pictures\MMDX12` (~20 s on an RTX 3060 Laptop).
  - 영상 렌더 (next to 플레이) = H.264 + AAC MP4 in `Videos\MMDX12`, 720p–4K at 24/30/60 fps. Renderer choice: raster, RT, PT or offline GI (offline GI at 4K: ~10 s per frame on an RTX 3060 Laptop, about a day for a full song). Esc stops and keeps the frames so far.

## Benchmark (for fun)

- Because no assets can ship, there is no official benchmark scene. The scene is picked automatically from your library: the character and stage with the most vertices, and the song whose dance length is closest to 2:30 (songs with a camera motion preferred).
- The GI render benchmark (Cinebench style, `dx12-gi-render`): one 3840×2160 image at 4096 samples per pixel, scored by render time (score = 6.25 × million samples per second). The cast is the three characters with the most vertices in your library, each holding a pose taken from a dance in your library (deterministic per library), behind a refracting glass cube with dispersion; softboxes and coloured rim spots.

| Categories | Resolution & Measurement |
|---|---|
| `dx12-raster-fhd`<br>`dx12-rt-fhd`<br>`dx12-pt-fhd` | **1080p real-time**: 120 warm-up + 3600 measured frames at a fixed 1/60 s step, vsync/upscaler off, MSAA 4x for raster/RT, PT 1 spp 3 bounces. |
| `dx12-raster-4k`<br>`dx12-rt-4k`<br>`dx12-pt-4k` | **4K real-time**: 120 warm-up + 3600 measured frames at a fixed 1/60 s step, vsync/upscaler off, MSAA 4x for raster/RT, PT 1 spp 3 bounces. |
| `dx12-gi-render` | **4K offline GI**: one 3840×2160 image at 4096 samples per pixel. |

- Scores depend on the library you use, so the leaderboard is for fun, not a reference. Results can be submitted to the online leaderboard from the result screen.
- Real-time score formula summary: `(avg*0.6 + low1*0.4) × resolutionFactor × stabilityFactor × 100`, tiers D…SSS.

## Command line

<details>
<summary>Command-line options</summary>

```text
--library <dir>  --character <s> --stage <s|none> --song <s>  --autoplay  --seek <sec>
--benchmark dx12-raster-fhd|dx12-raster-4k|dx12-rt-fhd|dx12-rt-4k|dx12-pt-fhd|dx12-pt-4k|dx12-gi-render  --bench-frames <n>
--bench-spp <n>  (GI render benchmark testing: samples per pixel; with --offline-size the result cannot be submitted)
--frames <n>  --capture <out.png>  --width <w> --height <h>  --debug
--free-camera  --camera tx,ty,tz,yaw,pitch,dist  --paused  --lighting 0..3  --quality 0..3  --no-physics
--render raster|rt|pt  --upscaler none|dlss|fsr|xess  --upscale-quality native|quality|balanced|performance|ultra
--dof 0|1  --volumetric 0|1  --bloom-conv 0|1  --lut <name|none>
--offline-still <out.png> | --offline-video <out.mp4> [--offline-range <a> <b>]   (offline GI render, then quit)
--offline-fps 24|30|60  --offline-bitrate <mbps>  --offline-quality 0..3   (video format for this run; default: the select screen dialog's)
--offline-renderer raster|rt|pt|gi   (video renderer for this run; default: the dialog's; --dof/--volumetric/--bloom-conv also apply to the video)
--offline-probe   (the dialog's time measurement from a play scene; logs "VIDEO PROBE", then quits)
--offline-spp <n>  --offline-size <w> <h>   (testing: cap samples / override the output size)
--screen select|stages|songs|settings|video|bench|bench-gi   (UI capture testing; video = the render dialog, or with
                                                              --offline-video renders straight from the select screen)
```

</details>

## Architecture

<details>
<summary>Module overview</summary>

```text
src/core    Log, text encodings (UTF-8 / UTF-16 / Shift-JIS)
src/asset   PMX 2.0/2.1 + VMD parsers, image decode (stb + WIC), library scanner
src/anim    ModelInstance (bone hierarchy, append bones, CCD IK with limits, vertex/bone/group morphs),
            PhysicsWorld (Bullet rigid bodies + 6DOF spring joints: hair, skirts), BoundMotion /
            CameraMotion (VMD bezier evaluation)
src/render  Dx12Context (device, swap chain, frame pacing, descriptor heaps, uploads, capture)
            Renderer = ordered IRenderPass list (Passes.h): Shadow (3 cascades) -> Scene (MSAA MRT:
            HDR colour, view normal + reflectivity, velocity) -> Resolve -> SSAO -> SSR -> Composite
            (AO, reflections, haze) -> Volumetric -> TAA (optional) -> Upscale -> DoF -> Bloom (mip
            chain or FFT convolution) -> Post (PBR Neutral tonemap, grade, LUT, vignette) -> UI
            backdrop blur -> Present
            GpuModel (GPU skinning via StructuredBuffer, morph delta stream), IUpscaler seam
            OfflineRenderer (offline GI: irradiance cache prepass + path tracing, DoF, motion blur, SSS, outlines)
src/app     App state machine + ImGui screens, VideoEncoder (Media Foundation MP4), scene loader (worker thread), benchmark, leaderboard (WinHTTP)
src/audio   miniaudio playback; audio cursor is the master clock
shaders     mmd.hlsl (MMD toon + shadows through the toon ramp, punctual lights, rim, sky, studio
            floor, shadow map), resolve/ssao/ssr/composite/taa/bloom/post/present.hlsl,
            dof, volumetric(_apply), bloom_fft (compute FFT), offline_gi/offline_post/offline_edge
tools       asset_probe, anim_probe, render_smoke (headless-ish self tests)
```

Conventions: MMD native space (left-handed, +Y up) everywhere. It matches D3D, so nothing is axis-flipped. DirectXMath row vectors (`v * M`); HLSL uses `pack_matrix(row_major)`.

</details>

## Third-party

| Dependency | License |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [stb](https://github.com/nothings/stb) | MIT / Public domain |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / Public domain |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [Bullet Physics 3.25 subset](https://github.com/bulletphysics/bullet3) | zlib |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NVIDIA RTX SDK license |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | MIT |
| [Intel XeSS SDK](https://github.com/intel/xess) | Intel Simplified Software License |
| [Pretendard](https://github.com/orioncactus/pretendard) | SIL OFL 1.1 |
| [Phosphor Icons](https://github.com/phosphor-icons/core) | MIT |

*Note: "MikuMikuDance" is by Yu Higuchi (樋口優) and Hatsune Miku is a Crypton Future Media character; MMDX12 is an unofficial fan project, not affiliated with either.*

## License

[MIT](LICENSE) for MMDX12's own code; third-party code under its own licences; MMD assets are not covered.

## Roadmap

- Material morphs, SDEF skinning, PMD support, VMD light track.
- Library index cache.
