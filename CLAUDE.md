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
  - Benchmark: `--benchmark dx12-raster-fhd --bench-frames 600 --frames 100000` (result goes to `build/bin/mmdx12.log` as a `BENCHMARK` line)
- Tools:
  - `asset_probe <library> [--full]`: scan, classify, and load everything
  - `anim_probe <pmx> <vmd> [cam.vmd]`: IK convergence, CPU skinning bounds, NaN scan, physics scan (explosions, cost)
  - `render_smoke`: renders a cube with no assets
- The play bar auto-hides while playing with no mouse movement, so captures usually don't show it.

## Architecture (src/)
- `core`: logging, text encodings (Shift-JIS/UTF-16/UTF-8).
- `asset`: PMX/VMD parsers, image loading (stb with a WIC fallback), `AssetLibrary` (content-based classification).
- `anim`: `ModelInstance` (bones, append, IK, morphs) and `Motion` (VMD Bezier evaluation, camera).
  - `PhysicsWorld` (Bullet, `Physics.cpp` only) runs between the before- and after-physics bones in `UpdatePose(dt)`.
    Only the character enables it. `App::UpdateScene` derives dt from the motion clock and resets the bodies on seeks/loops.
- `render`: `Dx12Context` (device, frames, descriptors, `UploadBatch`, PNG capture) and `Renderer`.
  - The renderer runs an ordered `IRenderPass` list (see `Passes.h`): Shadow → Scene → Resolve → PathTrace → SSAO → SSR → Composite → TAA → Upscale → Bloom → Post → Backdrop → Present. `PassContext::path` decides which passes work (RayTraced: ray-query sun shadows in the scene PS, RTAO, RT reflections; PathTraced: compute path tracer + temporal/à-trous denoiser writing the G-buffer). Passes own their targets via `OnResize`; inputs are bound through per-frame `TransientDescriptors`.
  - `GpuModel` keeps a 3-frame ring of bone/morph buffers so the previous pose (motion vectors) stays valid.
  - `GpuModel` handles GPU skinning. `RtScene` (RayTracing.h) skins every model into world-space `RtVertex` buffers with a compute shader and builds BLAS (characters every frame, stages once) + TLAS; shaders read geometry bindlessly (`rt_common.hlsli`, `RtGeometry` table, unbounded tables over the whole SRV heap, root signature 1.0 so descriptors stay volatile).
  - Render resolution (`targets.width/height`) vs output resolution (`outWidth/outHeight`): the upscaler (DLSS/FSR/XeSS behind `IUpscaler`) runs after TAA/composite; bloom, post, backdrop and present work at output size. Jitter uses the FSR convention (`jitterPx`), motion vectors are uv(cur) − uv(prev) so SDK MV scale is −renderSize.
- `app`: the `App` state machine, ImGui screens (`Ui*.cpp`) built on `UiKit` (tokens, fonts, widgets; see DESIGN.md), `ThumbnailCache`, `Lighting` presets, `SceneLoader` (worker thread), benchmark, WinHTTP leaderboard.
- `audio`: miniaudio. The audio cursor is the master clock.
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
- Shaders under `build/bin/shaders` compile at runtime, so a shader can be debugged by editing that copy and rerunning (restore it afterwards).
- `library/` (MMD assets, about 1.4 GB) and `captures/` are git-ignored test data. Never modify `library/`.
- `external/` is vendored third-party code (imgui, stb, miniaudio, DirectX-Headers, nlohmann json, Bullet 3.25 subset). Don't edit it.
  - Bullet is built per file, not from its `*All.cpp` unity files (`btVector3.cpp` defines `BT_USE_SSE_IN_API`, which breaks later files).

## Session close ("세션 마무리")
1. Commit everything so the working tree is clean.
2. Append a dated summary of the session to progress.md, and include it in the commit.
