# progress

## 2026-10-02 — MVP

**Goal**: a DX12 MMD Windows client. This session builds the MVP: an asset engine, a player engine, and the basic screen flow on a simple raster renderer, keeping extension points for RTX, DLSS/FSR/XeSS, and advanced shaders.

### Done
- Project skeleton: CMake + Ninja (`build.cmd`), vendored dependencies (imgui 1.92, stb, miniaudio 0.11.25, DirectX-Headers, nlohmann json).
- Assets: copied from a private asset collection into `library/` (git-ignored, about 1.4 GB, 971 files).
- Asset engine: PMX 2.0/2.1 and VMD parsers, plus a content-based library classifier.
  - Classification: characters, stages (a folder of PMX files forms one stage), songs (dance + camera + facial VMDs + audio), accessories excluded.
  - Result: 30 characters, 8 stages, 14 songs, scanned in 0.2 s.
- Player engine: bone hierarchy, append bones, CCD IK (knee plane solver, Euler limits), vertex/bone/group morphs, VMD Bezier, and the MMD camera (including the distance=0 style). Audio is the master clock for sync.
- Renderer: `Dx12Context`, plus an `IRenderPass` chain (Scene → Resolve → Present).
  - Features: GPU skinning, MMD toon shading, inverted-hull outlines, MSAA HDR target, letterboxed present, GPU timestamps, PNG capture.
  - Extension points: the `IUpscaler` seam, and device capability detection (RT tier 1.2, mesh shaders, VRS).
- App: a Korean ImGui UI with three flows: select → play, and benchmark lobby → run → result.
  - Settings persist in `mmdx12.ini`.
  - CLI automation: `--autoplay`, `--frames`, `--capture`, `--screen`, `--free-camera`, `--benchmark`.
- Benchmark: fixed workload (1/60 s of motion per rendered frame, muted, vsync off, MSAA 4x, fixed resolution), using the web MikuMark score formula.
  - Official preset: DIVA Miku / theater / worldismine.
  - Result on an RTX 3060 Laptop: FHD 86,562, 4K 74,904 (1,200-frame short runs).
- Server: added `dx12-raster-fhd` and `dx12-raster-4k` to the `CATEGORIES` whitelist on the leaderboard server.
  - Backup: `server/<server script>.pre-dx12.bak`. Not committed in that repo.
- Way of working: Claude designed the headers and contracts and wrote the anim module. opencode (GLM) implemented the asset, render, and app modules in parallel, and Claude reviewed and fixed them.

### Pitfalls found
- On Windows, extracting the server's lowercase symlink aliases deletes their case-variant targets. Copy regular files only.
- FXC ignores `pack_matrix` for StructuredBuffer elements, so bone matrices arrive transposed. Fix: a struct with a `row_major` member.
- The shader copy used to be a POST_BUILD step, so a shader-only change didn't reach `bin/`. It is now the `copy_shaders` target.
- Some PMX files have the magic `PMX\xA0`. Only the first three bytes are checked now.
- Worker bugs (fixed): texture upload copy direction reversed, per-frame buffers created in the DEFAULT heap, wrong pointer passed to the PNG writer, song preset searched among characters.

### Not verified / not done
- Leaderboard **submission** was never executed (to avoid polluting the public board). Fetching works.
- No physics (Bullet): hair and skirts are rigid. Missing: material morphs, SDEF, PMD, VMD lights, shadows.
- DX12 scores dwarf the web tier thresholds (SSS = 18,000). The tier table needs revisiting when RT/post-FX arrive.
- Variant models (PS models' ITTW46 versions, furina 荒, snow_miku/miku.pmx) appear as separate entries.

### Next
- Test leaderboard submission once (with the user's consent).
- Physics (Bullet), then shadow map, HDR, and post FX as passes; then Agility SDK + DXC, DXR, and upscalers.
- A library index cache, and a thumbnail/preview on the select screen.

## 2026-10-02 — DXR ray tracing, path tracing, DLSS/FSR/XeSS, RT/PT benchmarks

**Goal**: add DXR ray tracing, path tracing, and DLSS/XeSS/FSR, plus new benchmark categories (RT 4K/FHD, PT 4K/FHD).

This commit also contains earlier uncommitted work that never got a progress entry: Bullet physics, the shadow → SSAO → SSR → composite → TAA → bloom → post pass chain, the UiKit redesign (DESIGN.md/PRODUCT.md, Pretendard + Phosphor), lighting presets, and the thumbnail cache.

### Done
- Render paths (`RenderSettings::renderPath`; falls back to raster without DXR 1.1 / SM 6.5 / dxcompiler.dll):
  - RayTraced: raster G-buffer plus inline-RayQuery sun shadows in the scene PS (mmd.hlsl `RT_SHADOWS`), RTAO, and RT reflections.
  - PathTraced: compute path tracer (`pathtrace.hlsl`: sun/punctual NEE, stochastic alpha, analytic studio floor, part of MMD ambient emitted), followed by the denoiser (`pt_denoise.hlsl`: temporal → 3× à-trous → albedo remodulate). It writes the normal G-buffer, so composite/TAA/upscalers/post are unchanged. No outlines.
- `RtScene` (RayTracing.cpp): a compute shader (`skin.hlsl`) skins every model into world-space `RtVertex` buffers, including the previous pose for motion vectors. One BLAS per model with one geometry per drawable material (characters rebuilt every frame, stages once). The TLAS and the `RtGeometry` table are rebuilt per frame. Shaders read geometry bindlessly through unbounded tables over the SRV heap.
- `CompileShaderDxc` (Windows SDK dxcompiler, `-HV 2018`) and `ComputePipeline` (a shared RT root layout).
- Upscalers behind `IUpscaler`:
  - DLSS (NGX 310.9.1), FSR (FidelityFX API 2.3.0 via its loader DLL), XeSS 3.0.2 (delay-loaded).
  - Render and output resolution are now split: the upscale pass runs after TAA, and bloom/post/backdrop/present work at output size.
  - Jitter uses the FSR pixel convention; the motion-vector scale is −renderSize.
- App:
  - Render mode, upscaler, and quality on the select screen; PT samples and bounces in the advanced settings; all persisted in mmdx12.ini.
  - CLI flags `--render`, `--upscaler`, `--upscale-quality`.
  - Benchmark lobby: a render-mode selector plus 4 new categories (`dx12-rt-fhd/4k`, `dx12-pt-fhd/4k`). RT keeps MSAA 4x; PT uses 1 spp and 3 bounces; neither uses an upscaler.
- Server: added the 4 categories to the leaderboard server's `CATEGORIES` whitelist.
- SDKs vendored under `external/{dlss,xess,ffx}` (headers + import libs). The runtime DLLs are git-ignored and fetched with `tools/fetch_sdks.ps1`.
- Results on an RTX 3060 Laptop (600-frame runs, not submitted):

  | Category | Score |
  |---|---|
  | raster FHD | 23,391 |
  | RT FHD | 8,300 |
  | RT 4K | 5,282 |
  | PT FHD | 8,607 |
  | PT 4K | 4,411 |

- Way of working: Claude wrote the contracts (headers, `rt_common.hlsli`, CMake, SDK vendoring). Five opencode (GLM) workers built the parts in parallel, each in its own build dir; Claude reviewed and fixed the results.

### Pitfalls found
- HLSL 2021 (the DXC default) rejects the vector ternaries in common.hlsli, which FXC also compiles, so DXC runs with `-HV 2018`.
- Including `<d3d12.h>` before d3dx12 picks up the Windows SDK header and breaks d3dx12 (missing `TIGHT_ALIGNMENT`). Use `<directx/d3d12.h>`.
- `ffx_upscale.h` includes `../../api/include/...`, so the FidelityFX headers keep the SDK's `api/` and `upscalers/` layout.
- Worker bugs fixed: a buffer helper ignored its resource flags (UAV/AS buffers → E_INVALIDARG, then device removed); the index-buffer SRV was structured without a stride (it must be raw).
- A single jittered reflection ray is speckled without temporal accumulation. RT reflections now trace one sharp ray and approximate gloss with texture LOD and a distance fade.

### Not verified / not done
- Upscaler jitter/MV signs were checked only on paused frames, where all three upscalers converge sharp. No motion or ghosting check yet.
- At 1280x720 the benchmark lobby's start button is clipped (it fits at the default 1600x900).
- The tier table is still the web one: raster scores land in SSS.
- PT has no outlines. Lighting presets were tuned for raster; PT brightness relies on the ambient-emission heuristic.

### Next
- A temporal pass for RT effects (glossy reflections, soft RT shadows without noise) and reuse of PT history across slow camera moves.
- Retune benchmark tiers per category; test one leaderboard submission (with consent).
- A scrollable benchmark lobby for small windows.
