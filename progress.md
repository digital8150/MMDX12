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
