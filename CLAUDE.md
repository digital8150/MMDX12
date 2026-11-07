# CLAUDE.md

MMDX12: a Windows Direct3D 12 MikuMikuDance client (C++20). See README.md for features and CLI;
progress.md is the session log. Read its latest entry first.

## Build & verify
- `build.cmd [dir] [--target X]` sets up the VS 18 environment and builds with CMake + Ninja (RelWithDebInfo).
  The default dir is `build`; the output is `build\bin\MMDX12.exe`. CMake and Ninja come from pip (`%USERPROFILE%\miniconda3\Scripts`).
  From bash: `cmd //c "F:\\repos\\MMDX12\\build.cmd build"` (the drive letter changed F: -> D: -> F: on 2026-10-23; a build dir configured
  on the old path fails with "CMakeCache.txt directory ... is different": delete its CMakeCache.txt + CMakeFiles).
- The source lists are globbed (`CONFIGURE_DEPENDS`). If a new `.cpp` is not picked up, delete the build dir.
- Header dependencies come from cl's localized `/showIncludes` prefix stored at configure time; a dir configured from a
  UTF-8 console stored it in the wrong code page (no deps: header edits rebuilt nothing, stale-layout crashes).
  build.cmd detects that and reconfigures once.
- Shaders are compiled at runtime from `build/bin/shaders` (the `copy_shaders` target keeps them in sync).
  Raster shaders use FXC (`*_5_1`), except the lit / floor scene PSOs (mmd.hlsl VSMain/PSMain, VSFloor/PSFloor): DXC
  `*_6_0` on SM 6.0 devices because FXC needs ~5.5 min for PSMain since the PBR branch (FXC stays their fallback; a PSO
  never mixes DXBC and DXIL). Slow cold compiles (> 1 s) log a `shader compile` line. Ray-query shaders use DXC (`*_6_5`, `-HV 2018` because common.hlsli uses vector ternaries that HLSL 2021 rejects). `dxcompiler.dll`/`dxil.dll` are copied from the Windows SDK.
- Startup: `Renderer::Initialize` creates the passes' pipelines on one thread per logical processor, at most one per
  pass (one DXC compiler per thread, `DescriptorHeap` Allocate / Free locked; `STARTUP renderer passes` log line), and
  leaves the offline GI renderer to a background thread (`STARTUP offline renderer (background)`). `OfflineSupported`
  counts it as available meanwhile, `OfflineReady` says it is done: a pending GI job and the studio's GI viewport wait
  for that, `BeginOffline` and `Shutdown` block on it (`EnsureOffline`). Re-creating the pipelines for another MSAA
  count (`Render`) stays serial: a pass may then release resources through the context (`WaitForGpu`, `DeferRelease`).
- Upscaler runtime DLLs are git-ignored: run `tools/fetch_sdks.ps1` once (DLSS 310.9.16, XeSS 3.0.2, FidelityFX 2.3.0), then rebuild. Missing DLLs only disable that upscaler. The script checks their SHA-256 (a new SDK version updates its list).
- Release channels: stable = the tagged releases. Nightly = `.github/workflows/nightly.yml`, one build per master
  commit that passed CI (`tools/package_release.ps1 -NightlyBuild <n>`: version = next minor of CMakeLists' + `-nightly.<n>`,
  e.g. `3.4.0-nightly.12`; CMake `MMDX12_NIGHTLY_VERSION` -> `MMDX12_NIGHTLY`). The channel is fixed at build time
  (`updater::kNightlyBuild`, `kDefaultFeedUrl`), so another channel = another folder. A nightly cannot submit to the
  leaderboard, tells a new install once that its projects may not open in a stable version, and never fetches optional
  components (its zip has them). Zips: the `nightly` prerelease (the tag stays put; the feed's `commit` says which build
  is live). Update store: `/updates/nightly/` on the site.
  Security model: nothing a user installs may depend on the update server being honest. Every feed is signed away from the
  server (`core/FeedSignature.h`, RSA-PSS over the exact feed bytes, one key per channel, the public half in
  `updater::kFeedPublicKey`), the exe reads only a feed that verifies, and it refuses a feed without the hashes: they cover
  the zip, the manifest and every object. A lite install's component fetch checks the installed version's manifest
  against the signed feed's `manifests` map (version -> hash), so that map keeps every version out there. Old versions
  read the plain fields next to the signed bytes. Nightly signing: the publish job (secret `NIGHTLY_SIGN_KEY`). Stable signing:
  the website repo's `sign-feed` workflow (its own key, secret `STABLE_SIGN_KEY`; a new key also changes `PUBLIC_MODULUS` there and
  `kFeedPublicKey` here; losing it means restoring the secret from the offline copy). Nightly build job: no
  secrets, no compile cache, hash-pinned downloads. Publish job: runs no repository code, actions pinned to commits, deploy
  environment limited to master. The gate job only passes a master push by the repository owner (`workflow_run` also
  fires for a fork's branch named master) and sends a range holding anyone else's pull request to the `nightly-review`
  environment for approval. It takes the range's start from the `nightly` release's notes (written last by the publish
  job), never from the update server, and a missing start means review. Keep server names, paths and accounts out of this repository, the workflow and its logs: they
  live in Secrets and in CLAUDE.local.md.
  Stable release order: `package_release.ps1 -Version x.y.z` -> `make_update.py` + upload the store -> GitHub release -> in
  the website repo commit the manifest hash in `src/data/update_manifests.json` -> run its `sign-feed` workflow with the version
  (it refuses a feed the app would refuse, or a key that is not the app's, and commits `src/data/update_signature.json`) ->
  pull + deploy (the site's build fails when the signature is not for the latest release). The zip and store go first:
  the signed feed that names them must never be live before them.
  Tests: `tools/update_e2e.py` / `tools/components_e2e.py` run a real exe against honest and lying stores; they need a
  build that trusts a throwaway key (`-DMMDX12_FEED_TEST_KEY=`, `tools/sign_feed.py new-test-key`, see update_e2e.py's
  header), which `package_release.ps1` refuses. `updater_feed_test` (CI) checks what `FetchFeed` accepts.
- Render path / upscaler from the CLI: `--render raster|rt|pt`, `--rtao-rays <n>` (1..64), `--studio-renderer raster|rt|pt|gi` (studio viewport: lit single view), `--upscaler none|dlss|fsr|xess`, `--upscale-quality native|quality|balanced|performance|ultra`,
  `--dlss-model auto|cnn|transformer|transformer2`, `--dlss-rr 0|1` (DLSS Ray Reconstruction on the RayTraced / PathTraced
  paths with DLSS; it replaces the RT AO / reflection filtering and the PT denoiser, and runs instead of TAA),
  `--frame-gen off|2|3|4` (DLSS Frame Generation; 3x / 4x need RTX 50).
  DLSS is raw NGX 310.9.16 (no Streamline): SR + RR in `render/UpscalerDlss.cpp` (model = render preset hint: CNN E,
  transformer K, transformer 2nd gen M / L for Ultra Performance), FG in `render/DlssFrameGen.*`, one shared NGX init
  (`render/NgxShared.*`). RR guides: PT writes them from the tracer; RT draws them in `ScenePass::DrawGuides`
  (`PSGuides`) only while RR runs. The two albedo guides alpha-blend by coverage (translucent face overlays written opaque
  became the face albedo: dark "panda eyes"); the normal guide stays opaque (w = RR's packed roughness).
  FG: hudless = the scene before ImGui, back buffer after; `Dx12Context::EndFrameFrameGen` presents generated frame(s)
  then the held real frame. Play scenes only: off in the lobby, studio, offline renders, benchmarks, headless and
  `--capture` runs. DLSS 5 (neural rendering) has no public SDK yet (2026-10).
- PBR material data (2.0.0): per-character material source (기본 / Ray-MMD, saved as `characterMaterial=` ini lines and
  `.mmdxproj` "material"), `--material-source default|ray` overrides it for a run, and
  `--pbr-debug none|albedo|normal|roughness|metalness|ao|emissive|model` shows one channel (`asset/PbrMaterial`,
  `shaders/pbr_common.hlsli`). Imported glTF/GLB/VRM/FBX models fill the same block from their PBR maps
  (asset/ModelImport `ImportedPbr`, `BuildImportedPbrModelData`); VRM / MToon / KHR_materials_unlit stay NPR (no record).
  S2a adds the glTF material extensions to the record (transmission / ior / volume, HDR emissive strength, clearcoat,
  specular / sheen stored only, KHR_texture_transform) and imports KHR_lights_punctual as `ImportedLight`s
  (`asset/StageLights`, a stage's `"gltfLights": true`). S2b (`shaders/dielectric.hlsli`, `RtGeom_Transmissive`):
  transmission > 0 is a mesh dielectric — raster / RT blend with `alpha = 1 - transmission * (1 - Fresnel)` and report
  the Fresnel as reflectivity, RT / PT / GI refract (thin straight through, a volume with Snell + Beer-Lambert, rough
  interfaces GGX-perturbed), and shadow rays through it return a tint (`TraceShadowRayTinted`) instead of stopping.
- MMDX stages (docs/stage_format.md, `asset/StageManifest.h`): a folder with a `stage.json`. Play mode loads its parts
  through `LoadScenePackage` and the studio through `LoadStudioStage` / `LoadStudioPackage` /
  `LoadStudioProjectPackage` (manifest order, not the library's flat part list); the manifest's `materials` rules are
  applied on the loader thread (`asset/StageMaterials`), and `app/StageRuntime` evaluates the stage every frame at the
  song time: `lights.*` targets feed `BuildSceneLighting` (play mode's lighting picker "스테이지 조명", the scene default
  when the stage has lights) and `materials.*` targets go through `GpuModel::SetPbrOverrides`. Installed stages live in
  `<exe>/stages/<id>/` with the id prefix `stages:`; the lobby saves the variant as `stageVariant=<id>|<stage id>` and
  `--stage-variant <id>` overrides one run (`studioaddlib stage <substr>` works for them, `studiostate` logs
  `stageVariant=`). In the studio an MMDX stage is ONE entity (`studio::StudioStage`: the parts still load as models,
  `StudioModel::stageUid`) whose variant, param base values, keyed params (`RowKind::StageParam`, category "스테이지") and
  imported cue tracks are saved in the project ("stages", format version 4) and never in the installed stage; its rig
  lights are scene lights tagged with the stage and are replayed every frame (editor and offline), the fields a binding
  drives are read-only in the light inspector. Script: `studiostageparam` / `studiostagekey` / `studiostagecues`,
  `studiostate` logs one `STUDIOSTAGE` line per stage.
- Stage gallery (S12, docs/stage_format.md "Distribution"): `app/StageStore.*` (the shader-pack store's rules: index
  sources, SHA-256, revoked, `minEngine`) fetches `stages/index.json`, downloads a stage zip **resumably**
  (`net::DownloadToFileResumable`, `<exe>/cache/stage_downloads/<id>-<version>.zip.part`, `Range: bytes=` + 206 / 200 /
  416 handling) and installs it into `<exe>/stages/<id>/` through `InstallStageZip` (extract into
  `<exe>/stages/.incoming/`, one `stage.json` with `"format": "mmdx-stage"` and the index's id, rename into place; an
  update keeps the old folder until the new one is in, restores it on failure). `Screen::Gallery` + `app/UiGallery.cpp`
  (top nav "갤러리", card grid + detail pane, `--screen gallery`, `--gallery-index <url|path>` for one run; online stages
  only so far) uninstalls by deleting the folder (refused while a scene / the studio uses it) and calls
  `StartRevalidation()` after every install / update / uninstall so the lobby's stage list updates without a restart.
  `tools/stage_store_test` covers the index parse, the install core and the resume decision seam.
- Stage screens (S5, docs/stage_format.md "screens"): `render/StageScreens.*` owns one texture per screen (owned by the
  Renderer, updated before the scene passes) and binds it to the materials the screen claims through
  `GpuModel::BindMaterialTexture`, marked `MatFlag_Screen` (the texture is the emission; raster / RT / PT / offline GI
  read it via `ScreenEmission` in `shaders/common.hlsli`). Sources: `none` (black), `image` (decoded once),
  `video` (`render/VideoTexture.*`, one decoder per video screen, exact frames offline), `camera` (the scene
  re-rendered from that camera into the screen before the main scene: raster lit, own constant-buffer / light slots,
  no TAA / upscaler / DoF / volumetrics / effects / MME / packs, at most 960x540, two feeds per frame round robin) and
  `program` (S6: a bundled screen pack, `screen.hlsl` -> `PackScreen`, drawn into the screen texture once per frame by
  its own fullscreen PSO — see "Shader packs" below). The app fills `FrameView::screens` (`ScreenView` per screen per
  part) from `app/StageRuntime` (play) / `StudioStageScreens` (studio, `camera` may be `uid:<n>` = an S4 studio camera);
  the studio's stage panel writes per-screen overrides into the project (`"screens"`, version 6) and the ui-script
  command `studioscreen <stage index> <screen id> <none|image|video|camera> [file|camera]` sets one (`studiostate` logs
  one `STUDIOSCREEN` line per screen). S8b makes each screen an area light in raster / RT (`Renderer::UpdateScreenLights`,
  `shaders/screen_light.hlsli`: one rectangle per screen material fitted at load, axes oriented by the material's uv;
  diffuse colour = the screen texture's average from `shaders/screen_light_avg.hlsl` (one 16x16 group per light into a
  raw buffer, `screenLightParams.z`), one-sided polygon irradiance (sign oriented by the axes' handedness) + a
  representative-point specular that samples the texture, at most 16 per frame; off with
  `RenderSettings::screenLights` / ini `screenLights=0` / `--screen-lights 0`; PT / offline GI keep S8a's NEE).
- Planar mirrors (S10, docs/stage_format.md "Planar mirrors"): a stage material rule's `"mirror": true` sets
  `MatFlag_Mirror` (`render/ShaderInterop.h`, `shaders/common.hlsli` / `rt_common.hlsli`). `GpuModel::Create` fits one
  plane per mirror material once from its rest-pose vertices (area-weighted centroid + area-weighted vertex normals,
  coplanar within 1 % of the material's extent; otherwise the material shows its tint only) and exposes it through
  `GpuModel::MirrorPlanes()` (model space; the frame's root transform makes it world space). Raster only,
  `Renderer::UpdateMirrors` (called from `RecordScene` after the S5 feeds, so a mirror sees this frame's screens) keeps
  the at most two largest planes that face the eye and touch the view frustum, `EnsureMirrorTargets` creates the
  RGBA16F reflection targets at half the render resolution on the first mirror frame (released in `Shutdown`, recreated
  on resize), and `RenderMirror` draws the scene from the reflected camera (own constant-buffer / light slots,
  `SceneConstants::clipPlane` discards geometry behind the mirror, `mirrorParams.x = 0` so mirrors inside it show their
  tint). The scene PS samples the reflection in `MirrorReflection` (`shaders/pbr_common.hlsli`) by the pixel's own world
  position projected through `mirrorViewProj[slot]`, tinted by the base colour and blurred by the roughness. RT / PT /
  offline GI reflect a mirror hit as a delta event (tint weight, cone lobe when rough; `prevBsdfValid = false`); quad /
  ortho views, feeds and `RenderToImage` show the tint only. No mirror material = no textures, no extra render, identical
  frame.
- Stage draw cost (neon_plaza 45 -> 21 ms, RTX 4070 Ti): screens with the same camera + size are one feed (`StageScreens`:
  it renders once, the others `CopyResource`; `kMaxFeedsPerFrame` counts distinct cameras). A static stage (one bone, no
  vertex morph, >= 50k triangles) sorts its opaque materials' triangles by xz cell at load (`BuildDrawCells`,
  `STAGECELLS` log line; blending materials and material morph targets keep their order) and `GpuModel::DrawMaterial` draws only the cells inside a
  `CullFrustum` (sun cascades x / y planes, camera / feed / mirror views all six), since a baked stage's material spans
  the whole set and a whole-material test culls nothing. The raster camera view has a depth pre-pass
  (`ScenePass::DrawDepthPrepass`: `GpuModel::DepthPrepassMaterial`, no PS, DSV only; models with a surface pack or MME
  and materials with a bound screen / video texture skip it), which is why VSMain's clip position is `precise` (the two PSOs must rasterise the same depth).
- Spot gobos (S7b, docs/stage_format.md "`gobo`"): a spot may carry a projected stencil (`SceneLight::gobo` = a file
  path + `goboRotation` / `goboScale` / `goboSpin` / `goboFocus`; a stage light's path is relative to the stage folder and
  `app/StageRuntime` resolves it). `GpuLight` stays 112 B: the per-light gobo data is a second raw buffer
  (`GpuGobo`: texture SrvHeap index, rotation radians incl. spin x `FrameView::sceneTime`, uv scale, focus mip bias),
  uploaded per constant-buffer slot exactly like the light CDF (`Renderer::FillGoboTable`) and pointed at by
  `SceneConstants::goboParams.x` (-1 = no gobo in the frame: no fetch, the old image). Textures are decoded once per path
  (`asset/ImageLoader.h`, full mip chain) into the shared bindless `SrvHeap` and cached in `Renderer::gobos_`; a broken
  file is one `[W]` and the light shines without it. `shaders/gobo.hlsli` is the one projection helper (spot frame, uv =
  local.xy / local.z / tan(outer half angle) / scale, rotated, + 0.5; outside [0,1] = 0; the sample multiplies the
  light's rgb), used by the raster scene loops (`mmd.hlsl`), `pathtrace.hlsl`, `offline_gi.hlsl` (surfaces, skin, medium
  NEE) and the volumetric froxel injector / offline march (`volumetric_froxel.hlsl`, `offline_volumetric.hlsl`, the
  `VOL_GOBO` branch of `volumetric_common.hlsli`, +1 mip to keep the haze quiet). Out of scope: the legacy per-pixel march
  and the `world.lights` pack API.
- No debugger is installed (no cdb). For crashes, use the dbghelp `CrashFilter` in `tools/render_smoke.cpp`.
- Visual verification without a human: render a frame headlessly, then Read the PNG. First read "Running MMDX12.exe from the CLI" below.
  - Play scene: `MMDX12.exe --character <s> --stage <s|none> --song <s> --autoplay --seek <sec> --frames N --capture out.png`
    Unit formation: `--formation <spec>` (one entry per part in part order, each a character name substring or `-` for empty; optional `*` suffix marks the center; skips the modal).
  - Menu screens: `--screen select|bench --frames N --capture ui.png`
  - Offline GI still/video: `--autoplay --seek <sec> --offline-still out.png` (or `--offline-video out.mp4 --offline-range a b`);
    add `--offline-size 960 540 --offline-spp 256` for quick checks. The app quits by itself when done.
    Video format: `--offline-fps 30 --offline-bitrate 20 --offline-quality 0..3` (default: the saved lobby dialog settings, `AppSettings::video`).
    Lobby path (select screen -> load -> render -> back to select): `--character c --song s --screen video --offline-video out.mp4 --offline-range a b`.
    `--screen video` alone opens the lobby's render dialog for a capture.
    Video renderer: `--offline-renderer raster|rt|pt|gi` (the real-time ones render frame by frame at the video size via `Renderer::Render` +
    `ReadFinalImage`; PT accumulates several passes per frame). Video frames are pipelined: the real-time renderers
    queue each frame's final image into a 3-slot readback ring inside the frame's command list
    (`Renderer::QueueFinalReadback` / `PollFinalReadback`, one fence value per slot; a full ring falls back to the
    blocking `ReadFinalImage`), and `AsyncVideoSink` (app/VideoEncoder.h) encodes on a worker thread (queue of 4 =
    back-pressure; the alpha PNG sequence too). The window shows the exact encoded image every 5th frame
    (`Renderer::SetOfflinePreviewImage`, "미리보기 · 프레임 N / M"), not the live viewport. The render dialog measures its time estimate by itself: a headless sample render behind the UI (`--screen video` shows it; log `VIDEO PROBE`,
    results in `videoProbe=` ini lines). `--offline-probe` runs the same measurement from a play scene.
    The dialog's effects are `VideoRenderConfig` (bloom, convolution bloom, volumetric, DoF for every renderer: GI's thin lens
    follows `OfflineJobDesc::dof` / `dofAperture`, the render benchmark keeps the defaults); stills use the effects chosen at
    scene entry (`AppSettings`). In the studio the render dialog has no effect switches: studio videos and stills take the
    studio effects panel's settings (`ActiveVideoConfig` copies them; the real-time video path keeps the panel's shadows / SSAO / SSR
    instead of forcing them on). Check `build_dev` builds when `build/` is locked by a running render.
  - Studio: `--character <s> [--stage <s>] [--song <s>] --screen studio --seek <sec> --frames N --capture out.png`.
    `--screen studio` without `--character` opens a new empty project; `--project <file.mmdxproj>` opens a project.
    Studio render (quits when done): `--project p.mmdxproj --offline-video out.mp4 [--offline-range a b]` (timeline
    seconds; default: the timeline range, else the whole project; motion camera; project audio + offset) or
    `--offline-still out.png [--seek s]` (GI, the view the viewport shows). `--offline-renderer/-size/-fps/-spp` apply.
  - High-DPI captures: `--ui-scale 1.5` replaces the monitor's DPI scale (1080p window + 1.5 = a 150 % laptop screen).
  - Scripted UI input: `--ui-script file.txt` (lines `<frame> <command> [args]`: move/down/up/click/dblclick/wheel/key,
    `capture <png>`, `text <chars>`, `mod ctrl|shift down|up`, `studiostate` logs a `STUDIOSTATE` line,
    `mmeglobal 0|1` / `rayglobal 0|1` set the saved MME / Ray-MMD switches, `mmeworkflow auto|manual` the workflow,
    and `mmestate` logs a `MMESTATE` line (switch, Ray-MMD switch, workflow, per loaded model: id, resolved MME mode, resolved material ray 0|1),
    `fxpanel <open|close> [effects|shader|mme]` opens / closes the play-mode 화면 효과 side panel on a tab,
    `videorange <all|start end>` sets the lobby video render dialog's range (timecodes or seconds),
    `rendershutdown 0|1` / `rendershutdownanswer <cancel|now>` drive "완료 후 시스템 종료" (automated runs: a 3 s countdown and a `SYSTEM SHUTDOWN dry run` log line, never a real shutdown),
    `studioexport`/`studioimport <vmd>` and `studiovpdexport`/`studiovpdimport <vpd>` skip the file dialogs; pose editing:
    `studiobone <name>` clicks that bone's joint, `studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [steps]` drags a gizmo
    part; arm rest-pose correction is `studiorestpose <deg> [both|left|right]` (selected model, one undo step) and
    `studiorestposeanswer <accept|keep> [deg]` (presses the import modal's button); `studiostate` also logs `STUDIOPOSE`, `STUDIOPROJ` (including `aspect=`) and one `STUDIOMODEL` line per model). Projects/models
    without dialogs: `studionew`, `studiosave`/`studioopen <file>`, `studioautosave`, `studioadd <character|stage|prop> <file>`,
    `studioaddlib <character|stage> <substr>`, `studiosong <substr>`, `studioaudio <file|none> [offset]`,
    `studiobgvideo <file|none>`, `studiobgvideoopt <offsetSeconds> <asBackground 0|1>`,
    `studioselect`/`studioremove <index>`, `studioreplace <index> <file>` / `studioreplacelib <index> <substr>` (swap a model's file), `studiorename <text>`, `studiomaterial <default|ray> [emd path]` (PBR material source of the selected model), `studioattach <parent|-1> <bone|-> tx ty tz rx ry rz s`,
    `studioaspect <w:h>` (frame aspect preset, e.g. `9:16`, `16:9`, `2.39:1`), `studiorenderer <raster|rt|pt|gi>` (viewport renderer),
    `studioribbon <home|anim|camera|light|render|view>` / `studioribbon collapse 0|1` (ribbon tab / fold;
    `studiostate` logs a `STUDIORIBBON tab=<id> collapsed=<0|1>` line),
    `studiogravity <acceleration> [x y z]` / `studiowind <strength> [gust frequency jitter [x z]]` (the physics popup's
    undoable edits; `studiostate` logs a `STUDIOPHYSICS` line).
    Drag and drop: `drop <file>[|<file>...]` injects a drop path list (models → characters/props, `.vmd`/`.vpd` onto
    the selected / single new model, audio, `.mmdxproj`; `|` separates paths so they may contain spaces).
    Focus: `studiofocus <frame> <auto|target|manual> [model index|distance] [aperture] [transition] [head|upper|center]`
    upserts a focus key (`studiostate` logs a `STUDIOFOCUS` line: distance, aperture, subject).
    Lighting rig: `studiolight <vmd|preset|custom> [presetIndex] [spotCount] [keyOverride 0|1]` sets the studio's
    lighting source (+ demo spots / key override; `studiostate` logs them), `studiokeyspot <spotIndex> [frame]` keys
    that spot's values through the ordinary undoable insert.
    Effect stack: `studioeffect <add <pack id>|remove <index>|enable <index> 0|1>`, `studioeffectkey <index> <param key> <frame> <value> [stepped]` (`studiostate` logs `STUDIOEFFECT`).
    World entities: `studioworld <add <pack id>|remove <index>|enable <index> 0|1|place <index> tx ty tz rx ry rz s>`, `studioworldkey <index> <param key> <frame> <value> [stepped]` (`studiostate` logs `STUDIOWORLD`).
    While a script runs, real mouse/keyboard input is ignored.
    Frames count like `--frames`. Use it to click through editor features headlessly (examples: `captures/studio/t*.txt`).
  - `--frames N` counts frames only inside a scene or with `--screen`; a run with neither never quits.
  - Benchmark: `--benchmark dx12-raster-fhd --bench-frames 600 --frames 100000` (result goes to `build/bin/mmdx12.log` as a `BENCHMARK` line)
  - GI render benchmark: `--benchmark dx12-gi-render` (quits by itself; ~41 s). Quick check: add `--bench-spp 64 --offline-size 960 540`.
    Add `--frames 100000 --capture out.png` to capture the result screen.
  - Each run opens a visible window: never launch app runs in parallel or in batches without telling the user.
- Tools:
  - `asset_probe <library> [--full]`: scan, classify (prints notes on skipped/guessed files), and load everything
  - `anim_probe <pmx> <vmd> [cam.vmd]`: IK convergence, CPU skinning bounds, NaN scan, physics scan (explosions, cost)
  - `render_smoke`: renders a cube with no assets
  - `vmd_roundtrip <file|dir> [--vpd-selftest]`: VMD load -> save -> load comparison (SaveVmd), VPD self test
  - `pmm_probe <file.pmm> [--no-fuzz] [--fuzz-rounds N]`: PMM (MMD project) dump (models / per-track key counts /
    camera / accessories / audio / MME controllers) + a fuzz self test (truncate at every 1 KiB, random byte flips)
  - `rest_pose_test [model|dir...]`: T-pose -> A-pose self test (synthetic model: bones, weights, morphs, bodies, joints), then the arm angle of every PMX/PMD found
  - `restpose_retarget_test`: studio arm rest-pose correction (T-pose motion on an A-pose rig: FK exactness, un-keyed arms, detection, real library files)
  - `studio_edit_test`: Studio key-edit core (move/insert/delete frames, undo byte budget, 100k-key timings)
  - `studio_gizmo_test` / `studio_pose_test`: gizmo projection/hit/drag math and bone overlay; mirror names/poses, VPD pose ops
  - `studio_project_test`: .mmdxproj save/load round trip, VMD naming/cleanup, atomic writes, prop offset matrix
  - `pbr_tex_probe <pmx> [--dump <dir>] [--verbose]`: PBR (Ray-MMD) map decode per material: paths, channel, mip-0 size, loops, UV scale/flip
  - `gltf_pbr_test`: imported PBR records (`captures/assets_dl` BoomBox.glb maps / channels, Seed-san.vrm = no records; SKIP when absent; `tools/testdata/ext_test.glb` = the S2a glTF extensions + lights)
  - `spot_shadow_pick_test`: the spot shadow budget (reach test, score, hysteresis)
  - `studio_focus_test`: DoF focus track evaluation, automatic focus (score, hysteresis, rack), focus keys in projects
  - `mmdx12_mcp [--pipe <name>] [--pid <pid>] [--launch] [--timeout <ms>]`: Model Context Protocol stdio bridge
  - `mcp_smoke.py [--handshake-only] [--scenario]`: MCP test client (protocol handshake & end-to-end scenario)
  - `pack_previews.py [--only ids] [--out-dir d] [--bump]`: re-shoots every public pack's before/after preview over the pipe
    (shot table in the script; scene-only `screenshot`, `set_render_settings shader_texture_folder`; writes the website
    repo's `shader-packs/<id>/` and the built-in packs, contact sheet in `captures/pack_previews`)
  - `media_kit.py [--exe e] [--out dir] [--scene name] [--only v,...] [--size W H] [--clips] [--dry-run]`: regenerates the
    README / promo A/B media (classic MMD / raster / RT / PT / offline GI of one pinned frame, composed into `compare_2x2`,
    `compare_strip`, `compare_split` (classic | `--split-right`, default raster) + JPEG q90) in `captures/media_kit`;
    strictly sequential, backs up/restores `mmdx12.ini`. Captures use `--capture-scene 1` (the rendered scene only, no
    play bar / stats overlay; works for any `--capture` in a scene) and `--classic` (vanilla-MMD look for one run).
  - `make_update.py <stage> --version X --out d [--base-url u]`: delta-update manifest + content-addressed objects (+ .gz)
    for the smart updater (feed `manifest` / `manifestSha256`); `sign_feed.py`: prints a key's modulus, makes a test key, signs
    for the E2E tests; `update_e2e.py`: updater E2E (delta, corrupt-object fallback, a lying server);
    `components_e2e.py`: headless optional-component (app/Components.h) fetch E2E - the lite zip's upscaler DLLs / CJK fonts,
    fetched from the installed version's manifest via `--component <id> --download-component`, and a lying server
  - `mcp_pipe.py` / `mcp_x.py`: talk to a running app's pipe without the bridge (`python tools/mcp_x.py "click 플레이" "wait 30" "shot out.png" "items 셰이더"`)
  - `run_capture.py`: the only supported way to launch `MMDX12.exe` (pre-flight refusals + log watchdog + report;
    see "Running MMDX12.exe from the CLI")
- The play bar auto-hides while playing with no mouse movement, so captures usually don't show it.

## Running MMDX12.exe from the CLI (read this before EVERY launch)
Agents keep misusing the CLI and then blaming the app. These rules are verified (2026-10-12); follow them literally.
- **No `--autoplay` = no scene.** `--character/--song` only preselect cards in the lobby. Without `--autoplay` (or when
  either name does not resolve) the app sits in the lobby, and `--frames` never counts there (it counts only inside a
  scene or with `--screen`), so the run never quits. Every scene capture needs `--character <s> --song <s> --autoplay`.
- **Always wrap a run in a timeout and read `<build>/bin/mmdx12.log` afterwards** (overwritten every run;
  `mmdx12.previous.log` keeps the one before); `tools/run_capture.py` does both for you (it is the only way to launch).
  Scene reached = `motion bound` + `STARTUP first frame` lines; neither =
  it never left the lobby (bad `--character/--song`, multi-part song without `--formation`, or a cold library scan in a
  fresh build dir: the first run of a build dir rescans the library, ~25 s). Grep `[E]`/`[W]` after every capture;
  unknown arguments are only a `[W] unknown command line argument` line, never an error.
- **Never delete `<build>/bin/cache/library_result.json`** to force a rescan (a worker did, 2026-10-27, and every
  capture was then refused). A scan cancelled by the app quitting is never saved, so a short `--frames` run cannot
  rebuild it: run `python tools/run_capture.py --timeout 300 -- --screen select --frames 30000` once, which also
  picks up new `<exe>/stages` folders.
- **Name matching** = case-insensitive substring of the library id (the relative path, e.g.
  `model/Sekai Model Pack v4/LeoNeed/Ichika/01_9001.pmx`) or the display name; the FIRST match wins, so use a
  distinctive fragment (`01_9001`, `LeoNeed/Ichika`). `--stage none` = no stage. Songs with >= 2 dance parts open the
  formation modal (except in the studio, which loads its own models): pass `--formation` (see below) or pick a
  one-part song. Ids: `build/bin/cache/library_result.json`.
- **Same frame across runs (A/B comparisons): `--seek <sec> --paused --no-physics`.**
  `--seek` alone keeps playing: the capture happens `--frames` frames later in wall-clock time, so two identical runs
  differ (measured: 113k of 518k pixels). `--paused` holds the scene at the seek time (residual 1.2k px = physics
  settling); adding `--no-physics` makes depth, velocity and the lit scene match between runs, but not always the final
  image: the half-res SSR / SSAO passes fall into two states per run (neon_plaza: 15.5k px, max 25 levels, sparse X marks
  in the floor reflection, cause not found; spot pick, camera, bones, constants, bytecode, frame index and uv all ruled
  out, 2026-11-07), so diff against both states before calling a difference real. Use `--frames 60` or more so TAA
  converges. `--seek`, `--autoplay` and `--paused` are one-shot: consumed when the scene loads.
- **Settings come from `<build dir>/bin/mmdx12.ini`, one per build dir**, and they differ (window size, lighting preset,
  DoF, effect stack, shader packs per character). An A/B between two build dirs must either copy the same ini into both
  (back it up and restore it) or pin everything on the command line: `--width W --height H --render raster|rt|pt
  --upscaler none --lighting <n> --dof 0|1 --effect none --shader-pack none|<id>` (+ `--camera tx,ty,tz,yawDeg,pitchDeg,dist`
  for a fixed free camera; it implies `--free-camera`). A HEAD baseline = `git stash`, build another dir, `git stash pop`.
- **One window at a time.** Every run opens a visible window: never launch runs in parallel, tell the user before a
  batch. A running exe locks its build dir (`LNK1168`): when a worker is building in `build/`, run another build dir.
- **Agents and workers launch the app ONLY through `tools/run_capture.py`** (never `MMDX12.exe` directly, never
  `timeout ...`): it refuses argument lists that cannot reach a scene or cannot quit by themselves (one window at a
  time, a real `--character/--song/--stage` from `<exe dir>/cache/library_result.json`, `--formation` for multi-part
  songs, a quit condition), defaults `--width 1600 --height 900`, kills the process tree on a missing `STARTUP first
  frame` / `motion bound` / an unknown-argument line / `--timeout`, and prints the exit reason, elapsed time, the
  `[E]`/`[W]` lines and the capture path + size. Exit 0 = quit by itself, target reached, capture present; 2 = refused.
  `python tools/run_capture.py --selftest` checks the pre-flight logic without launching.
  - Play capture: `python tools/run_capture.py --character 01_9001 --stage none --song Evergreen --autoplay --seek 20 --paused --no-physics --frames 60 --effect none --shader-pack none --capture out.png`
  - Studio ui-script capture (scripts use absolute pixel coordinates: pin the window size the script was authored for,
    e.g. 1920 x 1080 overrides the 1600 x 900 default):
    `python tools/run_capture.py --exe build_dev/bin/MMDX12.exe --character 01_9001 --screen studio --width 1920 --height 1080 --ui-script captures/studio/m2_a.txt --frames 120 --capture captures/out.png`
  - Wrapper options go before the app's own arguments (`--exe`, `--timeout`, `--first-frame-timeout`,
    `--allow-gui` for a deliberate interactive run, `--allow-library`); `--` is optional.
- Canonical A/B capture (run once per build, then diff the PNGs):
  `python tools/run_capture.py --exe <build>/bin/MMDX12.exe --character 01_9001 --stage none --song Evergreen --autoplay --seek 20 --paused --no-physics --frames 60 --effect none --shader-pack none --capture out.png`

## Architecture (src/)
- `core`: logging, text encodings (Shift-JIS/UTF-16/UTF-8).
- `anim`: BoundMotion disables unkeyed IK targets when their links have FK-only rotation keys; explicit IK states win.
- `asset`: MVD import via LoadVmd magic sniffing (layer 0 only; `tools/mvd_test`); PMX/VMD parsers, image loading (stb with a WIC fallback, also from memory), `AssetLibrary`, `ModelImport`.
  - DDS (M5a / S11): `DdsLoader` decodes level 0 of any 2D DDS to RGBA8 (`LoadDdsLevel0`, the fallback path) and, for
    BC1/BC2/BC3/BC4/BC5/BC6H/BC7 (DX10 formats and the DXT1/DXT3/DXT5/ATI1/ATI2/BC4U/BC5U FourCCs),
    `LoadDdsCompressed` keeps the file's blocks and all its mips in a `CompressedImage` instead. `IsDdsCube` /
    `LoadDdsCube` handle `DDSCAPS2_CUBEMAP` files (six faces, +X -X +Y -Y +Z -Z) both as RGBA8 faces and compressed;
    `Dx12Context::UploadBatch::CreateTextureCompressed` / `CreateTextureCube` upload them as they are (no CPU decode,
    SRV format = the BC format; the model path uploads UNORM because the shaders apply `SrgbToLinear` themselves).
    `LoadImageWithCompressed` returns both forms from one file read: the model texture paths take the blocks and keep
    the RGBA8 copy only for the CPU checks (alpha test / toon ramp / thumbnails). BC7 and BC4/5/6H have no RGBA8
    decoder, so such a file's RGBA8 copy is empty and the GPU uses the blocks alone. glTF `MSFT_texture_dds` swaps a
    texture's image for the DDS one. Every model logs one `TEXTURES` line (count / RGBA8 / BCn / all-RGBA8 / saved);
    `tools/asset_probe --full` prints the same totals for the library and `tools/dds_test` checks the formats, mips,
    cube round trip and scans `library/`.
  - Unit songs: same-length dances in one song folder are the song's `SongAsset::parts` (byte-identical renamed copies
    merge, labels from the file names; `danceVmd` stays the primary part), ordered left to right on stage and with
    `centerPart` = the part closest to the group's middle in most samples (from `VmdProbe::stageX`, the sampled
    センター + 全ての親 X). Cameras: every candidate in `SongAsset::cameraVmds`; the default prefers plain MMD cameras over
    plugin ones ("Camera for FOVMOD", MME/MMM; "normal" wins); the lobby's song header offers the others when there are
    several (`songCamera=` ini, `App::SongAsChosen` applies it to every load). Play / Enter / the lobby video render go
    through `App::StartLobbyLoad`: >= 2 parts open the formation modal (`app/UiFormation.cpp`: slots, center, auto
    formation by part-label tokens, saved per song by dance file as `songFormationPart=` / `songFormationCenter=` ini lines); the cast loads into
    `ScenePackage::character` (the center: focus, spotlights, F framing) + `extraCharacters`, posed every frame as
    `SceneRuntime::extras` (the render benchmark's extras stay posed once).
  - `AssetLibrary` precedence: app overrides (`library_overrides.json` next to the exe, right-click on a card) > `mmdx.json`
    sidecars > folder names (characters/models, stages, songs/motions, several languages) > content. Several songs in one
    folder are split only when it also holds several audio files or cameras; audio/cameras are matched by length + file name.
    New installs get `assets/library_template` (characters/ stages/ songs/) copied into an empty library.
    Scan I/O: size / mtime come from the walk's `directory_entry` (no per-file stat calls). On a seek-penalty volume
    (`core/StorageInfo` `PathIncursSeekPenalty`, HDD) a `SequentialPrefetch` thread reads the to-probe files in path
    order while the probes still parse in parallel behind it; SSDs / unknown keep the plain parallel probes.
    `MMDX12_SCAN_IO=seq|par` forces a mode; the log's `library scan: storage ...` line says which ran.
  - `RestPose` (asset/RestPose.h): `LoadModelFile` turns a native PMX/PMD *character* whose upper arms rest above 20 deg below horizontal
    (T-pose: the Project Sekai rips; every other library model measures 28..43 deg) down to MMD's 38 deg A-pose, because VMD rotations
    are relative to the rest pose. Rotates the 腕 subtree (+ IK bones chasing it) about the shoulder: bone positions, vertices (linear
    blend by the chain's weight share), vertex morphs, rigid bodies, joints (follow body B), tail / fixed / local axes. VMD and bone
    morph data stay valid (MMD bone frames are world-aligned). Stages / props / glTF-FBX imports (own `ToMmdArmPose`) are untouched.
  - `ModelImport`: `LoadModelFile(path, role)` reads PMX natively and converts glTF/GLB/VRM (cgltf) and FBX/OBJ (ufbx) into a
    `PmxModel` via `ImpScene` (ImportScene.h). Characters: humanoid map (VRM table or name dictionary) -> MMD bone names,
    synthesised センター/グルーブ + leg IK, arms re-posed to 38 deg (MMD A-pose), morph aliases (あいうえお, まばたき); no physics.
    Stages: baked, one root bone, moved onto their main floor when nothing is under the origin (`ModelRole::Prop`: baked,
    not moved — studio props). Embedded textures live in
    `PmxModel::embeddedTextures`. Test assets: `captures/testlib` (messy layout) and `captures/assets_dl` (three.js/Khronos/VRM samples).
- `anim`: `ModelInstance` (bones, append, IK, morphs) and `Motion` (VMD Bezier evaluation, camera).
  - `PhysicsWorld` (Bullet, `Physics.cpp` only) runs between the before- and after-physics bones in `UpdatePose(dt)`.
    Only the character enables it. `App::UpdateScene` derives dt from the motion clock and resets the bodies on seeks/loops.
    `SetGravity` (m/s^2 along a direction; the world runs x 10) and `SetWind` set the environment (default 9.8 down,
    no wind); `Reset` settles the bodies in both (gusts held) and sets the wind's gust clock (`ResetPhysics(windClock)`).
- `render`: `Dx12Context` (device, frames, descriptors, `UploadBatch`, PNG capture) and `Renderer`.
  - The renderer runs an ordered `IRenderPass` list (see `Passes.h`): Shadow → Scene → Resolve → PathTrace → SSAO → SSR → Composite → Volumetric → TAA → Upscale → DoF → Bloom → Post → Backdrop → Present. `PassContext::path` decides which passes work (RayTraced: ray-query sun shadows in the scene PS, RTAO, RT reflections; PathTraced: compute path tracer + temporal/à-trous denoiser writing the G-buffer). Passes own their targets via `OnResize`; inputs are bound through per-frame `TransientDescriptors`.
    PT outlines: in PT mode `ScenePass::DrawPtEdges` rasterises a depth pre-pass (`PSDepthAlpha`) + the edges (default or the
    pack's) into the cleared MSAA scene targets with the tracer's jittered camera (`RenderTargets::ptEdges`), and
    `PathTracePass` composites that layer over the denoised colour (`pt_edge.hlsl`, outline velocity where it covers), as the
    offline GI renderer composites its outline layer.
    Offline video with the real-time PT renderer (`RenderSettings::ptAccumAlbedo`, `FrameView::ptFirstPass/ptStaticPass/ptPassIndex`,
    `RecordRealtimeVideoFrame`): the temporal pass only averages demodulated light (`light = radiance / albedo of sample 0`), so a
    primary hit that flips between passes (stochastic transparency: hair over skin, glass) or sits on a silhouette showed one pass's
    albedo (binary dither, stair steps) however many passes ran. Now `CSTemporalAlbedo` also accumulates the radiance R = light x albedo
    with the light's weights and hands the remodulation `R_acc / L_acc` (averaging the albedo itself is biased: light is correlated with
    it), later passes of one video frame accumulate in place (no depth / normal rejection), and the outline layer is averaged per frame
    (`edgeAcc_`). Per-quality history limits / carry live in `kVideoRealtimeQualities`. Real-time PT does not use any of it (TAA does).
    Measure pass effects with a seed pair (same settings, different `pc.frame` seed) on a static region; a "converged" reference
    carries bias. Scratch runs must not copy the user's `mmdx12.ini` (its effect stack changes brightness per pass).
    Pass implementations live in `Passes.cpp` and `Pass*.cpp` (shared helpers in `PassCommon.h`). Volumetric and the FFT bloom are `ComputePipeline` (cs_6_5) and silently disable themselves without DXR-class hardware.
    Punctual lights (S7a): `Renderer::kMaxPunctualLights` = 256 lights in the upload (`LightParams::punctual`, one
    `GpuLight` of 112 B per light per constant-buffer slot), `SceneConstants::numLights` stays the count every shader
    loops over. Spot shadow maps keep `kSpotShadowSlices` = 16 slices: `Renderer::PickSpotShadowSlices` scores the
    casting spots by power x falloff at the caster x cone position x how visible the caster is, and keeps last frame's pick unless
    a newcomer scores 1.5x more (hysteresis, slice kept while the light stays), so the 16 most important spots cast and
    the rest are unshadowed in raster / RT. The vector is uploaded into `SceneConstants::spotViewProj` and through
    `PassContext::spotSlices` to `ShadowPass` / `FillGpuLights`. Point shadows keep their budget (`PointShadowCount`,
    face size halves when the map no longer fits). A slice holds characters and props only, so `render/SpotShadowPick.*`
    (`tools/spot_shadow_pick_test`) gives a slice only to a spot whose cone and range reach a caster (`LightReachesSphere`
    against `GpuModel::LiveBoundingSphere`, from the bones of the last `UpdateSkinning`; such a map would be empty, so the
    image is the same) and `ShadowPass::DrawSlice` skips the models outside the light's volume (spots and points). The PT / offline GI light pick uses a power CDF
    (`Renderer::FillLightCdf`: intensity x luminance(colour) x range^2, x the spot cone proxy) uploaded as raw floats and
    bound through the bindless buffer table (`SceneConstants::lightCdfParams` = heap index + count); `shaders/light_pick.hlsli`
    (`LightCdfPick`) samples it. Offline GI keeps its exact all-lights estimator up to 4 lights and takes 4 CDF samples
    per shading point above that (`PunctualLighting`, the glints, skin translucency). The volumetric march considers at
    most `kVolMaxLights` = 64 lights. Clustered culling (Forward+, S7c): above `kClusterMinLights` = 16 lights the raster /
    RT scene pixel shaders no longer loop over every light: `PassLightCull` (`PassLightCull.cpp`, `shaders/light_cull.hlsl`)
    runs right before `ScenePass`, one thread group per froxel of a fixed 16 x 9 x 24 grid over the main camera (screen
    tiles x exponential view-depth slices between nearZ and min(farZ, 1000), `ClusterSliceParams`), and tests every
    light's range sphere (spots also reject the box outside their cone) against the froxel's view-space AABB; the
    survivors land in the frame slot's raw `Renderer::lightClusterBuf_` block, ascending, up to `kClusterMaxLights` = 64
    per cluster (a fuller cluster stores 0xFFFF and its fragments loop over every light). The scene shaders read their
    fragment's list through `shaders/light_cluster.hlsli` (`LightListFor` from the world position; a fragment past the
    grid's farthest slice, depth > min(farZ, 1000), is outside the culled range and keeps the full loop) via the same
    bindless buffer table as the CDF (`SceneConstants::lightClusterParams` = heap index + packed grid + log-slice scale /
    bias).
    The order is preserved, so the image is bit-identical to the full loop; `world.hlsl`'s world-surface lights keep the
    full loop, and camera feeds / quad extra views / ortho / `RenderToImage` / offline set the index to -1 (their camera
    is not the grid's). No root-signature change; nothing runs at <= 16 lights (`LIGHTS ... clusters=0`).
    Emissive-triangle NEE (S8a, `shaders/emissive_nee.hlsli`): every RtScene geometry whose material emits (S5 screen,
    AutoLuminous, a PBR record's emissive; `GpuModel::EmitEstimate` x the rest-pose `TriAreas`) puts its triangles into a
    pick table (`Renderer::BuildEmissiveTable`, at most `kEmissiveTriMax` = 262144 entries, heaviest materials kept;
    rebuilt only when the geometry list or an estimate changes, one `EMISSIVE tris= emitterGeometries=` line): a
    `GpuEmissiveTri` list, a normalised CDF and a per-geometry base table, raw SRVs per constant-buffer slot in the
    bindless buffer table (`SceneConstants::emissiveParams`, x = -1 = no emitters: no NEE, no MIS, the old image); offline GI
    uploads its own copy per image (`OfflineRenderer::EmissiveSrv`). PT (`CSPathTrace`) and offline GI (camera / bounce
    vertices and the cache's gather paths) add ONE emitter sample per shading point on top of the punctual NEE, weighted
    against the BSDF continuation with the power heuristic; the emission a BSDF-sampled ray finds takes the matching
    weight (`EmissionAt` is the one emission function of both sides). Both halves must estimate the same integral: the
    light sample's BSDF is what the continuation realises (a record's full diffuse x AO and VNDF GGX lobe
    `PbrSpecCosine`, the plain surface's (1 - pSpec) diffuse and pSpec cone), its visibility is `EmissiveVisible`
    (TraceClosest with the path's stochastic alpha: non-casting geometry and glass block, as they block the path, which
    crosses glass as a delta event whose emitter hits keep weight 1), and cut-out emitter texels carry their coverage.
    Camera hits, delta events, medium scattering events and back faces keep weight 1; the last vertex takes the light
    sample whole. In offline GI the camera vertex that reads the irradiance cache light-samples the emitters with the
    diffuse lobe unweighted (`EmissiveLighting` mode 1) and the gather paths leave out the table emitter they hit first.
    Measured on arena_live (2026-10-26): NEE on vs off agree within 0.2 % at 2048-4096 spp in every crop.
    Volumetric haze (S9, `shaders/volumetric_common.hlsli` shared by `volumetric.hlsl` and `offline_volumetric.hlsl`):
    every light scales its own in-scattering by `LightValues::volumetric` (`GpuLight::_pad` / `VolLight::volumetric`) and
    uses `SceneLight::volumetricG` as its Henyey-Greenstein `g` (`_pad2` / `VolLight::aniso`; absent = the fog's own).
    `RenderSettings::volumetricAnisotropy` (-1 = the built-in sun 0.55 / punctual 0.3) is the fog's `g`, set by a stage's
    `environment.fog.anisotropy`. Local `LightKind::FogVolume` lights become `LightParams::fogVolumes` -> `GpuFogVolume`
    (StructuredBuffer, root SRV t4 space1 = `PassContext::fogVolumes`) and add a tinted extinction inside their box /
    ellipsoid (density = a multiple of the base sigma0). The frame's medium (sigma0 / falloff / tint / radius) is
    `LightParams::volumetric*`, filled from a stage's `environment.fog`; the haze sphere (`VolumetricMediumRadius`:
    180 or the stage's `radius`, grown to hold every fog volume) is marched up to `VolumetricMaxDistance` (400, or far
    enough to cross the sphere from a distant eye). g = -1 means unset everywhere (negative g is valid backscatter).
    A fog stage turns the haze on for its play scene only (`App::stageFogVolumetric_`, never saved in the ini).
    S9c in-path media (`OfflineJobDesc::volumetricFull`, the default; ini `volumetricFull=` / `videoVolumetricFull=`,
    `--offline-volumetric single|full`, the render dialogs' "정밀 안개 계산"): the offline GI paths compute the same medium
    themselves (`shaders/medium_common.hlsli`: grey extinction, scattering albedo = the tint, HG phase, delta tracking
    against a per-segment majorant, ratio tracking / closed form for shadow rays, `kMediumMaxEvents` 32 then transmittance
    only). Every segment of every path (camera, bounces, prepass gather paths) travels through it, at each scattering event
    sun + one punctual NEE (phase weighted, times the light's `volumetric` share and the medium transmittance) and the path
    continues along the HG phase, so bounce light, emissive surfaces, the sky and the fog itself light the fog and the fog
    dims the light reaching the surfaces. `GpuMedium` (t5 space1, one element) carries the medium; `mode` 0 = off (the image
    is the pre-S9c one, no tracking loop entered). Full skips the single-scatter pass (no double counting); raster / RT / PT
    keep it. A pixel whose first interaction is in the medium writes the medium albedo and the view direction as its guides.
    Defaults (no volumes, share 1, g unset, eye within 220 of the origin) reproduce the pre-S9 image.
    Real-time haze modes (S9d): `RenderSettings::volumetricMode` 0 (default) = a froxel grid, 1 = the per-pixel march
    above (ini `volumetricMode=`, CLI `--volumetric-mode froxel|march`, kept for A/B). The grid is 160 x 90 screen tiles
    x 64 exponential view-depth slices between nearZ and the march's own `VolumetricMaxDistance` (RGBA16F `Texture3D`s,
    created on the first froxel frame in `VolumetricPass::EnsureFroxelTargets`, one `FROXEL grid=... MB=` line; nothing
    is allocated while volumetrics are off). `volumetric_froxel.hlsl` runs three compute passes: `CSFroxelLight` (one
    thread per froxel, centre jittered along z by an R2 sequence; medium = `VolMediumAt` of `volumetric_common.hlsli`
    so the march and the grid share sigma / tint / fog volumes; in-scattering = the isotropic sky term, the sun
    (raster: cascaded shadow map, RT / PT: `TraceShadowRay` against the TLAS -- `VOL_RT_SHADOWS`) and the froxel's S7c
    cluster list from `light_cluster.hlsli` (all lights, capped at `kVolMaxLights`, when clustering is off), each scaled
    by the light's `volumetric` share with HG phase; writes sigma_s x L, extinction), `CSFroxelTemporal` (blend with last
    frame's grid reprojected through `prevViewProjNoJitter`, current weight 0.1, rejected outside the previous frustum /
    on a camera cut or a frame gap, ping-pong), `CSFroxelIntegrate` (one thread per x,y column: front-to-back
    `L += T S (1 - exp(-sigma dz)) / sigma`, `T *= exp(-sigma dz)`, storing the prefix of every slice).
    `volumetric_froxel_apply.hlsl` samples that grid trilinearly at the pixel's own view depth and blends
    `lit * T + L` (Blend::Transmittance), so the O(1) per-pixel cost does not follow the march length.
    Spot beams are NOT in the grid (its 160 x 90 tiles drew thin beams as staircases): `volumetric_beams.hlsl` marches
    every spot's cone per pixel at the full render resolution (`IntegrateVolume` with `VOL_SPOTS_ONLY`: spot shadow maps on
    every path, gobos, the multiple-scatter phase via `VOL_PUNCTUAL_PHASE`), temporal blend 0.15, added on top as
    `lit + L` (`VolumetricPass::ExecuteBeams`; `gP3.x` bit 1 tells `CSFroxelLight` to skip spots). ~1 ms on arena_live.
    `RenderSettings::volumetricMultiScatter` (default on, ini `volumetricMultiScatter=`, CLI `--volumetric-ms 0|1`) adds
    the three Wrenninge 2015 octaves in the injector (a = b = c = 0.5: energy a^n, phase g c^n; the stored extinction
    stays the medium's own, since a and b coincide); off = single scattering exactly.
    Physical Sky light (S9b-1, `LightKind::Sky`, "sky"): a stage / studio light of kind `sky` replaces the hand-set sky
    gradient with Preetham 1999's clear sky (the Perez distribution in Yxy). `render/PhysicalSky.{h,cpp}` evaluates the
    model on the CPU: the sun's direction from `skyTimeOfDay` (sunrise 6:00, sunset 18:00, peaking at
    `skyMaxElevationDeg`) and `skyNorthDeg`, the 5 Perez coefficients of Y / x / y for `skyTurbidity`, the zenith Yxy
    divided by F(0, thetaS), the night floor and the sun's Rayleigh + Mie transmittance. `BuildSceneLighting` puts those
    into `LightParams::sky` (also writing `skyZenith` / `skyHorizon` / `groundColor`, so the hemisphere fill, the floor
    and the haze follow the sky) and the sun's direction / colour; `FillSceneConstants` copies them into the eight
    appended `SceneConstants::sky*` float4s. `SkyColor` (common.hlsli) branches on `gSkyParams.x` to
    `shaders/physical_sky.hlsli`; every path (raster, RT, PT, offline GI) calls `SkyColor`, so all of them change with no
    call-site edits. The model's chromaticity y gets + 0.012 on both sides (Preetham sits magenta of the daylight locus:
    a mauve noon horizon), and the sky-owned sun's colour peaks at MMD's default 0.6 in gamma space. A Sun light in the scene keeps its own direction and intensity (the sky only recolours it); with no
    Sun light the sky light owns the sun (and its shadows). No Sky light = `gSkyParams = 0` = the frame byte for byte
    the one before S9b-1.
    Stage / project data: the values live in the light's `values` (keyable, so their key frames carry them too):
    `skyTimeOfDay` (default 12), `skyNorthDeg` (0), `skyTurbidity` (3, 1.7..10), `skyHaze` (0.2, 0..1),
    `skyGround` (a ground albedo, 0.35 grey), `skyIntensity` (1), `skySunIntensity` (1), `skyMaxElevationDeg` (60). Their
    binding targets are `lights.<name>.sky.timeOfDay|northDeg|turbidity|haze|ground|intensity|sunIntensity` (the same
    nested one-level split as the S7b gobo's). Cloudless sky only: image skies / IBL are S9b-2..S9b-4.
    Sky as a light (S9b-2): `render/SkyEnv.h`'s `SkyEnvBuilder` turns the Physical Sky into a `shaders/sky_env.hlsl`
    environment buffer (one per renderer, one for the offline renderer; `kSkyEnvFloats` = 8296 floats): an
    equirectangular 128x64 grid of `f = (luminance(SkyColor(dir)) + 1e-4) sin(theta)` with its row / marginal CDFs and
    total, plus the 9 order-2 SH radiance coefficients. Four compute passes (lum, row prefix, marginal prefix, SH
    integration) run every frame a sky is on, in `RecordScene` right after `UpdateScreenLights` (and once per offline
    image, in `OfflineRenderer::Begin`), with `UAV` barriers between them; `SceneConstants::skyEnvParams.x` is the SRV
    heap index (-1 = no sky environment: nothing built, bound or evaluated, the frame is byte for byte the old one).
    * Raster / RT: `Hemisphere(n)` (mmd.hlsl) returns `SkyShIrradiance(n) / PI` while `SkyEnvOn()`: the sky's real
      Lambertian irradiance instead of the two-colour hemisphere (no `gSunIntensity` factor, `SkyColor` already has it).
    * PT / offline GI: `shaders/sky_env.hlsli` samples the CDF (`SkyEnvSample` / `SkyEnvPdf`) for one sky light sample
      per shading point exactly like S8a's emissive NEE (same MIS: `MisWeightLight` against `PathBsdfPdf`, weight 1 at the
      last vertex), and a BSDF-sampled ray that escapes is weighted by `MisWeightBsdf(prevPdfBsdf, SkyEnvPdf(dir))`.
      Offline GI mirrors each side in `SkyLighting` (the `EmissiveLighting` copy), in the prepass gather paths and at
      the camera path's escape; its first gather segment is left out, as the emissive table hits are there.
    Image skies (S9b-3): a Sky light's `skyMap` (stage-relative or absolute; `skyMapEncoding` auto / linear / srgb /
    rgbt, `skyMapMirror`, keyable `skyMapRotationDeg` / `skyMapIntensity`, targets `lights.<name>.sky.mapRotation` /
    `.mapIntensity`) replaces the Preetham model. `render/SkyMap.{h,cpp}` decodes it on the CPU into a linear RGBA16F
    equirect (.hdr via `stbi_loadf`, PNG / JPG sRGB, RGBA8 DDS: Ray-MMD RGBT when any alpha != 255 (`DecodeRGBT`, range
    1024), a DDS cube map converted with the D3D face table, max width 4096; EXR and BC6H / BC7 are not supported: one
    `[W]`, the procedural sky stays), uploaded once per path (`Renderer::EnsureSkyMap`, `UploadBatch::CreateTextureRaw`,
    one `SKYMAP` log line). `skyParams.x = 2` + `skyMapParams` / `skyMapSize`; `common.hlsli` only declares
    `float3 SkyMapRadiance(float3)` and each SkyColor user defines it after its bindless table through
    `shaders/sky_map.hlsli` (mmd / pathtrace / offline_gi / rtreflect / sky_env; composite.hlsl, which has no bindless
    table, uses the gradient of the map's CPU averages). Mapping: u = atan2(x, z) / 2pi + 0.5 + rotation, v = acos(y) / pi
    (Ray-MMD's IBL_QUALITY > 1 convention; mirror flips u). S9b-2 then lights from the image (SH ambient, PT / GI NEE). The
    sky light's own sun stays (time of day); with `skySunIntensity` 0 it casts no shadow, and raster / RT then get the
    image's light as ambient only (no directional shadow from the image's sun; PT / GI find it through the NEE).
    The offline background override (`gBgOverride.w > 0.5`) disables the build, since that escaped radiance is not sky.
    Clouds (S9b-4): a procedural layer on the S9b-1 sky (`shaders/physical_sky.hlsli` CloudHash / CloudNoise / CloudFbm /
    CloudDensity / `PhysicalSkyClouds`, called from `SkyColor` between the glow and `* gSunIntensity`). Values (keyable,
    defaults in brackets): `cloudCoverage` [0] 0..1, `cloudOpacity` [1], `cloudSoftness` [0.25] 0.02..1, `cloudHeightKm`
    [2] 0.3..12, `cloudScaleKm` [1.5] 0.2..20, `cloudWindDeg` [0] -360..360, `cloudWindSpeed` [10] 0..100 m/s; targets
    `lights.<name>.sky.cloudCoverage|cloudOpacity|cloudSoftness|cloudHeight|cloudScale|cloudWindDeg|cloudWindSpeed`.
    `SceneConstants::cloudParams` = (coverage, opacity, softness, heightKm), `cloudParams2` = (scaleKm, wind offset km x / z
    from `FillSceneConstants` at `view.sceneTime`); both stay 0 unless the procedural sky draws (never on an image sky), so
    coverage 0 is byte-identical to the frames before S9b-4. The layer is sampled on the ground plane at the ray's height
    (`dir.xz / dir.y * height` + wind), lit by the sun's colour (Henyey-Greenstein forward lobe + a self-shadow step 0.3 km
    toward the sun) and by the sky / ground ambient; the CPU dims the sky-owned sun colour by `1 - 0.6 * coverage * opacity`.
    Every path reads `SkyColor`, so raster / RT / PT / offline GI all show the clouds and (through S9b-2) light from them.
    Limit: no cloud shadows on the ground.
    AutoLuminous (そぼろ convention, native): `GpuModel::Create` flags a non-PBR material with black specular and
    specular power >= 100 as `MatFlag_AutoLuminous` (cleared for pack-shaded materials); `AutoLumEmission`
    (common.hlsli) adds diffuse x texture x (power - 100) / 10 x 4 as HDR after lighting in the raster / RT PS, PT and
    offline GI (emitters light their surroundings there). Switch `RenderSettings::autoLuminous` (ini `autoLuminous=`,
    `--auto-luminous 0|1`, lobby + studio effects toggles) -> `SceneConstants::autoLumParams.x`; off = identical frame.
    Translucent stage pixels (alpha < 0.98, `MAT_STAGE`, not S2b glass) write nt.z = -1 (no AO / no SSR): SSAO no
    longer greys sheer curtains. Character / prop pixels keep their reflectivity (`NormalTag` in mmd.hlsl): the normal
    target is alpha-blended, so a faint face overlay (blush) took the face's AO away in the overlay's whole footprint.
    Bloom look (`RenderSettings::bloomLook`, `BloomLook` in RenderTypes.h; ini `bloom*=`, `--bloom-kernel/-blend/-depth-mask/
    -intensity/-threshold/-radius`, MCP `set_render_settings bloom_*`, UI `App::DrawBloomControls`): mip chain levels
    weighted falloff^j (radius) and normalised (`RenderTargets::bloomScale`); bloom alpha = luminance x linear depth through
    every level, so post.hlsl's depth mask knows the sources' depth; energy-conserving blend subtracts the scattered share
    (`bloom_common.hlsli`) before adding the bloom. Convolution bloom = `render/BloomFft.*` (shared with the offline
    renderer): procedural per-channel kernel (core + Moffat glow + power-law spikes, dispersion = channel scale),
    (R,G) and (B,A) as two FFT runs, each channel normalised by its own sum, spectrum rebuilt only when the kernel changes.
    Colour LUTs (`ColorLut.h`) are 32³ strips uploaded with `Renderer::SetColorLut`; PostPass applies them after the sRGB encode.
  - `GpuModel` keeps a 3-frame ring of bone/morph buffers so the previous pose (motion vectors) stays valid.
  - Per-model GPU caches key by `GpuModel::Uid()` (process-unique), never the pointer: a freed model's address is
    reused by the next scene's model (stale MME SRV tables = GPU page fault / TDR). A model leaving the drawn set
    (menu / loading frames included) gets `Renderer::ForgetModel(uid)`. A cached descriptor of a recreatable texture
    needs a generation (e.g. `SetObjectShadowMap(tex, gen)`), not a pointer compare.
  - Device loss: `Dx12Context::DeviceLost()` (Present / ResizeBuffers / GetDeviceRemovedReason); DRED is on, the first
    detection logs the reason + breadcrumbs + page-fault allocation names; the app shows a message box and quits.
  - `GpuModel` handles GPU skinning. `RtScene` (RayTracing.h) skins every model into world-space `RtVertex` buffers with a compute shader and builds BLAS (characters every frame, stages once) + TLAS; shaders read geometry bindlessly (`rt_common.hlsli`, `RtGeometry` table, unbounded tables over the whole SRV heap, root signature 1.0 so descriptors stay volatile).
  - Render resolution (`targets.width/height`) vs output resolution (`outWidth/outHeight`): the upscaler (DLSS/FSR/XeSS behind `IUpscaler`) runs after TAA/composite; bloom, post, backdrop and present work at output size. Jitter uses the FSR convention (`jitterPx`), motion vectors are uv(cur) − uv(prev) so SDK MV scale is −renderSize.
- Theme: `ui::P()` is the live palette (light / dark, `ui::SetTheme`); never cache colours in statics. `App::ApplyTheme` (settings `theme`, `--theme auto|light|dark`) runs before the next ImGui frame and re-runs `ui::ApplyStyle`. Capture both: `--theme dark|light`.
- `app`: the `App` state machine, ImGui screens (`Ui*.cpp`) built on `UiKit` (tokens, fonts, widgets; see DESIGN.md), `ThumbnailCache` (rendered by
  `Renderer::RenderToImage` with default RenderSettings, never the user's effect stack / exposure / LUT), `Lighting` presets, `SceneLoader` (worker thread), benchmark, WinHTTP leaderboard.
- `audio`: miniaudio. The audio cursor is the master clock.
- `studio` (+ `app/UiStudio*.cpp`): the Studio editor (Screen::Studio). `StudioDoc` holds the models with name-keyed editable
  motions (`MotionData`, StudioMotion.h, converted to VmdMotion and re-bound with `BoundMotion::Bind` after edits), the camera
  track, `CommandStack` undo (edits are `TrackEditCommand` track snapshots / `MotionSwapCommand`) and the editor state.
  `UiTimeline`/`UiBezier` are data-agnostic widgets. The viewport is the renderer drawing into `RenderSettings::viewport*`.
  Compatibility with MMD goes through standard files: VMD (`SaveVmd`) and VPD (`asset/VpdFile.h`). PMM
  (MMD `.pmm` projects, "Polygon Movie maker 0002") is import-only (`asset/PmmLoader.{h,cpp}`, `tools/pmm_probe`):
  opening one (open dialog / `--project x.pmm` / recent / drop) builds a NEW unsaved studio project — models resolved
  by absolute path, then relative to the PMM folder, then by file name in the library; each model's PMM motion is
  split into a temporary VMD and re-read (VMD semantics), camera / light / self-shadow come from the PMM tracks, MME
  effect controllers (`MapMmeControllers`) are recognised and mapped onto native settings instead of becoming props,
  remaining accessories become props with an accessory key track. We never WRITE PMM (undocumented binary).
  - VMD bone interpolation: bytes 2/3 are MMD physics flags; Z/rotation x1 are bytes 17/18 (`GetBoneCurve`/`SetBoneCurve`).
  - Pose editing (`app/UiStudioPose.cpp`, `studio/Gizmo.*`, `studio/StudioPose.*`): viewport edits go to the model's
    `PoseLayer` (bone/morph overrides valid at one frame, applied after `BoundMotion::Evaluate`, so drags never re-bind).
    Leaving the frame discards it as an undoable `PoseEditCommand`; Register (I, Ctrl+I all bones) writes keys +
    clears the layer in one `CompositeCommand`. Bone values are VMD-local (parent frame); the gizmo converts world deltas
    with the parent's world rotation. Bones listed in two display frames select keys through `CanonicalRow`.
  - `asset/ModelImport.h` and `render/GpuModel.h` both declare `mmdx::ModelRole`: never include both in one .cpp.
  - Arm rest-pose correction (`studio/RestPoseRetarget.*`, `app/UiStudioRestPose.cpp`): T-pose-authored VMDs (game rips)
    on an MMD A-pose model. Detection = the median angle of the 腕 keys (0 = horizontal, 90 = down) >= 78 deg over >= 8
    keys; the retarget rotates 腕 by R (A rest -> the source arm direction) and each subtree bone by R x key x R^-1 (with
    translations rotated by R^-1). The VMD import asks "보정하고 불러오기 / 그대로 불러오기" before the merge; the model
    menu's "팔 레스트 포즈 보정…" runs it on the whole motion. Studio-only (play mode is untouched).
  - Camera/light/self-shadow (`app/UiStudioCamera.cpp`): `StudioDoc::camera` holds the camera, light (`LightKf`, linear) and
    self-shadow (`ShadowKf`, stepped, VMD distance = 0.1 - UI*1e-5) tracks as timeline rows `RowKind::Camera/Light/Shadow`.
    The lighting source is `StudioDoc::lighting` (`studio/LightRig.h`, a 3-way choice saved in the project, timeline rows
    `RowKind::Spot` key the rig's spots): VMD track (the camera VMD's light keys override the preset's key light, empty
    falls back), Preset (the project's own preset choice + a key-light override) or Custom (the concert spot rig, undoable
    add/delete, auto swing / follow centre / head / manual aim modes, warm front fill). `BuildStudioLighting` (app/Lighting.cpp)
    resolves the rig into `FrameView::light` for the studio (viewport, offline stills/videos). Play mode and the lobby keep
    the global preset (App.cpp `BuildFrameView`); the benchmark stays Studio/ deterministic. In the studio the shadow track
    sets `FrameView::shadowsOff`/`shadowDistance`; play mode ignores VMD light/shadow unless `settings_.motionLighting`.
    Perspective-off keys render as a 3 degree lens from far away.
  - Camera entities (S4, `studio/StudioDoc.*` + `app/UiStudioCameras.cpp`): `StudioDoc::camera` stays the MAIN camera
    (uid 0; it keeps the light / self-shadow / focus tracks and its own VMD) and `StudioDoc::cameras` holds the extra
    ones (`StudioCamera`: uid >= 1 from `nextCameraUid`, name, camera-keys-only `MotionData`, visible, stageUid of the
    stage.json camera it came from). `selectedCameraUid` says which camera the camera row / inspector / possession edit
    (`CameraTrack(uid)`, `SelectedCameraTrack()`); the outliner's 장면 group lists "메인 카메라" + every extra one (eye,
    rename, delete, "새 카메라" from the current view, "스테이지 카메라" = one static-key camera per stage.json camera).
    The main camera alone is the output: the viewport's camera view, play-through, offline stills and videos all use
    `cameraEval` (MMD compatible; a hard cut is a 1-frame key pair in the main track, there is no switch track). The
    extra cameras feed the stage screens only (`uid:<n>` screen sources). Possessing one is an editing view: the
    viewport shows and moves it (`StudioShownCameraEval`), leaving hands the view to the free camera, and renders drop
    possession first. They are also placed with the viewport's view ("현재 뷰를 키로 등록", `StudioViewportCameraKey`)
    or their eye / target handles in the free view, and their inspector / timeline show their camera keys only (the
    light / self-shadow / focus tracks are the main camera's). Evaluators: one `CameraMotion` per camera rebuilt in
    `RefreshCameraEvals` from `cameraVersion`. Undo: camera keys through `TrackState::cameraUid` (the same track
    snapshots), add / remove / rename / eye through `CamerasCommand` (whole list). Extra cameras are invisible to the
    render (their `visible` is an overlay flag). VMD export writes the selected camera's keys. Saved as version 5
    ("cameras"; one VMD per extra camera, "<stem> - camera <name>.vmd"). Script: `studiocamadd / studiocamselect /
    studiocamimport / studiocamview`; `studiostate` logs `STUDIOCAM` (uid, name, keys, visible, stageUid).
  - DoF focus (`studio/StudioFocus.*`, `app/UiStudioFocus.cpp`): the camera target's `RowKind::Focus` row = `MotionData::focus`
    (`FocusKf`, stepped segments: Auto / Target model uid + bone / Manual distance linear to a following Manual key; per-key
    aperture scale (linear, 0 = sharp) and `transition` frames = a smoothstep rack in 1/z). Saved in the project's editor
    "focus" array (targets as 1 + model index), never in VMD. No keys = `AutoFocus`: visible characters scored by on-screen
    height^1.5 x visible share x head centrality, 0.7 hysteresis, 0.35 s rack, snaps on camera cuts / time jumps (stateful:
    the viewport and renders share `studioAutoFocus_`). `StudioComputeFocus` writes `FrameView::focusDistance` and
    `apertureScale` (DofPass and the GI lens multiply it in). Play mode runs its own `AutoFocus` (`playAutoFocus_`, reset
    per scene) over every performer, falling back to the center performer's head; the benchmarks keep the fixed head focus.
  - Projects (`studio/StudioProject.*`, `app/UiStudioProject.cpp`): `.mmdxproj` is UTF-8 JSON (format version, relative
    paths) next to standard VMDs (`<stem> - <model>.vmd`, `<stem> - camera.vmd`); every file is written tmp + rename.
    Dirty = `history.Version()` or `projectVersion` (add/remove/rename/visibility/audio) changed since the save. Autosave
    every 60 s on a worker thread to `<exe>/recovery/` (also on window close with unsaved work, not in `--frames`/script
    runs); the select screen offers it at the next start; saving or leaving the studio deletes it.
  - Gravity and wind (`studio/PhysicsEnv.h`, `StudioDoc::physicsEnv`): the transport's physics popup (the caret left of
    the atom icon) edits them, one undo step per gesture (`PhysicsEnvCommand`, which re-seeds the bodies); saved as the
    project's "physicsWorld" (absent = MMD's 9.8 down, no wind; the format version is unchanged). `StudioUpdateModel`
    applies them every frame (viewport and renders), the bake through `BakeOptions::env`; play mode keeps the defaults.
    Every studio reset starts the gust clock at the timeline time (the bake at its preroll's first frame), so the gusts
    of the viewport, a render and a bake line up.
  - Models are added inside the studio on worker threads (`StudioJob`, `LoadStudioModel/Stage/Song`), uploaded in
    `StudioPollJobs`. `StudioModel::uid` is stable; removal waits for the GPU and clears the undo history (commands hold
    model indices). Replacing a model's file (outliner menu "모델 바꾸기", the "+" popup in replace mode: `StudioJob::replaceUid`,
    `App::StudioReplaceModel`) keeps uid, motion keys (re-canonicalised by name), placement, prop links, shader and the
    pose layer (remapped by name); it also clears the undo history (pose edits hold bone indices). Kinds: character, stage (static BLAS), prop (GPU role Character so its BLAS follows it): the prop's
    root = `PropOffsetMatrix(attach) * parent bone world * parent scale`, set with `ModelInstance::SetRootTransform`
    after the other models are posed (props parent to characters/stages only).
  - Rendering (`app/UiStudioRender.cpp`): the top bar's render menu opens the lobby's video dialog (`DrawVideoRenderDialog`
    is studio-aware: range section, no sample-render probe) or starts a GI still. `OfflineJob::studio` routes the offline
    job through `StudioPoseForRender` / `BuildStudioFrameView` (`OfflinePose`/`OfflineView` in UiOffline.cpp); the
    viewport rect is cleared for the render and `StudioRestoreAfterRender` puts time, camera mode and physics back.
    Videos skip unregistered pose edits. Audio: `VideoEncoder::Desc::audioStartSeconds` = start - audioOffset (negative:
    leading silence). Shortcut overlay: `DrawStudioHelp` (? / F1); `StudioModal()` blocks the studio's shortcuts.
  - UI restraint (the competitor-comparison pass, 2026-10): new actions go into existing places, not new toolbars.
    Timeline right click = key menu (`DrawStudioKeyMenu`: add / copy / cut / paste here / mirrored paste
    `StudioPaste(curves, mirrored)` / curves / delete); transport: volume icon + popover, one-frame steppers around
    the frame field; screen effects are a dock tab next to the properties (`###studio_effects`,
    `DrawStudioEffectsPanel`; the top-bar sparkle focuses it); camera-target rows carry a type tint
    (`TimelineRow::tint`); drag fields type on a plain click (`io.ConfigDragClickToInputText`).
  - Panels (`App::DrawStudio`): ImGui docking (the vendored imgui is the docking branch). A fixed ribbon window
    (`app/UiStudioRibbon.cpp`: a 44 px header row — back, project, tabs 홈 · 애니메이션 · 카메라 · 조명 · 렌더 · 보기,
    undo / redo, theme, help, save — over a 76 px tool row of grouped buttons per tab; Ctrl+F1 or the caret folds the
    tool row, `studioRibbonTab` / `studioRibbonCollapsed` in the ini), a transparent
    dock-space host and one dockable window per panel (`###studio_outliner/inspector/timeline/viewport`); each panel function
    still draws in screen coordinates and gets its window's content rect. The viewport window is transparent and its content
    rect is `RenderSettings::viewport*`. Default arrangement = `BuildDefaultStudioDock` (DockBuilder); the layout persists in
    `mmdx12_layout.ini` next to the exe, except in scripted / `--frames` runs (default layout every time; tab bars add ~32 px
    to the panels, so old `--ui-script` coordinates shifted). Panels float / dock inside the main window only (no OS-level
    viewports: one swap chain).
  - Editing model: auto-key (`StudioDoc::autoKey`, transport bar button): a finished pose / morph / camera edit keys itself at the
    playhead (`StudioSetPose` -> `StudioRegisterPose(…, layerBefore)`; camera fields live in `DrawStudioCameraKeyFields(live)`).
    Camera possession (`StudioPossess`, C4D style): viewport navigation edits the motion camera key (`StudioWriteCamera`, one
    undo step per gesture via `StudioNavEditTick`); without it the free view shows eye / target handles
    (`StudioViewportCameraHandles`). Characters have a world placement (`StudioModel::place`, a `PropAttach` used as
    translation / rotationDeg / scale; applied as root transform + display scale in `StudioUpdateModel`).
  - Viewports: `StudioViewportNavigate` (RMB look + WASDQE fly, Alt+LMB orbit, MMB pan, F focus, numpad views). Shading
    (`ViewShading`: Lit / Unlit / Wireframe, raster only) and the quad view (`StudioDoc::viewLayout`): the renderer draws
    `FrameView::extraViews` (orthographic top / front / left, `ExtraView`) as extra viewports of the same target inside
    `ScenePass::Execute`, each with its own SceneConstants (`PassContext::extraSceneConstants`); quad forces flat shading, no
    TAA / upscaler. `ViewProj::ortho` makes the overlays / gizmos work in those views; the active view is the one under the
    mouse (`studioActiveView_`).
- Shader packs (an ecosystem: anyone can make one; docs + gallery on mmdx.codingbot.kr `/<lang>/docs/shader-packs/`, sources
  in the website repo; direction + decisions for the native effect ecosystem vs MME: docs/effect_ecosystem.md): opt-in per-character replacements of the scene pass's material shading. A pack = `pack.json`
  (metadata, localized names, authors, license, tags, `apiVersion`, material class rules, <= 16 float params; `"requires": ["params.ext"]` = <= 128 typed params float / int / bool /
  color / vec3 in collapsible `"groups"`, one float4 slot each: surface b6 (`GpuModel` ring), effect b4 (`PackEffectPass`
  ring), PT / GI `gPackExtSlots` t3 space1 via `RtPtPackRecord::extSlotBase`; values `key#0..2` per component; UI
  `App::DrawShaderPackParameters`) + `surface.hlsl`
  implementing `PackShade` (contract: `shaders/pack_api.hlsli`, `kPackApiVersion`) + optional `preview.png|jpg` (AFTER) and
  `preview_before.png|jpg` (same frame without the pack; index.json `previewBefore`); with both, the manager's detail panes
  draw a draggable split (`App::DrawPackCompare`, Drag mode, uiprobe id `compare:<id>`; cards scrub on hover). Built-in in
  `shaders/packs/<id>`, installed in `<exe>/shader_packs/<id>`; template `shaders/pack_template`; `tools/pack_check`.
  Registry `render/ShaderPack.*` (status per pack, hot reload `PollChanges`, install / uninstall / template); online gallery
  `app/ShaderPackStore.*` (index.json, SHA-256 checked zips); manager screen `app/UiShaders.cpp` (Screen::Shaders, top tab
  "이펙트" - the ecosystem is "MMDX 이펙트": 표면 셰이더 / 화면 효과 / 월드 이펙트 / 룩, "팩" = the distribution unit; `--screen shaders|shaders-online`; per-tab type filter + sort `shaderType=` / `shaderSort=` ini, index.json
  `type` / `updated`), shared picker `app/UiShaderPack.cpp`. Networking / zip / version helpers live
  in `core/NetUtil.*` (shared with the updater).
  `GpuModel::SetShaderPack` writes class / head bone / params into `MaterialConstants`; `ScenePass::PackPsos` compiles
  mmd.hlsl's `PSPack` with DXC (FXC cannot `#include` a macro: `MMDX_PACK` = the quoted surface path) on first use and
  falls back to the default PSOs on errors (`[E]` + toast). `surface.hlsl` runs in the lit raster / RT camera view; PT and offline GI use the pack's
  `pt_surface.hlsl` when it has one (see below), else the default; unlit / wire / ortho views always use the default. A negative normal-target reflectivity (`nt.z < 0`) = "no AO" (composite). Choice is saved per
  character (`characterShader=` ini) / per studio model (`.mmdxproj` "shader"); `--shader-pack <id|none>` overrides a run.
  Per-material overrides (`ShaderChoice::materials`, material name -> PackClass 0..5 or -1 = default shading; ini
  `characterShaderMaterial=<cls>|<character id>|<material>` (name last: names may hold '|'), `.mmdxproj` "shader"."materials"): `GpuModel::SetShaderPack`
  writes the class, or `kPackClassOff` (0xFF) = that material takes the no-pack path everywhere (raster PSO / edge,
  RT, PT, offline GI; pack code never sees 0xFF). UI: `App::DrawMaterialShaderList` (studio inspector "재질" popup,
  play bar shader popup, library shader tab via `LoadCharacterModelOnly`).
  API v6 (`kPackApiVersion` 6; apiVersion 1..5 packs still load): `PackSurface` / `PtPackIn` carry the PMX additional
  UV1/UV2 (zero without them, feature macro `PACK_API_ADDUV` / `PT_PACK_API_ADDUV`), the raw sphere-slot (`_H`) and
  toon-slot (`_S`) textures are exposed (`PackHasSphereTex` / `PackSampleSphereUv`, `PackHasToonMap` / `PackSampleToonUv`,
  PT mirrors; a real sphere texture bound = `MAT_HAS_SPHERE_TEX`), and `PackEdge` sees the vertex's uv2 (`PackEdgeUv2`).
  Online-gallery-only `project_sekai_toon` ("프로젝트 세카이 툰", sources in the website repo `shader-packs/`, not bundled;
  `tools/pack_previews.py` has its shot): the Project Sekai MME look (hard terminator with the `_H` shadow bias, the `_S`
  shadow colour, the skin ramp, the uv1 face SDF and uv2 masks) from the model's own data only; `asset/MmeHints` follows the
  model's own `.emd` / `.fx` references (same-stem .emd, else the folder's only .emd; never folder / file-name guesses) to
  find its face SDF and set a per-model default pack texture folder. Packs declare the model-data traits they fit in an
  optional pack.json `"suggest"` block (`additionalUvsMin` / `toonMapMaterialsMin`; `ShaderPack::Suggests`), evaluated
  generically over `ModelTraits` (`ComputeModelTraits`: additional UV count + non-ramp UV-mapped toon-slot materials;
  `GpuModel::Traits()`), checked over the whole library by `tools/pack_suggest_test` (self test + scan). The app only
  SUGGESTS such a pack: one toast per (pack id, character id) when a character with no saved shader choice matches the
  first suggesting installed pack (remembered as `characterShaderSuggested` ini lines), a "추천" tag in the pack pickers,
  and the choice is never set by the app. Users fill their library their own way: nothing may depend on its layout or names.
  API v2 (`kPackApiVersion` 2; apiVersion 1 packs still load): a pack may declare `"textures"` in pack.json (at most 16:
  `{ "file": "textures/x.png", "address": "wrap"|"clamp", "srgb": true }`, png/jpg/jpeg, paths inside the pack), uploaded
  once per pack (DEFAULT heap, `ScenePass::EnsurePackTextures`) and shared by every model using it; sampled with
  `PackSampleTex` / `PackSampleTexLevel` / `PackTexSize` / `PackTexCount` (pack_api.hlsli, a fixed 16-SRV table at
  `t0, space5`, bound as root param 12). sRGB textures use `_SRGB` SRVs and sample as LINEAR values (no SrgbToLinear in
  the pack); `"srgb": false` returns stored values (data maps). Missing textures sample white (`[W]` per load; the
  manager shows the count). Game textures can't be redistributed: each pack gets a user "texture folder" (shader manager
  screen, picker + clear, saved as `packTextureFolder=<id>|<path>` ini lines, registry `TextureFolder`); lookup order:
  user folder (same relative path, then the same file name, then a file ending in `_<declared name>`, case-insensitive,
  shortest wins) → pack folder → white. The folder is per character too (`ShaderChoice::textureFolder`: ini
  `characterShaderTextures=<folder>|<character id>`, `.mmdxproj` "shader"."textureFolder", the "텍스처 폴더" row under the
  pack's sliders); it wins over the pack-level folder, and ScenePass keeps one texture set per (pack id, folder).
  Class rules can match the material's diffuse texture path (`"texture": [...]`), not only its names: game texture
  sets follow the texture sheet (hoyo_toon_v2 picks hair maps for anything on the hair sheet). A class rule may carry
  `"alphaKey": <0..1>` (colour-key cut-out: cut out pixels whose texture red is below the value, an additive-key
  sprite such as an MME eye highlight; applies in every render path; the first rule of a class wins). A pack with all textures missing
  still compiles and renders. `PACK_HAS_EDGE` + `PackEdge` in surface.hlsl wire the pack into the edge pass (per-material
  outline colour + width scale; `PSEdgePack` / a `VSEdge` width-scale build from `PackPsos`, root param 12 bound there
  too); otherwise the edge pass is unchanged. PT and offline GI draw the same pack outlines: PT through `ScenePass::DrawPtEdges`
  (below), offline GI through `offline_edge_pack.hlsl` (`VSEdgeOfflinePack` / `PSEdgeOfflinePack`, mmd.hlsl included with
  `MMDX_NO_SHADOW_PASS` because its `ShadowCB` is b2; one DXC PSO per pack in `OfflineRenderer::Impl::PreparePackEdges`). `PACK_WEAPON` = 5 joins the material classes; built-in `hoyo_toon_v2` (issue #2) = Genshin light maps / ramps / face SDF
  from a user texture folder, v1 fallback per missing map; `tools/pack_check` validates
  textures (format / count / size / total ≤ 32 MB / bad paths; missing files are warnings) and literal
  `PackSampleTex(N)` indices against the declared count.
  `pt_surface.hlsl` (docs/shader_pt_api.md, phases 1-3): optional `PackEvaluate(PtPackIn)` hook for offline GI (`CSRender`, `OfflineRenderer`) and real-time PT (`CSPathTrace`, `PathTracePass`) with pack textures supported (`PtPackSampleTex*`, `render/PackTextures.*`); packs without it leave PT/GI untouched; shared variant cache `render/PtPackVariants.*`. `PtPackIn` also carries `headPos`/`headScale`; `PtPackOut::terminator` = N.L edges of the ramp (hi <= lo: engine default), `shadowBias` = smoothed-normal offset, and every field must be assigned (`pack_check` warns). Online-gallery-only `nimble_toon` (sources in the website repo `shader-packs/`; Eternal Return MMD models) = soft cel look (low-contrast coloured shadows, lifted dark-material shadow floor, skull-sphere face normals, soft hair gloss, wide environment rim, tinted outlines via `PackEdge`); `surface.hlsl` (raster / RT) and `pt_surface.hlsl` (PT / GI) share `nimble_core.hlsli`. `PtPackOut::flatFace` alone decides the
  flat-face path for pack surfaces (a material without toon, `MAT_FLAT`, no longer drops the pack's terminator in PT / GI). Opt-in `PT_PACK_HAS_LIGHTING` / `PackEvaluateLighting` (API macro `PT_PACK_API_LIGHTING`) splits that: `flatIndirect` gathers indirect flat without dropping the pack's terminator, `sunShadow` (0..1) weights the traced sun visibility in the direct term / specular, and `lit` (0..1, -1 = unused) replaces the terminator / shadowBias; packs without it keep the old behaviour exactly.
  Online-gallery-only `starrail_face` = Star Rail: game light maps, warm / cool ramps
  read with the Unity row flip `1 - (2 id + 1) / 16`, body-type `*_FaceMap_00` SDF in A + nose line in B; the "weapon"
  class = a model's second sheet; test texture sets in `library/gamerip/StarRailTextures/_sets`; hoyo_toon_v2 stays Genshin-only.
  API v3 effect packs (docs/shader_effect_api.md): `"type": "effect"`, `"stage": "post" | "pre-bloom"`, `effect.hlsl` implements
  `PackEffect(PackEffectInput)` (contract `shaders/effect_api.hlsli`, host `shaders/effect.hlsl`, DXC `ps_6_0` with `MMDX_PACK`).
  The user's ordered stack is `RenderSettings::packEffects` (`EffectStackEntry`; `AppSettings::effectStack`, ini `effect=`,
  CLI `--effect id[,id]|none`). `PackEffectPass` (`PassEffect.cpp`) is registered twice (pre-bloom after DoF, post after Post), runs
  its share with ping-pong targets created only while an entry is enabled (zero effects = no targets, identical frame), skips
  unlit / wire / quad views, and ends with a CopyResource into `hdrFinal` / `ldr`. Pack textures use the v2 table (`PackFxSampleTex`,
  t0 space5), frame inputs are t0..t3 space6, params 16 root constants at b2. UI: `App::DrawEffectStackEditor` (shader manager detail
  of an effect pack + the play bar's effect button); surface pickers list surface packs only. Templates `shaders/pack_template`
  (surface) / `pack_template_effect`; online-gallery-only examples chromatic_aberration, film_grain, crt_scanlines, auto_luminous (not bundled; sources in the website
  repo). Studio projects don't store the stack yet (app-level setting). API v4 effect state (docs/shader_effect_api.md): pack.json `"state": {"floats": N}` (<= 16, apiVersion 4) gives each stack entry a 4x1 RGBA32F ping-pong slot (`PackEffectPass::stateSlots_`, key = stack index + pack id) updated by `PackEffectState` in a pre-pass before `PackEffect` (`PackState(i)`, `PackEffectInput::dt`); state packs use their own root signature (`t0, space7`), packs without state keep the v3 PSO / defines. dt / reset / advance come from `FrameView::effectDt / effectStateReset / effectStateAdvance` and `OfflineJobDesc::effectDt / still / effectReset` (GI stills run the state pass 64x). Offline GI runs the stack too (`job.effects`, `OfflineRenderer::Impl::Finish`):
  pre-bloom entries run on the lit HDR before the bloom (`CSLitCompose`, the bloom then reads the effect output), post entries on the sRGB
  result (`PackEffectPass::RunOffline`). Their depth / normal come from the offline G-buffer (depth from `offline_effect.hlsl`, the normal in view
  space like the real-time normal target), motion is zero.
  API v5 (docs/shader_effect_api.md): pack.json `"passes"` (<= 8 `{entry, scale}`; `PSPackPass` with `PACK_PASS_ENTRY`,
  RGBA16F targets pooled by size in `PackEffectPass::passPool_`, read as `PackPassSample(n, uv)` from a t0..t7 space8 table
  that exists only in packs with passes) and the focus helpers (`PackFocusZ` / `PackFocusAperture` / `PackDofAperture` /
  `PackCocPx`, gP1 = FrameView focus + the DoF settings). `"replaces": ["dof"]` (`EffectStackReplacesDof`) skips DofPass and
  the GI thin lens while that entry is enabled. One entry's draws = `PackEffectPass::RunEntry` (shared by RunStack /
  RunOffline). Gallery examples on it: lens_dirt, lens_reflection, lens_flare, bokeh_dof, screen_ao (and
  chromatic_aberration 2.0.0, still v3). MMD scenes rarely hold HDR lights (a lit window is ~0.4 linear): the lens packs
  pick "lights" by luminance x local contrast, not a plain threshold.
- World effects (docs/shader_world_api.md, contract `render/WorldTypes.h`): `"type": "world"` packs (`world.hlsl`, a
  pack.json `"world"` block: particles / fields / heightfield surfaces) drawn by `WorldPass` (`render/PassWorld.cpp`):
  deterministic fixed-step GPU simulation toward `WorldInstance::time` (re-simulation on seeks: persistent fields from 0 on
  their own k / rate grid, the rest over the rewind window; per-frame upload chunks, persistent field descriptors).
  The particle steps end on one global grid (k * `step`, no partial steps) and drawing is interpolated between the last
  two grid steps (`gParticleParams.x` = a, respawns / teleports drawn at their position), so high-fps playback is smooth;
  surfaces in the scene MSAA targets (raster / RT; host-lit with sun shadow from the cascades + `PackHemiAmbient`), particles
  after the composite into `lit` (`WorldParticlesPass`, soft depth fade; never in SSR / SSAO), PT / offline GI through the
  outline layer. Characters interact through a collider track (feet / toes / wrists, plus head / upper body / shoulders /
  lower body for packs that require `world.body`, 30 Hz, no physics; play mode builds
  it once per scene: `WORLD colliders ready` log line). Play mode: `AppSettings::worldStack` (`--world id|none`, the 화면 효과
  panel's 월드 이펙트 section, manager "월드에 추가"); studio: outliner 월드 entities with placement + keyable params
  (`RowKind::WorldParam`, saved in .mmdxproj). Surfaces also get the scene's spots / points + their soft / hard shadow
  maps (shared `shaders/shadow_common.hlsli`). Capabilities in `"requires"` (apiVersion stays 6): `world.camera`
  (`"space": "camera"` particles simulated in a world-fixed tile, drawn in a box around `PackWrapCentre` = the camera
  shifted 0.4 box ahead; `"wrap"` or `"wrapParam"` + `"wrapScale"` = a range param sizes the box; never re-anchor
  particles to the camera), `world.anchor` (fields centred on the performers' mean foot xz), `world.ground` (a top-down
  2048^2 depth map of stage + prop models, back faces culled, read conservatively 3x3 by `PackGround[Height]`;
  `"extent": "camera"` surfaces = 3 nested LOD grids around the camera, `"ground": true` = height above the ground,
  clipped where there is none), `world.overlay` (premultiplied over the lit scene, negative slope bias, normal / velocity
  untouched; keep it ~2 cm above the ground), `world.body` (body colliders: head r 0.9, upper body 2 / upper body r 1.2,
  left / right shoulder / arm r 0.7, lower body r 1.3, flag `kColliderBody = 8`; packs without it see only the old
  feet / hands colliders in the old order), `world.lights` (the scene's sun + punctual direct lighting and shadows through
  `PackSunLight` / `PackPunctualLight` / `PackDirectDiffuse` / `PackDirectThin`: shadow maps in raster, one TLAS ray per
  light in RT / PT / offline GI; non-lights packs keep the old bindings and zero cost). Built-in `shaders/packs/snow` 2.0 ("눈"): snow in the camera box, cover on
  every upward ground surface, performer-anchored footprints, landing particles that thin out on trodden snow. No world
  gizmo / transform keys yet. Zero cost without worlds.
- Stage-bundled packs (S6, docs/stage_format.md `packs`): a stage folder may bundle packs of three roles, registered by
  `RegisterStagePacks` (play-mode scene load, studio add / open) as `stage:<stage id>/<pack folder>` through
  `ShaderPackRegistry::RegisterStagePack` (`PackSource::Stage`, the installed `LoadPackFolder` parser, one `[W]` and the
  stage keeps loading on a broken pack). They never appear in the manager / pickers / gallery and are never hot
  reloaded (`Packs()` / `Scan()` stay installed-only; `Find()` sees both, `stagePacks_` is the separate list).
  `surface` = the first entry whose `materials` globs hit a part's materials (`ResolveStageSurfacePack` computes the
  `StageSurfacePack` once: pack id + `kMaterialPackOff` for every material it does not claim); every frame
  `ApplyStageSurfacePack` calls `GpuModel::SetShaderPack` with the entry's current params (a stage part takes surface
  packs only, `GpuModel::SetShaderPack` drops other types for `ModelRole::Stage`; `ShaderPack::ClassifyStage` = the
  pack's own class rules, else its FIRST rule's class, else Body, so unclaimed stage materials never fall back to a
  bare Body class). Raster, RT, PT and offline GI draw that pack: a part's record reaches
  `render/PtPackVariants.*` and `RayTracing.cpp` (`TakesPtPack` = a model with a pack id that has
  `pt_surface.hlsl`; characters and stage parts alike), so `PackEvaluate` shades a stage material exactly like a
  character's, and the pack owns the surface (the engine's ambient-only stage term is skipped; its head frame is zeros
  because a part has no head bone).
  `world` = `AppendStageWorldPacks` appends one `WorldInstance` per entry after the user's
  `worldStack` (play) / the project's world entities (studio) at its socket (the socket node bone's world matrix x the
  explicit offset, resolved after the parts are posed; uid = `kStageWorldUidBase` + the entry index, so nothing is an
  editable outliner entity). `screen` = a bundled `"type": "screen"` pack: `StageScreens` renders it into the screen's
  own RGBA8 render-target texture with a fullscreen PSO (DXC `ps_6_0`, host `shaders/stage_screen.hlsl`, contract
  `shaders/screen_api.hlsli` / docs/shader_screen_api.md: `float4 PackScreen(PackScreenInput)` with uv, size, the song /
  timeline seconds, the pack params as 16 root constants at b2 and the v2 texture table) once per frame before the scene
  passes, so PT / offline GI / camera feeds read this frame's content and an offline render is exact. A screen is driven
  by either `screens[].source == "program"` (`file` = the pack folder) or a `packs[]` entry with `role: "screen"` +
  `screen` = its id (the `packs[]` entry wins; both go through `ResolveStageScreens`, which also replays
  `packs.<index>.params.<key>` targets into `ScreenView::params`). `params` of a manifest entry are the initial values;
  `packs.<index>.params.<key>` binding targets override them per frame for all three roles. Zero cost without entries:
  no registry entries, no PSOs, no render targets, identical frames. Fixture: `tools/stagegen/stagegen.py --only
  pack_test` (one pack of each role; the surface pack's `surface.hlsl` and `pt_surface.hlsl` draw the same tint in
  raster / RT and PT / offline GI; `tools/stage_check` + `pack_check --compile` pass on it).
- MME (docs/mme/mme_design.md; compatibility list docs/mme/compat_list.md): own .fx parser / preprocessor / IR in
  `asset/Mme*`, FXC SM5 wrappers `render/MmeCompiler` (object mode = host MRT), `render/MmeRuntime` (instances per
  (file, objectMode), targets, samplers, constant ring), `MmeScriptVM`, `MmeSemantics` (CONTROLOBJECT: missing object =
  bool false / items 0), passes `PassMme` (post chain), `PassMmeOffscreen` (DefaultEffect lists, MMD self-shadow),
  scene-pass object draws, PT / offline GI MME layer. Per-model choice None / Auto / File (characters None, stages Auto),
  post stack `mmeEffect=`, effect objects (`.x` + same-stem `.fx`, `--mme-object`). In the Auto workflow play scenes
  auto-load a required CONTROLOBJECT controller when its file exists (`asset/MmeAssign.cpp` `FindMmeControllerFile`,
  Ray-MMD files refused by `IsRayMmdControllerFile`; they are CONTROLOBJECT-visible but never drawn). Ray-MMD material files stay with the
  native PBR (Auto skips them; adding ray.x asks first). App-level controls: global MME switch `AppSettings::mmeEnabled`
  (ini `mmeEnabled=`, `--mme-global 0|1`; off = no MME object effect / no post stack / no effect objects, everywhere)
  and the separate global Ray-MMD material-source switch `AppSettings::rayMmdEnabled` (ini `rayMmdEnabled=`,
  `--ray-global 0|1`; off = every model's material source resolves to Default), plus workflow `AppSettings::mmeWorkflow`
  (ini `mmeWorkflow=manual|auto`, `--mme-workflow`; Auto "완전 자동" = every model always gets the automatic choices, ignoring the saved per-model ones,
  no suggestion toasts). `App::ResolveMmeChoice` / `ResolveMaterialChoice` centralise this (CLI `--mme-auto` /
  `--stage-mme` / `--mme-model` / `--material-source` still win; `--mme-global 0` wins over every MME choice,
  `--ray-global 0` over every material choice); the MME tab
  (app/UiMme.cpp, `--screen mme`) and the play-mode "화면 효과" side panel's MME · Ray-MMD tab (app/UiPlayPanel.cpp, a
  draft: nothing applies until 적용; the same panel's 효과 / 셰이더 tabs apply live, and its effect block is shared
  with the lobby's 세부 tab as `App::DrawScreenEffectControls`) edit the per-model choices, the post stack and the
  global switch. Compat harness: `python -I tools/mme/mme_compat_run.py
  [--only ranks] [--no-static] [--sheet-only]` over `captures/mme_lib` (git-ignored downloads: never execute anything
  there). Song packages: bundled stage + stage motion, audio versions, `bg.avi` / overlay video layers (`PassVideo`).
- MCP (docs/mcp.md): `tools/mmdx12_mcp.cpp` is the stdio MCP bridge; it forwards tool calls over the named pipe
  `\\.\pipe\mmdx12_mcp[_<pid>]` to `McpServer` (app/McpServer.*, worker thread), which queues them for the main thread:
  `App::PumpMcp` / `ExecuteMcp` (app/AppMcp.cpp) before ImGui::NewFrame. Tool schemas + timeouts live in app/McpTools.h
  (shared by both sides) as one JSON raw string per tool (`kMcpToolSchemas`): never write schemas as nested
  nlohmann::json initializer lists (at ~50 tools every including .cpp needed > 25 GB in MSVC and froze the machine;
  JSON text compiles in ~0.2 GB). Studio AI tools: `AppMcpAnalysis.cpp` (scene / motion / framing / beats),
  `AppMcpKeys.cpp` (bulk keys), `AppMcpCamera.cpp` (framing solver, shot lists), `AppMcpLight.cpp` (rigs, music cues),
  `AppMcpMotion.cpp` (layered motion edits); agent skills in `skills/` ship in the release zip. Navigation goes through `McpLeaveScreen` (the UI's own leave paths); multi-frame calls
  (loads, wait_frames, ui_input) resolve later from PumpMcp; `ui_input` / `studio_command` reuse `ExecuteUiCommand`
  (UiScript.cpp) with their own queue counted in `mcpFrame_`.
  Widget registry (`src/ui_probe/UiProbe.*`, compiled into the imgui target with `IMGUI_ENABLE_TEST_ENGINE`, on while the
  MCP server runs): imgui's test-engine hooks record every item of the last frame (rect, window, label = text before
  "##", id string); UiKit widgets report their id + visible text (`uiprobe::Info`), `CardItem(..., label)` names cards,
  drawn-only elements register with `uiprobe::Add` (timeline `##tlrow` / `##tlkey:<row>:<frame>`). MCP `ui_items` /
  `ui_click` match label or id (Korean source text also through `Tr()`, so one script serves every UI language) and skip
  clipped / covered items (`uiprobe::Covered`: topmost window under the centre). `screenshot` takes `path` / `region`.
  The website's user guide screenshots come from `MMDX12_Web/scripts/guide` (capture.py drives a packaged copy outside
  the repo, e.g. `E:\MMDX12`, per language; annotate.py -> public/guide/<lang>/*.webp). New UI: give clickable widgets
  stable ids so the guide script keeps working. Renders use `mcpOfflineOutput_`, never `options_.offline*`
  paths (those make a CLI job that quits). On by default (`mcp=` ini, lobby detail switch), headless runs need `--mcp`.
- `OfflineRenderer` (render/OfflineRenderer.h) is independent of `RenderSettings`: `Renderer::BeginOffline` builds the TLAS,
  then `Renderer::RenderOffline` replaces `Render` each frame (GPU-time-budgeted iterations, preview present) until Done.
  Motion blur: each iteration re-skins the character at its shutter time (`RtScene::Build(..., time)`) from the models'
  previous ring entry (the shutter-open pose; for videos the App re-uploads the previous video frame's pose there) and
  interpolates the camera (`SceneConstants::prevInvView`, offline_common.hlsli).
  GI: the prepass builds a screen-space irradiance cache (6 adaptive levels, 512 gather paths per sample, smoothed);
  `CSRender` takes indirect diffuse at camera hits from it via `IcLookup` (projects the hit into the frame camera,
  so lens/shutter samples work) and falls back to brute force where it has no matching surface.
  Measured (2026-10-23, 1080p): the cache is NOT the bottleneck (brute force `--offline-prepass 0` = +43 % time, same
  image). `CSPrepassLevel` runs one 32-lane group per cache sample (the gather paths split over the lanes); video frames
  skip the cache-lit dot display (`job.still` only). The sampling tail dispatches only the 8x8 tiles still sampling
  (`CSActiveTiles` -> tile list u7 / t5, count in its last row) once <= half the pixels are active. Each image logs one
  `OFFLINE TIMING` line (wall / GPU per phase, time at 256..max spp). Studio viewport "GI 미리보기" (`--studio-renderer gi`,
  transport bar dropdown, `App::RecordStudioGiViewport`) runs the same renderer at the viewport size: restart key =
  camera, time, history / project version, light, job settings, pose hash; half resolution on changes, full after 0.3 s.
  Note `--autoplay` and `--seek` are consumed after loading (App.cpp), so CLI triggers must not depend on them.
- Headers are the module contracts. Keep `App.h`, `Renderer.h` etc. authoritative when extending.

## Conventions / pitfalls
- Coordinates: native MMD space (left-handed, +Y up), which equals D3D. Never flip axes.
- Math is DirectXMath row-vector (`v * M`). HLSL uses `pack_matrix(row_major)` + `mul(v, M)`.
  - FXC ignores `pack_matrix` for StructuredBuffer elements. Wrap the type in a struct with `row_major` (see `BoneMatrix` in mmd.hlsl).
- Quaternions: `XMQuaternionMultiply(a, b)` means "a, then b" (it equals glm `b * a`).
- VMD interpolation byte layouts are documented in `src/asset/VmdMotion.h`.
  Some camera VMDs use distance 0 (the target is the eye position). `CameraMotion::ToView` handles this.
- Per-frame GPU buffers must live in the UPLOAD heap and stay persistently mapped. `UploadBatch` is for DEFAULT-heap static data only.
- UI text is Korean. Fonts are Pretendard + Phosphor icons (assets/fonts, copied to bin by `copy_assets`), with YuGoth/msyh merged for JP/CN. Pretendard's Private Use Area is excluded so Phosphor glyphs win.
- Every Korean UI string goes through `Tr("...")` (or `TrKey("...")` when a table of labels reaches `Tr` through a variable) and needs En / Ja / Zh entries; the build runs `tools/i18n_check.py` (missing keys, duplicates, printf mismatches fail it); Ja / Zh fall back to English for a missing key.
- Runtime shader compile errors only appear in `build/bin/mmdx12.log` as `[E]` lines (warnings `[W]`); a failed optional pipeline silently falls back, so grep the log after every capture. `line` is a reserved word in HLSL (DXC rejects it as a variable name).
- Shaders under `build/bin/shaders` compile at runtime, so a shader can be debugged by editing that copy and rerunning (restore it afterwards).
- `library/` (MMD assets, about 1.4 GB) and `captures/` are git-ignored test data. Never modify `library/`.
- `external/` is vendored third-party code (imgui, stb, miniaudio, DirectX-Headers, nlohmann json, Bullet 3.25 subset, cgltf 1.15, ufbx 0.23.1). Don't edit it. imgui is the `docking` branch (same version number as master 1.93 WIP).
  - miniaudio's implementation lives in `core/AudioProbe.cpp` (the scanner measures audio lengths).
  - Bullet is built per file, not from its `*All.cpp` unity files (`btVector3.cpp` defines `BT_USE_SSE_IN_API`, which breaks later files).

## Private notes
- Server and asset-source details live in `CLAUDE.local.md` (git-ignored). Never commit server paths, hostnames or local user paths.

## Session close ("세션 마무리")
1. Commit everything so the working tree is clean.
2. Append a dated summary of the session to progress.md, and include it in the commit.
