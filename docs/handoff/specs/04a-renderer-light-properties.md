# Phase 4a: renderer support for the per-light properties (MMDX12 studio lights)

## Goal
The studio's scene lights (src/studio/SceneLight.h, edited in src/app/UiStudioLight.cpp, saved in the project) already carry
shadow type (NoCast / Hard / Soft), softness, density, shadow colour, falloff (None / Linear / InverseSquare), affect-diffuse
and affect-specular. Today the renderer ignores all of them. Carry them through BuildSceneLighting into the renderer and make
every render path honour them. Also fix the sky glow that still points at the sun when the sun is off.

## Product requirements (decided by the user)
- Per light (point, spot) and for the sun:
  - Shadow type: NoCast = the light casts no shadow; Hard = today's look; Soft = a wider penumbra growing with softness.
  - Density: 0..1, 1 = full shadow, 0 = no visible shadow.
  - Shadow colour: tints the occluded part. Black (the default) must equal today's result.
- Point and spot only: falloff None / Linear / InverseSquare, a range-bounded curve each. None keeps TODAY's curve
  (it is what every preset and the play mode use), so presets and play mode do not change.
- Point and spot only: affectDiffuse / affectSpecular switch that light's diffuse and specular terms off independently.
- Sun (decision D-c): the camera VMD's self-shadow track keeps deciding whether sun shadows are on and how far they reach
  (`FrameView::shadowsOff`, shadow distance). The sun's own type (NoCast also turns sun shadows off), softness, density and
  colour come from the sun light. The sun has no falloff and no affect switches in this phase.
- Paths that must honour all of the above: the raster scene shader, the RT path (ray-query sun shadows, RT reflections' sun
  term if it reads shadows), the real-time path tracer, and the offline GI renderer (stills and videos). Spot shadows come
  from the existing spot shadow maps in raster; the path tracer and offline GI already trace shadow rays for lights.
- Soft in the ray paths = a cone / area sample whose size grows with softness (the sun already samples a cone). In raster
  = a larger filter radius (the CSM Poisson taps for the sun, more or wider taps for spots).
- Sky glow: `SkyColor` draws a glow toward the sun direction. With no sun in the scene (the sun colour is black while
  sunIntensity stays 1: decision D2, keep that convention) the glow must vanish, and an ordinary sun keeps looking as today.
- Defaults must not change anything: a scene with every light at (Hard, density 1, black shadow colour, falloff None, both
  affect flags on), the play mode, the lobby and the benchmark render exactly as before this change.

## Architecture direction
- Extend `PunctualLight` and `LightParams` (src/render/RenderTypes.h) with the new fields, with defaults equal to today's
  behaviour, and fill them in `BuildSceneLighting` (src/app/Lighting.cpp). `BuildLighting` (play mode presets) is not changed.
- `GpuLight` (src/render/ShaderInterop.h) is 64 bytes with 3 spare floats. Grow it as needed; the HLSL mirrors must change
  with it: `Light` in shaders/common.hlsli / mmd.hlsl, `PtLight` in shaders/pathtrace.hlsl and shaders/offline_gi.hlsl, the
  `static_assert`, and any C++ that sizes the light buffers (`kLightBytes` in Renderer.cpp and OfflineRenderer.cpp). Sun
  values go into `SceneConstants` (its slot is 2048 bytes; check `static_assert`).
- Do not change the signature of `PunctualDiffuse` / `PunctualSpecular` or any name in shaders/pack_api.hlsli: shader packs
  call them.
- Share the falloff and shadow-density maths in one place per shader language where the shaders allow it (common.hlsli).
- Extend tools/studio_light_test.cpp with checks of the BuildSceneLighting -> LightParams mapping of the new fields
  (also with a disabled sun, and the default values).
- To verify visually you need to set the new light fields from a UI script. If `studiolightset` in src/app/UiScript.cpp
  lacks the fields (shadow type, softness, density, shadow colour, falloff, affectdiffuse, affectspecular), add them there.
  That is the only change allowed outside render/, shaders/, Lighting.* and the test.

## Constraints
- Do not change src/studio/ (data model, save format), the light UI (UiStudioLight.cpp), Gizmo or the project files.
- Do not commit and do not change git state.
- NEVER stop, kill or close any running process (an earlier worker force-stopped a running MMDX12.exe). If a build or a run
  is blocked by a locked file, stop and report it.
- App runs: strictly one at a time, at most 5 runs in total, each run opens a visible window and quits by itself. Put several
  `capture` commands into one `--ui-script` run (set a field, advance frames, capture, next). Write PNGs and logs to
  build_dev\captures_phase4a\.

## Non-goals (later phases, do not start)
Point-light shadows in raster (cube maps) and in RT (4b). Area lights (4c). Shadow-colour work beyond a tint of the occluded
amount. New UI, new light types, changes to what the inspector shows.

## Verification
1. `E:\repos\MMDX12\build.cmd build_dev` finishes with 0 errors; grep build_dev\bin\mmdx12.log for `[E]` after every run.
2. These build and pass: build_dev\bin\studio_project_test.exe, studio_light_test.exe (with your new checks), studio_edit_test.exe,
   studio_gizmo_test.exe, studio_pose_test.exe.
3. Before you change any renderer code, capture a baseline of the concert preset (raster) and of a studio-preset scene with
   the same command line you will use afterwards. After the change, the same scenes with default light values must be
   pixel-identical (or differ only by noise in the ray paths: say what you measured and how, with numbers).
4. Captures that prove each feature, raster first: (a) sun NoCast / Hard / Soft side by side as three captures of one
   scene, (b) a spot with NoCast / Hard / Soft, (c) density 1 / 0.5 / 0, (d) a coloured shadow, (e) falloff None / Linear /
   InverseSquare on a point light, (f) affect diffuse off and affect specular off on a point light, (g) the sun disabled:
   no glow in the sky. Then one run each for `--render rt` and one quick offline still
   (`--offline-still out.png --offline-size 960 540 --offline-spp 64`) proving the sun type and one punctual property work in
   the ray paths. List every run (command, result, what the picture shows) in the report. Look at the PNGs yourself.

## Start here
src/app/Lighting.h, src/app/Lighting.cpp (BuildSceneLighting), src/studio/SceneLight.h, src/render/RenderTypes.h
(PunctualLight, LightParams), src/render/ShaderInterop.h (GpuLight, SceneConstants), src/render/Renderer.cpp
(FillSceneConstants ~300-411, FillGpuLights, spot slices ~356-376), src/render/Passes.cpp (spot shadow pass ~150-205),
shaders/mmd.hlsl (ShadowCascade, Shadow, ShadowRt, SpotShadow, PunctualDiffuse, PunctualSpecular, lines ~100-216),
shaders/common.hlsli (cbuffer, Light struct, SkyColor ~89), shaders/pathtrace.hlsl (sun shadow, punctual NEE ~240-270),
shaders/offline_gi.hlsl (sun cone sample, PunctualUnshadowed / PunctualIrradiance ~305-375), src/render/OfflineRenderer.cpp
and RendererOffline.cpp (light buffers), shaders/rtreflect.hlsl (sun shadow in reflections), shaders/volumetric.hlsl
(reads the spot maps), tools/studio_light_test.cpp, src/app/UiScript.cpp (studiolightset, studiostate).

## Off limits
external/, library/, library.zip, captures/ (read-only; you may read captures\studio example scripts), videos/, dist/, build/,
build_au/, build_release/, CLAUDE.local.md, docs/, progress.md, src/studio/, src/app/UiStudio*.cpp. build_dev is yours.

## Decided (do not re-research)
- Falloff None = today's curve (windowed range term divided by 1 + d^2 * 0.0004); Linear and InverseSquare are new curves
  that fall to zero at the range. Defaults keep every preset unchanged.
- Hard = today's filter (sun: the 12-tap Poisson at 1.6 texels; spot: the 4-tap at 0.75 texel). Soft grows from there.
- Sun with no sun light: colour black, sunIntensity 1 (D2). The sky glow must be driven by the sun colour so black = no glow.
- D-c as above. The camera VMD self-shadow track stays in charge of on/off and distance.
- The GpuLight / PtLight / Light mirrors change together; update the static_asserts.
