# Phase 4c: rectangular area lights (MMDX12 studio lights)

## Goal
The studio gets a fifth light kind, **Area** (a one-sided rectangle, like C4D / Blender area lights). It is a full studio light
object: add menu, outliner row, inspector, keys, undo, save / load, viewport handles, script commands, and it renders in raster,
RT, the real-time path tracer (PT) and offline GI. Phases 1-4b built the other kinds; imitate how the **spot** and **point**
kinds are done everywhere.

## Model (decided)
- `studio::LightKind::Area = 4`, project-file name `"area"`. Default name "면광원 N" (like "점광원 N" / "스팟 N").
- Placement: `v.position` = the rectangle's centre, `v.aim` = the point it faces (manual aim only: no Target / Sway modes for
  area lights). The emitting side faces the aim point; the back side emits nothing.
- Size: add `XMFLOAT2 size{20, 20}` (width, height, MMD units) to `LightValues`: keyable, interpolated linearly like the other
  values, included in `operator==`. The rectangle's width axis is horizontal: right = normalize(cross(worldUp, normal)), with a
  fixed fallback axis when the normal is (anti)parallel to world up. The same rule must be used in C++ (handles) and in every
  shader, so the rectangle the handles draw is the one that is lit.
- Colour, intensity, range, falloff, shadow type / softness / density / colour, diffuse / specular affect: as for a point light.
- Shadow: NoCast = none. Hard and Soft both cast; the penumbra comes from the rectangle's size (Soft widens it further with the
  softness slider in the real-time paths).
- Not in presets and never produced by play mode (`BuildLighting`).
- Save format: version stays 2; the light's `"values"` gain `"size": [w, h]` (missing = default). Older lights load unchanged.

## Lighting definition (decided — all renderers must agree)
An area light of intensity I is the limit of I spread uniformly over the rectangle as tiny point lights, each emitting with a
one-sided cosine (`max(0, dot(lightNormal, -L))`) and each using the light's ordinary `PunctualFalloff(dist, invRange, falloff)`.
So, for a small rectangle seen head-on, an area light looks like a point light of the same intensity at its centre.
- PT and offline GI: real area sampling — pick a uniform random point on the rectangle per light sample, shade with the formula
  above, shadow ray to that point (characters + stage as the existing point-light shadow rays do in each shader; keep whatever
  mask each shader already uses for point lights). Every place in those shaders that loops over punctual lights must handle it
  (offline_gi.hlsl has several: camera hits, the irradiance-cache gather, ... — find them all).
- Raster and RT scene pass (mmd.hlsl `PunctualDiffuse` / `PunctualSpecular`): approximate with the centre point times the
  one-sided cosine factor. Keep these functions' signatures (shader packs call them).
  - Raster shadow: the area light uses the existing point-light shadow map path (it counts as a shadowed point light in
    `PointShadowCount` / `FillGpuLights` / ShadowPass; it is a light with `spotCosOuter <= -1` plus area data), with a Poisson
    radius growing with size / distance.
  - RT shadow: rays toward random points on the rectangle (like `PointShadowRt`, same ray count and mask).
- Volumetric (volumetric_common.hlsli `VolLight`): treat an area light as a point at its centre (no new feature), but its struct
  must stay in sync with the C++ layout.

## Renderer data
- `PunctualLight` (RenderTypes.h) gains the area data (width, height; zero = not an area light). `GpuLight` (ShaderInterop.h)
  grows from 96 to 112 bytes (one more float4: width, height, isArea, pad); update the `static_assert` and **every** HLSL mirror
  of it (mmd.hlsl `Light`, pathtrace.hlsl and offline_gi.hlsl `PtLight`, volumetric_common.hlsli `VolLight`, and any other
  struct read from the same buffer — grep for `invRange`). The emitting normal goes in the existing `direction` field (unused by
  point lights).
- `BuildSceneLighting` (src/app/Lighting.cpp) maps an Area light to a `PunctualLight` like a point light plus normal and size,
  with `castPointShadow` set from its shadow type exactly as for point lights.

## UI / editor
- Add menu entry, outliner icon (a Phosphor icon that reads as a panel/rectangle; the font has them in `UiKit`'s icon list),
  inspector section: position, aim, size (width, height), colour, intensity, range, then the shared shadow / falloff / affect /
  viewport-visible sections the point light shows. Keys / auto-key / undo go through the same code as the spot's values.
- Viewport (`StudioViewportLightHandles`, src/app/UiStudioPose.cpp): draw the rectangle outline and a short normal arrow on the
  emitting side; position handle and aim handle as the spot's manual aim (reuse that code path). Size is edited in the inspector
  only (no size handles).
- Strings: Korean in the UI code, with En / Ja / Zh entries in src/core/I18nEn.cpp, I18nJa.cpp, I18nZh.cpp for every new string.
- Script (src/app/UiScript.cpp): `studiolightadd area` works; `studiolightset <i> size <w> <h>` sets the size (keyable, like
  `range`); `studiostate`'s `STUDIOLIGHT` line prints the size for area lights.

## Hard constraints
- **No contamination of existing looks.** A scene without area lights must render exactly as before in every path: offline GI
  bit-identical, raster / RT / PT identical up to the usual frame noise. Shader packs (`shaders/packs/*`, `pack_api.hlsli`) must
  keep compiling and looking the same. Play mode, presets, the benchmark: untouched.
- Do not commit and do not change git state.
- NEVER stop, kill or close any running process. If a build or run is blocked by a locked file, stop and report it.
- App runs: strictly one at a time, at most 5 in total (each opens a window and quits by itself). Put several `capture`
  commands into one `--ui-script` run. Use relative paths in UI scripts (e.g. `../captures_4c/x.png` from build_dev\bin):
  backslash paths written from bash got mangled before.

## Verification
1. Build: PowerShell, `$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;C:\Users\admin\AppData\Roaming\Python\Python314\Scripts;" + $env:PATH; cmd /c "E:\repos\MMDX12\build.cmd build_dev"` -> 0 errors.
2. Tests (build_dev\bin): studio_project_test, studio_light_test, studio_edit_test, studio_gizmo_test, studio_pose_test,
   shader_choice_test all pass. Add: project round trip of an area light with size + keys (studio_project_test); a missing
   `"size"` loads the default; `BuildSceneLighting` maps an area light (normal, size, castPointShadow by shadow type) and
   `PresetLights` / `BuildLighting` contain no area light (studio_light_test); key interpolation of `size`.
3. Captures into build_dev\captures_4c\ (one run, several captures): open `build_dev\captures_cap\pl6.mmdxproj` is NOT what you
   want; start from `build_dev\captures_p4b\pl_hard.mmdxproj` (`--project ... --screen studio --seek 20 --ui-script s.txt
   --frames N`): delete its point light (index 2), add an area light in front-above the character aimed at her, capture with
   size 4x4, 40x20, then shadow NoCast, then the light turned to face away (should go dark). Save that project as
   `captures_4c\area.mmdxproj` and render one PT frame:
   `MMDX12.exe --project ..\captures_4c\area.mmdxproj --offline-video ..\captures_4c\v_pt.mp4 --offline-range 20 20.1 --offline-renderer pt --offline-size 960 540`
   (extract frame 1 with ffmpeg `-vf "select=eq(n\,1)" -frames:v 1`; ffmpeg is in the WinGet packages folder).
   Grep build_dev\bin\mmdx12.log for `[E]` after every run. Look at the PNGs yourself and list each run in the report.

## Start here
src/studio/SceneLight.h/.cpp (model, names, sampling, `==`), src/studio/StudioProject.cpp (lights save / load),
src/app/Lighting.cpp (`BuildSceneLighting`), src/app/UiStudioLight.cpp (add menu, rows, inspector per kind),
src/app/UiStudioPose.cpp (`StudioViewportLightHandles`, the spot handles), src/app/UiScript.cpp (studiolight* commands),
src/render/RenderTypes.h (`PunctualLight`), src/render/ShaderInterop.h (`GpuLight`), src/render/Renderer.cpp (`FillGpuLights`),
src/render/Passes.cpp (`PointShadowCount`, ShadowPass), shaders/mmd.hlsl (`Light`, `PointShadow`, `PointShadowRt`, punctual
loops ~290-350), shaders/pathtrace.hlsl (~250-290), shaders/offline_gi.hlsl (PtLight, every punctual loop),
shaders/volumetric_common.hlsli, tools/studio_light_test.cpp, tools/studio_project_test.cpp.

## Decided (do not re-research)
- Rectangle only (no disc), one-sided, manual aim, size keyable, width axis horizontal by the cross(worldUp, normal) rule.
- Real-time = centre point x one-sided cosine; PT / GI = uniform rectangle sampling; same falloff function everywhere.
- Raster shadow reuses the point shadow map (6 slices per shadowed point / area light; the light cap was removed in 94ab1fc).
- The emitter is not visible to camera rays (like the other lights).
- Icons: Phosphor glyphs already in assets/fonts via `UiKit` (look at how the point / spot rows choose theirs).

## Off limits
external/, library/, captures/ (read-only), videos/, dist/, build/, build_au/, build_release/, CLAUDE.local.md, docs/,
progress.md. build_dev is yours. Do not change the play-mode lighting (`BuildLighting`) or the presets.
