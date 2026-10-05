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
