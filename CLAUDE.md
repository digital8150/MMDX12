# CLAUDE.md

MMDX12: a Windows Direct3D 12 MikuMikuDance client (C++20). See README.md for features and CLI;
progress.md is the session log. Read its latest entry first.

## Build & verify
- `build.cmd [dir] [--target X]` sets up the VS 18 environment and builds with CMake + Ninja (RelWithDebInfo).
  The default dir is `build`; the output is `build\bin\MMDX12.exe`. CMake and Ninja come from pip (`%USERPROFILE%\miniconda3\Scripts`).
  From bash: `cmd //c "F:\\repos\\MMDX12\\build.cmd build"`.
- The source lists are globbed (`CONFIGURE_DEPENDS`). If a new `.cpp` is not picked up, delete the build dir.
- Shaders are compiled at runtime from `build/bin/shaders` (the `copy_shaders` target keeps them in sync).
- No debugger is installed (no cdb). For crashes, use the dbghelp `CrashFilter` in `tools/render_smoke.cpp`.
- Visual verification without a human: render a frame headlessly, then Read the PNG.
  - Play scene: `MMDX12.exe --character <s> --stage <s|none> --song <s> --autoplay --seek <sec> --frames N --capture out.png`
  - Menu screens: `--screen select|bench --frames N --capture ui.png`
  - Benchmark: `--benchmark dx12-raster-fhd --bench-frames 600 --frames 100000` (result goes to `build/bin/mmdx12.log` as a `BENCHMARK` line)
- Tools:
  - `asset_probe <library> [--full]`: scan, classify, and load everything
  - `anim_probe <pmx> <vmd> [cam.vmd]`: IK convergence, CPU skinning bounds, NaN scan
  - `render_smoke`: renders a cube with no assets
- The play bar auto-hides while playing with no mouse movement, so captures usually don't show it.

## Architecture (src/)
- `core`: logging, text encodings (Shift-JIS/UTF-16/UTF-8).
- `asset`: PMX/VMD parsers, image loading (stb with a WIC fallback), `AssetLibrary` (content-based classification).
- `anim`: `ModelInstance` (bones, append, IK, morphs) and `Motion` (VMD Bezier evaluation, camera).
- `render`: `Dx12Context` (device, frames, descriptors, `UploadBatch`, PNG capture) and `Renderer`.
  - The renderer runs an ordered `IRenderPass` list: Scene → Resolve → Present.
  - `GpuModel` handles GPU skinning. `IUpscaler` is the seam for DLSS/FSR/XeSS.
- `app`: the `App` state machine, ImGui screens (`Ui*.cpp`), `SceneLoader` (worker thread), benchmark, WinHTTP leaderboard.
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
- UI text is Korean. Fonts are malgun, with YuGoth and msyh merged in for JP/CN glyphs.
- `library/` (MMD assets, about 1.4 GB) and `captures/` are git-ignored test data. Never modify `library/`.
- `external/` is vendored third-party code (imgui, stb, miniaudio, DirectX-Headers, nlohmann json). Don't edit it.

## Session close ("세션 마무리")
1. Commit everything so the working tree is clean.
2. Append a dated summary of the session to progress.md, and include it in the commit.
