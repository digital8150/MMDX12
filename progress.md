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
