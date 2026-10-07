# CLAUDE.md

MMDX12: a Windows Direct3D 12 MikuMikuDance client (C++20). See README.md for features and CLI;
progress.md is the session log. Read its latest entry first.

## Build & verify
- `build.cmd [dir] [--target X]` sets up the VS 18 environment and builds with CMake + Ninja (RelWithDebInfo).
  The default dir is `build`; the output is `build\bin\MMDX12.exe`. CMake and Ninja come from pip (`%USERPROFILE%\miniconda3\Scripts`).
  From bash: `cmd //c "F:\\repos\\MMDX12\\build.cmd build"`.
- The source lists are globbed (`CONFIGURE_DEPENDS`). If a new `.cpp` is not picked up, delete the build dir.
- Shaders are compiled at runtime from `build/bin/shaders` (the `copy_shaders` target keeps them in sync).
  Raster shaders use FXC (`*_5_1`); ray-query shaders use DXC (`*_6_5`, `-HV 2018` because common.hlsli uses vector ternaries that HLSL 2021 rejects). `dxcompiler.dll`/`dxil.dll` are copied from the Windows SDK.
- Upscaler runtime DLLs are git-ignored: run `tools/fetch_sdks.ps1` once (DLSS 310.9.1, XeSS 3.0.2, FidelityFX 2.3.0), then rebuild. Missing DLLs only disable that upscaler.
- Render path / upscaler from the CLI: `--render raster|rt|pt`, `--upscaler none|dlss|fsr|xess`, `--upscale-quality native|quality|balanced|performance|ultra`.
- No debugger is installed (no cdb). For crashes, use the dbghelp `CrashFilter` in `tools/render_smoke.cpp`.
- Visual verification without a human: render a frame headlessly, then Read the PNG.
  - Play scene: `MMDX12.exe --character <s> --stage <s|none> --song <s> --autoplay --seek <sec> --frames N --capture out.png`
  - Menu screens: `--screen select|bench --frames N --capture ui.png`
  - Offline GI still/video: `--autoplay --seek <sec> --offline-still out.png` (or `--offline-video out.mp4 --offline-range a b`);
    add `--offline-size 960 540 --offline-spp 256` for quick checks. The app quits by itself when done.
    Video format: `--offline-fps 30 --offline-bitrate 20 --offline-quality 0..3` (default: the saved lobby dialog settings, `AppSettings::video`).
    Lobby path (select screen -> load -> render -> back to select): `--character c --song s --screen video --offline-video out.mp4 --offline-range a b`.
    `--screen video` alone opens the lobby's render dialog for a capture.
    Video renderer: `--offline-renderer raster|rt|pt|gi` (the real-time ones render frame by frame at the video size via `Renderer::Render` +
    `ReadFinalImage`; PT accumulates several passes per frame). The render dialog measures its time estimate by itself: a headless sample render behind the UI (`--screen video` shows it; log `VIDEO PROBE`,
    results in `videoProbe=` ini lines). `--offline-probe` runs the same measurement from a play scene.
    The dialog's effects are `VideoRenderConfig` (bloom, convolution bloom, volumetric, DoF only for the real-time renderers);
    stills use the effects chosen at scene entry (`AppSettings`). Check `build_dev` builds when `build/` is locked by a running render.
  - Studio: `--character <s> [--stage <s>] [--song <s>] --screen studio --seek <sec> --frames N --capture out.png`.
    `--screen studio` without `--character` opens a new empty project; `--project <file.mmdxproj>` opens a project.
    Studio render (quits when done): `--project p.mmdxproj --offline-video out.mp4 [--offline-range a b]` (timeline
    seconds; default: the timeline range, else the whole project; motion camera; project audio + offset) or
    `--offline-still out.png [--seek s]` (GI, the view the viewport shows). `--offline-renderer/-size/-fps/-spp` apply.
  - High-DPI captures: `--ui-scale 1.5` replaces the monitor's DPI scale (1080p window + 1.5 = a 150 % laptop screen).
  - Scripted UI input: `--ui-script file.txt` (lines `<frame> <command> [args]`: move/down/up/click/dblclick/wheel/key,
    `capture <png>`, `text <chars>`, `mod ctrl|shift down|up`, `studiostate` logs a `STUDIOSTATE` line,
    `studioexport`/`studioimport <vmd>` and `studiovpdexport`/`studiovpdimport <vpd>` skip the file dialogs; pose editing:
    `studiobone <name>` clicks that bone's joint, `studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [steps]` drags a gizmo
    part; `studiostate` also logs `STUDIOPOSE`, `STUDIOPROJ` and one `STUDIOMODEL` line per model). Projects/models
    without dialogs: `studionew`, `studiosave`/`studioopen <file>`, `studioautosave`, `studioadd <character|stage|prop> <file>`,
    `studioaddlib <character|stage> <substr>`, `studiosong <substr>`, `studioaudio <file|none> [offset]`,
    `studioselect`/`studioremove <index>`, `studiorename <text>`, `studioattach <parent|-1> <bone|-> tx ty tz rx ry rz s`.
    While a script runs, real mouse/keyboard input is ignored.
    Frames count like `--frames`. Use it to click through editor features headlessly (examples: `captures/studio/t*.txt`).
  - Benchmark: `--benchmark dx12-raster-fhd --bench-frames 600 --frames 100000` (result goes to `build/bin/mmdx12.log` as a `BENCHMARK` line)
  - GI render benchmark: `--benchmark dx12-gi-render` (quits by itself; ~41 s). Quick check: add `--bench-spp 64 --offline-size 960 540`.
    Add `--frames 100000 --capture out.png` to capture the result screen.
  - Each run opens a visible window: never launch app runs in parallel or in batches without telling the user.
- Tools:
  - `asset_probe <library> [--full]`: scan, classify (prints notes on skipped/guessed files), and load everything
  - `anim_probe <pmx> <vmd> [cam.vmd]`: IK convergence, CPU skinning bounds, NaN scan, physics scan (explosions, cost)
  - `render_smoke`: renders a cube with no assets
  - `vmd_roundtrip <file|dir> [--vpd-selftest]`: VMD load -> save -> load comparison (SaveVmd), VPD self test
  - `studio_edit_test`: Studio key-edit core (move/insert/delete frames, undo byte budget, 100k-key timings)
  - `studio_gizmo_test` / `studio_pose_test`: gizmo projection/hit/drag math and bone overlay; mirror names/poses, VPD pose ops
  - `studio_project_test`: .mmdxproj save/load round trip, VMD naming/cleanup, atomic writes, prop offset matrix
- The play bar auto-hides while playing with no mouse movement, so captures usually don't show it.

## Architecture (src/)
- `core`: logging, text encodings (Shift-JIS/UTF-16/UTF-8).
- `asset`: PMX/VMD parsers, image loading (stb with a WIC fallback, also from memory), `AssetLibrary`, `ModelImport`.
  - `AssetLibrary` precedence: app overrides (`library_overrides.json` next to the exe, right-click on a card) > `mmdx.json`
    sidecars > folder names (characters/models, stages, songs/motions, several languages) > content. Several songs in one
    folder are split only when it also holds several audio files or cameras; audio/cameras are matched by length + file name.
    New installs get `assets/library_template` (characters/ stages/ songs/) copied into an empty library.
  - `ModelImport`: `LoadModelFile(path, role)` reads PMX natively and converts glTF/GLB/VRM (cgltf) and FBX/OBJ (ufbx) into a
    `PmxModel` via `ImpScene` (ImportScene.h). Characters: humanoid map (VRM table or name dictionary) -> MMD bone names,
    synthesised センター/グルーブ + leg IK, arms re-posed to 38 deg (MMD A-pose), morph aliases (あいうえお, まばたき); no physics.
    Stages: baked, one root bone, moved onto their main floor when nothing is under the origin (`ModelRole::Prop`: baked,
    not moved — studio props). Embedded textures live in
    `PmxModel::embeddedTextures`. Test assets: `captures/testlib` (messy layout) and `captures/assets_dl` (three.js/Khronos/VRM samples).
- `anim`: `ModelInstance` (bones, append, IK, morphs) and `Motion` (VMD Bezier evaluation, camera).
  - `PhysicsWorld` (Bullet, `Physics.cpp` only) runs between the before- and after-physics bones in `UpdatePose(dt)`.
    Only the character enables it. `App::UpdateScene` derives dt from the motion clock and resets the bodies on seeks/loops.
- `render`: `Dx12Context` (device, frames, descriptors, `UploadBatch`, PNG capture) and `Renderer`.
  - The renderer runs an ordered `IRenderPass` list (see `Passes.h`): Shadow → Scene → Resolve → PathTrace → SSAO → SSR → Composite → Volumetric → TAA → Upscale → DoF → Bloom → Post → Backdrop → Present. `PassContext::path` decides which passes work (RayTraced: ray-query sun shadows in the scene PS, RTAO, RT reflections; PathTraced: compute path tracer + temporal/à-trous denoiser writing the G-buffer). Passes own their targets via `OnResize`; inputs are bound through per-frame `TransientDescriptors`.
    Pass implementations live in `Passes.cpp` and `Pass*.cpp` (shared helpers in `PassCommon.h`). Volumetric and the FFT bloom are `ComputePipeline` (cs_6_5) and silently disable themselves without DXR-class hardware.
    Colour LUTs (`ColorLut.h`) are 32³ strips uploaded with `Renderer::SetColorLut`; PostPass applies them after the sRGB encode.
  - `GpuModel` keeps a 3-frame ring of bone/morph buffers so the previous pose (motion vectors) stays valid.
  - `GpuModel` handles GPU skinning. `RtScene` (RayTracing.h) skins every model into world-space `RtVertex` buffers with a compute shader and builds BLAS (characters every frame, stages once) + TLAS; shaders read geometry bindlessly (`rt_common.hlsli`, `RtGeometry` table, unbounded tables over the whole SRV heap, root signature 1.0 so descriptors stay volatile).
  - Render resolution (`targets.width/height`) vs output resolution (`outWidth/outHeight`): the upscaler (DLSS/FSR/XeSS behind `IUpscaler`) runs after TAA/composite; bloom, post, backdrop and present work at output size. Jitter uses the FSR convention (`jitterPx`), motion vectors are uv(cur) − uv(prev) so SDK MV scale is −renderSize.
- `app`: the `App` state machine, ImGui screens (`Ui*.cpp`) built on `UiKit` (tokens, fonts, widgets; see DESIGN.md), `ThumbnailCache`, `Lighting` presets, `SceneLoader` (worker thread), benchmark, WinHTTP leaderboard.
- `audio`: miniaudio. The audio cursor is the master clock.
- `studio` (+ `app/UiStudio*.cpp`): the Studio editor (Screen::Studio). `StudioDoc` holds the models with name-keyed editable
  motions (`MotionData`, StudioMotion.h, converted to VmdMotion and re-bound with `BoundMotion::Bind` after edits), the camera
  track, `CommandStack` undo (edits are `TrackEditCommand` track snapshots / `MotionSwapCommand`) and the editor state.
  `UiTimeline`/`UiBezier` are data-agnostic widgets. The viewport is the renderer drawing into `RenderSettings::viewport*`.
  Compatibility with MMD goes through standard files: VMD (`SaveVmd`) and VPD (`asset/VpdFile.h`); PMM is not supported.
  - VMD bone interpolation: bytes 2/3 are MMD physics flags; Z/rotation x1 are bytes 17/18 (`GetBoneCurve`/`SetBoneCurve`).
  - Pose editing (`app/UiStudioPose.cpp`, `studio/Gizmo.*`, `studio/StudioPose.*`): viewport edits go to the model's
    `PoseLayer` (bone/morph overrides valid at one frame, applied after `BoundMotion::Evaluate`, so drags never re-bind).
    Leaving the frame discards it as an undoable `PoseEditCommand`; Register (I, Ctrl+I all bones) writes keys +
    clears the layer in one `CompositeCommand`. Bone values are VMD-local (parent frame); the gizmo converts world deltas
    with the parent's world rotation. Bones listed in two display frames select keys through `CanonicalRow`.
  - `asset/ModelImport.h` and `render/GpuModel.h` both declare `mmdx::ModelRole`: never include both in one .cpp.
  - Camera/light/self-shadow (`app/UiStudioCamera.cpp`): `StudioDoc::camera` holds the camera, light (`LightKf`, linear) and
    self-shadow (`ShadowKf`, stepped, VMD distance = 0.1 - UI*1e-5) tracks as timeline rows `RowKind::Camera/Light/Shadow`.
    In the studio the light track overrides the preset's key light and the shadow track sets `FrameView::shadowsOff`/
    `shadowDistance` (play mode ignores VMD light/shadow). Perspective-off keys render as a 3 degree lens from far away.
  - Projects (`studio/StudioProject.*`, `app/UiStudioProject.cpp`): `.mmdxproj` is UTF-8 JSON (format version, relative
    paths) next to standard VMDs (`<stem> - <model>.vmd`, `<stem> - camera.vmd`); every file is written tmp + rename.
    Dirty = `history.Version()` or `projectVersion` (add/remove/rename/visibility/audio) changed since the save. Autosave
    every 60 s on a worker thread to `<exe>/recovery/` (also on window close with unsaved work, not in `--frames`/script
    runs); the select screen offers it at the next start; saving or leaving the studio deletes it.
  - Models are added inside the studio on worker threads (`StudioJob`, `LoadStudioModel/Stage/Song`), uploaded in
    `StudioPollJobs`. `StudioModel::uid` is stable; removal waits for the GPU and clears the undo history (commands hold
    model indices). Kinds: character, stage (static BLAS), prop (GPU role Character so its BLAS follows it): the prop's
    root = `PropOffsetMatrix(attach) * parent bone world * parent scale`, set with `ModelInstance::SetRootTransform`
    after the other models are posed (props parent to characters/stages only).
  - Rendering (`app/UiStudioRender.cpp`): the top bar's render menu opens the lobby's video dialog (`DrawVideoRenderDialog`
    is studio-aware: range section, no sample-render probe) or starts a GI still. `OfflineJob::studio` routes the offline
    job through `StudioPoseForRender` / `BuildStudioFrameView` (`OfflinePose`/`OfflineView` in UiOffline.cpp); the
    viewport rect is cleared for the render and `StudioRestoreAfterRender` puts time, camera mode and physics back.
    Videos skip unregistered pose edits. Audio: `VideoEncoder::Desc::audioStartSeconds` = start - audioOffset (negative:
    leading silence). Shortcut overlay: `DrawStudioHelp` (? / F1); `StudioModal()` blocks the studio's shortcuts.
  - Panels (`App::DrawStudio`): ImGui docking (the vendored imgui is the docking branch). A fixed top-bar window, a transparent
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
  in the website repo): opt-in per-character replacements of the scene pass's material shading. A pack = `pack.json`
  (metadata, localized names, authors, license, tags, `apiVersion`, material class rules, <= 16 params) + `surface.hlsl`
  implementing `PackShade` (contract: `shaders/pack_api.hlsli`, `kPackApiVersion`) + optional `preview.png`; built-in in
  `shaders/packs/<id>`, installed in `<exe>/shader_packs/<id>`; template `shaders/pack_template`; `tools/pack_check`.
  Registry `render/ShaderPack.*` (status per pack, hot reload `PollChanges`, install / uninstall / template); online gallery
  `app/ShaderPackStore.*` (index.json, SHA-256 checked zips); manager screen `app/UiShaders.cpp` (Screen::Shaders, top tab
  "셰이더", `--screen shaders|shaders-online`), shared picker `app/UiShaderPack.cpp`. Networking / zip / version helpers live
  in `core/NetUtil.*` (shared with the updater).
  `GpuModel::SetShaderPack` writes class / head bone / params into `MaterialConstants`; `ScenePass::PackPsos` compiles
  mmd.hlsl's `PSPack` with DXC (FXC cannot `#include` a macro: `MMDX_PACK` = the quoted surface path) on first use and
  falls back to the default PSOs on errors (`[E]` + toast). Lit raster / RT camera view only; PT, offline GI, unlit / wire /
  ortho views use the default. A negative normal-target reflectivity (`nt.z < 0`) = "no AO" (composite). Choice is saved per
  character (`characterShader=` ini) / per studio model (`.mmdxproj` "shader"); `--shader-pack <id|none>` overrides a run.
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
  sets follow the texture sheet (hoyo_toon_v2 picks hair maps for anything on the hair sheet). A pack with all textures missing
  still compiles and renders. `PACK_HAS_EDGE` + `PackEdge` in surface.hlsl wire the pack into the edge pass (per-material
  outline colour + width scale; `PSEdgePack` / a `VSEdge` width-scale build from `PackPsos`, root param 12 bound there
  too); otherwise the edge pass is unchanged. `PACK_WEAPON` = 5 joins the material classes; built-in `hoyo_toon_v2` (issue #2) = Genshin light maps / ramps / face SDF
  from a user texture folder, v1 fallback per missing map; `tools/pack_check` validates
  textures (format / count / size / total ≤ 32 MB / bad paths; missing files are warnings) and literal
  `PackSampleTex(N)` indices against the declared count.
- `OfflineRenderer` (render/OfflineRenderer.h) is independent of `RenderSettings`: `Renderer::BeginOffline` builds the TLAS,
  then `Renderer::RenderOffline` replaces `Render` each frame (GPU-time-budgeted iterations, preview present) until Done.
  Motion blur: each iteration re-skins the character at its shutter time (`RtScene::Build(..., time)`) from the models'
  previous ring entry (the shutter-open pose; for videos the App re-uploads the previous video frame's pose there) and
  interpolates the camera (`SceneConstants::prevInvView`, offline_common.hlsli).
  GI: the prepass builds a screen-space irradiance cache (6 adaptive levels, 512 gather paths per sample, smoothed);
  `CSRender` takes indirect diffuse at camera hits from it via `IcLookup` (projects the hit into the frame camera,
  so lens/shutter samples work) and falls back to brute force where it has no matching surface.
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
