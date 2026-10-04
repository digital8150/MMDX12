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
