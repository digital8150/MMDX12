# Phase 4b: point-light shadows in the real-time renderer (MMDX12 studio lights)

## Goal
Point lights get shadows in the real-time raster path and in the RT path. Phase 4a (commit 6a6dd25) already carries the per-light
shadow type (NoCast / Hard / Soft), softness, density and shadow colour through `PunctualLight` / `GpuLight`, honours them for
spots (shadow maps), the sun, and in the path tracer and offline GI (those trace real shadow rays for every punctual light
already). What is missing: raster has no shadow map for a point light, and RT has none either (it reuses the spot maps).

## Product requirements (decided by the user)
- A point light with shadow type Hard or Soft casts a shadow in raster and in RT; NoCast casts none. Soft = a wider penumbra
  growing with the light's softness; density and shadow colour work through the existing `ShadowTransmission`.
- Only characters cast point-light shadows (like the spot slices and the path tracer's RT_MASK_CHARACTER): preset lights hang on
  a virtual truss above the stage, so the stage must not occlude them.
- At most N point lights get a shadow at once (N = 4; the first N enabled, shadow-casting point lights in list order). The
  others render unshadowed. 6 faces per light, so keep the face resolution modest (1024) and the cost zero when no point
  light qualifies: no extra passes, draws or per-frame work, and allocate the map lazily.
- **No contamination of existing looks (the user's main concern).** Play mode, the lobby and the benchmark build their point
  lights in `BuildLighting`; those lights must keep rendering exactly as before (no raster shadow). Make the new shadow
  opt-in with a flag on `PunctualLight` whose default is off; only `BuildSceneLighting` (the studio) sets it from the light's
  shadow type. Shader packs call `PunctualDiffuse` / `PunctualSpecular`: keep their signatures, they simply get the shadow
  when the light has one. A scene without a qualifying point light must be pixel-identical to the commit before this phase.
- Hard / Soft filtering follows the spot code (`SpotShadow` in shaders/mmd.hlsl): Hard = a small fixed PCF, Soft = a wider
  Poisson filter growing with softness.

## Architecture direction
- Raster: a cube-like depth array (6 slices per light) rendered by the existing ShadowPass (src/render/Passes.cpp, `DrawSlice`,
  characters only). The scene constants block is limited to 2048 bytes (`kSceneCbSize`, now ~1400 used): do not store 24
  matrices there. Derive the face and its projection in the shader from the major axis of (world position - light position),
  with a fixed near / far per light (the spots use `range * 0.004` .. `range`), or put the matrices in a small per-frame
  buffer if you prefer; your choice, but state it.
- Normal-offset bias and depth bias must hold up on character skin and hair (compare with `SpotShadow`).
- RT path: the scene pixel shader already runs ray queries for the sun (`RT_SHADOWS`, `ShadowRt`). Use ray queries against the
  characters (the same mask the path tracer uses) for qualifying point lights; soft = cone-jittered rays like the sun's. Do not
  change how spots are shadowed in RT.
- Volumetric light marching (volumetric.hlsl) is not part of this phase.
- Path tracer and offline GI: they already shadow every punctual light by tracing; change nothing unless you find that a
  property from 4a is not honoured there (then report it).
- Offline GI / PT results must stay bit-identical for scenes that do not use the new flag.

## Constraints
- Do not change src/studio/ (data, save format), the light UI, gizmos or project files.
- Do not commit and do not change git state.
- NEVER stop, kill or close any running process. If a build or a run is blocked by a locked file, stop and report it.
- App runs: strictly one at a time, at most 6 runs in total (each opens a visible window and quits by itself). Put several
  `capture` commands into one `--ui-script` run. Write PNGs and logs to build_dev\captures_phase4b\.
- NOTE `--render rt|pt` is ignored on `--screen studio` (the setting is restored after start-up, App.cpp ~234 / ~354).
  To capture the RT path in the studio you need a way to select it: find a minimal one (a UI-script command in
  src/app/UiScript.cpp such as `studiorender raster|rt|pt` is allowed; no other app change) and use it. Check that the
  capture really differs between raster and RT before you call anything verified.

## Non-goals
Area lights (4c), the volumetric path, a shadow for stage geometry, more than N shadowed point lights, UI changes.

## Verification
1. `E:\repos\MMDX12\build.cmd build_dev` finishes with 0 errors (PowerShell, PATH may need
   `C:\Program Files (x86)\Microsoft Visual Studio\Installer` and `C:\Users\admin\AppData\Roaming\Python\Python314\Scripts`);
   grep build_dev\bin\mmdx12.log for `[E]`.
2. These build and pass: build_dev\bin\studio_project_test.exe, studio_light_test.exe, studio_edit_test.exe,
   studio_gizmo_test.exe, studio_pose_test.exe, shader_choice_test.exe. Add checks to tools/studio_light_test.cpp for the new
   flag: BuildSceneLighting sets it for a Hard / Soft point light and not for NoCast / spot, BuildLighting never sets it.
3. Captures (raster, then RT): a studio scene with a character and a point light in front-above (`studiolightadd point`,
   `studiolightset`, see src/app/UiScript.cpp): NoCast / Hard / Soft / density 0.5 / red shadow colour, each showing a
   character shadow on the floor (or its absence). Also a scene with 5 point lights proving the 5th is simply unshadowed.
   List every run (command, result, what the picture shows) in the report and look at the PNGs yourself.
4. Play mode with the default preset must be untouched: say how you made sure (the flag default, no pass work).

## Start here
src/render/Passes.cpp (ShadowPass: SpotShadowCount, DrawSlice, Execute ~150-221), src/render/Passes.h (SpotShadowsWanted ~40),
src/render/Renderer.cpp (spot slices in FillSceneConstants ~356-376, FillGpuLights ~413, target creation ~218),
src/render/RenderPass.h (RenderTargets), src/render/RenderTypes.h (PunctualLight), src/render/ShaderInterop.h (GpuLight,
SceneConstants), shaders/mmd.hlsl (SpotShadow, PunctualDiffuse, PunctualSpecular, ShadowRt ~100-250), shaders/common.hlsli
(PunctualFalloff, ShadowTransmission, cbuffer), shaders/shadow.hlsl (the depth pass shader), src/app/Lighting.cpp
(BuildSceneLighting), src/app/UiScript.cpp (studiolightadd, studiolightset), tools/studio_light_test.cpp.

## Off limits
external/, library/, library.zip, captures/ (read-only), videos/, dist/, build/, build_au/, build_release/, CLAUDE.local.md,
docs/, progress.md, src/studio/, src/app/UiStudio*.cpp, src/render/PassDof.cpp (someone else edits the DoF pass).
build_dev is yours.

## Decided (do not re-research)
- Characters only; N = 4 shadowed point lights; 1024 per face; opt-in flag on PunctualLight, default off, set only by
  BuildSceneLighting.
- Spots keep their shadow maps in raster and RT; PT / GI stay as they are.
- Hard = small fixed PCF, Soft = wider Poisson growing with softness; density / colour via ShadowTransmission.
