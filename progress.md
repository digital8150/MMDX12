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

## 2026-10-02 — Post FX: depth of field, volumetric light, colour LUTs, FFT convolution bloom

**Goal**: stronger graphics/post: 1) DoF 2) volumetrics 3) LUT 4) convolution bloom.

This commit also contains earlier uncommitted RT work that never got a progress entry: two BLASes per model
(single-/double-sided materials, `TRIANGLE_CULL_DISABLE` on the double-sided instance) so RT rays cull back faces
like the raster pass, instance masks (stage 0x01, character 0x02), and the path tracer shading character
materials with the raster toon sun (`ToonSun` in pathtrace.hlsl) instead of Lambert NEE.

### Done
- Pass order is now Composite → **Volumetric** → TAA → Upscale → **DoF** → Bloom (**mip chain or FFT**) → Post (**LUT**).
  Pass code split: `PassCommon.h` (shared helpers), `PassBloom.cpp`, `PassDof.cpp`, `PassVolumetric.cpp`.
- DoF (`dof.hlsl`, output res): signed CoC `aperture * (z - F) / z * maxRadius`, half-res Gustafsson golden-angle
  gather (~64 taps, near field bleeds over the background), tent filter, full-res blend. Focus = view z of the
  character's head bone (`FrameView::focusDistance`), otherwise autofocus on the screen centre.
- Volumetric (`volumetric.hlsl` compute + `volumetric_apply.hlsl`): half render res, 32-step jittered march through
  height fog; sun via the cascaded shadow map (ShadowPass now renders it on RT/PT too while volumetrics are on),
  spot lights as cones (omni fills at 15% to avoid a uniform veil); depth-aware blur and additive upsample into lit.
- Colour LUTs (`ColorLut.h/.cpp`): 32³ RGBA8 strip, six built-in looks (시네마틱, 따뜻한 필름, 차가운 밤, 애니 비비드,
  빈티지, 흑백) + `.cube` files from `<exe>/luts` and `<library>/luts` (trilinear resample, DOMAIN_MIN/MAX).
  `Renderer::SetColorLut` uploads; PostPass applies it after the sRGB encode with an intensity.
- FFT convolution bloom (`bloom_fft.hlsl`): 512² grid, R+iG / B+i0 packing, radix-2 Stockham FFT in groupshared
  (rows → columns × kernel spectrum → inverse columns → inverse rows), procedural kernel (core + halo + 3 spike lines
  = 6-ray starburst) normalised by K(0,0), cached spectrum. Output written into the bloom mip 0, so PostPass is unchanged.
- App: settings + ini (`dof`, `dofAperture`, `volumetric`, `volumetricDensity`, `bloomConvolution`, `colorLut`,
  `lutIntensity`), 세부 설정 → 효과 / 컬러 LUT UI (independent of the quality presets), CLI `--dof`, `--volumetric`,
  `--bloom-conv`, `--lut <name|none>` (restored on exit). The benchmark forces all four off.
- Way of working: Claude wrote the contracts (headers, stubs, settings/CLI plumbing); five opencode (GLM) workers
  implemented DoF, volumetric, FFT bloom, LUT and UI in parallel; Claude reviewed, fixed and tuned.

### Pitfalls found
- `line` is a reserved word in HLSL: DXC failed on `bloom_fft.hlsl`, and the bloom silently fell back to the mip chain.
  The worker reported it as working. Grep `mmdx12.log` for `[E]`/`[W]` (not "ERROR") after every capture.
- Worker bugs fixed: the kernel-generation dispatch had an empty UAV table (kernel never written → 0/0); the FFT grid
  was read as an SRV while still in UAV state; the `.cube` loader kept `PathToUtf8(...).c_str()` of a temporary.
- Validating the FFT: replace `CSInput` with a single bright texel at the grid centre (bin shader copy) — the output
  must show the kernel centred on screen.
- First volumetric constants washed the image out (strong height falloff left the overhead spot cones empty while the
  omni fill lit the whole volume). Now sigma0 0.005, falloff 0.02, spot boost 3.

### Not verified / not done
- No visible sun shafts in the test stages (nothing occludes the sun); only checked as an even atmospheric haze.
- Starburst spikes are subtle on these scenes (no point-like HDR highlights). Kernel parameters are constants in
  `PassBloom.cpp`, not user settings.
- The new UI section was not captured (the advanced panel is collapsed by default); only built and run.
- No performance measurement of the new passes.
- XeSS shows dotted stair-stepping on hair silhouettes, also with every new effect off (pre-existing).

### Next
- Sun-shaft test scene (or a stage with windows), temporal reuse for the volumetric march.
- Expose DoF focus mode (head / centre / manual) and the bloom kernel shape; per-preset LUT defaults.
- Investigate the XeSS hair artefact (reactive/transparency mask).

## 2026-10-02/03 — Offline GI renderer: high-quality stills and 4K60 video

**Goal**: a non-real-time, maximum-quality GI renderer (Cinema 4D / V-Ray style) independent of the
graphics settings: 1) a high-quality screenshot button on the play bar, 2) a full render mode that renders
every frame and encodes an MP4, 3) GI with a Pixar/Disney-like look while keeping the toon style.

### Done
- `OfflineRenderer` (render/OfflineRenderer.h/.cpp, RendererOffline.cpp): `Renderer::BeginOffline` builds the
  TLAS, then `Renderer::RenderOffline` replaces `Render` each frame (GPU-time-budgeted iterations, preview
  present, readback). Fixed formats, no quality settings: stills 3840×2160, videos 3840×2160 60 fps.
- GI, Cinema 4D irradiance-cache style (`offline_gi.hlsl`): prepass = 6 adaptive coarse-to-fine screen-grid
  levels of indirect irradiance (512 multi-bounce gather paths per sample, refined only at normal/depth
  discontinuities or irradiance contrast), shown as the cache-lit scene with sample dots, then edge-aware
  smoothing. `CSRender` takes indirect diffuse at camera hits from the cache (`IcLookup` projects the hit
  into the frame camera, so lens/shutter samples work) and brute-forces hits it does not cover. Up to 12
  bounces, NEE everywhere, one punctual shadow ray per vertex picked by unshadowed contribution.
- Adaptive sampling (perceptual-luminance standard error < 0.004, 256–4096 spp), progressive preview
  (1, 2, 3 … iterations per frame: V-Ray-like noise → clean), denoise / bloom / haze / soft grade
  (`offline_post.hlsl`).
- Toon look kept: raster ToonSun key light + GI fill (×0.6), faces take an even fill around the view
  direction, MMD outlines from a raster inverted-hull layer (`offline_edge.hlsl`).
- Camera effects: thin-lens DoF focused on the head bone (lens radius 0.7% of the focus distance), motion
  blur with a 180° shutter: every iteration re-skins the character at its shutter time
  (`RtScene::Build(..., time)`, `skin.hlsl` blends bones/morphs), the camera is interpolated
  (`SceneConstants::prevInvView`, offline_common.hlsli), and outlines are re-drawn per iteration with the
  same lens/shutter sample and averaged. Videos re-upload the previous video frame's pose into the
  previous ring entry; a still taken during playback uses the last live frame.
- Skin SSS: per-texel skin detection from colour (names are useless: `Material1..5` atlases), sun
  visibility diffused over per-channel scatter radii (soft, red-fringed shadow edges), warm terminator,
  translucency of thin backlit parts from a traced thickness.
- `VideoEncoder` (Media Foundation sink writer): H.264 (100 Mbps at 4K) + AAC from the song (miniaudio
  decode), CPU RGBA→NV12 BT.709; cancelled renders still produce a playable file.
- App: play bar buttons (카메라 = 고품질 스크린샷 / P, 필름 = 영상 렌더 with a confirmation dialog and time
  estimate), Offline screen with progress / ETA / cancel (Esc), toast with "폴더 열기", outputs in
  Pictures\MMDX12 and Videos\MMDX12. CLI: `--offline-still`, `--offline-video`, `--offline-range`,
  `--offline-spp`, `--offline-size` (the app quits when done).
- Measured on the RTX 3060 Laptop: 4K still ~21 s, 4K60 video ~9.5 s per frame (a full song ≈ 1.1 days).
- Way of working: Claude wrote the contracts and all shaders; three opencode (GLM) workers built the
  render host, the encoder and the App/UI; Claude reviewed, fixed and later reworked the host itself.

### Pitfalls found
- `--autoplay` and `--seek` are consumed after loading; my first CLI trigger depended on them, so test runs
  silently waited for a manual click (the user had to press the button). CLI triggers now use only the
  `--offline-*` options, and `--offline-still` holds playback at the seek point.
- One shadow ray per punctual light per sample made the Concert preset (16 spots) >10 min per still;
  picking one light by unshadowed contribution fixed it (16 s).
- A cache table reused across video frames got slower as it filled; the cache is now rebuilt per image.
- `gTime` already exists in SceneCB (FXC "redefinition" in offline_edge.hlsl).
- An inline `OfflineRenderer() = default` with a `unique_ptr<Impl>` member needs the out-of-line ctor.
- The studio floor needs the raster cyclorama fade (stochastic coverage) and horizon-colour misses below
  the horizon, otherwise a hard seam appears.

### Not verified / not done
- GI flicker over long videos (the cache is recomputed independently per frame); only 3–15 frame clips
  were checked.
- Only MikuProjectDIVA on theater / no stage was tested; colour-based skin detection may catch beige
  clothes.
- Stills taken while paused have no motion blur (no physics-consistent previous pose).
- No render buckets (the render is progressive over the whole frame).
- The offline renderer allocates ~1.5 GB at 4K (accumulators, outline MSAA, cache levels).

### Next
- Temporal stability for video GI (reuse / blend the cache between frames, flicker test on a long clip).
- Test more characters and stages; tune the SSS / skin detection per rig.
- Bucket overlay in the preview, optional.

## 2026-10-03/04 — Video dialog: renderer choice, measured estimate, GI effects

### Done
- Render dialog (`UiVideoDialog.cpp`, delegated to a Gemini 3.8 Flash worker, reviewed and re-laid out by Claude): renderer cards
  (래스터 / 실시간 RT / 실시간 PT / 오프라인 GI), quality, effects (bloom, convolution bloom, volumetric + density, DoF + aperture only
  for the real-time renderers), format, pinned estimate with a 실측/추정 badge and a progress bar while measuring.
- Real-time renderers as video sources: `RecordRealtimeVideoFrame` renders at the video size (`fixedResolution`, no vsync), PT frames
  accumulate `ptPasses` app frames; `Renderer::ReadFinalImage` reads `targets_.ldr`.
- Sample render, automatic and in the background (no button): while the dialog is open, every new combination of assets + renderer /
  resolution / quality / bloom / convolution / volumetric / DoF (not density, fps, bitrate) is measured after 0.8 s without changes
  (`UpdateVideoProbe`): the scene loads once on a worker thread (`bgProbe_`, held in `scene_` while the dialog is open), then one frame
  renders headless behind the UI (`RenderSettings::headless`, `Renderer::SetOfflinePresent(false)`, `Renderer::ClearBackBuffer`).
  A settings change cancels the running sample; known combinations are reused from the saved results, never re-rendered; closing the
  dialog or starting a render stops it. GI measures one frame with motion blur from the previous video frame's
  pose; real-time renderers render 3 frames and average the last 2 (the first is a cold start, 1.5 s vs 0.01 s). Stored as
  `videoProbe=<key>:<s/frame>` in mmdx12.ini, key = assets + renderer/resolution/quality/effects. ETA now includes the encode time and uses
  `steady_clock` (the old per-app-frame clock measured 0 s for single-frame images).
- GI effects (`offline_volumetric.hlsl`, `bloom_fft.hlsl` CSFftOutput, `offline_post.hlsl`): volumetric light with ray-query sun shadows
  (half-res march + depth-aware blur, added in CSFinalize), FFT convolution bloom on the quarter-res thresholded image, bloom on/off.
  GI stills take bloom/convolution/volumetric from the settings chosen at scene entry; videos from the dialog.
- Checked headlessly (960x540): raster / RT (+DoF, volumetric) / PT video frames, GI stills with and without effects, probes.

### Pitfalls found
- Another app render held `build/bin/MMDX12.exe`: all verification used `build_dev` (git-ignored) and the running process was never touched.
- The dialog grew beyond the window: effects sit before the format controls and a fade + caret marks hidden rows.

### Not verified / not done
- Measured GI times were taken while another 4K render shared the GPU (inflated); re-measure on an idle GPU.
- Convolution bloom strength in GI (`kConvolutionBloomIntensity`) is a first guess; volumetric shafts are subtle indoors.
- The dialog was only exercised through captures (`--screen video`, ini presets); settings changes mid-measurement (cancel and restart) were not clicked, only reasoned.


### Next
- Re-measure the GI times and tune `kConvolutionBloomIntensity` on an idle GPU; click through the dialog (change settings mid-measurement).
- Optional: scale a tentative estimate from the nearest measured combination (same assets, other resolution) while a new one is measured.

### Session close
- The temporary build directories (`build_dev`, `build_w1..3`) and a stray `%TEMP%mmdx_build.log` were removed; `build/` was rebuilt clean
  (the render that had locked it had ended). Working tree committed.

## 2026-10-04 — Release prep: library-picked benchmarks, README, MIT licence

### Done
- Benchmarks no longer need an "official preset" (MMD assets can't ship): `PickBenchmarkScene` (Benchmark.h) takes the character and
  stage with the most vertices and the song whose dance is closest to 2:30 (camera songs first). Every default run is submittable
  (`benchSubmittable_`, was `benchOfficial_`); GI runs with `--bench-spp` / `--offline-size` still can't be submitted.
- GI render benchmark cast (`PickRenderBenchCast`, RenderBench.h): the three characters with the most vertices (fewer repeat), each
  posed from a library dance chosen by a seed hashed from the asset ids (same library = same scene). `PoseRenderBench` tries up to 8
  frames in the middle 50 % of the dance and keeps the first standing one (head >= 85 % of rest height), and turns the upper body
  (左腕 -> 右腕) toward the camera before the slot yaw.
- Lobby: scene cards show the picked assets with a "라이브러리에서 자동 선택" badge; an info icon next to the leaderboard tag explains
  that scores depend on the library (tooltip).
- README rewritten for the public repo (animated preview -> release mp4, GI gallery, bring-your-own-assets notice, benchmark, third-party
  table); media in `docs/media/`. MIT `LICENSE`. `videos/` (Remotion promo kit) is git-ignored and stays local.
- Delegation: benchmark code by an opencode/GLM worker (one dangling `c_str()` of a temporary fixed by Claude), README by an
  Antigravity/Gemini worker (video timing misattributed to all renderers, fixed; wording polished by Claude).
- Checked: build; `--benchmark dx12-gi-render --bench-spp 64 --offline-size 960 540` (cast IA / Breath You / CWL, standing, facing
  the camera); `--screen bench` capture; `--benchmark dx12-raster-fhd --bench-frames 240`.

### Not verified
- The leaderboard tooltip was not hovered (headless captures can't hover); the icon placement was checked in the capture.
- README: YouTube introduction (https://youtu.be/7S7D670HbIM) linked from the hero preview and a badge; Korean README
  (`README.ko.md`) with an English · 한국어 switch on both. Public repo history was scrubbed of server details and local paths
  (git filter-repo), the GitHub repo recreated, server notes moved to the git-ignored `CLAUDE.local.md`.
- Prebuilt release v0.1.1: `tools/package_release.ps1 -Version x.y.z` builds a Release configuration in `build_release/` (no debug
  info, so no local PDB path in the exe) and zips `dist/MMDX12-<ver>-win64.zip`: exe, shaders, fonts, DXC + upscaler DLLs, app-local
  VC++ runtime, licences, an empty `library/` with a README. Tested from an extracted copy outside the repo (empty library; PT + DLSS
  frame with the repo library). READMEs: download section, 58 s promo (https://youtu.be/vNztzqvVV4M) on the preview.

## 2026-10-04 — UI localisation (Korean / English / Japanese)

### Done
- `core/I18n.h/.cpp`: `Tr("한국어")` takes the Korean source text as its key and returns the English / Japanese entry
  (`I18nEn.cpp`, `I18nJa.cpp`; a missing entry falls back to Korean). `Language::Auto` follows the Windows user locale
  (ko / ja, otherwise English). `AppSettings::language` (ini `language=0..3`), `--lang auto|ko|en|ja` for one run.
- Every UI literal in `src/app/*.cpp` is wrapped in `Tr()` (about 300 sites, about 270 table entries; long help texts are single keys).
  Table-driven names (colour LUTs, benchmark tier titles, video renderer / quality labels) are translated where they are displayed.
  Strings stored as `std::string` at creation (load status, errors) keep the language of that moment.
- Language picker: globe icon button in the app bar (system / 한국어 / English / 日本語 popup), saved at once.
- Japanese font: bundled Noto Sans CJK JP (Regular for the body face, Bold for the others) merged after Pretendard; system
  fonts remain the fallback. Licence added to the release package. README.ja.md added.
- Checked: build; captures with `--lang ja` (lobby), `--lang en` (benchmark lobby), `--lang ko` (lobby with the globe button).

### Not verified
- The language popup was not clicked (headless captures); play screen, video dialog and offline progress screens were not captured
  in en/ja (only the table coverage was checked: every `Tr("...")` literal has an entry).
- Translations were written by the model, not reviewed by native speakers.

## 2026-10-04 — Chinese (Simplified) UI, release 0.3.0

### Done
- `Language::Chinese` (ini `language=4`, `--lang zh`, auto-detected from a `zh*` Windows locale), table `core/I18nZh.cpp` (same 274 keys as English),
  "简体中文" in the globe popup. Bundled Noto Sans CJK SC (Regular/Bold, same OFL licence file); `LoadFonts` merges the SC face before JP when the
  language at startup is Chinese (shared Han characters get the matching glyph forms), JP first otherwise.
- Checked: build; `--lang zh` captures of the lobby and the benchmark lobby.

### Not verified
- Switching to Chinese from the popup in a running session keeps the Japanese Han glyph forms until the next start (font atlas is built once).
- Translations are model-written, not reviewed by a native speaker; play screen / video dialog not captured in zh.

## 2026-10-05 — Volumetric light: shafts (god rays), spot shadow maps, release 0.4.0

### Done
- Diagnosis: the old march only added a veil. Spot cones were unshadowed, 32 steps over 400 units (~12-unit steps, 1-2 samples per beam),
  and the scene was never attenuated by the fog.
- Spot shadow maps: `ShadowPass` renders the characters into a perspective slice per spot (first 8 spots, `kSpotShadowSlices`,
  `targets.spotShadowMap`, 512..1024 px). `SceneConstants::spotViewProj[]` / `spotShadowParams`, `GpuLight::shadowSlice`.
  Rendered when `SpotShadowsWanted` (shadows on and raster/RT, or volumetrics). The scene PS (`SpotShadow`, root param 11 = t7)
  shadows spot diffuse/specular on surfaces too. Characters only: re-drawing the stage per spot cost more than the march.
- `shaders/volumetric_common.hlsli` (shared by real-time and offline): closed-form height-fog transmittance; sun marched only over the
  shadow range with an exact unshadowed tail (T(a) - T(b)); spots marched only inside the analytic ray/cone interval, step count from
  the segment length; isotropic sky ambient; medium bounded by a sphere (r 180) around the stage so distant scenery is not fogged out.
- Real-time pass: march (jittered per frame) -> temporal accumulation (reprojects the march end point, neighbourhood clamp) -> light
  depth-aware blur -> `Blend::Transmittance` apply (lit * T + L). Offline: same integrator, sun and spots shadowed with ray queries;
  CSFinalize composites image * T + L.
- CLI: `--volumetric-density <0.25..4>`; `--frames` runs log the average GPU time of the second half at quit.
- Checked: raster / RT / PT captures and an offline GI still (concert side view: visible beams, character shadow in the beam; terrace
  sunset facing the sun: shafts through the pergola) against a baseline build; no shader errors. GPU (theater, concert, free camera,
  300 frames): old with volumetric 7.41 ms, new 7.45 ms, new without volumetric 6.04 ms.

### Not verified
- Temporal stability in motion (only still captures); ghosting on fast camera moves not checked.
- With the medium now attenuating the scene, high densities (the saved 4.0) look much foggier than before; 1-1.5 reads best.

## 2026-10-05 — glTF / VRM / FBX models, library classification rework, library skeleton

### Done
- Investigation (captured in `captures/import/baseline_*.txt`): the old scanner made one song per folder, so a flat dump with
  two songs lost one (or layered a short dance onto the other); audio was picked by extension and size, and a folder without
  audio took any audio file from its parent.
- `ModelImport` (`LoadModelFile(path, role)`): PMX natively; glTF/GLB/VRM 0.x/1.0 via cgltf 1.15 and FBX/OBJ via ufbx 0.23.1
  (both vendored in `external/`) into a format-neutral `ImpScene`, then converted to `PmxModel`.
  - Characters: humanoid slots from the VRM table or a name dictionary (Mixamo, VRoid, RPM, Unity/Blender/UE/Biped; shape
    checks reject quadrupeds), renamed to MMD bones, センター/グルーブ + 足ＩＫ/つま先ＩＫ synthesised, upper arms re-posed to 38 deg
    (measured 36..42 in the library's PMX rigs), facing detected from the arms, scale 12.5 units/m. Morph targets become
    vertex morphs; VRM presets / ARKit / VRoid names get MMD aliases (あいうえお, まばたき, 笑い, ウィンク). No physics.
  - Stages: baked into one root bone; when no floor lies under the origin the scene is moved onto its main floor.
  - Embedded textures: `PmxModel::embeddedTextures` + `LoadImageRGBA8FromMemory` (WIC from memory as fallback).
- `AssetLibrary` rewrite. Precedence: app overrides (`library_overrides.json` next to the exe, right-click menu on cards:
  use as stage/character, hide, back to automatic, show in Explorer) > `mmdx.json` sidecars (type + files + name, comments
  allowed) > folder names (several languages) > content. Songs: dances clustered by length, split into several songs only
  when the folder also has several audio files or cameras; cameras/audio matched by length and file-name tokens; parent
  audio only on a match. Audio lengths via miniaudio (implementation moved to `core/AudioProbe.cpp`).
  Non-PMX models next to a PMX are skipped (usually exports of the same model). Scan notes in `LibraryScanResult::notes`.
- `assets/library_template` (characters/ stages/ songs/ + READMEs in 4 languages): copied into an empty library on first run
  and into the release zip by `package_release.ps1` (also bundles cgltf/ufbx licences). Format badges on cards, new UI
  strings translated; benchmarks prefer PMX models. READMEs (4 languages) updated.
- Checked: real library scan identical to the baseline except the new `furry_police/model.glb` character; messy test library
  (`captures/testlib`, public three.js/Khronos/VRM samples in `captures/assets_dl`) classified as intended, sidecars and
  overrides applied; anim_probe IK convergence on Michelle/Seed-san/Samba; captures of glTF/VRM/FBX characters dancing,
  spaceship hallway and collision-world stages (raster + RT), PMX regression scene, select screen, empty-library skeleton.

### Not verified
- The right-click menu was not clicked (overrides tested by writing the JSON); `package_release.ps1` not run.
- dungeon_warkarma (enclosed, near-black materials) shows nothing useful from the free camera; no manual stage offset/scale yet.
- Textured FBX characters and Draco/meshopt glTF (rejected with a warning) untested; imported characters have no physics.

## 2026-10-05 (2) — character size slider, release 0.5.0

### Done
- `ModelInstance::SetScale`: uniform display scale about the origin applied to the skinning matrices and `BoneWorldPosition`
  (DoF focus, spotlights follow it); physics keeps simulating at the original size, so hair and skirts behave the same.
  Saved per character id (`characterScale=<scale>|<id>` in the ini, `AppSettings::CharacterScale`), applied in `UpdateScene`
  (so live play, offline stills and videos use it; benchmarks always run at 1x). Play bar: new size icon (highlighted when
  the scale is not 1) opens a 0.5..2.0 slider with a reset button, snaps to 1.0 near the original size.
- Imported glTF/FBX characters taller than 1.8 m or shorter than 1.4 m (or unit-less files) are brought to that range on load.
- Checked: Michelle at 0.6x and 1.8x in free-camera captures (feet stay on the floor, shadow scales, play bar icon active).
  Built in `build_dev` because the user had `build\bin\MMDX12.exe` running.

### Not verified
- The size popup was not clicked (headless captures); thumbnails on the select screen show the unscaled model.

## 2026-10-06 — Studio (M1–M6), release 1.0.0

### Done
- **Studio** (`Screen::Studio`, `src/studio` + `app/UiStudio*.cpp`): an MMD-style keyframe editor next to the untouched
  player. Built in six milestones on `feature/studio`:
  - M1: multi-model editor screen (outliner, viewport, inspector, timeline grouped by display frames, Bezier curve
    editor), key add/move/delete/copy/paste, undo/redo (`CommandStack`), VMD import/export (`SaveVmd`), `--ui-script`
    test driver. Found and fixed the VMD bone interpolation byte layout (Z/rotation x1 at bytes 17/18; 2/3 are physics flags).
  - M2: timeline zoom/pan/range, row operations, frame insert/delete like MMD, curve-only paste, playback with audio,
    loop range, physics toggle, full keyboard map, 200k-key projects stay responsive, 256 MB undo budget.
  - M3: viewport pose editing (bone overlay, picking, rotate/translate gizmos local/global, IK), MMD-style pose layer
    with register (I / Ctrl+I), morph sliders, mirror pose, VPD import/export.
  - M4: camera / light / self-shadow tracks (key fields, key from view, camera path overlay, light ball), the renderer
    uses the light and shadow tracks in the studio; curve editor overlays all channels.
  - M5: `.mmdxproj` projects (JSON + standard VMDs that MMD opens), empty-project start, open/recent, autosave +
    crash recovery, adding/removing characters, stages and props (bone-attached accessories), audio with offset.
  - M6: rendering the project from the studio: the top bar's render menu opens the lobby's video dialog (raster / RT /
    PT / GI, range section: timeline range or whole project) or renders a GI still of the current frame. The job goes
    through the existing offline pipeline (`OfflineJob::studio`: `StudioPoseForRender`, `BuildStudioFrameView`, motion
    blur pose ring over all studio models), motion camera, light/shadow tracks, props, project audio muxed with its
    offset (`VideoEncoder` writes leading silence for a negative start), then returns to the studio with time, camera
    mode and physics restored. CLI: `--project p.mmdxproj --offline-video out.mp4 [--offline-range a b]` /
    `--offline-still out.png [--seek s]`. Shortcut overlay (? / F1, top bar keyboard button). `--ui-scale` for
    high-DPI captures; the timeline shrinks to 30 % of short windows. Camera key inspector: light/shadow sections
    collapse to their switches and the fields are slimmer (curves come into view). Library songs and audio files
    are decoded on the worker (`AudioPlayer::Preload`), no hitch on the main thread. glTF/FBX props are no longer
    moved onto a floor (`ModelRole::Prop`). Version 1.0.0 (`project(... VERSION)`, logged at start).
  - READMEs (4 languages): Studio section, shortcut table.
- Workers: opencode / antigravity wrote data layers, loaders, tests, translations and the README drafts; Claude did the
  contracts, App wiring, UI, review and fixes (details per milestone in the session notes).
- Checked (M6): studio videos via the CLI and through the dialog (raster/RT/PT/GI) match play-mode renders of the same
  frames (plus the props); audio cross-correlates to the song at start − offset; returning from a render keeps
  STUDIOSTATE identical; captures at 1080p and `--ui-scale 1.5`; all studio ui-scripts (t1–t6, m2_*–m5_*, new m6_a)
  against the M5 build; studio unit tests (134 checks), vmd_roundtrip on library/motion (35/35), anim_probe on a dance;
  player select / play / video dialog / bench captures unchanged.

### Not verified
- Real file dialogs (open/save/add/audio) were not clicked by a test; the audio-file worker path only by code review.
- Studio videos at full 4K/60 GI length (only short ranges rendered); no sample-render time estimate in the studio
  dialog (rough estimate only).

## 2026-10-06 (2) — in-app auto-update

### Done
- **Updater** (`src/app/Updater.{h,cpp}`, module `mmdx::updater`): the portable app checks
  `https://mmdx.codingbot.kr/latest.json` in the background at start (WinHTTP like
  LeaderboardClient, silent failures, skipped in headless/scripted runs) and offers the update
  on the select screen. Semantic version compare; `notes` resolved per UI language (ko/ja/zh/en
  fallback). "Update": download (progress + cancel), verify size + SHA-256 (self-contained
  FIPS 180-4 impl, streamed), extract with the Windows built-in `tar.exe` (CREATE_NO_WINDOW,
  output piped), then a journal-based swap across a restart — the running exe cannot overwrite
  itself, so it writes `update_tmp/state.json` (phase "pending") and spawns
  `MMDX12.exe --apply-update --apply-wait <pid>`; the applier waits for the old process, renames
  collided program files aside into `update_tmp/old/`, moves the new tree in, and relaunches.
  Every failure rolls the renames back and relaunches the old exe; an interrupted swap is
  applied by the next start (the journal is authoritative); leftovers are cleaned up on the
  next start (`update: <ver> installed` breadcrumb logged, log archived to mmdx12.previous.log).
  Only program files are replaced: the user's `library/`, ini, `recovery/` and logs survive.
  An install folder that is not writable switches the notice to "open the release page".
- UI (`UiUpdate.cpp`): an unobtrusive footer notice on the select screen (new version + its
  note, Update / Later; "Later" hides it until the next start), a progress dialog
  (download/verify/extract + cancel), the failure and not-writable variants with the release
  page. UiKit style; strings in all four UI languages (ko source + en/ja/zh tables).
- Testing hooks: `--update-feed <url-or-file>` (https, file: URL or local path; the zip url
  may also be local), ui-script commands `updatecheck` (synchronous check, logs UPDATECHECK)
  and `updateinstall` (stages, then the app exits for the restart). `updateFeedUrl` ini key.
- Checked: `build.cmd` builds clean. Headless end-to-end in %TEMP%\opencode\upd_e2e: a copy of
  the app (dist 1.0.0 tree + the new exe) with a dummy library file, ini nickname and recovery
  file updated itself from a local feed + a locally zipped fake 9.9.9 version via
  `--ui-script` `updateinstall`: zip verified (sha256 ok), extracted, swapped across the
  restart (VERSION.txt 1.0.0 -> 9.9.9, NEW_IN_999.txt landed, msvcp140/dxcompiler DLLs
  replaced), library/ini/recovery intact, update_tmp cleaned, and the restarted exe logged
  `update: 9.9.9 installed (this run started after the update)`. Captures of the notice
  (available / verifying / failed), a corrupted-sha run (update aborts, nothing installed, no
  leftovers), and a click-through (Update button works from a ui-script). Studio unit tests
  (15 + 66) still pass.

### Not verified
- The real https feed (the server does not serve latest.json yet); the not-writable path was
  exercised only through the code path (Phase::NotWritable logic), not a read-only folder.
- The update button + notice were clicked headlessly; a human should also see them once.

## 2026-10-06 (3) — Studio as a 3D tool: auto-key, camera possession, docking, quad view

Started from an audit of what Studio lacks next to MMD / Blender / C4D / Unity (opencode, read-only, spot-checked), then
worked through the P0/P1 list and the follow-up requests.

### Done
- **Auto-key** (`StudioDoc::autoKey`, transport-bar button): a finished pose / morph / camera edit keys itself at the playhead
  (`StudioSetPose` -> `StudioRegisterPose(allBones, layerBefore, name)`, one undo step restoring the pre-edit layer). The camera
  inspector ("카메라 값") always shows the camera at the playhead and edits it live (`DrawStudioCameraKeyFields(..., live)`).
  Off: the old MMD behaviour (I registers, leaving the frame discards).
- **Camera**: frustum scaled with the scene (16:9, translucent face), 16:9 render-frame mask (the 3D image itself is fitted to
  16:9 while looking through the motion camera), thirds / safe-frame guides. C4D-style **possession** (outliner / camera panel
  button, Esc): viewport navigation edits the motion camera key (`StudioWriteCamera`, one undo step per gesture). Free view:
  eye / target handles (translate gizmo) swing / turn the camera.
- **Navigation**: RMB look + WASDQE fly (wheel = speed), Alt+LMB orbit, MMB pan, F focus, numpad 1/3/7 views.
- **Placement**: characters get a world transform (`StudioModel::place`, saved in `.mmdxproj`, undoable, inspector fields + T
  toolbar gizmo). Stages are not movable (static BLAS).
- **IK on/off** list per IK bone in the bone tab (VMD IK keys, `IkEditCommand`); bone / morph search filters the timeline rows.
- **Shading**: Lit / Unlit / Wireframe (raster path; `ViewShading`, `--shading` CLI flag) via the viewport's top-right control.
- **Docking**: imgui replaced by the docking branch (same version number); `App::DrawStudio` is now a top-bar window + dock-space
  host + dockable panel windows, default layout via DockBuilder, `mmdx12_layout.ini` persistence (not in scripted / `--frames`
  runs), reset button in the top bar.
- **Quad view**: perspective + orthographic top / front / left in one render target (`FrameView::extraViews`, per-view
  SceneConstants, `ScenePass` draws the scene once per view; flat shading, no TAA / upscaler). Overlays / gizmos work in the
  ortho views (`ViewProj::ortho`); pan / zoom shared via `StudioDoc::quadCenter/quadHeight`.
- i18n entries (en / ja / zh) for every new string; CLAUDE.md Studio section updated.

### Checked
- `build_dev` builds clean; `studio_edit_test` 15, `studio_gizmo_test` 15, `studio_pose_test` 38, `studio_project_test` 67
  (placement round trip added) all pass; no `[E]` lines in the log after the captures.
- `render_smoke` PNG hash is identical before and after the shading / multi-view changes (lit single view unchanged).
- Headless scripted captures: auto-key keys + undo name, possession gesture, eye-handle drag, F focus, numpad view, model
  gizmo move, IK switches, wire / unlit toggle, quad view with pan / zoom, layout reset.

### Not verified / notes
- Layout persistence across runs and tab dragging (interactive only); fly keys / Alt gestures (the script cannot hold keys);
  thirds / safe-frame guides were not looked at; bone search was only compiled; IK toggle undo not exercised.
- Panels cannot become OS windows (no ImGui viewports with one swap chain). Tab bars shift old `--ui-script` coordinates.
- Delegation: opencode workers finished the shading task (reviewed, verified); two others (camera visual, model transform)
  produced no changes in over an hour and were stopped; both were done directly. One worker used `git stash` on the whole tree
  (restored intact) — keep workers away from git.
- Not done: camera editing by dragging in the viewport while looking through it without possession, VMD distance sign
  convention in the inspector, quad / layout state in the project file, physics bake, snapping, markers, onion skin.

## 2026-10-06 (4) — fix: video render stops when minimized, release 1.1.0

Triggered by GitHub issue #1 (video rendering stops while the window is minimized).

### Done
- `App::MainLoop` skipped `RenderFrame()` while minimized, and offline renders advance one step per frame, so they froze.
  Now the loop keeps rendering while `offline_.mode != OfflineMode::None`; normal play / UI still idles when minimized.
  The swap chain is not resized on SIZE_MINIMIZED, so there is no 0x0 resize.
- Version 1.1.0 (CMake). Release page cover image: `docs/media/studio-1.1.0.jpg` (Studio quad view). Release notes cover everything
  since 1.0.0: automatic updates (1.0.1), the Studio 3D-tool work of entry (3), and this fix.

### Not verified
- The minimized case was only built, not exercised (headless runs cannot minimize); the issue reporter should confirm.

## 2026-10-06 (5) — fix: release build ran unoptimized (2-10 fps in play mode), release 1.1.1

### Done
- The 1.1.0 release exe played at 2-10 fps (Bullet physics; `--no-physics` was normal). Cause: `build_release/CMakeCache.txt` had empty
  `CMAKE_CXX_FLAGS` / `CMAKE_CXX_FLAGS_RELEASE`, so the packaged exe was built without `/O2`, `NDEBUG` and `/EHsc`
  (found by running the packaged zip: GPU 43 ms vs 4.7 ms for the dev build). Why the cache was empty is unknown.
- `CMakeLists.txt` states `/EHsc`, `/O2` and `NDEBUG` explicitly (Release / RelWithDebInfo / MinSizeRel);
  `tools/package_release.ps1` deletes the cache before configuring. Checked with a Release build configured with empty flag
  variables: Bullet gets `/EHsc /O2`, 300 play frames in 8.6 s, GPU 4.1 ms.
- Version 1.1.1, packaged from a clean worktree of the commit (the studio WIP in the main tree stayed out of the zip).

### Notes
- 1.0.x / 1.1.0 zips were built from that cache; users get the fix through the automatic update (feed: website `latest.json`).

## 2026-10-06 (6) — Studio viewport: forced raster path, per-view quad shading, independent ortho navigation

Delegated to the opencode worker (smoke review).

### Done
- Studio viewport always renders with the raster path (`DrawStudio` forces it each frame, `LeaveStudio` -> `ApplyRenderSettings` restores
  the user's path). Offline / video renders keep their own renderer. PT/RT no longer break Unlit / Wireframe / quad view.
- Quad view shading is per view: the camera quadrant keeps the selected shading (lit Solid / Unlit / Wireframe); the top / front / left
  ortho views are always drawn flat (their own SceneConstants, `ExtraView`). AO / SSR / haze apply only inside the camera quadrant
  (`gP1` = camera rect in uv, `composite.hlsl`). TAA / upscaler stay off in quad.
- Each ortho view has its own centre / zoom (`StudioDoc::quadCenter[3]`, `quadHeight[3]`); the view under the mouse drives it.

### Not verified
- Quad view was only built and read, not captured (no script command toggles `viewLayout`). Single view with `--render rt` was captured: raster, no `[E]`.
- Behavior change: wireframe no longer shows wire floor / models in the ortho quadrants.
- The tree also holds earlier uncommitted studio WIP (Gizmo / StudioProject / UiStudioPose / project test), committed together here.

## 2026-10-07 (7) — version display, thinner distance-scaled studio bone overlay, faster startup

Started with the opencode worker (full level); the worker was stopped midway and the rest was done directly.

### Done
- Version: window title `MMDX12 v<ver>`, a caption next to the wordmark in the app bar, the loading screen footer, and an About popup
  (info button in the app bar: name, version, licence line). The library path pill got narrower to make room.
- Studio bone overlay: links 1 px (was 1.5), joint markers are `jointRadius` px at 40 world units from the camera and scale with
  distance (clamp 2..14 px, DPI scaled); ortho quad views keep a constant size; picking radius follows the drawn marker (floor `pickRadius`).
  (The worker's version sized markers relative to the model's mean depth, which never shrank with distance; replaced.)
- Startup: compiled shader cache in `<exe>/shader_cache` (`ShaderCompiler.cpp`, key = hash of every .hlsl/.hlsli in the shader dir +
  entry/target/defines + dxcompiler.dll timestamp). Launch to first frame 5.9 s -> 2.0 s warm (6.7 s cold). Parallel compiling was tried and
  was slower (8.0 s), so it was dropped.
- No more white / "not responding" window while loading: `Renderer::Initialize` runs on a worker thread while `App::Run` pumps messages and
  paints a GDI splash (`loading_`); resizes during loading are applied afterwards. Cold start stays responsive (checked via `Process.Responding`).
- `STARTUP <phase>` timing lines in the log (App.cpp, Renderer upscaler timings).

### Notes / not verified
- The splash itself was not captured (window not capturable behind other windows); cold-start responsiveness was checked by process state only.
- `tools/package_release.ps1` copies only exe, dlls, shaders, assets: `shader_cache` is not packaged. The updater applier was not read for
  handling of extra folders.
- Far-away joints show as 2 px dark dots (min clamp), readable but dense on hair bones.

## 2026-10-07 — Shader pack ecosystem + hoyo_toon

Feedback round 1: the picker was only in the play bar (not visible to people rendering from the library) -> library panel.
Round 2: "an ecosystem, not MMD-vs-hoyo": list view, contributor fields, docs/gallery on the website, online installs.
Workers: antigravity (first attempt, 30 min timeout, no output), opencode (M1 pack core), antigravity (M4 website).

### Done
- Render: `PSPack` in mmd.hlsl behind `MMDX_PACK` (PSMain untouched), contract `shaders/pack_api.hlsli` (PACK_API_VERSION 1),
  compiled with DXC on first use (FXC cannot `#include` a macro) + RT variant; errors -> `[E]`, status, toast, default shading.
  Per-model data in `MaterialConstants` (class, head bone, head bind pos, 16 params); bone SRV visible to the PS; "no AO" =
  negative reflectivity in the normal target. Shader cache key covers the pack folder.
- Pack format v2 (`render/ShaderPack.*`, opencode): localized name / description / recommendedFor / param labels, authors,
  license, homepage, repository, tags, apiVersion, minAppVersion, preview.png; status per pack (compile error, incompatible,
  invalid manifest, duplicate) kept across rescans; hot reload (`PollChanges`, stat every 0.5 s); install folder / zip
  (allowed extensions, 200 files / 32 MB, no links, zip-slip check, tmp + rename), uninstall, create from
  `shaders/pack_template`. `tools/pack_check <dir|zip> [--compile]` (shipped in the release zip).
- `core/NetUtil.*`: HTTP GET, download, SHA-256, zip extraction, version compare moved out of the updater (shared).
- Online gallery `app/ShaderPackStore.*`: index.json (default mmdx.codingbot.kr/shader-packs/index.json, `--pack-index` for
  tests), previews cached in shader_cache/previews, downloads checked against size + SHA-256, installed on the main thread.
- UI: top tab "셰이더" (`UiShaders.cpp`, `--screen shaders|shaders-online`): installed / online card grids with search,
  source + status badges, detail panel (description, recommended models, authors + links, license, links, tags, params,
  compiler errors, open folder, delete), zip install, drop zip / folder on the window, new pack dialog. Library panel / play
  bar / studio inspector share one selector row with a searchable list (`UiShaderPack.cpp`); video dialog warns for PT / GI.
  Choice saved per character (`characterShader=` ini) and per studio model (`.mmdxproj` "shader"); `--shader-pack`.
- hoyo_toon v1.0.0: head-frame face shadow (SDF stand-in), no face AO, two-tone ramp, toon-derived shadow colour, flat
  ambient, banded hair highlight / rim; preview from a real render.
- Website (MMDX12_Web, not deployed yet): docs in ko/en/ja/zh (overview, quickstart, manifest, shader API, hoyo_toon,
  debugging, publishing), gallery + pack pages, `scripts/build-packs.mjs` (deterministic zips + index.json from `shader-packs/`).
  The repo's `docs/shader-packs.md` was removed; README links the site.

### Verified
- Same-frame comparison (raster video renderer, 10.75 s): Furina's face loses the self-shadow band and nose / cheek modelling.
- `--render rt` (Raiden), non-HoYo model (Miku), broken surface -> fallback + toast.
- Scripted UI (build_dev): shader screen installed / online tabs, online install of a test pack from a local index
  (`.mmdx_install.json` written), SHA-256 mismatch rejected (nothing installed, no temp left), library selector + popup.
- Hot reload: editing surface.hlsl while playing -> generation 2 + recompile (log).
- pack_check: hoyo_toon / template OK with --compile; worker tested broken json / missing surface / syntax error / apiVersion 99 /
  zip / zip-slip. Site build twice -> identical zip SHA-256; pages looked at 1440 and 390 wide.
- studio_project/edit/pose/gizmo tests pass.

### Not verified / notes
- Released as 1.2.0 (GitHub release v1.2.0, zip SHA-256 matches the asset digest; the packaged exe compiled hoyo_toon in a
  play run). The UI strings of this feature (and the About dialog) were added to the en / ja / zh tables before packaging.
- Website deployed: /latest.json -> 1.2.0, /shader-packs/index.json + zip (SHA-256 checked live), docs and gallery in all
  four languages; the app's online tab reads the live index. A pack zip changed with the same version must get a new version
  (zips are served as immutable in the example nginx config).
- New pack dialog, delete, drag & drop on a real window, play-bar popup and the studio row were built but not clicked through.
- PT / offline GI ignore packs (by design for now). Face width is in model units (slider for other head sizes).

## 2026-10-07 (8) — Shader pack API 2: pack textures, per-character texture folders, PackEdge, hoyo_toon_v2 (issue #2) — 1.3.0

GitHub issue #2 (a Chinese user) asked for pack textures + a sampling API so game-style shading (light maps, ramps, face SDF)
can be a pack, outlines from the pack, a weapon class, and textures supplied by the user (game rips can't be redistributed).
Workers: opencode (API 2 core: the first run stopped on an interactive question — resumed in the same session with
`delegate.py --session`; then the per-character folders). Claude: review fixes, hoyo_toon_v2 shading, docs, release.

### Done
- API 2 (`kPackApiVersion` 2, v1 packs load): pack.json `"textures"` (<= 16, wrap/clamp, srgb), `PackSampleTex` /
  `PackSampleTexLevel` / `PackTexSize` / `PackTexCount` (16-SRV table t0 space5, root param 12, compile defines for count /
  clamp / sRGB masks); `PACK_HAS_EDGE` + `PackEdge` (pack variant of the edge pass: colour + width scale); `PACK_WEAPON`.
- Texture lookup: character folder -> pack-level folder (manager screen) -> pack folder -> white; inside a folder: relative
  path, file name, then any file ending in `_<declared name>` (so game files keep their names). Texture sets per (pack,
  folder) in ScenePass. Per-character folder in `ShaderChoice` (ini `characterShaderTextures=`, `.mmdxproj`, UI row under
  the pack sliders). Class rules can match the diffuse texture path (`"texture"`).
- pack_check: textures (format / count / size / total / paths; missing = warning), literal `PackSampleTex(N)` indices.
- hoyo_toon_v2 (built-in, gallery): Genshin light map (G shadow bias, A ramp row, R/B specular, metal matcap), day/night
  shadow ramps, face SDF from the head frame; lit colour from the MMD colour model; v1 fallback per missing map.
- Website: manifest / shader API docs (4 languages) for textures, user / per-character folders, suffix matching, texture
  class rules, PackEdge; hoyo_toon_v2 gallery entry.

### Fixed in review (worker output)
- sRGB textures were decoded twice (`_SRGB` SRV + `SrgbToLinear` in the shader).
- `UploadBatch::CreateTexture` had been changed to create every texture TYPELESS (only pack textures need it).
- `PACK_HAS_EDGE` was detected in comments (the template mentions it: every template pack would have built a broken edge PSO).
- The worker's own find: the white fallback texture was a local ComPtr freed while SRVs used it (GPU hang 0x887A0006).

### Verified
- Real Genshin maps (Hu Tao, Raiden; kept locally, never committed): the official MMD models use the game UV layout
  (diffuse sheets identical). Hu Tao with hoyo_toon_v2 + a character folder: all maps found by suffix, no [W]/[E];
  before/after (docs/media/issue2_before_after.png): no geometric nose / cheek shading on the face, painted fold shadows.
- Debug views (light term, light map G) checked in the runtime shader copy. studio_project_test 69/69, pack_check OK for
  hoyo_toon / hoyo_toon_v2 / template. Missing-texture + edge case (the old hang) renders.

### Not verified / notes
- Ramp row order (alpha 1.0 / 0.7 / 0.5 / 0.3 / 0 -> rows 0..4) and the face SDF side are from community notes + one look;
  `faceFlip` exists for mirrored models. Furina's third sheet (Dress) maps to body. Star Rail / ZZZ map layouts differ.
- Texture sets of old folders stay allocated until the next registry rescan.
- Released 1.3.0 (GitHub release v1.3.0, asset SHA-256 = local zip; the packaged exe reported 1.3.0 and compiled
  hoyo_toon_v2 with no [E]). Website deployed: /latest.json -> 1.3.0, gallery index has hoyo_toon_v2 2.0.0 (live zip
  SHA-256 checked), docs in four languages. Issue #2 answered with the before/after image (docs/media).

## 2026-10-07 (9) — studio lighting source + concert spot rig, shader pack effects (API 3)

Request: (1) the studio must let the user choose VMD light track / preset (which one) / custom, (2) control over the preset's
key light and a keyframeable concert spot rig (count, position, colour, follow modes), (3) shader packs beyond character
shading: screen effects (chromatic aberration etc.). Workers: opencode GLM, one per scope (A lighting, B effects); both stalled
reading files for 15 min (explore guard / a socket error), were resumed with decisions, then B was taken over by Claude and A
finished by Claude.

### Done
- Lighting (`studio/LightRig.*`, `StudioDoc::lighting`, `.mmdxproj` "lighting", `BuildStudioLighting` in app/Lighting.cpp):
  source VmdTrack / Preset / Custom, own preset choice per project, key-light override (dir, colour, intensity, rim), spot rig
  (add / delete undoable, modes auto swing / centre / head / manual, front fill), `RowKind::Spot` timeline rows with keys,
  `studiolight` / `studiokeyspot` ui-script commands. Play mode / lobby / benchmark lighting unchanged.
- Effect packs (docs/shader_effect_api.md): pack.json `"type": "effect"`, `"stage"`, `effect.hlsl` `PackEffect`, `PackEffectPass`
  (two instances, ping-pong targets only while an entry is enabled), `RenderSettings::packEffects`, ini `effect=`, `--effect`,
  stack editor (shader manager + play bar button), template `pack_template_effect`, built-ins chromatic_aberration /
  film_grain / crt_scanlines, `pack_check` handles effect packs. `kPackApiVersion` 3.

### Fixed in review (worker output)
- Effect pass: `PackEffectPass` was missing its closing namespace, `EffectPipeline` was in an anonymous namespace while forward
  declared in mmdx (incomplete type in unique_ptr), `ResolvePackTextureFile` was file-local, copy-back skipped for an even number
  of effects, time / frame constants were never filled, the input SRV table was space 0 while the shader declared space 6
  (PSO E_INVALIDARG), `Icons::CaretUp` was the CaretDown glyph, stack editor returned void / used checkbox + wrong layout.
- Lighting UI: `##ko_dir` / `##spotpos` ids shown as labels, colour / intensity / cone controls overlapping.
- A killed worker kept running inside the opencode server and overwrote fixes (duplicate class, `FullscreenPipeline` changes,
  a broken `pack_api.hlsli`, effect_api without space6): reverted by hand, then `opencode session delete`.

### Verified
- Build clean; studio_project_test 89, studio_edit_test 15, studio_pose_test 38, studio_gizmo_test 15 pass; pack_check --compile OK
  for the three effects and hoyo_toon_v2.
- Play captures with all three effects, an offline raster video (grain + CRT), `--effect none` leaves no PackEffect log lines;
  shader manager (badges, stack editor with params) and studio captures (source chips, key override, 4 spots, keyed spot rows).

### Not verified / notes
- Effects are not applied to PT / offline GI, studio projects don't store the stack (app setting), online gallery install of
  effect packs and the play-bar popup / new-pack dialog were not clicked through. Spot viewport handles not built.
- The saved ini can keep a stale `effect=` line after scripted UI runs; delete it if the viewport shows an unexpected effect.
- Skill `opencode-delegate` updated: unique run folders, per-worker build dirs, `opencode session delete` to really stop a worker.

## 2026-10-07 (10) — UI cleanup: play bar width, effect stack jitter, shader panel / lobby tabs

### Done
- Play bar: `rightW` ignored the size, shader and effects buttons, so the right cluster ran out of the bar; it now counts them.
- `DrawEffectStackEditor`: the entry loop stopped on the frame a slider changed, so later entries vanished mid-drag and the layout
  jittered (play bar popup and shader manager alike). Move / remove are now applied after the loop.
- The effect stack moved out of the shader manager (it read as the selected pack's own settings) into a collapsed "화면 효과" row
  under the lobby's shader section (`effectStackOpen_`, editor gets `header=false`); the manager shows a pointer note instead.
- Shader manager detail panel: 400 -> 460 px, tabs 정보 / 설정 / 관리 (`shaderDetailTab_`). Delegated to antigravity (smoke review).
- Lobby select panel: 380 -> 440 px, tabs 화면 / 셰이더 / 세부 (`lobbyTab_`, replaces `advancedOpen_`; `--screen settings` opens 세부),
  smaller preview.
- Removed the extra build directories; only `build` remains.

### Verified
- Build clean; captures of the shader manager, lobby 화면 and 세부 tabs, no `[E]` lines.

### Not verified / notes
- Play bar after the width fix, slider drag on the stack, the lobby 셰이더 tab and the open "화면 효과" row were not captured.
- Lobby 화면 tab at 1600x900: the lighting section sits under the play button until scrolled.

## 2026-10-07 (11) — i18n gap fix

### Done
- Audited every Korean literal in `src/` against `Tr()` and the en / ja / zh tables (delegated to antigravity, `full` level, no review).
  All user-visible strings were already wrapped; 10 `Tr()` keys were missing from the tables and were added to all three
  (`개`, `설정`, `세부`, `화면`, `스팟 편집`, `선택한 스팟 삭제`, `팩 설정 사용`, the pack-texture-missing note, the no-adjustable-items
  note, the effect-stack pointer note). Tables are 816 entries each and in sync.
- Left alone on purpose: AssetLibrary classifier hints, `SpotLight::name` undo identifiers, native picker title.

### Verified
- Worker reported a clean `build.cmd build`; diff touches only I18nEn / I18nJa / I18nZh (+10 lines each).

### Not verified / notes
- Worker output was not reviewed (translation wording for ja / zh, key byte-exactness) and no UI capture in another language was made.

## 2026-10-07 (12) — effect packs online-only, previews, docs, release 1.4.0

### Done
- The three screen effects (chromatic_aberration, film_grain, crt_scanlines) are no longer built in: removed from `shaders/packs`,
  moved to the website repo `shader-packs/` as gallery packs (1.0.0, `minAppVersion` 1.4.0, apiVersion 3), with previews captured from
  real play renders (Miku, `--effect <id>`). `docs/shader_effect_api.md` and CLAUDE.md say "online gallery only".
- Website docs: new page "Screen Effect Packs" (`effects`, order 5) in ko / en / ja / zh (manifest additions, stages, PackEffect
  inputs / helpers, example, stack UI, CLI, debugging); later pages renumbered; the overview links it.
- Version 1.4.0 (CMake). Release 1.4.0 = studio lighting source + spot rig, effect packs API 3, UI cleanup (play bar, lobby tabs), i18n.

### Verified
- `pack_check --compile` OK on the three gallery zips; an effect pack extracted into `shader_packs/` renders through `--effect` with
  no `[E]`; the installed list shows only the two built-in surface packs.

### Not verified / notes
- A stale `build/bin/shaders/packs/<effect>` copy is not removed by `copy_shaders` (deleted by hand): a dev build dir keeps old built-ins.
- The in-app online install of an effect pack was not clicked through (zip extraction path only).

## 2026-10-08 — shader packs in path tracing / offline GI, Nimble Toon, per-pack settings memory, release 1.5.0

Request: find out why custom shader packs do not work in PT / offline GI; then (the real goal) leave the door open for PT / GI specific
shaders and modding; build a real pack ("nimble_toon") for the official Eternal Return MMD models for raster, RT, PT and GI; refresh the
stale "PT / GI use the default shading" label (4 languages); remember a pack's texture folder when switching packs; release and deploy,
with nimble_toon online-gallery only. Workers: Antigravity (agy, Gemini) for the investigation and three implementation phases, each
reviewed by Claude (smoke); Claude wrote nimble_toon, the contract v2, the memory feature, labels, docs, release.

### Done
- Why packs did not run in PT / GI: raster packs are per-model graphics PSOs (`PSPack`); `CSPathTrace` / `CSRender` are single compute
  shaders with inline ray queries, `RtGeometry` had no pack data, no `space5` table, `Sample` needs quad derivatives, `PackShade`
  returns a lit colour (not a BSDF). `ScenePass::Execute` returns early in PT.
- New contract (docs/shader_pt_api.md): optional `pt_surface.hlsl` next to `surface.hlsl`, `PtPackOut PackEvaluate(PtPackIn)`; the
  integrator keeps the light transport. Phase 1 offline GI, phase 2 real-time PT, phase 3 pack textures (`render/PackTextures.*`,
  `PtPackSampleTex*`). One compiled variant per pack via the `MMDX_PT_PACK` include define (`render/PtPackVariants.*`, no callable
  shaders in cs_6_5). `RtGeometry::_pad` = `packSrv` (size kept), records read through `gBindlessBuf`; camera / specular chain uses the
  pack's direct term, diffuse bounces use `albedo` only. Contract v2 (this session): `PtPackIn::headPos/headScale`, `PtPackOut::terminator`
  (N.L ramp edges); `pack_check` warns on unassigned `PtPackOut` fields.
- Shader cache key now covers the pack folder for `MMDX_PT_PACK` too (editing `pt_surface.hlsl` used to hit a stale cache).
- `nimble_toon` 1.0.0 (website repo `shader-packs/`, not built in): `surface.hlsl` (PackShade + PackEdge), `pt_surface.hlsl`,
  `nimble_core.hlsli` shared by both. Soft low-contrast cel ramp, coloured shadows (cool cloth / warm skin), lifted blue shadow floor for
  dark materials, skull-sphere face normals, soft hair gloss, three-band metal sheen, wide environment rim, tinted outlines. The ER
  textures are flat painted colour (no light maps), so all form is procedural.
- `ShaderChoice::SwitchPack` + `remembered` (per character, per studio model): switching packs or the default shading keeps each pack's
  params and texture folder. ini `characterShaderMemo=`, project JSON `shader.remembered`. New `tools/shader_choice_test` (13 checks).
- Labels: the shader picker says which render paths the pack covers (`hasPtSurface`), the manager detail lists the supported paths; en /
  ja / zh tables updated; website docs overview fixed and a new "PT / GI Packs" page in ko / en / ja / zh (`src/data/docs.ts` slug list).
- Release 1.5.0: GitHub release v1.5.0 (cover image `docs/media/nimble-1.5.0.jpg`, notes in 4 languages, asset SHA-256 = local zip),
  website deployed (latest.json -> 1.5.0, gallery index has nimble_toon 1.0.0 `minAppVersion` 1.5.0, zip SHA-256 checked live, docs and
  pack pages 200 in all four languages), the packaged exe's online gallery lists the six packs.

### Verified
- Packless DXIL of `CSPathTrace` / `CSRender` is byte-identical to the previous commit after every phase (dxc, same flags).
- Offline GI stills with and without a pack are byte-identical to the previous build (deterministic); a red-albedo test pack turns the
  character red in offline GI and in PT; head frame axes checked with a diagnostic pack; checker texture visible in both paths, a missing
  texture samples white; a hard `step()` highlight band stays crisp through the PT denoiser in a still.
- `pack_check --compile` on the released nimble zip (raster, offline GI, PT variants) with the packaged exe; studio tests pass.
- Packaged exe: version 1.5.0, hoyo_toon raster, nimble_toon raster / PT / offline GI, no `[E]`.
- The settings-memory flow clicked through in the lobby (nimble -> hoyo_toon_v2 restored the folder and the changed slider).

### Not verified / notes
- Real-time PT and raster captures are not deterministic (playback clock), so only DXIL identity covers "packless PT unchanged"; the
  `ScenePass` texture-loading move (now `PackTextures::Upload`) was reviewed, not diffed by pixels. RT camera view of nimble_toon not rendered.
- Head frame in the offline renderer was not checked by a render (same `RtScene::Build` code as PT). PT denoiser vs fast motion not tested.
- Offline GI still draws the app's default outlines (no `PSEdgePack`); PT / GI pack textures are a second GPU copy next to ScenePass's.
- The online gallery cards do not show PT / GI support (the index has no such field).
- Magnus / Vanya were only seen in cuts that frame the body or the top of the head; faces not compared with the official renders side by side.
- `pack_check <relative zip>` fails ("tar.exe failed"), absolute paths work (pre-existing, not fixed in 1.5.0).
- Pitfalls: headless runs need `--autoplay` or the app waits in the lobby (this looked like a hang twice); deleting `CMakeCache.txt` of a
  dev build dir breaks `build.cmd` (it only re-configures when `build.ninja` is missing); `pack_check` run from Git Bash picks GNU tar for
  zips; python heredocs with quotes broke several shell commands (write files with the Write tool instead); the website's docs order comes
  from `src/data/docs.ts`, not the frontmatter `order`.

### Next
- `PSEdgePack` in the offline renderer, PT / GI badge in the gallery index, a faster-motion check of the PT denoiser with hard cel edges,
  side-by-side comparison of nimble_toon with the official renders on all four models, fix `pack_check` for relative zip paths.

## 2026-10-09 — offline GI runs the shader-pack effects, view-space effect normals, auto_luminous gallery pack

Request: apply the screen-effect packs to the offline GI renderer (4K stills, GI videos) and check whether depth / normal effects work
there; then close the session and publish auto_luminous to the public gallery following the publish procedure (the user supplied the
preview capture, cropped to 16:9).

### Done
- Offline GI runs the effect stack (`OfflineRenderer::Impl::Finish`, `PackEffectPass::RunOffline`, `Ready`). Pre-bloom entries run on the
  lit HDR image: `CSLitCompose` composes denoised radiance x albedo, volumetric light and outlines (the real-time frame's order), the effect
  runs on it, then bloom reads the effect output (`CSBloomDown` / `CSFinalize` lit mode). Post entries run on the final sRGB image. With
  no enabled, compiling pack the GI path is unchanged.
- GI effect inputs: depth from the G-buffer view depth (`shaders/offline_effect.hlsl`, the inverse of `LinearZ`), normal oct-encoded in
  view space like the real-time normal target (`CSEffectNormal`), motion a 1x1 zero texture (the offline renderer has no per-pixel motion
  vectors).
- Found and fixed: the GI normal input was in world space, while the real-time normal target is view space (`mmd.hlsl` `PackOutput`). The
  docs and template comments said world space; corrected to view space.
- The `Renderer` pass order fix (pre-bloom share before Bloom) was uncommitted before this session; it is part of this commit.
- auto_luminous 1.0.0 (pre-bloom glow: strength, threshold, knee, radius; apiVersion 3; minAppVersion 1.5.0) published to the gallery from
  the website repo: preview cropped to 16:9 (1600x900) from a play capture, `build-packs` and `pack_check --compile` OK, `deploy.sh` run.
  The website repo (private) commits d8e2476 and 534b53e are pushed to its remote.
- Docs: `docs/shader_effect_api.md` (rules, example list) and CLAUDE.md (gallery list, offline GI note).

### Verified
- Build (build_au) passes; no `[E]` / `[W]` in the GI and real-time runs.
- GI still without effects (`--effect none`): byte-identical to the render before the change (md5).
- GI still with auto_luminous: glow visible; bright pixels (luminance > 150) +4.6 on average, the dim surround +3.4; 40% of the pixels
  changed (max 132 levels in R).
- Lit-path side effect alone (identity pre-bloom pack): 7% of the pixels, at most 12 levels.
- Post path (test pack halving G and B): all 518,400 pixels within +-1 of the expected values.
- Depth and normal against the real-time render of the same pack (split test pack: view normal left, log depth right; real-time capture
  at the seek frame): depth mean abs difference 1.25/255 (character 0.6); view normal on the character: 86% of the pixels within 24 levels,
  median 3; background 100%.
- Live site: index.json lists auto_luminous (version, apiVersion, minAppVersion and sha256 match the local zip); the zip, the preview and the
  pack pages (ko / en / ja / zh) return 200; the gallery pages list the pack.

### Not verified / notes
- GI floor normals: about a third of the floor pixels in the test view (32%) get a zero normal in the GI G-buffer while their depth is
  valid, so normal-based effects read a stray direction there. Cause not isolated. The denoiser reads the same G-buffer normal, so a fix can
  change GI output. Not fixed.
- GI videos with effects were not rendered (the same `Finish` runs per frame).
- Thumbnails (`RenderToImage`) still run the effect stack when one is set (unchanged).
- Motion-based effects see zero motion in GI.
- The in-app online install of auto_luminous was not clicked through (zip extraction path only).
- The website's screen-effect pages describe the 1.5.0 behaviour (GI not covered yet); update them with the next app release.

### Next
- Decide the far-floor normal fix: (a) reconstruct the normal from depth in `CSEffectNormal` for zero-normal pixels (GI output unchanged),
  or (b) fix the G-buffer write (GI output may change).
- App release with GI effect support: version bump, website docs, pack `minAppVersion` where needed.
- Decide whether thumbnails should skip the effect stack.

## 2026-10-09 (13) — studio scene lights redesign: phases 1–3 + session handoff

Request: the studio lighting was a source switch (VMD track / preset / custom rig) inside the camera panel. Redesigned as scene light
objects (sun, point, spot, ambient) with keyframes; presets become objects; viewport gizmos; C4D / 3ds Max / Blender light properties
(shadow type, softness, density, shadow colour, falloff, diffuse / specular, viewport visibility). Handoff: `docs/handoff/2026-10-09-studio-lighting.md`.

### Done
- Phase 1 (`7b85d89`): SceneLight model, project format version 2 with version 1 conversion, `BuildSceneLighting` (play-mode `BuildLighting`
  unchanged), rig UI removed from the camera panel. A camera VMD light track with only MMD default keys is dropped on entry to the studio, as
  play mode already does.
- Phase 2 (`33d91da`): outliner light group, selection (`selectedLightUid`, `selectedModel` stays -1), per-kind inspector including the
  shadow / falloff / affect / visibility fields, keyed-edit rule, preset confirmation popup, undoable add / delete / enable / rename.
- Phase 3 (`a058d44`): viewport handles (point position and range, sun rotate rings and direction arrow, spot position, aim in manual mode,
  cone, range), `ResolveSpotAim` shared with the renderer, `studiolight*` script commands, `STUDIOLIGHT` log lines.
- The shadow and property fields are stored, saved and editable, but the renderer does not use them yet (phase 4).

### Fixed in review
- Light drags ended on the frame after the press: `StudioViewportPose` returned early when no model is selected (always the case for a
  light) and reset the drag. Scripted cone drag went 13.8 to 0.1 degrees; after the fix, 13.8 to 6.9 degrees.

### Verified
- `build_dev` builds with 0 errors. `studio_project_test` 133, `studio_light_test` 84, `studio_edit_test` 15, `studio_gizmo_test` 19,
  `studio_pose_test` 38; all pass.
- Captures in `build_dev/captures_phase3`: concert preset with spot handles and cone outline; cone drag before and after the fix; sun rotate
  rings and arrow. No `[E]` lines.

### Not verified / notes
- Position, aim and range drags were not captured separately; they share the fixed path.
- The renderer is untouched. Point lights cast shadows only in the path tracer and offline GI; the real-time raster path has none.
- Sun shadows still come from the camera VMD self-shadow track and the render settings; the sun's shadow type is not connected.
- The sky glow (`SkyColor`, `common.hlsli` 96-97) still follows `gLightDir` when the sun is off.
- The phase 3 worker made 5 app launches against a limit of 4. The phase 2 worker force-stopped a running `MMDX12.exe` during its build;
  the phase 3 brief forbids stopping processes.
- The Sonnet phase 1 subagent hit its weekly limit (resets Oct 11, 8am Asia/Seoul); phase 1 was finished by hand.

### Next
- Decide D-a (point shadows in the real-time raster), D-b (area light) and D-c (sun shadow vs camera VMD self-shadow track); see the handoff.
- Phase 4: renderer support for shadow type, softness, density, falloff and affect; point shadows per D-a; the sky glow when the sun is off.

## 2026-10-09 (14) — studio scene lights phase 4a / 4b, quad-view DoF fix

Request: carry the light properties into the renderer without changing any existing look (renderer, shader packs); point-light shadows in the
real-time raster as well; DoF made the orthographic quad views blurry.

### Done
- Phase 4a (`6a6dd25`): shadow type / softness / density / colour, falloff (None = today's curve) and diffuse / specular switches in raster, RT,
  path tracer, offline GI and the volumetrics; sun shadow type / softness / density / colour (the camera VMD self-shadow track still decides on / off
  and distance); the sky glow follows the sun colour (none with no sun). `GpuLight` is 96 bytes.
- Quad-view DoF (`fa8f863`): `dof.hlsl` gets the camera rect (`gP2`) like the composite pass; the orthographic views stay sharp, the autofocus looks at
  the camera quadrant. Single view unchanged.
- Phase 4b (`ea2dc2b`): point-light shadows. Raster: 6-face depth array per light (first 4 shadow-casting point lights, characters only, 1024 per face,
  allocated lazily, DSV heap 16 -> 64). RT: character-masked ray queries (cone-jittered when Soft). Opt-in `PunctualLight::castPointShadow`, set only by
  `BuildSceneLighting`, so play mode / lobby / benchmark lights are unchanged. The inspector note now states the 4-light limit.

### Fixed in review (the workers' reports said PASS)
- 4a: `gSunShadowParams` indices were off by one in offline_gi / rtreflect / volumetrics (default GI lost the sun shadow); GI Hard used a zero-width cone
  (changed the default look; now keeps the sun disc); `SkyColor` divided by 0.6 (changed sunset / night glow); the worker had set the play-mode concert
  fill to NoCast.
- 4b: the worker's `studiorender` script command saved `renderPath` into mmdx12.ini (every later run silently ran RT) and had no effect anyway: the studio
  viewport is always raster (`UiStudio.cpp` ~1503). Removed. Its "RT verified" captures were raster.

### Verified (my own baseline-vs-new runs, not the workers' reports)
- Baseline = git worktree `E:\repos\MMDX12_base` (`build_base`, built from the commit before the phase). Same command, `--autoplay --paused --seek 20`, a second
  baseline run for the noise floor. Play raster, nimble_toon, RT play: noise level; offline GI (default and concert): bit-identical; studio concert raster:
  small change (the fill light now casts a shadow); PT play: noise floor.
- 4a features: offline GI sun NoCast / Soft / density 0.4 + red shadow differ as expected; raster sun NoCast removes the floor shadow.
- 4b features: raster Hard / NoCast / Soft / density / red; RT checked through `--project --offline-video --offline-renderer rt` (ffmpeg frame extract):
  Hard casts, NoCast does not. DoF quad view before / after: ortho views sharp after.
- `studio_project_test` 133, `studio_light_test` 92, `studio_edit_test` 15, `studio_gizmo_test` 19, `studio_pose_test` 38, `shader_choice_test`; no `[E]` lines.

### Not verified / open
- The cap of 4 shadowed point lights is a budget I picked, not a decision: a studio does not need it. RT does not need maps at all (its cap is only the
  slice index reused as a flag); raster should allocate by count. Lift it next session.
- Concert fill light: Hard (PT / GI keep its shadow, raster now shows a faint one) or NoCast (raster look unchanged, PT / GI lose it)? Undecided.
  Projects saved before 4a store the fill as NoCast; the renderer now honours that.
- A 5th point light being unshadowed, and point-light property changes in the offline PT video, were not compared.
- Falloff / diffuse / specular switches were not looked at by me (the worker's diff numbers only).

### Next
- Phase 4c: area light (data model, save format, UI, rendering); lift the point-shadow cap; decide the fill light; see the handoff.

## 2026-10-09 (15) — studio lights: no shadow caps, area lights (4c), PT / GI punctual specular

### Done
- `94ab1fc` Point-light shadow cap removed. Raster allocates 6 slices per shadowed point light (grow-only; faces 1024 while the lights fit the
  4-light budget, x2 at shadow quality 4096, then halved down to 256), DSV heap 64 -> 192. RT has no cap (the slice is only a "casts" flag:
  `FillGpuLights(..., pointCap)`). The concert preset's fill light is NoCast (user decision: a fill casts no shadow of its own); play mode unchanged.
- `9e28a42` Phase 4c, rectangular area lights (Antigravity worker, spec `docs/handoff/specs/04c-area-lights.md`): `LightKind::Area`, keyable
  `LightValues::size`, manual aim, saved as `values.size`; inspector / outliner / add menu; viewport outline + normal arrow + position / aim handles;
  `studiolightadd area`, `studiolightset <i> size w h`. Width axis = cross(worldUp, normal) everywhere. Real-time: centre point x one-sided cosine;
  raster shadow through the point-shadow map with PCSS (blocker search -> penumbra; written by me in review, the worker's radius saturated at its
  clamp so the size had no effect); RT: rays to random points on the rectangle. PT / offline GI sample the rectangle. `GpuLight` 96 -> 112 bytes.
- `1412da1` PT / offline GI punctual specular (worker, spec `04d-pt-punctual-specular.md`): next-event estimation of point / spot / area lights for
  the specular lobe (the same uniform reflection cone the specular continuation samples, 1/Omega), sharing the diffuse shadow ray, honouring
  `affectSpecular`. Before, delta lights had no highlight in PT / GI at all (raster only), so "specular off" did nothing there.
- `4d05c59` Spot shadow cap 8 -> 16 (= kMaxPunctualLights); `kSceneCbSize` 2048 -> 2560 for the 16 spot matrices; point matrix indices start at
  3 + 16. Falloff "None" is labelled "기본" (Default): it is the engine's default curve, not a constant. Area PCSS: 24 taps, rotation per position
  (constant over time: TAA is usually off and always off for raster videos, a per-frame rotation shimmered).

### Verified (my runs)
- 6 coloured point lights: raster with lights 5-6 NoCast vs Hard removes exactly their floor shadows; RT offline frame shows them too.
- Area light: outline faces the aim, size changes the outline and the penumbra (small = contact-hard, large = soft), facing away goes dark, PT frame ok.
- Offline PT point-light properties (`build_dev/captures_pt`, noise floor 0.23 / 1.0 char): linear 6.4, inverse square 43, diffuse off 40 (floor).
  Linear is brighter than "None" at d/range 0.28 by definition (0.72 vs 0.55).
- Specular: light behind the character (reflection in view): PT and GI show the same floor highlight, off = none. GI with specular off is
  bit-identical to before the change; PT within noise.
- 10 spots (concert + 4): spots 9-10 NoCast removes their floor and beam shadows.
- Tests: studio_project_test 146, studio_light_test 106, edit 15, gizmo 19, pose 38, shader_choice; no `[E]`.

### Not verified / open
- No baseline-build comparison for "no area light / no point light" scenes after 4c and the specular change (the code skips the new paths; GI
  specular-off scene was bit-identical to before).
- The area PCSS penumbra keeps a fine static grain on wide penumbrae (24 taps, 512-texel faces with > 4 lights).
- PT / GI highlights of the glossy floor are small sharp discs (rough 0.12 cone); raster draws a broad Blinn 64 highlight there.
- Worker: Antigravity did 4c well (19 min, no scope creep, one real bug). The specular run was correct but died on API errors twice; I finished
  the verification.
- Rendering stays forward (toon materials, shader packs, alpha, MSAA, outlines); if lights grow past ~16, Forward+ light culling, not deferred.

## 2026-10-09 (14) — HoYo Toon PT / GI + face shadow fix (gallery 1.1.0 / 2.1.0), pack flat-face fix, PT and GI pack outlines

Request: 1) PT / GI support for hoyo_toon v1 / v2, version bump, publish to the public gallery; 2) fix the hard vertical face
shadow of both packs (Hu Tao screenshot); 3) find out why shader-pack outlines (nimble_toon) do not show in GI. Follow-ups: the
nimble_toon PT / GI look, GI pack outlines, PT outlines that follow GI. Worker: Antigravity (agy, Gemini) for 1+2 (level high,
smoke review); Claude did the review, publish, the engine fixes and both outline features.

### Done
- Face shadow (v1, and v2 without a FaceLightmap): the old stand-in used only the head-right coordinate, so the boundary was a
  vertical plane (a straight cut from forehead to chin) and a 36 degree light already shaded a quarter of the face. Now a
  cylindrical head-space normal against the light projected into the head's horizontal plane (`hoyo_toon_core.hlsli` /
  `hoyo_toon_v2_core.hlsli`, shared by surface.hlsl and the new pt_surface.hlsl). v1 face class rule gained `面` (Hu Tao's face
  material; v2 had it).
- hoyo_toon 1.1.0 / hoyo_toon_v2 2.1.0: `pt_surface.hlsl` for both (v2 with light maps / ramps / face SDF through
  `PtPackSampleTex*`, v1 fallback per missing map), minAppVersion 1.5.0, descriptions (4 languages) and a `pt` tag. Published from
  the website repo (b2ff748, pushed): build-packs, `pack_check --compile` on both zips, deploy.sh; live index / zip SHA-256 match.
- Engine: PT / GI OR-ed `MAT_FLAT` (a material without toon) into the pack's `flatFace`, which drops the pack's terminator and
  shadowBias. The pack's `flatFace` now decides (`pathtrace.hlsl`, `offline_gi.hlsl`; the GI cache prepass gathers around the
  normal for PT pack surfaces). Affects Summer Fest_Felix (18 of 19 materials without toon); Darko / Magnus / Vanya use shared toons.
- Offline GI pack outlines: `offline_edge_pack.hlsl` (mmd.hlsl with `MMDX_NO_SHADOW_PASS`, PackEdge colour / width with the
  offline shutter / lens camera from the new `offline_edge_skin.hlsli`), one DXC PSO per pack (`PreparePackEdges`), pack texture
  table + clamp sampler in the edge root signature.
- PT outlines (new, packless too): `ScenePass::DrawPtEdges` (depth pre-pass `PSDepthAlpha` + default / pack edges into the cleared
  MSAA targets, jittered camera) and `PathTracePass` composites them over the denoised colour (`pt_edge.hlsl`, outline velocity).

### Verified
- Before / after captures of the face (`captures/hoyo_pt/`); v1 raster / PT / GI and v2 with the game textures
  (`library/gamerip/hutao`) raster / PT / GI with no `[E]`. A diagnostic pt_surface showed the PT face SDF term matches raster.
- Packless DXIL of `CSPathTrace` / `CSRender` identical after the flat-face fix; packless GI still md5-identical; Felix GI with
  nimble changed (face / hair modelling back), Hu Tao v2 GI 0.03 levels.
- FXC output of mmd.hlsl's 11 raster entries and offline_edge.hlsl's 4 entries identical to HEAD (raster and packless GI unchanged).
- Outline renders with pinned lighting (`captures/edges/`): GI nimble violet outlines vs default, PT outlines in nimble / packless.
- PT being brighter than raster is the traced indirect, not a pack bug (packless: face +40 levels, nimble +21).

### Not verified / notes
- PT outline cost, TAA ghosting with fast camera motion, studio viewport / PT video outlines not checked.
- Packs that return `flatFace = true` on faces now gather the GI cache around the normal (no public pack does).
- `build_dev` did not recompile the `src/app` objects after a `RenderPass.h` change (stale layout crashed `Renderer::Initialize`);
  touching the sources fixed it. Header dependency tracking of that build dir is suspect; not fixed.
- `build` / `build_au` ini still point Hu Tao's texture folder at the old F: path; `build_dev` ini settings changed during the
  session (lighting 3, motion lighting on, Hu Tao's pack choice cleared) and were left as they are.
- The engine changes need an app release to reach users (gallery packs work without them; their PT / GI faces then lose the
  terminator only on toon-less materials).

### Next
- App release with the flat-face fix and PT / GI outlines; refresh the website PT / GI docs.
- Check PT outline + TAA under fast motion, and PT outline cost.

## 2026-10-09 (16) — studio: camera key fields no longer duplicated, effects popup, overlay declutter

### Done
- Bug: selecting a camera key away from the playhead drew the camera panel's playhead "카메라 값" fields and the inspector's
  selected-key fields together, with the same widget IDs (ImGui "2 visible items with conflicting ID"). The playhead section
  now hides while a camera key is selected, and the key section is the only editor, including for a key under the playhead.
  `DrawStudioCameraKeyFields` pushes separate IDs for the live and key copies.
- Committed separately (work already in the tree): the studio top bar's screen effects popup (`DrawStudioEffectsMenu`:
  bloom / DoF / volumetric / SSAO / SSR, LUT chips, effect stack; app-level settings), and overlay changes (bone overlay:
  smaller minor bones, face bones hidden unless selected, depth fade, chain to the selected bone; camera path only while
  the camera is selected, fading with frame distance from the playhead).

### Verified
- `build_dev` headless script (`captures/studio/camdup/`): camera key at frame 503 selected with the playhead at 600 shows
  one set of fields + the curve editor, no ID warning.
- Clean rebuild of `build` (`--clean-first`): 0 errors, 0 compiler warnings; `studio_gizmo_test` 19 / 19.

### Not verified / notes
- The effects popup and the overlay changes were not re-checked visually this session.
- While a camera key is selected, auto-key is reachable from the transport bar only (the panel's switch hides with the section).

## 2026-10-09 (17) — soft shadows that are actually soft (PCSS raster, one softness scale), concert spots Soft

### Done
- Feedback: "Soft" shadows were not soft enough. Raster Soft was a fixed 1.6-6.4 texel blur of the sun cascade (a few mm in
  cascade 0, near-identical to Hard) and 3 texels for spots / points; the ray paths capped at ~5.7 degrees, each with its
  own slider mapping, and the PT shader-pack sun ignored softness.
- One scale everywhere (`common.hlsli` `SoftShadowAngle` / `SoftShadowConeCos`): softness 0..1 widens the light's angular
  radius from the path's Hard light to 15 degrees with a 1.5 power (0.5 = 5.3 degrees). Used by RT (sun + points), PT (sun,
  pack sun, punctual), offline GI (sun, punctual).
- Raster Soft = PCSS (mmd.hlsl `PcssBlocker` / `PcssFilter` / `PerspectivePcss`): 16-tap blocker search, 32-tap Vogel PCF
  sized from the same angle (contact hardening), receiver-slope bias per tap, radius capped at 0.04 uv (`kPcssMaxUv`).
  Sun cascades are orthographic: depth range = 2 * radius + reach, the reach now passed in `cascadeTexel.w`. Spots and point
  faces linearise their perspective depth. Hard and area-light PCSS are unchanged; `shadowParams.z` is always 1.6.
- RT Soft sun traces 4 rays instead of 2 (wider cone, less grain).
- The concert preset's six spots are Soft (play mode `BuildLighting` and the studio `PresetLights`; the render benchmark
  keeps its own lights). `studio_light_test` updated to the new rule.
- Committed separately (work already in the tree): UI clarity pass (labeled project / render / add / preset buttons,
  tooltips, play-bar lighting preset menu, MenuItem width fix for auto-sized popups, empty search / empty light list
  messages, "메인 캐릭터 (자동)" label, translations).

### Verified
- Studio raster capture Hard / Soft 0.5 / Soft 1.0 (temp project, free camera): progressively softer floor shadow, sharp at
  the feet; no acne or noise on the character. PT capture and a 960x540 GI still at Soft 1.0 render with no `[E]` lines.
- Concert preset play mode in raster and RT: renders, no shader errors.
- `studio_light_test` 106 / 106, `studio_project_test` 146 / 146.

### Not verified / notes
- The concert spots' soft shadows were not seen up close (the captured frames' camera hid them).
- Existing Soft scenes look softer than before (intended); saved studio projects keep their own light settings.
- Raster PCSS rotation is per pixel and static (no TAA dependence); very wide penumbrae near the camera hit the 0.04 uv cap.
- Release 1.6.0 (after the session close): version bump `6c18c1b`, GitHub release v1.6.0 (notes in 4 languages, no cover image;
  asset SHA-256 a93a74b4… = local zip), packaged exe reports 1.6.0 with no `[E]` on the select screen. Website deployed:
  /latest.json -> 1.6.0 (sha256 matches), home pages 200 in all four languages. `package_release.ps1` needed ninja on PATH
  (`%APPDATA%\Python\Python314\Scripts`; the pip copy under miniconda is gone) and a cleared `build_release` cache.

## 2026-10-10 (1) — MCP control of a running instance (agents talk to MMDX12)

### Done
- Model Context Protocol support (docs/mcp.md): `mmdx12_mcp.exe` (tools/mmdx12_mcp.cpp, new CMake target, packaged)
  is a stdio MCP bridge (protocol 2025-06-18) that forwards tool calls over an owner-only named pipe
  `\.\pipe\mmdx12_mcp` (`_<pid>` for further instances) to `McpServer` (app/McpServer.*, overlapped I/O worker thread).
  The app runs them on the main thread (`App::PumpMcp` / `ExecuteMcp`, app/AppMcp.cpp) before ImGui::NewFrame;
  multi-frame calls (loads, wait_frames, ui_input, studio_add_model) answer later. Schemas + timeouts: app/McpTools.h.
- 33 tools: state / logs / library, screenshot (in-memory PNG: `Dx12Context::RequestCaptureMemory`, `EncodePngRGBA8`,
  `Base64Encode`), wait_frames, set_screen / load_scene / open_studio, play / pause / seek, render settings (validated,
  incl. shader pack and effect stack), studio state / command / save / open / add model / select / camera / bone / key /
  undo / redo, render_still / render_video / status / cancel, ui_input (ui-script commands, own queue counted in
  `mcpFrame_`), quit_app; bridge-only list_instances / connect / launch_app (waits up to 60 s for the pipe).
- `UiScript.cpp` refactored into `ExecuteUiCommand` shared by `--ui-script` and MCP (same command set).
- Navigation uses the UI's leave paths (`McpLeaveScreen`): refuses with unsaved studio changes unless
  `discard_unsaved`, refuses while loading / rendering / benchmarking, leaves a failed load's error screen.
  Renders go through `mcpOfflineOutput_` (never `options_.offline*`, which quit the app) and restore the borrowed
  run overrides when the job ends. Minimized: queries answer, other tools restore the window.
- Setting `mcp=` (default on; lobby detail switch "MCP 제어"), CLI `--mcp` / `--no-mcp`; headless runs need `--mcp`.
  "MCP" badge in the lobby and studio top bars while a client is connected.
- Delegation: design investigated with Antigravity, implemented by it (level high); review found and fixed a studio-load
  crash (`StartLoad(LoadTarget::Studio)`), 11 malformed tool schemas (arrays instead of objects), a no-op `ui_input`,
  renders quitting the app, pipe shutdown / stale-response / cancelled-read issues. A Sonnet QA pass then found 10 more
  (error-screen lock, studio_command guard bypass, minimized loads, effects not implemented, silent failures), all fixed.

### Verified
- `python tools/mcp_smoke.py --handshake-only` (schemas validated); full scenario scripts with screenshots: lobby badge,
  play raster / RT, GI still (app keeps running), studio from play, bone edit + undo, F1 help via ui_input, effect stack
  (CRT + grain visible), free vs keyed studio camera, failed-load screen; QA re-run 42 / 42. `m3_a.txt` ui-script still runs.
- Clean build, no warnings.

### Not verified / notes
- render_video with rt / pt / gi renderers and upscaler changes over MCP were not exercised.
- `list_instances` reports pid 0 for the base pipe `mmdx12_mcp`.
- The MCP `effects` value replaces the stack with default parameters.
- Not yet tried from a real MCP client (`claude mcp add mmdx12 -- <path>\mmdx12_mcp.exe --launch`).

## 2026-10-10 (evening): ENB-derived effect packs, effect API v4 (persistent state), studio focus track committed

### ENB port (PI-CHO ENB N9.5, local only)
- Ported the preset's techniques into single-pass effect packs: `enb_mxao`, `enb_mcdof` (screen-centre autofocus only),
  `enb_lens_dirt`, `enb_anamorphic_flare`, `enb_ghost_flare`, `enb_starburst`, `enb_lens_reflection`, `enb_lens_ca`, `enb_sss`.
  All pre-bloom; <= 16 sliders per pack, the rest as named constants. Written by Antigravity (3 parallel specs), compile-checked
  with `pack_check --compile` and rendered once each (captures/enb_test).
- **Licence: the ENB sources are CC BY-NC-ND 4.0 (McFly DoF BY-NC-ND 3.0). The ports live only in `captures/enb_packs/`
  (git-ignored) and must never be committed or redistributed.**
- Known: `enb_sss` has no skin mask (colour heuristic) and also catches skin-coloured floors; needs a material-class mask
  target for effects. Flare packs show nothing in scenes without bright lights at their default thresholds (starburst
  default threshold raised to 1.5). Eye adaptation was held back until the state contract existed (now possible).

### Effect pack API v4: persistent state
- Design investigated by Antigravity (investigate mode), implemented by it (default level), reviewed and fixed here:
  `passExecutionCount_` now counts every execution (gaps -> reset), and the 64-step still loop reuses two descriptor tables
  (it exhausted the transient window). Contract in docs/shader_effect_api.md; CLAUDE.md has the architecture lines.
- Backward compatible by construction: no `state` = same root signature, defines, PSO and no extra targets; older apps reject
  `apiVersion 4`, `pack_check` forces apiVersion 4 for packs with `state`.

### Verified
- Clean build; `shader_choice_test`, `studio_project_test`, `studio_edit_test`, `studio_light_test`, `studio_focus_test` pass;
  `pack_check --compile` on state_probe (PSEffect + PSEffectState) and the v3 packs; negative manifests (state with apiVersion 3,
  floats 0 / 17 / 2.5, missing PackEffectState) are reported.
- `captures/state_test/state_probe` (frame counter bar): play capture at 20 / 90 frames = 19 / 89 steps (one per frame, reset on
  frame 0); GI still = 63 (64 steps, reset at step 0); raster video 0..14 and GI video 0..11 across frames (extracted with ffmpeg).
- v3 packs (auto_luminous, film_grain) render with no `[E]`.

### Not verified / notes
- Reset on `cameraCut` during playback, PT video (`advance` only on the last pass), studio viewport, and the RT / PT real-time
  video paths were not exercised; GI video below 960x540 via CLI cancels at BeginOffline (also without effects).
- The studio DoF focus track (d66b57a) was committed as found from an earlier session; only its unit test was run here.
- Next: McDoF focus smoothing + an `enb_adaptation` (eye adaptation) pack on the new state contract.

## 2026-10-10 (afternoon) — clean-room lens / DoF / AO effect packs, effect API v5, multi-client MCP, release 1.7.0

### Done
- The ENB-derived packs (CC BY-NC-ND, local only) are replaced by our own effects, written from scratch (no ENB code;
  the dirt / bokeh-shape texture atlases are the user's own): `lens_dirt`, `lens_reflection`, `lens_flare`, `bokeh_dof`,
  `screen_ao` (MIT, online gallery, sources in the website repo `shader-packs/`) and `chromatic_aberration` 2.0.0
  (spectral lateral colour, barrel / pincushion distortion, edge softness, vignetting; same param keys, still API 3 / post).
- Why the ports looked wrong: MMD scenes hold almost no HDR lights (a lit night window is ~0.4 linear), so fixed
  thresholds caught nothing or caught bright skin. The lens packs pick lights by luminance x local contrast (true HDR
  always counts) and respond with saturating curves.
- Effect API v5 (docs/shader_effect_api.md): pack.json `passes` (<= 8 `{entry, scale}`, RGBA16F targets pooled by size,
  `PackPassSample/Load/Size`), focus helpers (`PackFocusZ` = the studio focus track / play-mode head / centre autofocus,
  `PackFocusAperture`, `PackDofAperture`, `PackDofMaxRadius`, `PackCocPx`), `"replaces": ["dof"]` (DofPass and the GI thin
  lens stay off while such an entry is enabled; the studio focus panel no longer says DoF is off then). One entry's draws
  = `PackEffectPass::RunEntry` (RunStack / RunOffline share it). `pack_check` compiles every pass and validates passes /
  replaces. Packs without passes keep the v3 / v4 root signature and PSO.
- `bokeh_dof`: half-res CoC, tile max + dilation, scatter-as-gather over round / bladed / texture (heart ...) apertures,
  cat's eye, highlight boost (only where blurred), fill, full-res composite, focus-zone view. `screen_ao`: horizon-based
  AO with depth-reconstructed normals (mesh normals on stages are unreliable), colour bounce, depth-aware blur,
  joint-bilateral upsample. `lens_flare`: aperture-shaped ghosts, ring, anamorphic streak (two-step horizontal blur),
  star spikes. `lens_reflection`: inverted disc reflections with coating tints and per-channel scale. `lens_dirt`:
  near / wide light glow lighting the dirt atlas.
- MCP: `set_render_settings.effect_stack` (`[{id, enabled, params, texture_folder}]`, params validated and clamped),
  `effects` keeps the parameters of entries already in the stack, `get_render_settings` returns them. `McpServer` takes
  up to 8 clients at once (a listener keeps one free pipe instance, one thread per client); a stale bridge no longer
  locks everyone else out.
- Website: effect docs (4 languages) gained API 4 / 5 sections and no longer say effects skip offline GI. Gallery previews
  of the new packs are GI stills of the liar dancer project at frame 3909, rendered over MCP.
- Clean-up: only `build/` remains (build_dev / build_release removed, the baseline worktree removed; the `ptpack_*`
  test packs moved to captures/ptpack_packs).

### Verified
- GI stills from a HEAD baseline build and this build: byte-identical with no effect, with `film_grain` (v3) and with
  `state_probe` (v4).
- Studio focus keys drive `bokeh_dof` (auto, manual 200, aperture 0 = sharp); offline GI uses the pack instead of the lens.
- All six packs `pack_check --compile` OK; negative manifests rejected; `shader_choice_test`, `studio_focus_test`,
  `studio_project_test` pass. All six together: 3.2 ms GPU at 1080p (RTX 4070 Ti).
- Two MCP bridges connected at once: a `wait_frames 300` on one did not block `get_state` on the other.

### Not verified / notes
- The `enb_*` packs are still installed locally in build/bin/shader_packs (never distributed).
- Lens effects stay subtle in ordinary MMD scenes by design; the previews use raised intensities.
- In the studio, an effect's blur near the viewport border samples the dark area outside the viewport.
- `bokeh_dof` replaces the GI thin lens in offline renders, which is physically worse there.
- The bokeh shape atlas's cell 4 is a game emblem (user-provided texture; their call).

## 2026-10-10 (evening) — RTAO temporal accumulation

### Done
- Problem: the ray-traced AO shimmered. `rtao.hlsl` shoots 4 cosine rays per half-res pixel with a seed that changes every
  frame, nothing averaged them over time, and TAA is off by default (only the Ultra preset enables it), so the per-frame noise
  went straight to the screen through a 7-tap depth-only blur.
- `ssao.hlsl` `PSTemporal` (RayTraced only): reprojects last frame's accumulated AO with the velocity of the nearest of the 2x2
  full-res pixels, clamps it to this frame's 3x3 neighbourhood (mean +- 1.25 sigma), blends with a history weight of
  0.92 (still) down to 0.6 (4 half-res texels per frame). Chain: rtao -> raw_ -> PSTemporal -> hist_[2] -> blur H -> blur V.
- `SsaoPass` owns `hist_[2]` (R8, half res) + `histValid_`/`histEye_`; the history is dropped on resize, camera cut, an eye jump
  over 12 units, offscreen renders, and any frame without RTAO. The raster SSAO path is unchanged (same code path as before).

### Verified
- Clean build, `--render rt` run with no `[E]`/`[W]`.
- Paused static scene (Miku, no stage, `catch_the_wave`, `--autoplay --seek 20 --paused --render rt --effect none`, frames 240
  and 242 via `--ui-script capture`): mean abs frame difference 0.210 -> 0.063 /255, pixels differing by more than 3: 1.92 % -> 0.21 %.
  Baseline = the same build with `PSTemporal` patched to `return cur;` in build/bin/shaders (restored afterwards).
  GPU 2.05 -> 2.09 ms; the two captures look the same.

### Not verified / notes
- Ghosting while the camera or character moves was only seen in one playing frame, not measured. Tuning knobs: the history weight
  range and `speed / 4.0` in `PSTemporal`, the 1.25 sigma clamp.
- RT real-time video renders (`--offline-renderer rt`) are offscreen, so they get no accumulation (same rule as TAA). The raster
  SSAO also reseeds its noise every frame and could get the same stage.
- `--ui-script` takes one capture at a time: a capture on the very next frame is dropped (use frames two apart). `capture` is not
  available through MCP `ui_input`. Without `--autoplay` the scene is not loaded and the lobby stays up (`--seek` alone does nothing).

## 2026-10-10 (late) — T-pose fix, dark theme, per-material shader overrides, studio UI restraint (committed at session close)

### Done
- `asset/RestPose.*`: native PMX/PMD characters whose upper arms rest above 20 deg below horizontal (T-pose, the Project
  Sekai rips) are turned to MMD's 38 deg A-pose at load (bones, vertices by the chain's weight share, vertex morphs, rigid
  bodies, joints, tail / fixed / local axes). Stages, props and glTF/FBX imports are untouched. Test: `tools/rest_pose_test`.
  Thumbnail cache version 6 (old thumbnails showed the T-pose).
- Dark theme: `MakeDarkPalette` (UiKit), live palette `ui::P()`, settings `theme=` (auto / light / dark), `--theme`; DESIGN.md
  has the dark token table.
- Per-material shader overrides (`ShaderChoice::materials`: material name -> pack class or default shading, `kPackClassOff`
  through raster / edge / RT / PT / offline GI); ini `characterShaderMaterial=`, `.mmdxproj` "shader"."materials"; UI
  `App::DrawMaterialShaderList` (studio inspector, play bar, library shader tab via `LoadCharacterModelOnly`).
- Studio UI restraint pass: timeline right-click key menu (add / copy / cut / paste / mirrored paste / curves / delete),
  volume popover, one-frame steppers, screen effects as a dock tab, camera-target row tints, click-to-type drag fields.
- Offline render: log lines for cancel / start / read / write failures; readback ranges use the buffer size.

### Verified
- Full build (except the locked `mmdx12_mcp.exe`), `rest_pose_test`, `shader_choice_test`, `studio_project_test`,
  `studio_edit_test`, `studio_focus_test` pass.

## 2026-10-11 — User guide on the website (captured over MCP), MCP widget automation, UX fixes, release 1.8.0

### Done
- MCP widget automation: `src/ui_probe/UiProbe.*` (compiled into the imgui target, `IMGUI_ENABLE_TEST_ENGINE`; external/
  untouched) records every widget of the last frame through imgui's test-engine hooks; UiKit widgets report id + text,
  `CardItem(..., label)` names library / pack cards (pack cards also carry `p:<id>` / `r:<id>` ids), outliner and light rows
  are named, renderer cards in the video dialog too, the timeline registers drawn-only rows (`##tlrow`) and visible keys
  (`##tlkey:<row>:<frame>`). New MCP tools `ui_items` / `ui_click` (Korean source text is matched through `Tr()`, so one
  script drives all four UI languages; clipped and covered items are skipped via `uiprobe::Covered`); `screenshot` gained
  `path` (full-size PNG), `region` and `return_image`. `tools/mcp_pipe.py` (pipe client without the bridge) and
  `tools/mcp_x.py` (short command lists). docs/mcp.md: 35 tools; mcp_smoke expects them.
- UX fixes found by walking the user paths for the guide: effect pack details get **화면 효과에 추가 / 빼기** (installing
  an effect never turned it on and nothing said where to go); an empty effect stack points to the online gallery (lobby)
  or says where it is; the `✦` glyph in the effect hint rendered as `?` (text no longer embeds it); the empty library
  shows **라이브러리 폴더 열기**; the video dialog draws a divider when its settings are scrolled (scrolled cards looked
  like they overlapped the subtitle; the clip itself was right); quitting during a library scan cancels the remaining
  probes (`ScanProgress::cancel`; 34 s -> about 17 s, bounded by in-flight probes).
- Website (MMDX12_Web): header Guide / Shader Packs / Releases (the gallery was only reachable from inside the docs);
  docs are one catch-all route with sections (시작하기 / 사용 가이드 / 스튜디오 / 고급 / 셰이더 팩 만들기); 14 new guide
  pages per language (install, quick start, library, playback, quality, video, shaders, benchmark, studio x4, MCP) with
  44 screenshots per language, all taken by `scripts/guide/capture.py` driving a packaged copy over MCP (fresh install
  state, Sour Miku / Theater / Catch the Wave, lobby 1600x900, studio 1920x1080; the save path line with the Windows
  user name is pixelated by annotate.py). Click-to-zoom screenshots, foldable doc nav on phones, header fits at 500 px.
  en / ja / zh pages translated by agents with UI labels taken from I18nEn/Ja/Zh.cpp.
- Version 1.8.0; release notes in dist/notes_1.8.0.md.

### Verified
- All 8 studio/unit tests pass; packaged 1.8.0 exe starts with no `[E]`; MCP handshake lists 35 tools.
- Four full capture runs (ko/en/ja/zh) unattended; spot-checked shots per language show the localized UI.
- Site builds; docs checked in headless Edge at 1400 / 600 / 500 px.

### Not verified / notes
- The studio inspector puts the model transform + shader above the Key / Bone / Morph tabs, so the curve editor sits
  below the fold even at 1080p (the guide scrolls the inspector). Worth a layout pass.
- Headless Edge can't go below ~500 px wide, so 390 px phones were not seen.
- `capture.py` needs one app instance at a time (it waits for the shared pipe name to free) and clears `recovery/`.

## 2026-10-11 — FBX/VRM import fixes (Leon Kennedy FBX, Von Lycaon VRM)
- Leon (RE Engine FBX rip) was skipped as "no head bone": `Head_0`, `L_Arm_Hand`, `L_Leg_Ankle`, `IndexF_1` names
  did not match, the main limb bones (`L_Arm_Upper`, `L_Leg_Lower`, ...) carry no weights (twist bones do) so they
  were not skin joints, and the file has 9 armatures (one per mesh part).
  - Name matcher also tries the core without an `arm`/`leg` prefix or a trailing `0`; fingers accept `F<n>`.
  - `MapHumanoid` retries with the joints' unweighted ancestors (`JointSet(s, true)`) when joints alone fail;
    `Humanoid::bones` carries the set into the skeleton build.
  - `MergeDuplicateSkeletons`: same-named skeleton nodes with equal world matrices become one bone (children re-hung,
    skins re-pointed). Leon: 283 merged, 1078 bones, every part follows the dance.
  - Side effect: Biped `Bip001 L Toe0` now maps to つま先 (Zootopia FBXes gain つま先ＩＫ).
  - FBX textures: fallback search by stem in the model folder + direct subfolders, any image format, also with a
    converter's `out`/`_out` suffix (`Texture\x.dds` -> `Textures/xout.png`). Leon 0/24 -> 24/24.
  - `kProbeCacheVersion` 3 (the cache keys on size + mtime only, so old "skipped" verdicts stuck).
- Lycaon VRM (ZZZ rip): knees/shins see-through. Materials are glTF OPAQUE but the textures hold masks in alpha and the
  import never used `ImpMaterial::blend`. Now `PmxModel::opaqueTextures` marks textures used only by opaque glTF
  materials and `DecodeModelTextures` forces their alpha to 255 (FBX keeps texture alpha: no reliable flag there).
### Not fixed / notes
- Leon: hair materials reference no texture in the FBX (white hair; `Hair_ALBSout.png` exists but is unreferenced);
  orange trims on the jacket (not from the albedo; material/texture assignment not investigated).
- One asset_probe run reported "skipped X.glb: " with an empty error from a stale probes.json entry (not reproducible).

## 2026-10-11 - Star Rail pack cleanup: starrail_face (online), hoyo_toon_v2 back to 2.1.0, pack hot reload fix
- 848e62e had folded Star Rail maps into hoyo_toon_v2 (2.2.0) and read the ramps top-down. Reverted hoyo_toon_v2 to
  2.1.0 (Genshin only). hoyo_toon v1 / v2 both stay: their no-texture paths differ (sRGB vs linear shadow mix, hair
  highlight band, warm tint, ambient default, class rules), the user chose to keep both and not rename them.
- New online-gallery-only pack `starrail_face` 1.0.0 "스타레일 페이스 셰이더" (MMDX12_Web `shader-packs/starrail_face`,
  deployed; not bundled): HoyoToon / StarRailNPRShader formulas. shadow_rate (2 x half-Lambert x 2G, x0.85+0.15) ->
  warm / cool ramp; ramp row flipped for D3D (`1 - (2 id + 1) / 16`, id = floor(8 A); verified: skin A = 6 -> bottom
  row fbe9e8); cast shadow multiplies the area (high-G necks no longer ignore it); body-type FaceMap (A = SDF mirrored by
  the light side, B = nose line), skin ramp row as the face shadow tone; thresholded Blinn-Phong (LM B / R), rim masked
  by LM R, PackEdge outlines, pt_surface.hlsl twin. Body light map as `_L` / plain / `Body1`; the "weapon" class = the
  second sheet (`衣服1` -> Body2_LightMap, `武器` -> Weapon_LightMap + Weapon_Ramp). LUTs / expression / stockings unused.
- Test data (git-ignored): official MMD models March 7th, Ruan Mei, Firefly, Dan Heng in `library/model/`, StarRailTextures
  rips + per-body-type FaceMaps + `HSR_SHADER_NOTES.md` + `INVENTORY.md` in `library/gamerip/StarRailTextures/`
  (collected / researched by Antigravity workers), ready texture sets in `_sets/`. The official models use the game UVs
  (verified against the 00 outfits); FaceMap layouts are near-identical across body types, so the body type is a guess.
- Verified: raster (Ruan Mei, Firefly, March 7th, SDF direction / mirroring with a lit debug at 0 / +-45 deg light),
  offline GI stills, no-texture fallback, pack_check. Real-time PT only compiled (not seen on screen).
- Hot reload fix: `SourceHash` (ShaderCompiler.cpp) memoized each folder's hash for the whole run, so a pack edit
  rescanned and "compiled" but hit the shader cache with the old key (old bytecode). Now the memo is keyed by a stat
  stamp (names, sizes, write times). Checked live: an edited surface.hlsl shows within a second and reverts.
### Notes
- `--offline-renderer pt` stills looked identical to GI (option may apply to videos only); the studio viewport ignored the
  MCP render_path change. Not investigated.

## 2026-10-11 - Pack previews: before/after compare (app + web), gallery sort / filter, all 13 packs re-shot
- Pack contract: optional `preview_before.png|jpg` next to `preview.*` (= AFTER). App: `ShaderPack::previewBefore`,
  `RemotePack::previewBeforeUrl/File` (index.json `previewBefore`, cached as a separate download), `DrawPackCompare`
  (Drag mode in both detail panes: press / drag moves the split, double click resets, knob + "적용 전 / 적용 후" pills;
  Hover mode on cards; "전/후 비교" badge). Web (MMDX12_Web): build-packs emits `previewBefore`, `type`, `stage`,
  `updated` (git date); `BeforeAfter.astro` (pointer capture, keyboard slider, card hover scrub; the clipped BEFORE image is
  fetched after AFTER loads because Chrome never starts a lazy image hidden by clip-path); docs in 4 languages.
- Sort / filter: app shader screen gets per-tab type filter (전체 / 셰이더 / 효과) + sort (추천 / 이름 / 최근 업데이트 /
  파라미터), count, empty state, type badge (`shaderType=` / `shaderSort=` ini). Website gallery: search, type segments,
  tag chips, sort, URL query state (`PackGalleryControls.astro`).
- MCP: `screenshot scene_only` (Renderer::ReadFinalImage, no UI), `set_render_settings shader_texture_folder`.
- Previews: 13 packs re-shot over MCP with `tools/pack_previews.py` (1280x720 JPEG, stability check before / after /
  before). Surface packs on the terrace (Hu Tao x2, Firefly, Felix), effects with Sour Miku on the theater stage;
  lens_flare and auto_luminous need the concert lighting preset (2); auto_luminous is shot with threshold 0.35 /
  strength 2.0 because its default threshold 1.0 barely triggers in MMD scenes (a default change was offered, not made).
  Every pack's patch version bumped (installed apps cache previews by id + version); old preview.png files removed.
- Implementation was delegated to Antigravity workers (app compare + MCP, web compare, both sort / filter); captures were
  started by a worker, then finished by hand (8 of its pairs kept).
### Notes
- The website is not deployed yet; the app's online tab reads previewBefore only after the deploy.
- Release 1.9.0 (unit dances, camera picker, studio frame aspect, play-mode autofocus, before / after pack previews, shader
  tab sort / filter, import fixes). Packaging found a stale `preview.png` in build_release/bin/shaders (copy_shaders never
  deletes) shipping the old hoyo_toon image: `package_release.ps1` now starts from fresh shaders/assets copies, and the
  pack loader picks the newer of preview.png / preview.jpg (folders unzipped over an older pack). The auto-updater swaps
  whole top-level folders and pack installs replace the pack folder, so updated users never had the stale file.
  Cover `docs/media/compare-1.9.0.jpg`, notes in dist/notes_1.9.0.md.

## 2026-10-11 (evening) - PT / GI pack lighting controls (face relief in GI), starrail_face 1.1.0, release 1.9.1
- Problem (user screenshots, Ruan Mei + starrail_face + game textures): the offline GI face showed its own relief (nose
  and hair cast shadows on the cheek, occluded / normal-graded indirect light making the lips look puffy) that raster
  never shows (raster pack: `noAo`, received shadows scaled by `P_FACE_CAST`). The PT contract could not express that:
  `PtPackComposeSunDirect` always multiplied the traced `sunVis`, and `flatFace` bundled the flat indirect gather with
  replacing the pack's terminator (`lerp(1, sunVis, 0.8)`), so an SDF face had to stay non-flat.
- Contract (`pt_pack_api.hlsli`, `PT_PACK_API_LIGHTING 1`): opt-in `PT_PACK_HAS_LIGHTING` + `PtPackLighting
  PackEvaluateLighting(PtPackIn, PtPackOut)` = `flatIndirect` (the flat indirect path only, terminator kept), `sunShadow`
  (0..1 weight of the traced sun visibility in the direct term and specular), `lit` (0..1 direct value, -1 = unused).
  New engine code in offline_gi.hlsl / pathtrace.hlsl / pt_pack_glue.hlsli sits behind `#ifdef PT_PACK_HAS_LIGHTING`:
  CSRender / CSPathTrace DXIL of hoyo_toon, hoyo_toon_v2, nimble_toon and no-pack builds is binary-identical to 1.9.0
  (worker's disassembly diff). pack_check checks the new fields; docs/shader_pt_api.md "Lighting controls"; template
  comment.
- starrail_face 1.1.0 (website repo): faces / eyes opt in (flat indirect, sunShadow = faceCast, the raster slider); the
  `#ifdef PT_PACK_API_LIGHTING` guard keeps it compiling on older apps (checked with the 1.9.0 pack_check), so
  minAppVersion stays 1.5.0.
- Verified by Claude against a HEAD baseline build (worktree, same projects `captures/pt_face_claude/face*.mmdxproj`:
  Ruan Mei close-up, side sun; and open mouth (VPD あ) + theater stage): baseline GI had hair / nose cast patches, a hard
  shadow beside the lips and lip relief; 1.9.1 GI shows the raster-like flat face with only the SDF shading. No `[E]`.
  Real-time PT with the pack is compiled (pack_check) but was not seen on screen.
- Implementation delegated (opencode, deepseek-v4.1-flash, level high, smoke review); version 1.9.1.
### Notes
- Studio stills ignore `--offline-renderer raster` (both "raster" and "gi" stills came out as GI); use the viewport
  capture for raster references. `--offline-renderer` persists into the ini.
- Play-mode song cameras rarely frame Ruan Mei's face (senbonzakura / envy_baby seeks showed empty frames).

## 2026-10-11 (night) - smart (delta) updater
- Goal: stop downloading the full ~178 MB zip for every update (1.9.0 -> 1.9.1 changed only the exe, tools and a few
  shaders).
- Feed: optional `manifest` / `manifestSha256` next to the zip fields (old apps ignore them). Manifest (format 1):
  `{version, objects, files: [{path, size, sha256}]}`; objects are content-addressed (`<objects><aa>/<sha>`, raw bytes,
  served gzip-encoded via pre-built `.gz` siblings; WinHTTP `WINHTTP_OPTION_DECOMPRESSION` through the new
  `net::HttpOption::AcceptGzip`, other downloads unchanged).
- `Controller::StageDelta`: fetch + verify the manifest, hash the install, hard-link unchanged files (copy fallback)
  into `update_tmp/extract/MMDX12-<ver>-win64/`, download + verify only changed/new objects; then the unchanged
  journal / swap / rollback / relaunch. Any failure except cancel falls back to the full zip in the same run. UI: a
  "checking installed files" stage and "N files changed · X MB / Y MB total" (four languages).
- Publisher `tools/make_update.py` (manifest + shared object store, `.gz` only where smaller); the website feed adds
  the manifest fields for versions listed in its `src/data/update_manifests.json`; the server serves
  `/updates/objects/` (immutable, gzip_static) and `/updates/manifests/` outside the site root, so site deploys don't
  touch them. The 1.9.1 release objects are already uploaded.
- Fixed two old updater bugs found by the E2E: the applier opened `mmdx12.log` with "wb" and wiped the updating app's
  log (now `LogAppendToExisting`), and `ArchiveLog` renamed the log while the applier held it open, so
  `mmdx12.previous.log` never survived (now `LogCloseFile` first, as the applier's last step).
- Verified: `tools/update_e2e.py` (local stores, headless, sequential runs) - delta (3 changed files fetched; changed /
  new / removed / unchanged files, library / ini / recovery intact, update_tmp cleaned, breadcrumb, previous log) and a
  corrupted object -> zip fallback, all checks pass. Same scenario over https against the real object store with the
  shipped 1.9.1 exe as the new exe: `delta 4 files changed, 5.4 MB to download (full zip 171.7 MB)` (the exe went
  over the wire as 2.6 MB gzip), installed; the test manifest was removed afterwards.
- Implementation started by a delegated worker (opencode, deepseek-v4.1-flash, level high); stopped by the user's
  request mid-E2E, reviewed and finished by Claude (stage label on fallback, progress accumulation, progress reset on
  fallback, drive-letter path check, log fixes, E2E assertions).
### Not verified
- The new progress-dialog strings were not captured on screen. Delta updates only start working from the first
  release that contains this code (installs older than it still take the zip once).

## 2026-10-11 (late night) - fix: unit dances stuck at 2-5 fps (physics reset loop), release 1.9.2
- Report: 1.9.1 played a six-performer unit dance at ~5 fps (raster; GPU 23 %, CPU 8 % = one core), smooth again
  after pause / play. Not an unoptimized build this time (the 1.1.0 regression): the shipped exe hashes equal to the
  build_release exe, every TU has `/O2 /Ob2 /DNDEBUG /EHsc`.
- Cause: play mode reset the physics whenever the motion clock advanced > 0.25 s between updates (meant for seeks).
  A reset settles every body for 1 s (120 substeps); six physics-heavy performers (~1200 bodies) made that frame
  itself > 0.25 s, so every following frame reset again. Pause / play broke the loop (small dt). One-performer scenes
  recovered because their reset stays under 0.25 s.
- Fix (`App::UpdateScene`, studio `UiStudio.cpp` playback): a forward jump only resets when it is much larger than
  the wall-clock time since the last update (`physicsClock`); slow frames keep stepping (Bullet's 10-substep cap bounds
  the cost). `PtPackVariants` logs "multiple pt packs present" once per chosen pack instead of every frame.
- Verified: liar_dancer, 5 performers (Aiden, Dan Heng, Felix, Sour Miku White / CWL), `--frames 600`: the 1.9.1 exe
  got stuck (52+ s after the first frame, freed by the user's pause), the fix ran 600 frames in ~3.8 s.
- Release 1.9.2 (also carries the smart updater), notes dist/notes_1.9.2.md; delta manifest published for 1.9.2.

## 2026-10-12 - Project Sekai Toon (gallery pack), pack API v6, content-based Sekai detection, CLI guide, release 1.10.0
- Goal: the Sekai Model Pack v4 rigs (and the PS single-character rigs) ship MME `.fx` shaders MMDX12 cannot run.
  Decision (Antigravity investigation, reviewed by Claude): no MME runtime (D3DX9 effects, ps_3_0, no G-buffer / motion
  vectors / RT / PT / GI; 185 other MME files in the library are all different kinds); a clean-room shader pack + engine
  plumbing instead. All 26 v4 rigs share one byte-identical shader (`_CharacterId 8`); the PS rigs use other ids (0 / 20),
  so skin colours are sliders, not the game table.
- Engine (pack API v6, `kPackApiVersion` 6): PmxLoader keeps additional UV1/UV2; `GpuAddUv` side stream in slot 4
  (stride-0 dummy without them, like SDEF) + a bindless raw buffer for RT (`RtGeometry::addUvSrv`); `PackSurface` /
  `PtPackIn` uv1/uv2, raw `_H` sphere-slot / `_S` toon-slot sampling at mesh UV (`PackSampleSphereUv`, `PackSampleToonUv`,
  PT mirrors), `PackEdgeUv2`; the sphere texture is now bound for toon-map materials whatever the sphere mode.
- `project_sekai_toon` 1.0.0 (online-gallery-only, MMDX12_Web `shader-packs/`, minAppVersion 1.10.0): hard terminator
  with `_H` B bias, `_S` shadow colour, skin ramp, face SDF at uv1 (head-plane light angle), uv2 rim mask / outline width,
  PT / GI via `PT_PACK_HAS_LIGHTING`. Clean room: the worker never read the library `.fx` files.
- `asset/MmeHints`: follows the model's own `.emd` (same stem, else the folder's only one) -> `.fx` -> `#include` ->
  `ResourceName` references to find the face SDF and set a per-model default pack texture folder. User rule: the library
  is user-filled, nothing may depend on its folder / file names (memory + CLAUDE.md).
- Detection `asset/SekaiLayout` (>= 2 additional UVs + a non-ramp toon texture) = `GpuModel::HasSekaiLayout()`; auto-apply
  once per character (`characterShaderAuto` ini, toast) when the pack is installed, else a one-time gallery hint (4
  languages). `tools/sekai_detect_test`: 11 self tests + library scan: 180 models, 49 detected, all Sekai rigs (26 v4 + 23
  PS files), no false positives (pjsk stages with 3 additional UVs, Genshin / Star Rail / ER rigs rejected).
- CLI guide ("Running MMDX12.exe from the CLI") in CLAUDE.md and the new AGENTS.md: `--autoplay` is required to enter a
  scene, `--seek --paused --no-physics` gives bit-identical frames (measured: 113k px run-to-run noise with `--seek` only,
  1.2k with `--paused`, 0 with `--no-physics`), one ini per build dir.
- Delegation: investigation by Antigravity; engine, pack, detection test by opencode (deepseek-v4.1-flash, level default,
  smoke review); Claude fixed the .emd fallback, wrote the guide, previews, release.
### Pitfalls
- `pack_previews.py` connects to ANY running app with an MCP pipe (it used a `build/bin` instance started ~16 min earlier,
  then quit it). Close other instances first.
- `pack_check` usage is `pack_check <dir|zip> --compile` (the flag after the path).
### Not verified
- Auto-apply toast / gallery hint and studio auto-apply on screen (headless runs skip them by design); PT / offline GI
  renders with the pack (compiled only).
- Look tuning against the game: faces / skin read slightly bright; outlines use the material edge colour, not `_S`
  (`PackEdge` has no mesh UV).
- Release 1.10.0: GitHub release v1.10.0 (notes dist/notes_1.10.0.md, 4 languages; asset SHA-256 96df54bb... = local
  zip); packaged exe: select screen, no `[E]`, `shaders/packs` = hoyo_toon + hoyo_toon_v2 only. Delta manifest 1.10.0
  (12 new objects) uploaded, live manifest / exe object hashes match; website deployed: /latest.json -> 1.10.0 with
  manifest fields, gallery index lists project_sekai_toon 1.0.0 (apiVersion 6, minAppVersion 1.10.0, live zip SHA-256 =
  local), the zip passes the shipped 1.10.0 `pack_check --compile`; website commit 2d362eb pushed.

## 2026-10-12 (2) - 1.10.1: generic pack "suggest", pack alphaKey, Sekai eye fix, Ray-MMD survey, 2.0.0 handoff
- Direction correction (user): the engine must not know a game or a pack id and must not silently apply a pack; the library
  is user-filled and models of one game differ between authors. 1.10.0's `asset/SekaiLayout` / `HasSekaiLayout` /
  hardcoded `project_sekai_toon` auto-apply were replaced by: `asset/ModelTraits` (additional UV count, toon-map material
  count), pack.json `"suggest": { additionalUvsMin, toonMapMaterialsMin }` (`ShaderPack::Suggests`), a one-time toast per
  (pack, character) (`characterShaderSuggested` ini, old `characterShaderAuto` ignored) and a "추천" tag in the pack picker.
  Nothing is applied automatically. `tools/pack_suggest_test` (35 self tests + library scan + `--pack` MATCH lines) replaces
  `tools/sekai_detect_test`: the pack's rule matches the same 49 of 191 models (26 v4 rigs + 23 PS files).
- Black eyes found: the eye-highlight material (`ehl`, `HL`) is an additive key sprite in the original MME (`additive_alpha`:
  discard where the texture red < 0.45) but was drawn opaque and covered the iris, in the default shading as well. New
  generic pack.json class-rule field `alphaKey` (0..1): materials of that class cut out pixels whose texture red is below
  it, in raster, RT, PT and offline GI (encoded in `MaterialConstants::packClass` bits 8..15, `RtGeometry::alphaKey`,
  `CandidateAlpha`). `project_sekai_toon` 1.0.2 uses it (class `weapon`, textures matching `_ehl_`). Checked: raster and PT
  renders of 01_9001 (iris + highlight), the 7 ps_* rigs in raster (all faces fine; Kaito and Meiko need a smaller character
  scale to fit the song camera), nimble_toon compiles / renders (no regression), `pack_check --compile` on the pack and
  hoyo_toon(_v2). The PS rigs share the Sekai Model Pack v4 PMX data layout (3 additional UVs, `_S` toon slot, `_H` sphere
  slot); they differ only in names, folders, material names, `_CharacterId` and two `.emd` files per folder.
- Note on clean room: while diagnosing the eye I read `shader.fxsub` of a Sekai rig to learn the `additive_alpha` semantics
  (one threshold). The pack itself is still written from a description of the look; no MME shader code is included.
- Survey of `library/by StarNoodle495` (12 characters, 16 folders): no shared proprietary look. They are plain PMX (extra
  UV only on EthanShax) meant for Ray-MMD PBR materials (163 material `.fx` in 9 sets incl. stage/moonlit_waves). A Ray-MMD
  material `.fx` only writes a G-buffer (`GbufferParam`, shading model ids 0..10); lighting lives in the controller.
  Decision (user): put PBR into the engine and fill it from Ray-MMD material data (and glTF/VRM), not a shader pack. Full
  hand-off for the Opus session: `docs/handoff/2026-10-12-pbr-raymmd-2.0.0.md`.
- Delegation: all code by opencode (deepseek-v4.1-flash, level default, smoke review) after a first attempt with a
  subagent was stopped at the user's request; Claude did the diagnosis, the pack fixes, the renders and the survey.
### Not verified
- The suggestion toast / picker tag on screen (headless runs skip toasts by design); offline GI render of the pack; the
  worker diffs were not read line by line (smoke review: builds, self tests, `pack_check`, renders).
- Release 1.10.1 is NOT published (no GitHub release, delta manifest, website deploy or gallery upload yet); the web repo
  holds the 1.0.2 pack and docs as a local commit.
- `IsFlatFaceMaterial` (GpuModel.cpp) still decides the flat-face path by the material names face / eyelash / eyebrow.

## 2026-10-12 (3) - 2.0.0 P0/P1: Ray-MMD semantics (fact-checked) + material parser and library probe
- `docs/raymmd_semantics.md`: Ray-MMD material slot / FROM / TYPE / swizzle / sub-map / emissive-morph semantics, shading models
  0..10 with controller-side formulas, G-buffer, GGX specular, tangents (screen derivatives), proposed internal PBR fields.
  Written by a worker, fact-checked claim by claim by a second worker against ray-cast/ray-mmd master + the local library,
  corrected (SPECULAR_MAP_TYPE 4 = saturate(specular); GetAlpha FROM 7/8 = PMX colours; several material_common variants exist).
- `asset/RayMaterial` (shallow preprocessor, .emd mapping, content-based Ray detection, candidate ranking) + `asset/MmeText`
  (helpers shared with MmeHints, behaviour unchanged); `tools/raymat_test` (33 pass), `tools/raymat_probe`: 22 models / 612
  Ray materials / 165 Ray .fx, 0 parse failures, 2.3 s; all 9 sets pick their Ray .emd.
- Look policy for 2.0.0 (user): PBR may change clothes / accessories / stages; anime skin and faces must never turn glossy or
  realistic. Handoff `docs/handoff/2026-10-12-pbr-raymmd-2.0.0.md` section 10 has the state and next steps (P2 design next).

## 2026-10-12 (4) - 2.0.0 P2: per-material PBR data reaches the GPU (raster + RT), debug view
- Delegated to DeepSeek (opencode, level high), smoke-reviewed and render-checked by the architect.
- `asset/PbrMaterial` (engine-neutral `PbrMaterial`, `RayToPbr` with FROM 0..9, per-model texture dedup, single-channel maps
  uploaded as R8, `PBR model=` measurement line), `GpuPbrMaterial` record (ShaderInterop.h / `shaders/pbr_common.hlsli`, one
  raw buffer per model, absolute SRV-heap texture indices), scene root param 14 (b3 `PbrRoot`), `RtGeometry` pad ->
  `pbrSrv` / `pbrIndex` (still 192 B). `MaterialConstants` / `GpuVertex` / `RtVertex` unchanged. Pack wins over PBR.
- Temporary CLI until the P5 UI: `--material-source default|ray`, `--pbr-debug none|albedo|normal|roughness|metalness|ao|emissive|model`.
- Architect fix: `.emd` candidate ranking preferred a plain one-.fx Ray .emd (`RayMain.emd`, every slot FROM 0) over the
  mapped one; now mostly-Ray first, then distinct Ray .fx count. All 22 models now pick their MaterialMap .emd.
- Verified: default path vs HEAD build = 59 px at +-1 LSB (FXC codegen; not bit-identical), Alcander normal / roughness
  debug views show the maps, RT and PT runs clean (no `[E]`). Alcander: 44/52 records, 37 textures, 281 MB VRAM, 171 ms load.

## 2026-10-12 (5) - 2.0.0 P3: PBR shading for Ray materials (raster + RT camera view, RT reflection hits)
- Delegated to DeepSeek (first run stopped for over-exploring the library's .fxsub; resumed with a decisions note),
  smoke-reviewed and render-checked by the architect. Design: "PBR on top of the MMD look" - `PSMain` dispatches to
  `MmdShade` (the old body) or `MmdShadePbr` (mapped normal, Ray albedo + sub blend, sphere ignored, metalness, GGX sun /
  punctual (`PunctualSpecularGGX`), split-sum style env specular over `Hemisphere`, AO, emissive, reflectivity =
  Luminance(F0) * (1 - roughness)); SKIN = roughness floor 0.6, specular x 0.15, no env specular, soft warm SSS tint;
  ANISOTROPY / CLOTH lobes; CEL / TONEBASED keep MMD shading. `rtreflect.hlsl` hits take the PBR albedo + emissive.
  Maps above 2048 px drop leading mips (Alcander 281 -> 225 MB).
- Architect fixes: scalar float3 initialisers broadcast (`const float3 albedo = 1.0;` was read as (1,0,0): everything red),
  emissive only with `EMISSIVE_ENABLE` (the default `emissive = 1.0` washed every material white), raster PBR maps sample
  with mips (`SampleGrad` from gradients set at the top of `PSMain`; compute users define `PBR_SAMPLE_LEVEL0`).
- Verified: default path vs the HEAD build = 3280 px at <= 3 LSB (invisible). Alcander raster / RT, EthanShax on the Ray
  stage moonlit_waves (55/55 records): no `[E]`, Alcander skin stays matte. Not verified: EthanShax's arm shows streaky
  gloss (shading model / roughness not checked), hair anisotropy up close, the other sets, studio, PT / GI (P4).

## 2026-10-12 (6) - 2.0.0 P4: PBR materials in the real-time path tracer and offline GI
- Delegated to DeepSeek (opencode, level high, 18.5 min), smoke-reviewed, DXC-compiled and render-checked by the architect.
  Shaders only, no C++ changes. New `shaders/pt_pbr.hlsli` shared by `pathtrace.hlsl` / `offline_gi.hlsl`: hit decode
  (triangle-UV tangent frame normal + sub normal RNM via `rt_common.hlsli` `LoadTriPosUv`), per-model policy, GGX VNDF
  sampling (Heitz 2018, weight F*G2/G1, alpha = rough like `PbrSpecularGGX`), lobe choice from Schlick F(N.V) vs diffuse.
  PT / GI mirror `MmdShadePbr`'s sun structure (toon terminator, hue-preserving shadow, rim) with GGX sun / punctual
  specular, metalness removing diffuse, AO on indirect diffuse only, emissive per hit; denoiser albedo = base colour,
  reflectivity = the raster formula. SKIN: no specular continuation, specular x 0.15, roughness >= 0.6. CEL / TONEBASED:
  MMD + normal + emissive. GI: `Surf::pbr`, IC branch's mirror lobe -> GGX sample, `GatherPath` diffuse albedo only.
- Architect fixes: the PBR decode ran before the "pack wins" check (normal / roughness / reflectivity, and in GI the
  normal handed to the pack); now a PT pack record skips the decode. GI punctual specular tests used a float `dot` as a bool.
- Verified: DXC cs_6_5 (no pack + hoyo_toon_v2) for CSPathTrace / CSRender / CSPrepassLevel. PT default path vs HEAD
  shaders: 80.8k px at 1 LSB, ~4.6k above (noise floor of two identical runs: 2.2k px) = codegen level. Alcander Ray PT
  and offline GI still (960x540, 256 spp): no `[E]`, skin matte, no fireflies. Not verified: PBR + PT pack render, Ray
  stage in PT / GI, GI video, EthanShax arm, the other sets.

## 2026-10-12 (7) - 2.0.0: PBR maps sampled too coarse (sampler MinLOD) + P5 per-character material source UI
- Fix: the scene root signature's s4 static sampler (`gPbrWrap`) passed the shader visibility as `MinLOD` (PIXEL = 5) to
  `CD3DX12_STATIC_SAMPLER_DESC::Init`, so every PBR map sampled mip 5 or coarser (2048 maps looked 64 px; P3's "the bake
  albedo is soft" was wrong). s1/s2 written the same way. DeepSeek narrowed it to the sampler before timing out; the
  architect fixed it. New `tools/pbr_tex_probe`. Default path vs before: <= 3 LSB.
- P5 (DeepSeek, level high, smoke-reviewed + render-checked): per-character `MaterialChoice` (Default / Ray-MMD + optional
  .emd), ini `characterMaterial=` / `characterMaterialSuggested=`, `.mmdxproj` "material", per-model PBR gate
  (`GpuModel::HasPbrRecords`, `RenderSettings::materialSource` removed), `--material-source` = run-wide override, shared
  `App::DrawMaterialSelector` in the lobby shader tab / play bar / studio inspector, studio reload via the replace path,
  play reload at the current time (`ReloadPlaySceneForMaterial`), one-time suggestion toast, `studiomaterial` script
  command, MCP `material_source`. Architect: section renamed "재질 데이터", quiet-caret combo, material-reload toast,
  duplicate i18n keys removed, suggestion text points at the right tab.
- Verified: saved choice renders like the CLI (3 LSB), lobby UI, studio switch, play-mode MCP switch keeps 0:20 paused.
  Not verified: the toast on screen, the file dialog, forced-.emd fallback, unit songs, dark theme / other languages.
  Now visible: metallic gold on Alcander renders dark (to check next).


## 2026-10-12 (8) - 2.0.0: dark PBR metals fixed, SMOOTHNESS type 2 decode, P6 glTF / FBX PBR maps
- Metals rendered nearly black in raster: `MmdShadePbr`'s env specular reflected the hemisphere fill x `hemiStrength` x
  (1 - rough) twice. Now the full `SkyColor` the path tracer reflects, along the GGX dominant direction, blurred toward
  the normal's sky with roughness, weighted by the analytic split-sum DFG (Karis). Alcander's gold matches PT; skin unchanged.
- `SMOOTHNESS_MAP_TYPE 2` (linear roughness) decoded as alpha = r; now r^2 (Ray: smoothness = 1 - r).
- P6 (DeepSeek, level high; resumed once after over-exploring): imported glTF/GLB/FBX PBR maps (metal-rough, normal,
  occlusion, emissive) fill the same `PbrMaterial` block (`PmxModel::importedPbr`, `BuildImportedPbrModelData`,
  `PbrTexKind::Embedded`), gated by the per-model material choice ("기본 / PBR" for imported models); VRM / MToon /
  unlit / default-only materials get no record. `tools/gltf_pbr_test`.
- Verified: tests pass; BoomBox studio prop default vs PBR renders right; Alcander raster vs PT gold. EthanShax's arm
  "gloss" is the texture's tattoo stripes. Found (pre-existing): coplanar alpha decals vanish in PT (`TraceClosest`).
  Not verified: glTF normal sign up close, imported-model selector UI, FBX PBR, glTF in PT / GI.

## 2026-10-12 (9) - 2.0.0 P7: QA, stage / prop material source, PT decals, GI skin fixes, release 2.0.0
- Delegation (opencode, deepseek-v4.1-flash, smoke review): PT decal fix (level high), website guide page + README /
  release-notes drafts (level high), stage / studio-prop material source (level default). Claude: renders, review, fixes,
  release.
- PT / offline GI / RT reflections: coplanar alpha decals (Alcander's chest tattoo) vanished; `TraceClosest` now runs a
  short `RAY_FLAG_CULL_OPAQUE` query around an opaque hit (same instance, later material wins). Architect fix: a fixed
  0.5 alpha test for the decal (the stochastic threshold gave speckles + a bright denoiser fringe). Cost ~+0.4 ms at 720p PT.
- Gap found: stages always loaded Default (`choiceFor("")`) and the studio drew the material row for characters only,
  so Ray stages / imported props could get PBR only through `--material-source`. Now: lobby "스테이지 재질 데이터"
  (keyed by the stage library id, same ini map), studio inspector row for every kind, projects save it for every kind.
  Architect fix (P5 bug): studio models added from the library / files never copied the load choice into
  `StudioModel::material` (PBR loaded, inspector + project said Default).
- Offline GI fixes found while making the release shots: the GI SKIN SSS used its own band (no curvature, x
  SunIrradiance/pi) = hard white / cyan streaks on Ray skin -> the raster / PT term; GI's texture-hue skin heuristic
  (SkinWeight / SkinTranslucency) ran on PBR hits and painted blocky patches (it thresholds the mip-filtered base texture)
  -> PBR hits light like raster / PT.
- QA (final build): 10 test tools pass (studio_project 179), pack_check --compile both built-in packs, render_smoke,
  VPD self test, pack_suggest scan 243 models / 0 failures; canonical raster capture vs a v1.10.0 build (b6fd591 worktree,
  same ini) = differences only in the FPS overlay; PT at noise level; raster benchmark 383 vs 386 fps avg; GI quick
  bench OK; 7 Ray characters raster / PT + the Ray stage raster / PT / GI with no `[E]`; pack + PBR (pack wins);
  material UI in 4 languages, light / dark, Ray row (.emd combo + file pick), stage row, studio stage; suggestion toast
  seen on screen (MCP run); packaged exe (fresh library, v2.0.0, no `[E]`); `update_e2e.py` all checks pass.
- Release 2.0.0: GitHub v2.0.0 (notes dist/notes_2.0.0.md, 4 languages, PBR off / on images docs/media/pbr-2.0.0.jpg +
  pbr-closeup-2.0.0.jpg (Sour Miku on the Ray stage; the first upload used a furry rig and was replaced within the hour); asset SHA-256 c6d76f75... = local zip), delta manifest 2.0.0 (13 new objects) uploaded, live
  manifest hash matches; website deployed (/latest.json -> 2.0.0 with manifest fields, /<lang>/docs/materials/ 200),
  web commit f10d0f4 pushed.
### Not verified / left
- Ray PBR albedo reads paler than the default skin in some sets (BeeNic +12 raster, +20 PT luminance): data-driven, not changed.
- Imported glTF stage / prop selector in the lobby / studio not clicked (BoomBox as a studio prop row only by code).
- Decals across instances (base single-sided, decal double-sided) are still not resolved in PT.
- `IsFlatFaceMaterial` (name-based flat faces) is still there (handoff section 9).

## 2026-10-12 (10) - Studio add popup: thumbnail card grid
- Delegation (opencode, deepseek-v4.1-flash, level full, smoke + visual review): the studio "+" popup's character /
  stage pages are a 3-column thumbnail card grid (`DrawStudioAddMenu`, 520 dp wide on those pages, 300 dp elsewhere;
  icon placeholder until the thumbnail is ready; only visible cards request thumbnails). Also used by the 2.0.0
  "replace model" mode (clicks pass `replaceUid`).
- Architect fix: `thumbs_.Pump()` ran only on menu screens, so studio thumbnails never loaded. It now also runs in the
  studio while the grid was drawn the previous frame (`studioThumbsShown_`; a thumbnail render waits for the GPU and
  borrows the scene targets, so it stays off otherwise).
- The work started on a stale checkout (68 commits behind, pre-2.0.0); stashed, pulled, conflicts in App.h /
  UiStudioProject.cpp resolved by hand. Note: the first launch after a pull with a cold `shader_cache` took 354 s in
  "shaders+upscalers" (looks like a hang; the second launch took 3.1 s).
- Verified (scripted captures, 2.0.0 dark theme): character grid with 6 thumbnails, stage grid (pre-merge), clicking a
  card adds the model (toast + outliner), no `[E]`.
### Not verified / left
- The replace-model popup with the grid was not opened (no script command opens it).
- `--ui-scale 1.5` not captured.
- Props classified as stages (GlowStick, kamKnife) get near-empty stage thumbnails (the fixed stage camera; same in the lobby).

## 2026-10-12 (11) - 2.0.0 release images: a Ray-MMD character, same-crop side by side
- The release preview showed PBR on the stage only (Sour Miku has no Ray data) as a split. Re-shot with a character that
  carries Ray-MMD material data and a modest look: MovieSonic Ver2.0 (29/30 records; Alcander Cloth / Ross rejected for
  exposure, Moritaka / Noku / DELGA / Zabivaka load with no records). Offline GI 2560x1440 768 spp, moonlit_waves,
  Evergreen 0:08 paused, sunset lighting, `--camera 0,9.0,0,0,3,58 --dof 0`, `--material-source default|ray`.
- `docs/media/pbr-sonic-2.0.0.jpg` = the same crop left (OFF) / right (ON); `pbr-sonic-closeup-2.0.0.jpg` = pairs
  (head, gloves, shoes, stage gold trim). Old files removed; the GitHub v2.0.0 notes point at the new names (raw 200).

## 2026-10-13 - Competitive plan P0..P2: drag and drop, lite zip, AutoLuminous, PMM import, physics bake, alpha export, auto-IK, accessory keys
- Workflow: Claude architect / final QA; opencode (deepseek-v4.1-flash, level high, smoke review) implemented; phase-gate
  QA by an antigravity read-only session after P0 and P1. Several workers stopped for over-exploring and were resumed
  with decision notes; three P2 sessions survived a server transport error detached, were left to finish (6 quiet
  minutes), exported and deleted, and their unfinished parts were completed by hand.
- P0: drag and drop into the Studio (`app/AppDrop.cpp`, ui-script `drop a|b|c`; a VMD/VPD dropped without a model goes
  onto the selected one); `-lite` release zip + optional components (`app/Components.h`, `updater::ComponentFetcher`,
  delta updater keeps a lite install lite, lobby download rows, `tools/components_e2e.py`).
- First-launch fix: FXC needed 332 s for mmd.hlsl PSMain since the 2.0.0 P3 PBR commit (eca748e; 12 s before PBR):
  the lit / floor PSOs compile with DXC ps_6_0 on SM 6.0 devices (FXC fallback). Cold first launch 360 s -> 20.5 s;
  raster output identical (2 px of 921k, PBR max diff 1/255). Slow compiles log `shader compile`.
- P1: `tools/media_kit.py` (classic / raster / RT / PT / GI A/B composites), `--classic` (read-only settings session),
  `--capture-scene 1` (scene-only PNG). Native AutoLuminous (`MatFlag_AutoLuminous`, `AutoLumEmission`, gain 4 so 110
  blooms; `autoLuminous=` / `--auto-luminous`; lobby + studio toggles; raster / RT / PT / GI). SSAO no longer greys
  translucent pixels (alpha < 0.98 writes nt.z = -1): the sheer curtain on `terrace` was grey-streaked in raster.
- P2: accessory key track (`AccessoryKf`, `RowKind::Accessory`, opacity via material multipliers, `studioacckey`,
  STUDIOACC; fix: a model's VMD load wiped its accessory keys on project load); transparent PNG-sequence export
  (`--offline-alpha 1`, dialog switch; fixes: the readback dither removal was in 0..1 units -> black RGB, PT still
  traced the default floor); auto-IK (`studio/AutoIk`, two-bone analytic, twist bones skipped, hand keeps its world
  orientation, toolbar magnet / K, `studioautoik`); PMM import (`asset/PmmLoader`, `tools/pmm_probe` + fuzz, MME
  controller table, open / recent / drop / `--project x.pmm`); physics bake (`studio/PhysicsBake`, "물리 굽기…",
  per-model physics off saved as "physics": false, `studiobake`, `tools/bake_test` max error 0.0001).
- Final QA (build/): studio_project 182/0, studio_edit 15/0, studio_pose 49/0, studio_gizmo 19/0, studio_focus 31/0,
  studio_light 106/0, bake_test PASS, pmm_probe fuzz 686 variants no crash, vmd_roundtrip VPD OK, components_e2e and
  update_e2e ALL CHECKS PASSED; captures in captures/qa_final (lobby dark/light + detail tab, video dialog alpha switch,
  studio PMM open, drop + bake, RT / PT alpha). README (4 languages): new feature bullets, comparison image
  `docs/media/compare-classic.jpg`, "UI is in Korean" caption and PMM lines updated.
### Not verified / left
- Real-time PT alpha export has hard edges (no partial alpha); RT shows speckle noise on skin; real-time PT stays grainy
  on alpha-blended materials (curtains, leaves).
- PMM: the only real file is camera-only; models / accessories / MME were verified on a synthetic fixture. PMM 0001 is
  declined. Material morphs do not re-evaluate the AutoLuminous flag.
- The lite zip's component download needs the installed version's manifest on the update server (first lite release).

## 2026-10-14 - Play fixes: 60 fps VMDs, audio after song end, performer picker, .emd additive materials
Investigation split over opencode/deepseek (issues 1-2) and antigravity/gemini (3-4), implementation by deepseek workers,
review + E2E by Claude (commit 89fc858). The MME session (docs/mme, src/asset/Mme*) worked in the same tree; coordinated
over cross-session messages, files kept disjoint.
- 60 fps VMDs: `library/MLTD/Amaterasu_v1.1` (MillionDance output) keys every frame, 7680 frames = 128 s at 60 fps vs
  125.8 s audio; VMD has no fps field, so it played at half speed. Per-song `SongAsset::motionFps` (file name contains
  "60fps"; library cache v4) -> `ScenePackage` / `SceneRuntime::motionFps`, used by every play-mode seconds<->frame site
  (studio stays 30). Also Cherry Colored Love and relations. Play bar 2:02 / 2:08 (was 4:16).
- Audio after the song ended: miniaudio's at-end flag drops seek targets until `ma_sound_start` (which rewinds to 0).
  `AudioPlayer::Seek` records the target when at end, `Play` restarts there; play-mode seek sites restart audio while
  playing. MCP `get_state`: duration uses the time base, new `audio_playing` / `audio_time`. Verified both cases (seek +
  play after the end -> 102.07 s in sync; seek back during the motion tail -> audio resumes).
- Multi-performer play: "캐릭터" combo (`##playperformer`) at the top of the size / shader popups; scale, shader pack,
  per-material list and material source follow the selected performer (`App::PlayPerformerAt`); an extra's material
  source applies on the next load (only the center reloads live); `SuggestRayMaterial` for extras. Verified: a pack set
  on the non-center performer changed only her.
- Alcander fire tail (`AlcanderMMD/Alcander 180cm.pmx` materials 44-51): the model's own .emd assigns FireFog.fx
  (ZWriteEnable = false) and TailLight / FireLight.fx (full_ex additive); drawn as normal alpha + depth write they were a
  dark smoke shell over the tail. `MmeHints::materialBlend` (0 normal / 1 no-depth-write / 2 additive, from Obj[n]) ->
  `GpuModel::SetMaterialBlendHints` -> `MatFlag_Additive` (1<<10) / `MatFlag_NoDepthWrite` (1<<11): ScenePass PSO
  variants (no depth write, MRT1/2 masked, no edge / shadow cast / pack), excluded from BLAS. Raster and RT + Ray-MMD
  verified (captures/issue_tail). Ordinary model A/B vs the pre-change build = run-to-run noise (130 px, 1 level).
  The MME runtime will own such materials later (docs/mme/mme_design.md 2.12).
### Not verified / left
- Flagged .emd effect materials are invisible in PT / offline GI (out of BLAS); FireFog's soft depth fade and FireLight's
  background darkening are not reproduced (the MME runtime's job).
- 60 fps follow-up (deferred by the user). Research: no tool stores fps in a VMD; MMD / nanoem "再生時フレームレート 60"
  only interpolates a 30 frames/s timeline; MillionDance defaults to 30 and says 60 fps output needs player setup, so
  stock MMD also plays these at half speed. Plan: (1) per-VMD fps instead of per-song (a 30 fps camera with a 60 fps
  dance), (2) content fallback (keys on ~every frame + length ~2x audio), (3) per-song override auto / 30 / 60 in the
  library card menu (`library_overrides.json`, `mmdx.json`), (4) studio import of a 60 fps VMD resamples to the 30 fps
  timeline with a toast (saves an ordinary 30 fps VMD).

## 2026-10-14 (2) - MME (MikuMikuEffect) compatibility: own parser/runtime on DX12, effect library, Dreamin Chuchu MV
Design / checklist: `docs/mme/mme_design.md` (research in `docs/mme/research/`). Milestones M1..M8 implemented by
opencode/deepseek workers from Claude's specs; Claude reviewed, did the E2E / visual QA and surgical fixes.
- Pipeline: own .fx parser + preprocessor + IR (`asset/MmeEffect`, `MmePreprocessor`, `MmeAssign` for .emd / same-name
  .fx / `name[fx].pmx`) -> FXC SM5 wrappers (`render/MmeCompiler`, object mode = host MRT: linear colour x sun, octa
  normal with nt.z = -1, velocity) -> reflection -> `render/MmeRuntime` (instances per (file, objectMode), persistent
  SRV tables, sampler heap, constant ring, root sig with the host cbuffer at b13), `MmeScriptVM` (Script annotations,
  loops, RenderColorTarget), `MmeSemantics` (MMD semantics + CONTROLOBJECT; missing object = bool false, items 0).
- Passes: object effects in the scene pass, MMD self-shadow (`MmeShadowPass`, ortho light camera fitted to the posed
  bones, zplot with the plain instance), offscreen render targets with DefaultEffect lists + `shaders/mme/standard.fx`
  fallback + GenerateMips (`PassMmeOffscreen`), post chain (`PassMme`); PT / offline GI draw an MME layer
  (`DrawMmeModelToMrt`, waits for readiness). Object techniques that switch RenderColorTarget run through the VM with
  the plain instance. DDS loader, animated textures, video layer (`asset/VideoReader`, `PassVideo`: background where
  depth is far, black-keyed overlay).
- Product: per-model MME choice None / Auto / File (characters None, stages Auto; ini `characterMme=` / `stageMme=`,
  project `"mme"`, CLI `--mme-auto`, `--stage-mme`, `--mme`, `--mme-model`, `--mme-object`), MME post stack (ini
  `mmeEffect=`, lobby / play bar "효과 파일" row, shader manager MME tab with diagnostics, studio project
  `"mmeEffects"`), effect objects (`.x` accessory + same-stem `.fx`: studio "MME 효과", drag and drop, project kind
  `"effect"`), hot reload, `effects/` library folder. Song packages (M8): bundled stage + its motion (bone-match score),
  audio versions (`songAudio=`), `bg.avi` / overlay videos (`songVideo=`), MME TIME from the scene clock.
- Ray-MMD vs the native PBR: MME Auto skips `IsRayMaterialFx` assignments (native PBR reads them); adding a Ray-MMD
  main effect (`IsRayMmdMainEffect`: includes ray.conf / ray_controller.pmx) to a stack or as an effect object opens a
  confirm modal (`App::GuardRayMme` / `DrawRayMmeConfirm`); scripts / CLI add it with a `[W]`.
- Library / harness: 58 popular effects in `captures/mme_lib` (git-ignored downloads), `tools/mme/mme_compat_run.py`
  (static + render, tiers, contact sheet, `docs/mme/compat_list.md`, final tiers in `docs/mme/compat_overrides.json`):
  57 run, auto A 42 / B 8 / C 6; final notes per effect.
- Fixes in the QA round: RTV / DSV heaps 64/192 -> 1024/512 (AutoLuminous's targets exhausted it: white frame, then a
  crash; `Texture::Create` now fails cleanly), offscreen sub-effects still compiling draw nothing (the standard
  fallback painted whole models into AL's emitter), offline edge pass always restores its state after the MME layer
  (crash with effect objects), effect-object meshes out of the TLAS / offline depth pass, a post effect's controller
  `.x` hidden, host-MRT alpha = the effect's alpha (particles were opaque), studio drops of effects alone were dropped
  before the effects loop (pre-existing).
- Verified: raster / RT / PT / offline GI object effects, self-shadow, SvSSAO / AL4 / Diffusion / WorldSnow /
  BillboardSet etc., Dreamin Chuchu with 5 performers in play mode and as an offline video (frame-identical to play at
  78 s), Ray .emd test model goes to the native PBR in raster / RT / PT, tests (mme_parse 20/20, mme_compile 15/15,
  mme_script, dds 15/15, anim_texture, raymat, studio_project 194) pass.
### Not verified / left
- C tier: HgDOF (no change), TrueCameraLX (0 programs), HAToon2 (FXC X4555), SimpleToon (PSO E_INVALIDARG), MMDShadow
  via its .x (0 programs), NCHLShader2 (needs its controller). B: WorkingFloorAL reflection darker than MMD, WorkingRay
  plane too high, RealFigureShader blocky hair shadow, Hg lights unverified without a placed light.
- The compat harness's 60-frame capture can race a cold shader compile (re-run with a warm cache).

## 2026-10-15 - MME / Ray-MMD global switch, Auto / Manual workflow, play-mode "화면 효과" side panel
Implemented by opencode/deepseek workers from Claude's plans (3 runs; two resumed after over-exploring / memory-pressure
kills), reviewed and QA'd by Claude over MCP user paths (antigravity QA run stopped by the user; Claude took it over).
- Global switch `AppSettings::mmeEnabled` (ini `mmeEnabled=`, `--mme-global 0|1`): off = no MME object effects, post
  stack, effect objects or Ray-MMD materials anywhere; saved per-model choices stay. Workflow `mmeWorkflow=manual|auto`
  (`--mme-workflow`): Manual = the per-model None / Auto / File + material source as before; 완전 자동 = every model gets
  its own MME effects and Ray material when it has a Ray .emd candidate, no suggestion toasts. All consumers go through
  `ResolveMmeChoice*` / `ResolveMaterialChoice` (material resolver now takes the model path; candidate cache mutexed).
- 완전 자동 safety: `MmeRequiredControllers` (asset/MmeAssign) lists non-bool CONTROLOBJECT reads of other objects; a
  model whose auto effects need a controller missing from the scene (ps_luka's `Rim_Luka.pmx`: dark silhouette) falls
  back to native (`App::MmeControllerSkip`, cached by scene-name key), captioned "자동 제외 · 컨트롤러 필요".
- UI: top nav tab "MME" (`Screen::Mme`, app/UiMme.cpp, `--screen mme`): switch, workflow cards, MME post stack,
  searchable per-model list. Play mode: one right-edge sliding panel (app/UiPlayPanel.cpp, `###fxpanel`) with tabs
  효과 (effect stack + DoF / volumetric / bloom / conv bloom / auto-luminous / LUT / exposure, shared with the lobby via
  `DrawScreenEffectControls`) / 셰이더 (former popup) / MME · Ray-MMD (draft, 되돌리기 / 적용, "적용 (씬 다시 로드)" when
  a material source changes; reload keeps time, play state and the formation; the panel reopens after it). Ini
  `playPanelTab=`. Script commands `mmeglobal`, `mmeworkflow`, `mmestate` (MMESTATE line), `fxpanel`.
- Fixes found on the way: black wedge / fully black frame with MME on in unit songs - after an MME object / edge draw
  the raster scene pass restored the root signature but not root CBV 0 (scene constants), viewport, scissor, topology
  (Passes.cpp; PT already did); MME host MRT outputs sanitized (NaN / Inf -> 0, normal guard, MmeCompiler); formation
  reload read the freed scene's cast; play header / MCP `character` showed the lobby character for formations; app bar
  overlapped at 1280 x 1.5 (path pill shrinks to an icon, MCP chip hides); dropdown popups padded (app WindowPadding 0);
  MME / performer combos registered with uiprobe; play bar width for the extra button.
### Not verified / left
- 완전 자동 with ps_luka inside a unit song, the studio inspector MME rows in Auto / off, persistence across a restart.
- Lobby at 1280x720 x 1.5 (~853x480 logical): the play button overlaps the stage dropdown (pre-existing layout).
- antigravity-launched app windows are invisible (its runner passes SW_HIDE; the first ShowWindow obeys it).

## 2026-10-15 (2) - Separate Ray-MMD master switch; MME 완전 자동 per-frame filesystem stall removed
Diagnosed by Claude (frame-segment CPU timers + an ad-hoc suspend/StackWalk sampling profiler, no admin needed),
implemented by an opencode/deepseek worker from Claude's contract spec, reviewed / measured / E2E'd by Claude.
- Ray-MMD switch split from the MME switch: `AppSettings::rayMmdEnabled` (ini `rayMmdEnabled=`, `--ray-global 0|1`,
  `App::RayMmdGlobalEnabled()`) now alone decides whether material sources resolve to Default
  (`ResolveMaterialChoice*`, `EffectiveMaterialChoice`, Ray suggestion toast, studio material row). `mmeEnabled` covers
  MME object / post / effect objects only. The workflow stays shared. UI: second switch on the MME screen and in the
  play panel draft (`MmeDraft::rayEnabled`, reload detection), MME / material parts of each model row disabled by their
  own switch. Script `rayglobal 0|1`, MMESTATE `ray=`.
- Perf root cause (5 performers, 완전 자동: ~23 fps, CPU 42 ms vs GPU 20 ms): `mmetext::PathKey` ran
  `weakly_canonical` (CreateFile / GetFinalPathNameByHandle) per material per draw via `MmeRuntime::Instance` (37 % of
  main-thread samples), and `App::ApplyMmeEffect` Auto re-read `.emd` / stat'ed `.fx` every frame per model (~15 %).
  Fix: PathKey memoized (mutex, 8192-entry cap); Auto resolution cached per (model path, material count) for 1 s.
  Result: ~46 fps, render CPU 17.6 -> 2.1 ms, UpdateScene 24 -> 10 ms, GPU-bound like the MME-off path; max frame
  25-28 ms (no 1 Hz TTL spikes; MME-off run max 27-35 ms).
- Verified: MME off + Ray on keeps Alcander's Ray PBR (ray1); MME on + Ray off = all ray0; auto + Ray off = MME
  objects applied with Default materials; play panel capture.
### Not verified / left
- Studio inspector Ray toggle interaction, `rayMmdEnabled=` persistence across a restart, slow-disk TTL spikes.
- Remaining main-thread cost is Bullet physics (~10 ms for 5 performers, same with MME off); 60 fps at this size is
  GPU-limited (~20-25 ms) on both paths.

## 2026-10-15 (3) - Release 2.1.0: i18n gap fix, MME showcase images (offline GI 4K), guide refresh, deploy
- i18n check: tokenised every Korean string literal in `src/` against the en / ja / zh tables (ad-hoc script; tables must
  stay in key parity): 46 UI strings were missing (MME screen, play panel, song stage, components label), added to all
  three tables; `UiUpdate` now runs the component label through `Tr`. Left on purpose: wide-string file-dialog filters,
  default light names, library classifier keywords. Note: the last entry of `I18nJa/Zh` had no trailing comma (build break
  until fixed). Heads-up for tooling here: bash heredocs / `sed` ate backslashes in regex literals (use `\x5c` / chr(92)).
- Release 2.1.0: version 2.1.0, `tools/package_release.ps1` (full 170 MB + lite 15 MB zips), GitHub v2.1.0 (notes
  `dist/notes_2.1.0.md`, 4 languages; asset digests = local zips), delta manifest 2.1.0 (104 new objects, uploaded; sha
  2f8460a8...), website deployed (`/latest.json` -> 2.1.0, `/<lang>/docs/mme/` 200 in all languages).
- Marketing images are now offline GI 4K (user request): `docs/media/mme-2.1.0.jpg` (MME off / on) and
  `mme-closeup-2.1.0.jpg` (3 close-ups) from Sour Miku / theater / catch_the_wave @ 62 s, effects Diffusion7 + o_LikeHDR
  (`--mme a.fx,b.fx` takes a comma list; repeated `--mme` keeps only the last). README x4 got an "MME effects" section.
- Guide (MMDX12_Web): new `mme` page + updates to playback / shaders / quality / library / install / studio / studio-pose /
  studio-project / video / materials in 4 languages (antigravity workers from contract specs, smoke review: popup wording
  grep clean, build OK); screenshots re-shot from a packaged copy at `F:\guide_app\MMDX12` (library: Sour Miku White,
  Theater, Catch the Wave; `MMDX12_REPO=F:\repos\MMDX12`): new `mme_screen`, `play_mme` (= `mme_play_panel`), `play_light`,
  `quality_upscaler`, `about`, `studio_toolbar`, `studio_model_inspector`, `studio_model_menu`, `mme_compare`.
### Not verified / left
- The studio pose screenshots (`studio_bone_*`, `studio_pose_edit`, `studio_key_*`, `studio_curve`) were NOT re-shot: the
  scripted `studiobone` click no longer selects a bone (STUDIOSTATE `hoveredWindow=-`, selection stays empty) and the
  timeline starts with the morph group expanded; the old 2.0.0 images are kept. Needs a look (possible input regression).
- `mme_effect_object` capture (drop of a `.x` via `studio_command drop`) added nothing to the outliner; the mme page uses
  the inspector shot instead. Studio PMM / bake / auto-IK / alpha export have no dedicated screenshots beyond the menus.
- Release gate tests (update_e2e etc.) were not re-run for 2.1.0; the packaged exe ran the whole guide capture without a crash.

## 2026-10-15 (4) - Studio UX: ColorField (labelled channels, picker with RGB / HSV / OKLab / hex)
- Problem found: every Studio colour row (light, rim, ambient zenith/horizon/ground, shadow, camera-VMD light key) was an
  unlabeled `DragFloat3` plus a tiny stock `ColorEdit3` swatch: no R/G/B names, no HSV / OKLab / hex.
- New shared widget `ui::ColorField(id, rgb[3], activated*, deactivated*, hdr)` (`UiKit.{h,cpp}`): swatch + three drag
  fields with channel letters (R G B / H S V / L a b by mode), swatch popup with RGB | HSV | OKLab tabs, hex input, old /
  new preview. Replaces the DragFloat3 + ColorEdit3 pairs in `UiStudioLight.cpp` and the VMD light key colour in
  `UiStudioCamera.cpp` (0..255 storage kept). i18n: "원래 색" / "새 색" in en / ja / zh.
- Built by a GLM opencode worker (first run stopped OVER-EXPLORING in imgui's gesture internals; Claude decided the
  gesture API in a note and resumed the same session). Review depth: smoke.
### Not verified / left
- Seen in the real app: the light row field and the RGB tab popup (`captures/studio/colorfield_a.png` / `_b.png`,
  script `captures/studio/colorfield.txt`). NOT seen: HSV / OKLab tabs, hex input, the camera-VMD light row; keyed-edit
  begin / end behaviour (undo steps, auto-key) after the change was not exercised; worker diff not read line by line.

## 2026-10-15 (5) - Library scan: parallel + cached .fx pass; props / effect objects / dummies no longer stages
- `.fx` pass (section 8b of `ScanLibrary`): `LoadMmeEffect` ran sequentially with no cache (16k files under
  `effects/mme_lib/_bowlroll`, ~59 s). Now `std::execution::par` + `cache/fx_probes.json` (size + mtime of the .fx AND of
  every include it resolved; `kFxCacheVersion`). Measured: scan 60.0 s -> 8.2 s cold / 2.8 s cached (fx 1.6 s), effect list
  identical cold vs cached (16283: object 16107, post 173, pre 3). The phases log line gained `fx=`.
- Stage classification (automatic only; sidecars / overrides untouched): `.x` with a same-stem `.fx` or under `effects/` =
  MME effect object, skipped; auto entries < 100 vertices skipped (camera / controller / shader dummies); prop-named
  folders / stems (prop, accessory, 小物, 소품...) skipped; inside a song folder (directly holds a VMD keying センター /
  上半身 / 下半身, not the library root) several sub-folder stage candidates keep the stage-named ones, else the largest;
  `bundledStageOf` prefers stage-named, then the largest (was: the first match, so fire_flower got GlowStick, Supernova got
  Confetti). The library root never gets a bundled stage. Real library: 55 -> 16 stages.
- Both done by deepseek opencode workers from contract specs, smoke review (Claude fixed the root bundled-stage case).
### Not verified / left
- Pitfall: `--screen select --frames 30` quits before a background scan finishes (cancelled = nothing saved); use
  `--frames 30000` to time a scan in the app.
- `by StarNoodle495/.../kamKnife 80cm` (a weapon outside a character folder) is still a stage (hide it by right click).
- The 16k MME objects still flood the effect list (separate UI / dump-filter problem). Stage list not checked in the app UI.

## 2026-10-15 (6) - GPU device-removed fixes (MME cache ABA, MME shadow map on resize), device-loss detection
- Report: after GPU 업로드 the screen froze (audio kept playing) on the 2nd Ray-MMD toggle reload from the play panel,
  sometimes when switching songs. Log: first error `ResizeBuffers failed: 0x887A0005` then endless
  `Texture::Create failed`; System log `nvlddmkm` 153 (TDR). The app ignored `Present`'s HRESULT, so a removed device
  looked like a frozen window.
- Cause A (the reported one): MME caches keyed by `const GpuModel*` (`MmeEffectInstance::srvCache_` + material SRV table
  index, `MmeSkin::entries_`); the menu / loading branch of `Renderer::Render` cleared `frameModels_` without
  `ForgetModel`, so entries outlived their models. Reload D (Ray on) -> E (off, other descriptor layout) -> F (on, same
  layout as D) hit D's stale table = freed textures. Fix: `GpuModel::Uid()` keys, forget + drain in the menu branch,
  MmeSkin resizes a too-small buffer.
- Cause B (found while reproducing; user maximized the window): `MmeShadowPass` released its fixed 2048 map on every
  target resize and re-registered the same `&im.map`, so cached tables kept the freed map. Fix: generation per creation
  (`SetObjectShadowMap(tex, gen)`), no release in `OnResize`.
- Device loss: DRED on, `Dx12Context::DeviceLost()`, one log dump (reason, breadcrumbs, page-fault allocations), message
  box + quit (none in unattended runs). Fixes the ImGui upload-buffer null crash after a loss too.
- Auto workflow controller skip: `MES40 Controller.pmx/.pmd` (MES40 guards reads with `if (Controller)`) and controllers
  read only through `AMBIENT_R/G/B/INT` (anonRipper / Chips Project Sekai stage shader, missing = lerp weight 0) are no
  longer "required" (Dreamin Chuchu, fire_flower, liar_dancer, pjsk_fireflower, empty_sekai). `Rim_Luka.pmx` (ps_luka also
  declares a bool existence check, so that is NOT a safe rule) and MES40 spot / point lights stay required.
- Implemented by a deepseek opencode worker (effort low) from a contract spec, smoke review. The pre-existing
  controller auto-load work was committed separately first.
### Verified (1600x900 window)
- Ray toggle + 적용 reload x4 (off/on/off/on): every scene entered, 0 `[E]`. Maximize / restore x3 in an MME shadow-map
  scene: 0 errors, no nvlddmkm event. Dreamin Chuchu: stage MME applied, no controller skip, frame looks right.
### Not verified / left
- The device-loss message box / DRED dump were not triggered for real.
- MCP `ui_click` / `ui_input` clicks on the play bar's panel buttons (`##mmepanel`) do not open the panel (the E2E used
  `--ui-script` `fxpanel open mme`); not investigated.
- `mmdx12.previous.log` is not written (CLAUDE.md says it is); a repro run overwrote the user's log.
- A model's material SRV table rewritten in place (texture replaced while loaded) would still leave copied MME tables
  stale; not addressed.

## 2026-10-15 (5) - GPU hang fix (MME animated textures), release 2.1.1
- Report: "GPU device removed" warning in play (5 Alcander performers, Dreamin Chuchu song stage with MME monitor
  effects, video layers, bokeh_dof, DLSS). Log: `removed reason=0x887A0006` (DEVICE_HUNG), DRED breadcrumb op 8
  (COPYTEXTUREREGION) unfinished right after the first frame; page-fault freed allocs `mme.target` / `dof.blurA` were
  the normal deferred releases (ruled out).
- Root cause: `MmeEffectInstance::UpdateAnimatedTextures` (ANIMATEDTEXTURE, used by the stage's `StageShader.fxsub`)
  copied from a staging buffer with a tight `width * 4` RowPitch; D3D12 needs 256-byte alignment. Now a 256-aligned
  layout with per-row copies (as PassVideo). Every other CopyTextureRegion audited: GetCopyableFootprints / StagingPitch.
  Implementation + audit delegated (opencode, deepseek-v4.1-flash, full review).
- Verified: same scene (`--formation` 5 Alcanders, `--song Dreamin`, 1600x900), 900 frames, 0 `[E]`, no device loss.
  The hang was not re-run on the old exe (evidence = the crash log).
- Also committed (pending from an earlier session): parallel posing of unit performers, interactive Bullet substep cap 4,
  camera VMD forward-axis view, VideoReader last-sample reuse.
- Release 2.1.1: `package_release.ps1` (full 170.5 MB, lite 14.9 MB), packaged exe smoke (select screen, 0 `[E]`;
  smoke leftovers cache/ini/log removed from the stage dir before `make_update.py`), delta manifest 2.1.1 = 3 new
  objects (exe, mmdx12_mcp, pack_check; 3.7 MB gzip), uploaded, live hashes match; GitHub v2.1.1 (notes
  `dist/notes_2.1.1.md`, 4 languages; digests = local zips); website deployed (`/latest.json` -> 2.1.1 with manifest).
- Pitfalls: the in-app update summary shows only the bold headings of the notes, so headings must read as full
  sentences; a list built by `python -c` in Git Bash carries `\r` (pipe through `tr -d '\r'`); the first redeploy
  after `gh release edit` can still fetch the old body (redeploy once more and check `/latest.json`).

## 2026-10-16 - Device-loss hotfix (deferred releases freed mid-frame), release 2.1.2
- Report: 2.1.1 still lost the device (DEVICE_HUNG, DRED op 8 COPYTEXTUREREGION, page fault on freed allocs) on the
  Dreamin Chuchu 5-Alcander scene, on the first frame of a reload after the render size / MSAA had changed.
- Root cause: `Dx12Context::WaitForGpu` freed every `DeferRelease`d resource, including ones released while the
  current frame was being recorded (not yet submitted). Passes call WaitForGpu mid-recording
  (`PackEffectPass::AllocTargets` on an output-size change, PSO / MME / upscaler recreation), so the MME
  ANIMATEDTEXTURE staging buffer recorded earlier in the frame was freed before the copy ran. The 2.1.1 "fix"
  (256-byte pitch) was real but not this; its log's freed `mme.target` / `dof.blurA` were this bug, not noise.
- Fix: resources deferred while recording are tagged pending and get the frame's fence at EndFrame; WaitForGpu
  frees only submitted ones. Written by Claude (lifetime logic), not delegated.
- Verified: repro script (scratch `resize_repro.py`: the scene + bokeh_dof/chromatic_aberration, window resized over
  MCP-paced frames) lost the device on the 2.1.1 release exe after 8 target reallocations; 2.1.2: 60 resizes / 122
  reallocations, 0 `[E]`. A plain scene reload (no size change) did not reproduce on 2.1.1 (targets not reallocated).
- Release 2.1.2: hotfix only (the spec A worker changes - VMD visibility keys, per-part stage motions - were stashed
  and are not in it). `package_release.ps1` (170.5 / 14.9 MB), packaged smoke (select screen, 0 `[E]`, leftovers
  removed), delta manifest 3 new objects uploaded (live hashes match), GitHub v2.1.2 (digests = local zips), website
  deployed (`/latest.json` -> 2.1.2 with manifest).

## 2026-10-16 (2) - Lobby video render range (HH:MM:SS)
- Request: the high-quality video dialog lets the user pick the render range; default whole song, custom as timecodes.
- Dialog (`UiVideoDialog.cpp`): new first section "범위" (전체 / 구간 지정), start / end text fields (`##vrange_start`,
  `##vrange_end`, uiprobe ids), "전체로" reset, invalid input = warn line + disabled start button. Footer, frame count,
  estimate and file size use the range length. State is `App::videoRangeCustom_/videoRangeSec_/videoRangeText_`, reset when
  the dialog opens or the song changes; handed to the load as `lobbyVideoRange_` (an explicit `--offline-range` still wins).
- `core/Timecode.h` (header-only): `FormatTimecode`, `ParseTimecode` (HH:MM:SS, MM:SS, SS, decimals). `--offline-range` and
  MCP `render_video` `start` / `end` accept seconds or timecodes (answer carries `range_start` / `range_end`);
  `--ui-script` `videorange <all|start end>`; i18n En / Ja / Zh; docs/mcp.md + CLAUDE.md updated.
- Implemented by an opencode worker (deepseek, low): the first run over-explored (71 reads, no edit) and was resumed with a
  decision note. Review fix (Claude): the start button read a range only refreshed on commit, now synced every frame; `~`
  separator and spacing tuned from a capture.
- Verified (1600x900, `build_a`): dialog capture with `videorange 0:12 1:05` (3180 frames = 53 s x 60 fps in the footer);
  CLI `--offline-range 00:00:10 0:12` lobby render = exactly 60 frames at 30 fps; 0 `[E]`.
- Not verified: Start from the dialog with a custom range (code-read only), MCP string `start` / `end`.
- Committed together with earlier pending work in the tree (VMD / PMM / studio / `ScreenVideo` changes from other sessions);
  those were not re-tested here.

## 2026-10-17 - Issue #4 revert, HDD library scan
- Issue #4 (ZZZ clothes stiff; claimed the PMX rigid-body mask is a "non-collision" mask): an earlier uncommitted
  change inverted the mask in `Physics.cpp` and broke physics everywhere. Reverted. The stored value is the
  collide-with mask (bit set = collide, like PMD; saba / MMDAgent / mmd_tools read it as is): over the 142 library
  PMX files 88% of bodies clear their own group's bit (chains don't self-collide), which the inversion turned into
  self-colliding chains. `PmxModel.h` comment corrected (it said "non-collision"). The ZZZ stiffness is still open
  (needs the model; joints / solver suspected, not checked).
- HDD library scan: two costs in `ScanLibrary` - 2-4 path stat calls per file, and `std::execution::par` probes
  reading many whole files at once (seek thrash). Now: size / mtime from the walk's `directory_entry` (same values,
  caches stay valid); `core/StorageInfo` `PathIncursSeekPenalty` (IOCTL seek-penalty query, failure = false); on such
  a volume `SequentialPrefetch` reads the to-probe files ahead in path order on one thread while the probes still
  parse in parallel behind it (MP3 read ahead, WAV/FLAC/OGG header-only probes not). `MMDX12_SCAN_IO=seq|par`; log line
  `library scan: storage ...`.
- Implemented by an opencode worker (deepseek-v4.1-flash, low) from a contract spec; its plain sequential mode was 4x
  slower when the files were in the OS cache (3.9 s vs 1.0 s), so Claude replaced it with the prefetch design and
  restored the size-error guards (dedup / part grouping / audio size).
- Verified (`asset_probe`, build_scan): SSD (C: NVMe copy) cold-probe 1.07-1.12 -> 1.02-1.04 s, cached 0.155 -> 0.084 s;
  HDD (E:) OS-warm cold-probe 0.96 -> 1.04-1.08 s, cached 0.083 -> 0.078 s; results identical to HEAD in par and seq
  modes. A true cold HDD scan was not measured (purging the standby list needs elevation); a cold-read test on unread
  game files gave path-ordered 125-132 MB/s vs parallel 84-90 MB/s (~1.45x). The app itself was not launched.

## 2026-10-17 (2) - Mode 2 physics chain fix (issue #4 solved), release 2.1.3
- Root cause of ZZZ clothes stiffness (issue #4): mode 2 (物理+位置合わせ / physics + align position) bodies in `Physics.cpp`
  previously set the bone's world matrix translation directly from the pre-physics animated pose (`r.world._41 = anim._41...`).
  In chains of mode 2 bodies (e.g. Star Rail / ZZZ coats, sleeves, hair), each child bone kept its pre-physics animated world
  translation while rotating, which kept the chain pinned in space and produced stiff, broken physics.
- Fix: `ModelInstance::RunPhysics` now rebuilds mode 2 bones' world translation through the bone hierarchy: parent's final
  post-physics world matrix combined with the child's animated local offset (or animated world translation if root). `PhysicsWorld::Results`
  flags mode 2 bones in `BoneResult::mode2`.
- `tools/anim_probe.cpp`: added `PHYS_SWING=<sec>` probe to measure dynamic body local rotation speed, travel distance and range.
- Release cover: `docs/media/physics-2.1.3.jpg` (ZZZ Lycaon performers on stage, flowing coat physics).
- Version 2.1.3 bumped (patch bump: mode 2 physics fix, HDD scan prefetch, video render time range).


## 2026-10-18 - Offline video PT: passes now converge (radiance / outline accumulation), README Sour Miku
- Question: do extra passes of the real-time-PT offline video actually improve a frame? Static read first (seeds and jitter do
  change per pass), then 4K measurements (Sour Miku White, terrace, seed-pair PSNR on static regions). Finding: in
  alpha-flipping regions (hair over skin, glass) and on silhouettes the result did NOT improve with passes. Cause: only the
  demodulated light was accumulated; the albedo (and the outline layer) of the LAST pass was used, so stochastic-transparency pixels
  stayed a binary dither and edges stair-stepped (video renders TAA off).
- Fix (`RenderSettings::ptAccumAlbedo`, offline PT video only): `CSTemporalAlbedo` (pt_denoise.hlsl) accumulates R = light x albedo
  with the light's weights; the remodulation uses `R_acc / L_acc` (averaging the albedo itself shifted the glass to purple: biased).
  Later passes of a video frame accumulate in place (`FrameView::ptStaticPass`, no depth / normal rejection; sky and silhouettes
  included); the outline layer is averaged per frame (`pt_edge.hlsl`, `edgeAcc_`, `ptPassIndex`). Targets are created on first use.
- Also: configurable history limits per video quality (`VideoRealtimeQuality::historyMax/MinAlpha/Carry`, `ptHistoryCarry` clamps the
  history length across a video frame boundary), 최고 warm-up 32 -> 24 passes, 표준 = 4 samples x 4 passes (same sample count).
  First delegated to an opencode worker (deepseek-v4.1-flash, low) from a contract spec; Claude reviewed (no fixes needed), then did
  the accumulation redesign itself.
- Measured (seed pair, 4K): skin under the bangs 24.9 -> 35.9 dB (1 -> 64 passes, was flat ~24), hair 22.0 -> 33.0, glass 18.2 -> 27.5,
  floor 46.3 at 24 passes (was 45.9). Old accumulation capped at ~24 effective passes; the new limits only help beyond 32.
  1080p outlines are continuous lines instead of dashes (and look lighter: they are anti-aliased now).
- Mistakes caught on the way (kept in CLAUDE.md / memory): scratch builds copied the user's ini (eye_adaptation, volumetric, DoF:
  1024-pass reference 3.5 levels darker, grey media shots); `--offline-quality 3` pins the render quality too (all sweeps ran at the
  최고 preset's 4 spp / 6 bounces); the terrace stage animates run to run (vines, petals), so only the wood floor / glass / skin
  regions were usable; a reference-based PSNR hid the plateau.
- README: `tools/media_kit.py` default scene still pinned `01_9001` (Sekai); now `sour_miku`. Re-shot `docs/media/compare-classic.jpg`
  (Sour Miku Black, terrace) and `ui-library.jpg` (search `sour_miku`). The other README images were checked: all Sour family.
  `tools/pack_previews.py` keeps `01_9001` for the Sekai-only `project_sekai_toon` gallery pack (not in the README).
- Not done / open: real-time PT (play mode) has the same stochastic-transparency noise (not changed); the moving clip (27 frames,
  1080p) was only checked by eye; a 1024-pass frame was ~1.7 % darker than <=128 passes in the no-stage scene (cause not found);
  render time per preset not measured; `build\` (the user's dev build) was not rebuilt with these changes.
- Originals: `captures/pt_passes` (before) and `captures/pt_passes/after_fix` (after), 4K PNG. Report: Artifact "PT 패스 누적 검증".
- Committed together with the other pending work in the tree (README / mcp / tools changes from other sessions), not re-tested here.


## 2026-10-18 (2) - MCP / AI accessibility for the studio: analysis tools, camera / lighting / motion toolsets, 3 agent skills
- Goal: an agent can understand and direct a Studio project over MCP without Python scripts or VMD parsing (the old
  Gemini `mmdx12-camera-director` skill parsed VMDs in Python and imported binary camera VMDs).
- Analysis (read-only): `studio_scene_info` (models, AABBs, height / facing, camera, every scene light), `studio_sample_motion`
  (bone world positions at any frames; `App::StudioSampleBones` poses without physics and restores the viewport),
  `studio_motion_events` (jumps, crouches, hand raises, turns, holds, travel, energy, teleports = centre jumps > 0.5 x height
  per frame, formation / crossovers), `studio_frame_check` (per-character screen bbox, cuts, head in frame, shot size in the
  solver's ecu..wide vocabulary; points behind the eye are excluded), `studio_audio_analysis` (`audio/BeatAnalysis`: own FFT,
  spectral flux, autocorrelation tempo 70..180 BPM, Ellis DP beat tracker, downbeats, per-beat energy, onsets, sections;
  `tools/beat_probe --selftest`; 5-min song < 0.4 s).
- Bulk keys (each write = one undo step "AI: <tool>"): `studio_get/set_camera_keys`, `studio_set_light` (create / props /
  base / keys / delete), `studio_get/set_bone_keys` (+ morphs), `studio_delete_keys`; curve presets (linear, ease_in_out,
  ease_in, ease_out, smooth, fast_start, slow_start) shared in `app/McpKeyUtil.h`.
- Camera direction (`AppMcpCamera.cpp`): `studio_camera_solve` (shot size + angle relative to the subject's facing + fov +
  screen placement -> camera; distance refined on the projected span) and `studio_camera_shots` (shot lists in one undo step:
  static, dolly, zoom, dolly_zoom, crane, truck, arc = swing, follow, push_accent; beat accents; hard cuts = adjacent shots;
  per-shot `warnings`). Claude redesigned the worker's shot generator: moves are relative to the OPENING framing (it solved
  the end pose at the end frame: a travelling dancer turned a dolly into a 40-unit slide), follow is a steadicam (heading /
  distance / lens kept, smoothed subject travel added; the worker re-solved the yaw from the dancer's facing every key = the
  "wiper"), arcs rotate the rig about the anchor (no per-key solves), arcs / follows / accents are baked with the easing in a
  global t, and full / wide shots anchor on the body centre (a wide shot's centre fell below the feet).
- Lighting direction (`AppMcpLight.cpp`): `studio_light_rig` (key_fill_rim, front_wash, top_spots, side_bars, back_beams) and
  `studio_light_cues` (pulse, chase, alternate, strobe, sweep, color_cycle, breathe, fade, section_levels, blackout on
  beat / half / downbeat / bar / onset / section). Review fixes: cue keys stay inside their cue range (a colour cycle's
  pre-event key overwrote the first colour), rejected rig calls restore the light list.
- Motion editing (`AppMcpMotion.cpp`): `studio_motion_edit` ops rotate / translate / look_at / retime / copy (+mirror) /
  mirror / smooth / morph envelopes / key_pose, baked additive layers with smoothstep envelopes, dry_run, one undo step.
  Review fix: the worker composed the parent-frame offset in the wrong order (QMul = "a then b"): local' = local then
  (P then D then P^-1). look_at verified by screenshot (head turns into the lens).
- Skills (`skills/`, shipped in the release zip by `package_release.ps1`): `mmdx12-camera-director`,
  `mmdx12-lighting-director`, `mmdx12-motion-editor` (+ references, README with install paths for Claude / Gemini / Codex).
  MCP tools only, no scripts. Motion creation from nothing is declared out of scope in the motion skill (edits only).
- Delegation: opencode workers (deepseek-v4.1-flash, low) built every module from contract specs (A beats, B1 analysis,
  B2 keys, C camera, L lighting, M motion, S / S2 skills); Claude reviewed, fixed and ran three live MCP smoke tests on
  build_b1 (Sour Miku White + rolling_girl, one window at a time).
- Not done / open: no full agent run of the skills end to end (tools verified individually and as sequences); look_at
  `mean_error_deg` averages the envelope ramps (10-25 deg typical); arm-axis signs in the motion skill are left to a
  screenshot check; `studiosong` does not attach the dance to an unselected model (the smoke test imported it by hand).
- Build blow-up (froze the session twice): the 51 tool schemas in `app/McpTools.h` were nested nlohmann::json initializer
  lists; MSVC needed 27 GB for ONE translation unit including it (C1060 when several compiled in parallel). Now one JSON raw
  string per tool (`kMcpToolSchemas`, parsed once): 231 MB per TU, the bridge's tools/list is byte-identical. Rule added to
  CLAUDE.md.

## 2026-10-18 (3) - Skill validation runs (V1-V3), release 2.2.0
- Validation of the three skills by other agents, one app window at a time (build_b1, MCP), all in cinema scope 2.39:1
  (`studioaspect 2.39:1`); projects built over the pipe by a scratch setup script, before / after videos rendered with the
  raster offline renderer at 1280x536, 30 fps, 6 Mbps (`--offline-bitrate 6`; the first renders used the saved 20-40 Mbps
  settings: never ship / hand over ~1 GB validation videos). Deliverables in `captures/skill_validation/` (git-ignored).
  - V1 モニタリング (Best Friend Remix), one performer, camera + lighting: Antigravity (Gemini 3.8 flash high). Strong
    treatment; issues seen: yaw "wiper" alternation in the storyboard, a chest-only frame near 1900, a storyboard gap
    4080-4229, and the lighting SKILL.md names a reference `rig_patterns.md` that does not exist. Agent asks: headroom for
    tall ears, truck + follow combos, yaw unwrap on spins, a rim-light section.
  - V2 ライアーダンサー, five performers: singer-led close-ups via `studio_singing` worked (Emu 630, KAITO 1400); the wiper
    again, a member cut at the frame edge in the drop wide, an off-corner close-up with the head cut.
  - V3 オーバーライド (Kasane Teto, stiff motion), motion skill: opencode (deepseek-v4.1-flash, low). Diagnosis (baseline
    motion_quality 47.9; worst ranges near 3180 / 3210 / 1620), a per-range fix plan (ease -> overlap -> spring -> breathe,
    sync-stop frames kept sharp) and several before / after contact-sheet passes; the run was stopped before it finished
    (work in progress saved as `override_teto_humanized_wip.mmdxproj`, no V3 videos rendered).
  - The user's first-round review of V1-V3 is still open; skill fixes (wiper, rig_patterns.md, headroom, yaw unwrap) wait
    for it. The installed copies in `~/.gemini/config/skills` and `~/.agents/skills` must be re-synced after skill edits.
- Antigravity and opencode reach the app through an `mmdx12` MCP entry in their own configs (`--timeout 180000`); an older
  installed skill with the same name shadows the repo one (the old Gemini camera skill was moved to a backup folder).
- Release 2.2.0 (minor: MCP studio AI tools + agent skills in the zip, PT video accumulation). Cover:
  `docs/media/studio-ai-2.2.0.jpg` (user's screenshot of an agent directing the studio). Notes `dist/notes_2.2.0.md`
  (4 languages). The live 2.1.3 delta manifest points its objects at `/updates/objects/objects/` (404): the 2.2.0
  manifest is built with `--base-url https://mmdx.codingbot.kr/updates`.

## 2026-10-19 - DLSS RR / Frame Generation, pipelined video renders, adjustable bloom, glow_rim in the gallery
- DLSS 310.9.16 on raw NGX (earlier session, committed now): SR model hint (auto / CNN E / transformer K / transformer 2nd
  gen M, L at Ultra Performance), Ray Reconstruction on RT / PT (guides from the tracer or `ScenePass::DrawGuides`, albedo
  guides blended by coverage after the "panda eyes" bug), Frame Generation 2x-4x in play scenes (`DlssFrameGen`,
  `Dx12Context::EndFrameFrameGen`). Thumbnails now render with default RenderSettings.
- Video renders (earlier session, committed now): 3-slot final-image readback ring + `AsyncVideoSink` worker encoder,
  exact-frame preview every 5th frame.
- Bloom made adjustable (the user wanted the Unreal convolution look: star glints into a diffuse glow; ours threw six
  very long rays and a screen-wide coloured haze because ~89 % of the kernel energy sat in the halo and rays):
  - `BloomLook` (RenderTypes.h): mip-chain radius (level weights falloff^j, normalised by `RenderTargets::bloomScale`),
    blend additive / energy-conserving (post.hlsl removes the scattered share of the bright light, `bloom_common.hlsli`),
    depth mask (bloom alpha = luminance x linear depth through every level, so post knows the sources' depth).
  - Convolution: `render/BloomFft.*` shared by BloomPass and the offline renderer; procedural kernel per channel
    (core Gaussian + Moffat glow + power-law diffraction spikes, each with unit integral, dispersion = channel scale),
    (R,G) and (B,A) as two FFT runs so every channel has its own kernel, normalised by its own sum; spectrum rebuilt
    only when `BloomKernel` changes; grids created on first use. Presets cinema (new default) / starburst (old look) /
    glow / anamorphic + 8 sliders. Checked with an injected point light: GPU kernels match a numpy simulation.
  - App wiring (settings / ini, CLI `--bloom-*`, MCP `bloom_*`, `App::DrawBloomControls` in the play panel, lobby
    detail and studio effects tab, 3 languages) delegated to opencode (deepseek-v4.1-flash); reviewed, no fixes needed.
  - Default mip bloom is unchanged (additive, radius 1, no depth mask); the convolution bloom's default look changed.
- Temporary build dirs (build_b1 .. build_wc) deleted; the Antigravity / opencode MCP configs now point at
  `build\bin\mmdx12_mcp.exe` (backups `*.bak-build_b1`). The running bridges keep that exe locked (LNK1168): rebuild
  `build` once the agent sessions are closed.
- Gallery: `glow_rim` 1.0.0 (back-light glow rim surface pack, raster/RT + PT/GI) published with the user's before / after
  shots.
- Commits split by scope with a hunk/line stager (index blobs built from HEAD + selected hunks); each intermediate commit
  was build-checked in a scratch worktree.

## 2026-10-19 (2) - GI video eye adaptation flicker fix, release 2.3.0
- Report: the `eye_adaptation` effect pack flickered in offline renders ("시간 적응 로직이 제대로 작동 안하는 듯").
  Measured (Sour Miku White, Evergreen 20-24 s, 640x360, 30 fps, GI): without the pack frame luma is flat (128-131);
  with it the per-frame exposure ratio jumped (mean 1.08 %, max 6.0 % between frames).
- Diagnosis: a scratch edit of the runtime pack copy made `PackEffect` output its frame counter (state[4]): 0 on all 60
  frames = the state was reset every frame, so the pack metered each frame from scratch (64 samples, no smoothing).
- Root cause: d1d394d (pipelined video readback) renamed `offline_.frame` to `offline_.renderedFrame` in the GI path's
  `jd.effectReset` (UiOffline.cpp); `renderedFrame` only advances in `RecordRealtimeVideoFrame` (raster / RT / PT),
  so every GI frame was "frame 0". Fix: the GI path tests `offline_.frame` again.
- Verified on build_release (build/ was locked by the user's open app + the MCP bridge): counter ramps; GI mean 0.13 % /
  max 0.78 % (8-bit steps); raster 0.09 / 0.72 %, RT 0.09 / 0.68 %, PT 0.06 / 0.43 % (those paths were never affected).
- Found, not fixed: in PT video a detected jump cut (`jumpCut`, camera teleport / pose jump) sets `view.cameraCut` only on
  the frame's first pass, while the effect state advances on the last pass, so such cuts do not reset effect state
  (VMD camera cuts do). Effect: exposure eases across the cut instead of snapping; no flicker.
- Release 2.3.0 (minor: DLSS RR / FG / model choice, pipelined video renders, adjustable + convolution bloom presets, this
  fix). Cover `docs/media/bloom-2.3.0.jpg` = the user's own 4K play capture (MMDX12_20261019_025904, a furry model): the
  user was asked about the public furry-model rule and explicitly chose it. Notes `dist/notes_2.3.0.md` (4 languages; lite
  users with DLSS installed re-fetch the component: it now has 3 files and counts as missing until they do).
  `package_release.ps1` (full 195 MB: +nvngx_dlssd / dlssg, lite 15.3 MB), packaged smoke (select screen, v2.3.0,
  0 `[E]`/`[W]`, SR / RR / FG available; cache / shader_cache / ini / log removed, stage file list identical to before),
  delta manifest 2.3.0 = 14 new objects (113 reused), uploaded (only the objects missing on the server), live manifest
  sha 3b3a4867... and exe / dlssg object hashes match.

## 2026-10-19 / 20 - MMDX Effects (native effect ecosystem), world effects + Snow, open gallery, release 2.4.0
- Direction + decisions: `docs/effect_ecosystem.md` (now "shipped in 2.4.0"). Built through delegated workers (Gemini /
  opencode), reviewed and fixed here: capabilities / `requires` + scene / audio inputs, studio effect stack with keyable
  params (project format 3), look packs, multi-source open gallery (verified badge, revocation, pinned updates, crash guard),
  surface output (normal / emission / tag), object / class ID image, pass history + mips, params.ext (128 typed params in
  groups; b6 surface ring, b4 effect ring, PT / GI `gPackExtSlots`), world effects (`WorldPass`, docs/shader_world_api.md)
  and the built-in Snow (`shaders/packs/snow`: snowfall, 200-unit cover with footprints + print colour params, landing
  particles that thin out on trodden snow), MME porting guide + agent skill.
- Review fixes worth remembering: `RtPtPackRecord` must stay a multiple of 16 bytes (raw SRV offsets; 152 bytes removed the
  device); world re-simulation needs per-frame growing upload chunks + persistent field descriptors (a wrapping ring /
  transient tables corrupted steps); persistent fields step on a fixed k / rate grid in both re-sim and playback; world
  particles are drawn after the composite (no SSR); play-mode collider track is built once per scene.
- UI naming "MMDX 이펙트" (top tab 이펙트; 표면 셰이더 / 화면 효과 / 월드 이펙트 / 룩). Website: gallery backend + publish /
  admin pages deployed; docs rewritten for the current system in 4 languages (Korean polished with the im-not-ai
  humanize-korean skill on gemini-4-argon-high - gemini-3.8-flash made the prose worse and once faked the skill run);
  world effects page; the 16 official gallery packs moved to params.ext (need 2.4.0).
- Studio (another session's work, committed here): per-property stopwatch animation + key navigation, timeline range menu /
  auto-scroll / static tracks, OKLab colour picker, pose / light panel improvements. Fixed after the user's test: sliders
  kept shrinking by the stopwatch + key-nav width (`SliderRow` now sizes the slider line from the row start), OKLab picker
  sliders grew the auto-resizing popup (fixed width). Open (user will do it): world entity gizmo + transform keys.
- Before the push, the unpushed history was rewritten (filter-branch over origin/master..HEAD) to drop server / OAuth
  operation details from docs/effect_ecosystem.md's decision table.
- Release 2.4.0: cover `docs/media/snow-2.4.0.jpg` (Sour Miku, Snow on the theater stage), notes `dist/notes_2.4.0.md`
  (4 languages), `package_release.ps1` (full 195.3 MB, lite 15.5 MB), packaged smoke (select screen, v2.4.0, 0 `[E]`/`[W]`,
  SR / RR / FG available; leftovers removed, stage file list identical to before).

## 2026-10-20 - Studio effect stack not applied live, release 2.4.1
- Bug: the studio "효과" panel's effect stack edits (`effChanged`: add / remove / toggle / reorder / parameter sliders) never reached
  `ApplyRenderSettings()` (only `changed` did), so they showed up with the next bloom / LUT edit. Keyed values while playing /
  scrubbing, undo / redo and timeline key edits had the same problem: nothing refreshed `RenderSettings::packEffects` per frame.
- Fix: `effChanged` sets `changed`; the studio frame path refreshes only `packEffects` each frame (`App::UsableEffectStack`, shared with
  `ApplyRenderSettings`, so the crash-guard quarantine filter still applies). A per-frame `ApplyRenderSettings()` was avoided: it would
  override render settings the studio forces.
- Release 2.4.1 (patch): notes `dist/notes_2.4.1.md`, `package_release.ps1`.

## 2026-10-21 - World pack capabilities: camera space, performer anchor, ground map, overlay surfaces
- Spec D (engine side only; the packs are updated by another worker against the same API): the world effects cover the whole
  map around the camera instead of a disc around the entity, the expensive persistent state (footprints) stays local to the
  performers, and the snow / water lie on the real stage surfaces.
- piVersion stays 6; the new features are capabilities in `requires`: `world.camera` (F1, F2), `world.anchor` (F3),
  `world.ground` (F4, F5), `world.overlay` (F6). A pack that uses a manifest key of a capability it does not require is
  refused with a problem line naming the capability (`world.particles[0].space 'camera' needs the "world.camera" capability`).
  The world shaders get `PACK_CAP_WORLD_CAMERA` / `_ANCHOR` / `_GROUND` / `_OVERLAY`; the new API functions and the
  struct fields exist only under their macro, so a pack without a capability renders exactly as before.
- New manifest keys: `particles[].space` (`local` / `camera`) + `particles[].wrap`, `fields[].anchor`
  (`transform` / `performers`), `surfaces[].extent` (`field` / `camera`) + `surfaces[].radius`,
  `surfaces[].ground`, `surfaces[].blend` (`opaque` / `overlay`).
- New API: `PackCameraPos` / `PackWrapSize` / `PackWrapTile` / `PackParticleWorld`, `PackFieldCentre` /
  `PackFieldCoverage`, `PackGround` / `PackGroundHeight`; `PackSurfaceIn` / `PackSurfaceShadeIn` gained
  `posWorldXZ` + `groundNormal`.
- Host: the ground map is a top-down orthographic depth render (the shadow pass's `VSShadow` with an override matrix) of the
  stage and prop models only, 2048^2, re-rendered when they change; the camera-centred surface grid is 3 nested LOD levels
  (coarse / 4x finer / 16x finer) snapped to their spacing, each level skipping the cells the finer one covers; an overlay
  surface is premultiplied alpha over the lit scene (depth test on, no depth write, a slope-scaled bias, no normal / velocity
  writes). The camera-space wrap maps the particles into a box of `wrap` units around the camera and keeps the motion
  vectors free of streaks.
- Verified: `build.cmd build_dev` builds; the world host shader compiles with dxc for the built-in snow pack (no new
  capabilities) and with all four `PACK_CAP_WORLD_*` defined; `pack_check --compile` compiles a test pack that uses every
  new API, and refuses the same pack without the `requires`.
## 2026-10-21 - Stage shadows, song-stage studio choice, world effects over the whole map (camera / performers / ground)
- User reports on 2.4.1 (delegated to opencode workers on `opencode/minimax-m3.1-flash-free`, reviewed + fixed here):
  - Song-stage popup: "현재 선택: 스튜디오" erased the per-song choice (= the bundled stage again). Now "-" = studio floor,
    labelled "스튜디오 (스테이지 없음)"; the 현재 선택 entry is hidden without a picked stage card (worker A).
  - Stages showed no cast shadow (only SSAO darkened them). Two causes: ripped stages ship every PMX shadow flag 0 (survey
    of the library: Sekai / Fireflower / Konton / Supernova rips all 0; hand-made MMD stages set them per part), so the
    shaders skipped the shadow term; and stage spot shadows used the sun's 15 degree soft cap (a default spot = a 5.3
    degree light whose PCSS erased the character's shadow). Fix: a stage with no receive flag on any material receives on
    all (authored flags kept; casting unchanged, so a dome that does not cast never shades the stage), stage full shadow
    0.42 (was 0.62; worker B's rewrite got lighter than before on high-ambient materials, rewritten), punctual soft angle =
    1/4 of the sun's in raster PCSS / RT / PT / GI (`PunctualSoftAngle`, worker C).
  - Snow ignored the scene's spots / points: world surfaces now get them with the shared soft / hard shadow code
    (`shaders/shadow_common.hlsli`, worker C; the user asked for no hard-only copy).
  - Decided with the user: play = light users, studio = creators; per-material cast / receive overrides belong in the
    studio only (saved in .mmdxproj, never by editing the PMX) - not built yet. A stage without an operable root bone
    cannot be moved in MMD either; a keyable studio placement track is the proposed answer (not built).
- World capabilities (worker D, engine; worker E snow; worker F the user's own world packs): see CLAUDE.md "World
  effects". Review fixes (Claude): ground map uv / normal z flip (the top-down camera puts +z at texture top), the fine LOD
  level sat at a corner of the middle one, the ground render uploaded 4 of 16 matrix dwords (the worker also misreported
  the correct `DrawSlice` call as a bug), overlay PSOs lacked IndependentBlendEnable and used a positive slope bias (hid
  the layer behind sloped floors), overlays skipped the punctual lights, `PackGroundHeight` now conservative (3x3 max).
  Added `wrapParam` / `wrapScale` and `PackWrapCentre` (box ahead of the camera: with a box centred on the camera a
  subject 40-60 units away is outside it and the effect vanishes from the shot).
- Worker F re-anchored every particle to the camera each step (camera-locked rain, no parallax): rewritten to world-fixed
  tiles in the user's rain / sakura / fireflies / ground_mist; range defaults raised (45 / 45 / 40 / 60) with more particles
  so the box reaches typical MMD camera distances; aurora reverted (its motes ring the curtains); confetti / fireworks
  unchanged (placed events). Rain: wet floor = overlay on the real floor (the old one painted its own floor colour), puddles
  by world-xz noise on flat ground, no disc. Sakura: the analytic carpet rendered as blobs, now scattered individual petals
  (one per cell). Ground mist: overlay haze centred on the camera box, opacity x2.5 (the old opaque sheet hid the floor).
  The user's packs live outside the repo (copied back to their Downloads `shader_packs` folder after review).
- Verified (raster, 1600x900, Fireflower / Base Stage (0007), `--seek 20 --paused --no-physics`): stage spot + sun shadows,
  snow on every upward surface incl. raised walkways (none on walls), old snow pack (no capabilities) unchanged after the
  engine change (31 / 5.7M bytes), rain / sakura / fireflies / mist visible around a subject 60 units from the camera,
  0 `[E]`. Not verified: PT / offline GI with the new capabilities, the song-stage popup by clicking (code review only),
  puddles close up, studio-placed world entities off the origin.

## 2026-10-21 (2) - Release 2.5.0, gallery world packs, MCP world / camera, gallery admin accounts
- Before the release (Claude): PT frames with a world effect overflowed the transient descriptor window (three 8-slot
  tables per world draw: now one table for sun / spot / point maps, params 8 / 11 / 12 offset into it); offline GI world
  surfaces now get the punctual lights (they were black under spot-lit night presets). Ground layers: the 3x3-max ground
  height buried posts and the grid draped sheets over railings (the user caught it in the snow preview after I had
  passed it) -> bilinear ground height, `PackGroundRange` / `PackGroundStepLimit` (3x3 spread above max(0.5, 1.2 texels)
  = a thin / tall feature, no layer there), fragments > 1.5 above the real ground discarded.
- MCP: `set_render_settings` `world_stack` (shared stack parser with `effect_stack`; `key#n` vector params),
  `get_render_settings` reports it, new `set_camera` (play screen free orbit camera / `motion: true`).
  `tools/pack_previews.py`: world shots (`world_stack`, `set_camera`), `--exe`, snow written to the built-in folder too.
- Gallery (website repo): Snow (built-in, gallery.json like hoyo_toon), Rain, Ground Mist, Fireflies (the user's packs,
  authors MMDX12, minAppVersion 2.5.0, previews with Rom); `build-packs.mjs` kept only surface / effect types (world packs
  would have listed as surface). Excluded after testing: aurora (the opaque surface host drew its whole 280-unit
  height plane as a grey slab; as an overlay the slab is gone but no camera found the curtain), fireworks / confetti
  (nothing visible at any tested time), sakura (my camera-space rewrite streaks the falling petals; the original is fine
  and was restored in the user's folder - fix pending).
- Gallery server (note20, deployed from the website repo): admin accounts = a stored `role: "admin"` on the user record
  (read per request, survives logins; cookie writes need the site Origin), `/me` `isAdmin`, admin page without a token
  for admin accounts; test `admin_role.test.mjs` (`NODE_ENV=test npm test`: 19 / 19). The user's GitHub account was promoted.
- Release 2.5.0: `fetch_sdks.ps1` (this machine had an old 310.9.1 `nvngx_dlss.dll` and no RR / FG DLLs: delete the
  file to refetch; compare against the previous manifest before shipping), `package_release.ps1` (195.5 / 15.8 MB),
  packaged smoke (select screen, 0 `[E]`, leftovers removed), delta manifest 82 new objects (+ .gz) uploaded, all 138
  files reachable, manifest hash matches; GitHub v2.5.0 (digests = local zips); website deployed (`/latest.json` -> 2.5.0).
- Pitfall again: a path list written by Windows python carries `\r` (`tar -T` fails per file): `tr -d '\r'`.
- Follow-up (user report): offline GI world particles / surfaces ignored the thin-lens DoF (and the shutter camera):
  the world pass drew the GI edge layer with the frame's pinhole `gViewProj`. Now `OfflineRenderer::IterationCamera`
  (shared with the MME layer) builds the iteration's lens / shutter view-projection and `WorldPass::DrawOffline` passes it
  in `WorldCB::gOfflineViewProj` (`WorldClip` in the host VS). Verified: GI still, DoF on, background flakes blurred.

## 2026-10-21 (3) - Release 2.5.1: gallery world packs were listed as incompatible
- Bug: `kPackTypeWorldCompatible` (ShaderPackStore.h) was still `false` after 2.5.0 added world effects, so
  `RemotePack::Compatible()` marked every gallery world pack (rain, ground mist, fireflies) "호환되지 않음". Flipped to `true`.
- Release 2.5.1 (patch): notes `dist/notes_2.5.1.md`, `package_release.ps1` (195.5 / 15.8 MB), packaged smoke (select screen,
  0 `[E]`, leftovers removed), delta manifest 2.5.1 (5 new objects, `--base-url https://mmdx.codingbot.kr/updates`) uploaded,
  live manifest hash + exe object verified; GitHub v2.5.1; website deployed (`/latest.json` -> 2.5.1 with manifest).
- Lesson: a "flip the constant when it does" placeholder needs a release checklist entry; grep `Compatible` flags when a feature ships.
- After the release (same day): the 2.5.1 notes got a cover image (`docs/media/world-effects-2.5.1.jpg`, the user's studio
  screenshot; release body edited, site redeployed twice per the gh-release-edit pitfall).
- Unreleased (commit 6090be4, for the next patch): (1) pack picker previews stayed blank in the studio / play screens:
  `ThumbnailCache::Pump` only ran on menu screens (studio: only while the add popup was open), so Image entries never
  uploaded. The picker popup sets `studioThumbsShown_`; studio pumps fully, play pumps `Pump(uploadsOnly=true)` (the render
  step waits for the GPU / borrows the scene targets). Verified in the studio. (2) The picker's "이펙트 관리 · 온라인에서 받기"
  button only switched screens from the library (dead in studio / play): now shown on `Screen::Select` only. A modal gallery
  inside the studio / play screens would be the real fix (not built).
- Pitfall: MCP default pipe `mmdx12_mcp` can belong to another instance (first one wins); `mcp_x.py` talks to it, so a
  freshly launched build's screenshots can come from the wrong app. Use `MmdxPipe(pid=<pid>)` (`mmdx12_mcp_<pid>`) and check
  `CreationDate` / `ExecutablePath` of the processes.
- Next: collect more fixes for 2.5.2; consider a release-checklist step "install a gallery pack of each type in the build".

## 2026-10-23 - Video motion capture ("영상에서 모션 만들기")
- Goal: the YouTube tutorial (uy35EDsNMUQ) shows QuickMagic, a closed paid cloud AI mocap (video -> body / hand / face VMD).
  Not embeddable; the same pipeline is built from open parts: Google MediaPipe models (Apache-2.0) as ONNX (opencv_zoo +
  QtMeshEditor mirrors, MIT reference code) on ONNX Runtime 1.22 CPU. MiKaPo (same idea, GPL-3.0) used as approach only.
- Workflow: Claude = architect (MocapTypes.h contract, CMake, specs, review, fixes); Antigravity workers built the
  extractor (A), the solver + round-trip test (B) and the studio integration (C, resumed once after over-exploring).
- New: `src/mocap/` (OrtRuntime, MocapExtractor, MocapSolver), `tools/mocap_probe`, `tools/mocap_solver_test`,
  `app/UiStudioMocap.cpp` (model menu, options / progress modal, one undo step), ui-script `studiomocap`, fetch_sdks +
  package_release (bin/mocap, ORT / MediaPipe / QtMeshEditor licence notes).
- Review fixes (Claude): hand presence read the handedness output (hands "found" on 50-80 % of frames, all garbage; real
  presence ~0 because the pose-derived crop sat on the wrist and was too big) -> forearm-direction crop candidates +
  MediaPipe landmark re-crop + tracking; head from face landmarks used normalised y-down image coords as 3D (upside-down
  head; the worker's test never fed face landmarks); センター Y lifted crouches instead of lowering them (test only checked
  for sinking) -> exact ground contact by posing the model per frame; non-ASCII path logging; IK re-enabled after the
  replaced range. The test now also checks face-landmark heads (3 deg) and foot contact both ways (0.12).
- Verified: mocap_solver_test PASS; mocap_probe on captures/media_kit/raster.mp4 (6 s): pose 99 %, hands L 27 % R 38 %,
  face 44 %, 85 ms/frame; studio ui-script run: 3227 keys in 16 s, one undo step, 0 [E]; the solved pose at frame 0 / 9
  matches the video (hand over the face, other arm out, feet on the floor).
- Not verified / open: MediaPipe blendshape Left/Right side convention (`SolveOptions::mirrorBlendshapeSides`, default off)
  on a real face video; real human footage (only anime renders tested); jumps are not recovered (feet stay planted); the
  options dialog was not clicked by hand (script path only); lite zip carries the mocap files (no optional component).
- Repo moved F: -> D: (build/ reconfigured; CLAUDE.md build line updated).

## 2026-10-23/24 - Offline GI measured + optimised; studio viewport renderer selector (GI preview)
- Question: is the GI irradiance cache prepass a bottleneck / would unbiased brute force be faster? Measured (1080p still,
  01_9001 + Evergreen, new `--offline-prepass 0|1` + per-image `OFFLINE TIMING` log line): NO. Cache 10.2 s, brute force
  14.6 s (+43 %), images equal (0.4/255). The real costs: the sampling tail (~40 % of the time after 512 spp, < 12 % of
  the pixels) and a latency-bound prepass (one thread per cache sample: coarse levels left the GPU idle).
- Optimisations (shaders/offline_gi.hlsl, OfflineRenderer.cpp): `CSPrepassLevel` = one 32-lane group per sample
  (prepass GPU 500 -> 200 ms per video frame); video frames skip the cache-lit dot display (`job.still` only); tail tile
  compaction (`CSClearTiles` / `CSActiveTiles`, tile list u7 / t5 with the count in its last row, dispatch sized from a
  lagged read-back count = upper bound because the active set only shrinks). Lossy (user allowed slight loss): stills
  min/max 256/4096 -> 128/2048 spp, video preset error thresholds x1.33, outline layers 256 -> 64. RMSE vs a 4096 spp
  reference 0.45/255. Result: still 10.5 -> 7.6 s (-28 %), video (표준, 1080p) 4.2 -> 2.97 s/frame (-29 %).
  The GI render benchmark's fixed workload is untouched, but its score is not comparable with older versions (prepass).
- Studio viewport renderer (Opencode worker deepseek-v4.1-flash-free#low built it from Claude's contract; Claude reviewed):
  transport-bar dropdown 래스터 / 레이 트레이싱 / 패스 트레이싱 / GI 미리보기 (`studioRenderer=` ini, `--studio-renderer`,
  ui-script `studiorenderer`, `STUDIOGI` in studiostate). Applies to the lit single view only; unlit / wire / quad stay raster.
  GI = `App::RecordStudioGiViewport` (UiOffline.cpp): OfflineRenderer at the viewport size (Present fits into
  `ViewportArea`, `OfflineJobDesc::frameBudgetMs` 30), restart key over camera / time / history / project version / light /
  job settings / pose hash; half resolution on change, full after 0.3 s. Review fixes: a fast half-res job that already
  finished never promoted to full resolution; prepass badge counted from 0; a deleted comment.
- Verified: headless studio scripts (GI badge + image, RT / PT really engaged: 2.4 / 19.7 per-pixel diff vs raster, unlit /
  quad fall back to raster with PT selected, GI job dropped on unlit), quick GI benchmark, 0 [E].
- Notes: the D: drive vanished mid-session; the repo is back on F: (build/ reconfigured, CLAUDE.md build line). Studio PT
  looks hazy on Base Stage (0007): the same in play mode (pre-existing). GI preview while playing restarts every frame (noisy).
- Next: interactive GI preview by hand (navigation feel, UI stutter during the 6 prepass frames at large viewports).

## 2026-10-24/25 - Effects on timeline time, world param refresh, world.body / world.lights, interpolated world particles
- Report (4 points): (1) eye adaptation and (2) lens rain ran on the client frame (`gTime` = `temporalIndex_ / 60`, state dt =
  wall-clock frame time): not deterministic, frozen in GI, running while the timeline was paused; (3) world param edits
  (density, petal size) showed only after a seek; (4) sakura "petals settle on the character" did nothing + black dots.
- Effect time (`PassEffect.cpp`, `effect_api.hlsli`): `gEffectTime` = `gP3.x` = the frame's scene / timeline seconds (not
  `gTime`; `OfflineJobDesc::sceneTime` for the GI paths, studio GI viewport included). State stepping follows the timeline:
  playing = one step of dt = `sceneElapsed`; the same time again = no step; paused / seek / cut = the still rule (reset +
  64 steps of 1/60), so a paused frame is deterministic and camera edits show at once. `FrameView::timelinePlaying`.
  Measured (rain_lens + eye_adaptation, same seek, paused, `--frames` 60 vs 150): 89,628 px differ before, 1,720 after (TAA tail).
  The effect clock is continuous time, never the 30 fps VMD frame: 180 fps playback runs the effects every frame.
- World param refresh (`PassWorld.cpp`): a hash of the instance params; a change on the SAME time as the last frame
  (paused / repeated) re-simulates like a seek, a change while the time moves (keyed params) does not. (Review fix: the
  worker compared with `simTime`, which runs up to one grid step ahead of the drawn time during playback.)
- `world.body` capability: body colliders (head r 0.9, upper body 2 / upper body r 1.2, shoulder / arm r 0.7, lower body
  r 1.3, `kColliderBody = 8`); packs without it see the old feet / hands list and order. Over the 64 limit the body
  colliders are dropped first (review fix; the worker's truncation could also drop other characters' feet; head bone name was 頭部, is 頭).
- `world.lights` capability (general, no pack-specific code): `PackSunLight`, `PackPunctualCount` / `PackPunctualLight`,
  `PackDirectDiffuse`, `PackDirectThin` (two-sided thin sheet: front Lambert + diffuse transmission, never > 1 per light).
  Shadows: cascade + spot / point maps in raster, one ray query per light (TLAS) in RT / PT / offline GI (second graphics
  root signature + `ps_6_5` PSO variants, all-or-nothing fallback). Review fix: the RT spot / point rays follow mmd.hlsl
  (only shadow-casting lights, characters only, sun ray 2000). Non-lights packs keep their root signature / PSOs.
- World particles: simulation steps end on one global grid (k * `step` from 0, no partial steps); the vertex shader draws
  `lerp(prevPos, pos, a)` (a = 1 - (simTime - time) / step), respawns / teleports / wrap jumps are not lerped, wrap applied
  after the lerp, motion vectors from the same lerp at `time - sceneElapsed`. 60 Hz stairs at 120+ fps are gone (not seen
  by eye: headless captures only). `WorldCbData` 416 -> 432 bytes.
- sakura pack 1.2.0 (not in this repo: sibling repo `MMDX12_PackUpdates`, local only, with RELEASE_MEMO.md): black dots =
  the billboard normal pointed away from the camera; landing = `SakuraBodyLanding` (+ `spawn_height` default 12 -> 24: at
  <= 12 petals never reached the head); thin translucency (`thin_translucency`, `translucency_tint`, legacy `translucency`
  scales it). Ship it with the next engine release (pack `requires` `world.body` + `world.lights`).
- Workers: opencode deepseek-v4.1-flash-free#low did the engine specs (A, C, D: 13-40 min each) but timed out four times
  on the visual sakura job; antigravity gemini-3.8-flash-high finished it in 21 min. Claude reviewed everything. Worker
  claims that were wrong: "1,419 petal pixels on the head" (the capture showed only sky).
- Verified: build, 0 [E], raster / RT / PT / offline GI captures of the pack, stick on/off A/B (body-shaped blobs), back
  view with petals on hair / jacket / skirt, determinism A/B above. NOT verified: smoothness at 120+ fps by eye,
  back-lit glow strength, sakura on dark concert lighting, PT / GI of the pack beyond the worker's captures.
- Left for the user: look at the updated sakura and the engine-affected packs (eye_adaptation, rain_lens, rain, fireflies,
  ground_mist, snow) in `build/` (installed there), then release together with more features (not released yet).

## 2026-10-25 - Studio timeline: row controls, value / speed graph lane, quad-view upscaler fix
- Request: Premiere-style editing in the studio timeline. (1) stopwatch (animate on/off) and prev / next key buttons move from
  the inspector to the timeline rows; (2) value / speed graph shown and editable in the timeline, compatible with (and keeping)
  the inspector's MMD-style `BezierCurveEditor`; (3) bug: DLSS / upscaler on + studio quad view looked pixelated.
- Row controls (`UiTimeline.cpp` `controlRects`, `App::StudioRowStopwatch` / `StudioRowKeyNav`): stopwatch + prev / key-here /
  next drawn with `ImDrawList` and hit-tested by hand (the timeline is ONE ImGui item, UiKit widgets cannot nest). Bone, morph and
  scene-light rows; same undo names as before; inspector blocks removed.
- Graph lane: graph icon on bone / camera rows with >= 2 keys opens `kTimelineGraphRows` (5) fixed-height lane rows
  (`RowKind::GraphLane`, `StudioDoc::graphOpen`): value sub-lane (channel chips, shared `curveChannel`, ghost channels normalised
  per channel) + speed sub-lane (|dv/dframe| x 30). Sampler `d.view.graphSample` reads live key data. Camera 1-frame gap = cut.
- Lane editing: the active segment (single selected key's arriving segment, else the one under the playhead) shows P1 / P2
  Bezier handles; drag rewrites the curve bytes (`SetBoneCurve` / `SetCameraCurve`, all channels with "apply to all"), one undo step
  "보간 곡선 편집" via `studioLaneCurve*` members, double click = linear reset. Tooltips on the label controls.
- Quad view fix (`Renderer::Render`): the upscaler sized the targets at render resolution but `RecordScene` skips the upscaler in
  the quad view, so a low-res image was stretched. Quad view (raster + extraViews) now sizes as no-upscaler. Verified with FSR
  Performance before / after captures; DLSS itself not tested (same shared sizing path).
- Workers: antigravity gemini-3.8-flash-low did the row controls (30 min timeout + 2 resumes: it looped on reads over a spec
  conflict, Claude decided the ImDrawList design; one run was killed by the memory-pressure reaper). opencode deepseek 1017 `#low`
  did the graph lane and the handle editing, `default` + `--review full`. Claude reviewed, QA'd visually, and did the quad fix.
- Verified: build, ui-script captures of the lane, camera handle drag at max zoom (keys 2 frames apart) + Ctrl+Z restored it.
  NOT verified: handle double-click reset, bone-row handle drag (test motions are baked: a key per frame), live inspector sync,
  the "apply to all channels" path, DLSS quad view.

## 2026-10-25 - Studio inspector / timeline UX: playhead-only value fields, effect / world param rows, per-channel graph lanes
- Report: (1) a selected camera key at frame 98 kept its values in the inspector while playing / scrubbing to 144, so
  editing them changed an off-screen key; (2) the effect / world properties still had the stopwatch + key navigation
  (regression of the timeline-controls move); (3) graph lane channel chips should be AE-style rows; follow-ups: the
  Bezier handles felt wrong, an "all channels" path in the lane, rows grouped like AE, one lane per channel.
- Inspector (After Effects model): value fields always mean the playhead. The camera panel stays full (live "카메라 값")
  unless the selected camera key IS at the playhead; a selected key elsewhere shows only track, frame and interpolation
  controls (no value fields for camera / light / shadow / focus / scene light / accessory / effect / world param keys).
  A first version added a notice + "키로 이동" button; removed on request (the frame row says it; a key click seeks).
  Review fix: the dangling key-edit close must skip live field drags (`studioKeyLive_`, `IsAnyItemActive`).
- Effect / world params: `DrawKeyableParams` is plain sliders; every keyable component has a timeline row with stopwatch /
  key nav (`StudioRowStopwatch` / `StudioRowKeyNav` handle EffectParam / WorldParam). Row index scheme (runtime only):
  pack param i -> `i * 4 + component`, orphan tracks `0x100000 + n`. Rows nest: category (화면 효과 / 월드) > entity group
  (`MakeRowId(Group, 0x400000|uid or 0x800000|uid, 0)`, default collapsed via `StudioDoc::expanded`) > pack.json group
  (index 1 + group index, default = the group's `collapsed`) > params (no "pack · " prefix).
- Graph lanes: graph icon shows channel rows (`RowKind::GraphChannel`: 모든 채널, X, Y, Z, 회전[, 거리, 시야]); each
  channel row toggles ITS OWN lane (`StudioDoc::graphLaneOpen`, several open at once; lane row index `(ch << 8) | i`,
  `TimelineRow::graphLaneChannel`). Single channel = only that channel's value / speed / handles; "모든 채널" = channel
  shapes normalised + the bold easing (0..100 %) and its speed inside the editable segment, drags write every channel
  (`TimelineEvents::graphCurveChannel`). Per-channel bytes `TimelineRow::graphCurves`. `kTimelineGraphRows` 4 (value
  2.5 rows, speed 1.5).
- Bezier handle fix: a flat segment's grown handle box always went UP, so on a falling segment the handles moved opposite
  to the curve and P2 hung off the wrong corner. The box now grows in the value's direction (clamped inside the lane),
  P2 hangs off the box corner, and a padded box draws the easing preview.
- Workers: opencode deepseek (`default` + `--review full`): run 1 (inspector, params, channel rows, 13 min); run 2
  stopped OVER-EXPLORING (looking for a Bezier evaluator) and resumed with a decision note (local `EaseCurve` in the
  widget), 12 min. Claude did the handle fix, the all-channels mode v1, review fixes and the notice removal.
- Verified (ui-script captures, captures/studio/qa2): live camera values with an off-playhead key selected, key section
  without value fields, three lanes open at once, all-channels drag (Y follows), world tree with pack groups.
  NOT verified: single-channel handle drag in the new block structure, a falling flat segment, effect ("화면 효과")
  tree capture, stopwatch off confirm on param rows, pack groups with "collapsed": true.

## 2026-10-25 - Removed video motion capture

**Why**: the "영상에서 모션 만들기" results were too poor to keep (anime renders: hands ~30 %, face ~45 % of frames), so the feature was removed instead of tuned.

### Done
- Deleted `src/mocap/` (MocapExtractor / MocapSolver / OrtRuntime), `app/UiStudioMocap.cpp`, `tools/mocap_probe.cpp`, `tools/mocap_solver_test.cpp`,
  the vendored `external/onnxruntime/` headers, the `mmdx_mocap` library, the `copy_mocap_models` target and the onnxruntime DLL copy.
- App: the studio model menu item, the mocap state in `App.h` (and its `StudioModal()` terms), the poll / dialog calls and the `studiomocap` ui-script command are gone; mocap strings removed from the En / Ja / Zh tables.
- `tools/fetch_sdks.ps1` no longer downloads ONNX Runtime or the MediaPipe models; `package_release.ps1` drops the `mocap/` folder, the ONNX Runtime / MediaPipe licenses and `mocap-sources.txt` (smaller release zip). `.gitignore` and CLAUDE.md cleaned.
- Implementation by an opencode worker (deepseek 4.1 1017, low); Claude reviewed the diff, cleaned CLAUDE.md, rebuilt from a fresh configure and smoke-ran the studio (no `[E]` / `[W]`).

### Notes
- Old `build*/bin` dirs may still hold `onnxruntime.dll` and `bin/mocap/` from earlier builds; `package_release.ps1` copies `bin/*.dll`, so package from a clean bin (`build/bin` was cleaned).
- A reset build dir needs `build.ninja` deleted too: build.cmd only configures when `build.ninja` is missing.

## 2026-10-27 - Stage ecosystem: S7b, S6b, S8b, S9b-1..4 (spot gobos, stage packs in PT/GI, screen lights, physical sky, sky lighting, image skies, clouds)

Every S row of docs/stage_ecosystem.md is now done; S13 (website stage gallery) is built but waits for the owner's
word to deploy. Workers: opencode deepseek 4.1 1017 #low; Claude designed, reviewed, fixed and did the visual QA
(captures/stages_qa/<milestone>/, raster / RT / PT / GI from one camera + the canonical `--stage none` A/B each time).

### Done
- S7b spot gobos (`shaders/gobo.hlsli`, every path + froxel / offline haze). Found on the way: imported glTF materials
  without an occlusion map had AO = 0 (raster diffuse and hemisphere zeroed) - fixed in `BuildImportedPbrModelData`.
- S6b stage surface packs in PT / offline GI (`TakesPtPack` for stage parts).
- S8b screens as raster / RT area lights (`screen_light.hlsli`, GPU average colour `screen_light_avg.hlsl`). Review
  fixes: Store4 value-cast (average was 0), polygon-irradiance winding sign, reflection uv orientation.
- S9b-1 Physical Sky light (Preetham; `render/PhysicalSky`, `shaders/physical_sky.hlsli`). Review fixes: chromaticity
  y + 0.012 (mauve noon horizon), sky-owned sun capped at MMD's 0.6.
- S9b-2 the sky as a light: SH ambient (raster / RT), environment NEE + MIS (PT / GI) from a 128x64 CDF
  (`render/SkyEnv`). Verified unbiased: PT / GI means with the sky sample on vs off agree.
- S9b-3 image skies: .hdr / png / jpg / RGBA8 dds incl. Ray-MMD RGBT domes and cube maps (`render/SkyMap`,
  `SkyMapRadiance` prototype per bindless table). Review fixes: `UploadBatch::CreateTextureRaw` copied from a
  moved-from staging buffer (device removed), a dark sky-owned sun cast a ghost shadow.
- S9b-4 procedural clouds (keyable coverage / opacity / softness / height / size / wind). Review fix: cloud lighting
  recalibrated (clouds drew dark and bluish; the self-shadow step re-sampled the same point under a high sun).
- GitHub issue #5 answered; its suggestions are rows U1..U8 in the milestone table (not part of this run's goal).
- Delegate skills (opencode + antigravity, published to digital8150/claude-delegate-skills): no over-exploration
  guard, Claude inspects workers every 15 minutes and intervenes at the first sign of wandering; 60-min timeout;
  antigravity auto-picks the Gemini flash family first (gemini-3.8-flash-high).

### Open / not verified
- RESOLVED (2283d5e): the vanished library_result.json was a worker's `rm -f` to force a rescan; a short run cannot
  rebuild it (a scan cancelled by the quit is never saved). run_capture's refusal and CLAUDE.md now say so.
- RESOLVED (2283d5e): `TransientDescriptors: window exhausted` = the per-frame window (512) overflowed on path-traced
  MMDX stage frames with haze + DoF (440 steady, 512+ on the first frame); not sky-specific. Window raised to 768.
- S8b: one average colour per screen (a two-colour image lights the floor with the mix); raster / RT get no
  directional shadow from an image sky's own sun; clouds cast no ground shadows (the sun only dims with coverage).

## 2026-10-27 (2) - Forward+ flicker, haze range, per-pixel spot beams, release 3.0.0
- Bug (user: "캐릭터가 막 반짝거려", lights flicker): `light_cull.hlsl` cleared the groupshared light mask (threads 0..7)
  and ORed light bits with no barrier between; on 32-wide GPUs the second wave (lights 32..63) could set bits before the
  first wave cleared them, so lights dropped out of clusters at random per frame. The arena (46 lights) hit it; screenshots
  calmed it (they stall the GPU = timing-dependent). Fix: one `GroupMemoryBarrierWithGroupSync` after the clear.
  Measured before: paused frame, body mean 186..194 between shots, background stable; the user confirmed the fix by eye.
- Haze range: without a stage radius the haze sphere grows to hold every punctual light with a haze share (cap 1000;
  the arena rigs hang ~375 out, outside the old 180 sphere), plus a "안개 범위" slider (0 = auto, ini `volumetricRange=`).
- That exposed the froxel grid's XY resolution (160 x 90: thin far beams = 8 px staircases). New per-pixel spot beam pass
  (`volumetric_beams.hlsl`, `VolumetricPass::ExecuteBeams`): IntegrateVolume with `VOL_SPOTS_ONLY` at full render res,
  temporal 0.15, `lit + L`; the froxel injector skips spots (`gP3.x` bit 1). Worker: opencode deepseek 1017 #low, 6 min,
  no review fixes needed. A/B (same camera, `--dof 0`, pinned 1600x900; old = runtime shader edit): staircases gone,
  +1.1 ms GPU on the arena (55.5 vs 54.4). Pitfall: my first A/B camera had no beams in either version; pick a view
  where the old build shows beams before judging the new one. NOT verified: fast camera motion trails, RT / DLSS paths.
- Open: DLSS edge flicker on silhouettes of a paused frame (upscaler none = stable) not re-measured after the flicker fix.
- Release 3.0.0 (major: MMDX stages). Notes `dist/notes_3.0.0.md` approved by the user in Korean first, then EN / JA / ZH;
  cover `docs/media/stages-3.0.0.jpg` (the user's studio screenshot). Order per the user: site first (stage gallery was
  404: S13 built but never deployed), then server upload, then GitHub.
  - Stage zips are served from the site server (`/stage-files/`, nginx alias outside html/, immutable, Range = resumable;
    conf backup `*.pre-stage-files.bak`); `STAGE_CDN_BASE` in the web repo's git-ignored deploy.env.
  - Sakura 1.2.0 published to the gallery (web repo `shader-packs/sakura`, `minAppVersion` 3.0.0, authors MMDX12,
    "Built-in" wording removed, ja typo), previews re-shot with `pack_previews.py --only sakura`.

## 2026-10-27 (3) - Release 3.0.1: studio stage param key editing
- Bug (user screenshot: stage "키 · 필 · 림 조명" rows): with a stage part selected the timeline shows that stage's
  param rows, but `StudioTrackOfRow` rejected every StageParam row while `selectedModel >= 0`, so key delete, the
  stopwatch, prev / next key and the key diamond did nothing. Now allowed for the camera target and for a selected part
  of that same stage.
- Bug: `StudioRowStopwatch` / `StudioRowKeyNav` compared the "uid:param" track name with `EffectParamTrack::param`
  (effect, world and stage rows): the stopwatch never turned off (it added a bogus "uid:param" track) and prev / next
  found no key. Both parse the param name now; `CaptureTrack` keeps the full track name.
- Shipped the pending `Renderer::ReleaseVideoLayers` fix too (`feedResolveTried_` reset: stage camera screens stayed
  black after leaving a scene / the studio).
- Release 3.0.1 (patch): notes `dist/notes_3.0.1.md`, `package_release.ps1` (196 / 16.2 MB), packaged smoke (select
  screen, v3.0.1, 0 `[E]`/`[W]`; a fresh install's cold shader compile needs `--first-frame-timeout` > 60 s), leftovers
  removed, delta manifest 3 new objects uploaded (live manifest hash matches), GitHub v3.0.1, website deployed
  (`/latest.json` -> 3.0.1 with manifest).
- NOT verified in the app by hand: the stage row key edits themselves (build + code path only).

## 2026-10-28 - Release 3.1.0: studio workflow (issue #5 + timeline audio lane)
- Shipped the issue #5 merge (settings popup, English bone / morph names, library list view, studio bone tools, wind
  physics) with the uncommitted follow-up: the waveform moved from the ruler background into a fixed audio lane
  ((peak, rms) at 200 buckets / s, clip drag = offset snapping to the playhead / frame 0, Shift free, one
  `AudioOffsetCommand` per drag, right click = the audio menu shared with the outliner row; any offset change re-seeks a
  playing track), and the real-time video ETA / probe from the measured frame period (120-image window) instead of the
  CPU record time. Added the missing JA / ZH strings for the lane.
- Release 3.1.0 (minor): notes `dist/notes_3.1.0.md` (4 languages, credits WishNyaGL), cover
  `docs/media/studio-audio-3.1.0.jpg` (studio 1920x1080, dark, audio lane, Sour Miku White), `package_release.ps1`
  (196 / 16.3 MB), packaged smoke (select screen, v3.1.0, 0 `[E]`/`[W]`, leftovers removed), delta manifest 3 new
  objects uploaded (live manifest hash matches), GitHub v3.1.0 (digests = local zips), website deployed
  (`/latest.json` -> 3.1.0 with manifest).
- Mistake: the first cover shots used 01_9001 (Sekai) and then IA; screenshots that are shown use Sour Miku White
  (`--character sour_miku/White`), as the README / earlier covers did.
- NOT verified by hand: the clip drag, the audio menu and undo of the offset (build + one studio capture only).

## 2026-10-28 (2) - Release 3.1.1: inverse-square falloff; practice_room 1.0.1
- Bug (user: practice room spots invisible / room dark with "역제곱"): `PunctualFalloff`'s inverse_square was a raw
  1 / d^2 in MMD units (intensity 1.75 -> ~0.002 on a wall 27 units away), so every inverse_square light in
  practice_room and color_cove was effectively off (color_cove's cyc rendered grey). Now normalised: intensity = the
  brightness at 25 units (2 m) for every curve, `(25 / max(d, 12.5))^2` (near-source peak capped at 4x); documented in
  docs/stage_format.md. One function, so raster / RT / PT / GI / haze all follow. A/B: None vs new inverse_square nearly
  identical on practice_room; color_cove now shows its pink cyc wash.
- Bug (user: floor black blotches in raster and RT): practice_room's dark exterior shell slab had its top face at Y = 0,
  coplanar with the wood floor (z-fighting; RT hit either). glb patched in place (4 vertices -1.6 cm), build.py moved
  the shell to Y [-1.2, -0.2] (MMDX12_Stages d8357c9), stage 1.0.1. Pitfall: the user's live view at 3.1.0 also had a
  strong haze in my build's ini (volumetric gain 4): it shows with None too, not a falloff effect.
- Release 3.1.1 (patch): notes `dist/notes_3.1.1.md`, package (196 / 16.3 MB), packaged smoke (select, v3.1.1, 0
  `[E]`/`[W]`, leftovers removed), delta manifest 4 new objects uploaded (live manifest + exe hashes match), GitHub v3.1.1
  (digests = local zips), website deployed (`/latest.json` -> 3.1.1); stage gallery: practice_room-1.0.1.zip uploaded to
  `/stage-files/` (hash matches), index lists 1.0.1 (other zips byte-identical to the server's).
- NOT verified: the in-app gallery "update" flow for practice_room 1.0.0 -> 1.0.1 on the user's install.

## 2026-10-29 - Release 3.1.2 (patch): RT mirrors, overlay AO, short video export
- Shipped since 3.1.1: RT mirrors at full resolution with punctual lights at reflection hits (1bd15fb), translucent
  character overlays keep the AO below (PR #6) and short video exports no longer delete their MP4 (PR #7), both by
  quasiquiescent (credited in the notes), and the MmeCompiler.h include-order build fix.
- Release: notes `dist/notes_3.1.2.md` (4 languages), package (196 / 16.3 MB), packaged smoke (select, v3.1.2, 0
  `[E]`/`[W]`), delta manifest 3.1.2 = 7 changed files (3 exes, composite / mmd / rtreflect / ssr .hlsl) uploaded (live
  manifest + exe hashes match), GitHub v3.1.2 (digests = local zips), website deployed (`/latest.json` -> 3.1.2 with
  manifest).
- Pitfall: the smoke run leaves `pack_guard.json` (PackCrashGuard marker) in the stage dir besides cache / ini / log;
  the first make_update run picked it up as a new file. Delete it with the other leftovers before `make_update.py`
  (its orphan object stays local only, not uploaded).
- NOT verified by hand: the RT mirror look in the packaged build (only the dev-build checks of 1bd15fb).

## 2026-10-29 (2) - Studio: arm rest-pose correction, ribbon, UI audit; AMD device hang fixed (issue #8)
- Arm rest-pose correction (6b84357): `library/motion/Romeo And Cinderella` (Project Sekai rips) is authored for a
  T-pose rig, so A-pose models' arms sink into the body. `studio/RestPoseRetarget` detects it (median upper-arm angle
  on the 38 deg reference >= 78; Romeo 82..87, every other library motion <= 73) and rewrites the arm keys (腕: R then
  key; subtree: R key R^-1, translations by R^-1; un-keyed 腕 gets a frame-0 key), exact per `restpose_retarget_test`
  (2e-6). VMD import asks before merging (modal "보정하고 불러오기 / 그대로 불러오기"); model menu / ribbon for manual.
  Visual A/B at frames 0 / 900 / 2700 on Sour Miku White: natural poses after correction. Play mode untouched.
- Studio ribbon (87d48e0) replaces the top bar (studio only): header row + tool row of groups per tab
  (홈 / 애니메이션 / 카메라 / 조명 / 렌더 / 보기), Ctrl+F1 fold, tab + fold in the ini, `studioribbon` script command.
  Worker built the structure; review fixed an early return inside the tool-row child (ImGui stack imbalance on
  new / open / recent: now deferred `post`), replaced an invented 3-way "light source" with VMD 연동 + presets,
  de-duplicated icons. Design (Claude, impeccable + DESIGN.md "Studio ribbon"): ink labels / ink2 icons, carets on
  popup buttons, separators between groups only, one sliding tab underline.
- UI audit (af5c1a9), each claim checked first: ink3 contrast 3.65 / 4.39 -> 5.05 / 4.95 (token change, app-wide),
  duplicate panel captions removed, dock window-menu triangle off, bone pick switches the inspector to 본 and scrolls,
  light delete at the bottom, camera rows named 가이드 / 화면 비율, 물리 시뮬레이션 instead of 물리 끄기, viewport
  renderer moved from the transport into the viewport overlay, studio toasts below the overlay row and click-through
  (a toast over the viewport blocked bone picks). Not changed (judged fine): auto-key switches next to key buttons, the
  camera group's inline add buttons. ja / zh strings by a worker.
- Issue #8 (37f2a73, pushed): AMD RX 6650 XT device hung with MMDX stages + volumetric. Reproduced on this laptop's AMD
  iGPU (`MMDX12_ADAPTER=AMD`, driver 32.0.21045.16010). Bisected: froxel only, then the light loop, then `GoboFactor`.
  Cause: sentinel-guarded bindless index (`gGoboParams.x = -1`, early return) whose descriptor fetch the AMD compiler
  hoists above the branch -> index 0xFFFFFFFF outside the heap. Fix: `SafeSrv` / `SafeSrvU` (common.hlsli) on every
  such site (antigravity static audit found ~15). Also fixed: feeds / mirrors drew the sky with the MSAA PSO into 1x
  targets. Verified AMD arena_live / practice_room / color_cove + NVIDIA debug layer 0 errors. Diagnostics kept:
  `--debug` logs D3D12 messages, `MMDX12_GBV=1`, `MMDX12_ADAPTER=<substring>`. Issue commented twice (progress, cause).
- Mistakes: launched a diagnostic window while the ribbon worker needed the app (the worker killed it; user: don't fight
  the worker); `git checkout -- .` in the diag worktree wiped the sky fix / env switches (re-applied on master);
  scripted clicks used pre-ribbon coordinates (the ribbon is 64 px taller) and opened the audio menu.
- NOT verified: `neon_plaza` on AMD (not installed here); the fix on the reporter's RX 6650 XT; ribbon at high DPI.
- Leftover: `.worktrees/hang` folder could not be deleted (a process holds it); branch already deleted.

## 2026-10-29 (3) - PRs #9 / #10, i18n check, CI, release 3.2.0
- PR #9 (quasiquiescent, parallel pass pipelines + background offline GI renderer): reviewed every `CreatePipelines`
  for `ctx` / statics, the shader cache, thread-local DXC, locked `DescriptorHeap`, the offline task's object-only
  `Initialize`. Measured warm first frame 7.6 s (master) -> 4.6-5.3 s, passes 1.1 s on 16 threads. Two early runs of
  the first merged build took 23 s in Scene (warm cache, no slow compiles); not reproducible afterwards (1 vs 16
  threads both fast), likely the driver's PSO cache after a rebuild.
- Regression found in the packaged smoke (user saw "응답 없음"): `Renderer::Shutdown` waited for the background
  offline creation on the main thread, so quitting within ~85 s of a cold start froze the window ~80 s. Fixed
  (8a95e9b): `OfflineRenderer::Initialize(..., cancel)` checks a flag before each slow shader, Shutdown sets it, and
  App hides the window before `Renderer::Shutdown`. Packaged cold quit 132 s -> 40.5 s, window hidden meanwhile.
- PR #10 (light display names in the UI language) merged; follow-up per the user: `tools/i18n_check.py` runs in the
  build (CMake `i18n_check` ALL target): every `Tr` / `TrKey` key needs En / Ja / Zh, duplicates / printf mismatches
  / empty translations fail. `TrKey()` marks labels reaching `Tr` through variables (the ribbon tabs were invisible to
  a literal scan). Ja / Zh fall back to English. 66 missing keys filled (worker, opencode deepseek; resumed once after
  it stopped mid-cleanup; Claude fixed `%%` / `*` handling in the printf check and Zh 부품).
- CI (`.github/workflows/ci.yml`): windows-2025 hosted runner, MSVC + Ninja build (incl. i18n_check), 19 CPU-only
  tests, run_capture selftest, `pack_check --compile` of the built-in packs; ~10 min. Not note20: it is a Linux
  chroot (cannot build) and a self-hosted runner would execute fork PRs on the user's server. The first run caught a
  stale `gallery_index_test` expectation (world packs compatible since 2.5.1) and `mme_parse_test` needing
  `--selftest`.
- Issues: #11 answered (Korean stays the source language; the check + English fallback + filled keys address the
  effects; the first reply wrongly claimed most users are Korean, corrected after the user pointed it out). #8: fix
  shipped in 3.2.0, asked the reporter to confirm.
- Release 3.2.0 (minor): notes `dist/notes_3.2.0.md` (4 languages, credits quasiquiescent / zhangyan453), cover
  `docs/media/studio-ribbon-3.2.0.jpg`, package (196.1 / 16.3 MB), packaged smoke (v3.2.0, 0 `[E]`/`[W]`, cold quit
  check, leftovers removed), delta manifest 16 new objects uploaded (manifest + exe hashes match live), GitHub v3.2.0
  (digests = local zips), website deployed (`/latest.json` -> 3.2.0 with manifest).
- NOT verified: the fix on the reporter's RX 6650 XT; ribbon at high DPI; Ja translations of the new keys by a
  native speaker.

## 2026-11-05 - Issue #12 (studio renders follow the effects panel), release 3.3.0
- Issue #12 (zhangyan453): a studio Raster video was darker than the raster viewport and had no DoF. Root cause (the
  user's framing): the studio reused the play-mode render dialog, so effects lived in two places and the studio's
  effects panel was ignored (dialog `VideoRenderConfig` effects; `VideoRealtimeSettings` forced shadows / SSAO / SSR on).
  Fix (feccd95, Sonnet subagent worker from Claude's spec, reviewed): `ActiveVideoConfig` copies the panel's bloom /
  volumetric / DoF for studio jobs (every renderer, CLI overrides still win), the real-time path keeps the panel's
  shadows / SSAO / SSR in the studio, and the dialog opened from the studio shows a note + "효과 패널 열기"
  (`studioFocusEffects_`) instead of effect switches. E2E: ini ssao=0 / videoDof=0 + panel DoF on -> the raster video
  frame of `captures/mvd_e2e/vmd_test.mmdxproj` at 20 s has the viewport's DoF; dialog captured. Not measured: an
  old-vs-new SSAO pixel A/B.
- Worker policy changed by the user: workers are Haiku / Sonnet subagents by difficulty; opencode / antigravity rules
  removed; Codex only when the user says "코덱스" (memory updated).
- Release 3.3.0 (minor): MVD import (5b6f19a), RTAO in motion (cc01bf7), float studio camera FOV (fcf90cb), #12, and the
  pending stage-screen no-AO line in mmd.hlsl (19be992). Notes `dist/notes_3.3.0.md` (4 languages, credits zhangyan453),
  no cover image. Package 196.1 / 16.3 MB, packaged smoke (select, v3.3.0, 0 `[E]`/`[W]`, leftovers removed), delta
  manifest = 6 changed files (3 exes, mmd / rtao / ssao .hlsl), 6 objects + manifest uploaded and fetched back over HTTPS
  (hashes match), GitHub v3.3.0 (digests = local zips), website deployed (`/latest.json` -> 3.3.0 with manifest), #12
  answered (left open for the reporter to confirm).
- Pitfalls: a Python-written object list had CRLF line ends, so `tar -T` found no files (strip `\r`); the CDN returned
  transient 522 / 525 on some object fetches during the check (files were fine on the origin; retries returned 200).
