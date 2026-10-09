# Punctual-light specular highlights in the path tracer and offline GI (MMDX12)

## Problem
Point / spot / area lights are delta lights: the specular continuation of a path (a reflection cone around reflect(d, n)
with half-angle `rough * 0.5`, taken with probability `pSpec`) can never hit them, so glossy surfaces (the stage, the
studio floor, anything with `refl > 0`) show no highlight from them in the real-time PT (shaders/pathtrace.hlsl) or the
offline GI (shaders/offline_gi.hlsl). Raster shows them (mmd.hlsl `PunctualSpecular`). The light's `affectSpecular`
flag therefore does nothing in PT / GI today.

## Requirement
Add next-event estimation of punctual lights **for the specular lobe** at every vertex where the shaders already take a
specular continuation with probability `pSpec`:
- pathtrace.hlsl `CSPathTrace` (the punctual-light block ~270-320 and the `Rand(rng) < pSpec` continuation ~328);
- offline_gi.hlsl: camera hits (`CameraDirect` ~607 and its callers ~985-999) and later path vertices (~1011-1035).
Find every such place; list them in the report.

The light term must use **the same lobe the continuation samples**, so the energy matches what an infinitesimally small
light would give: a uniform cone of half-angle `rough * 0.5` around the mirror direction R, i.e. density 1 / Omega with
Omega = 2 pi (1 - cos(rough * 0.5)) when the light direction is inside the cone, 0 outside. Contribution per light:
`throughput * pSpec * lightColour * atten * shadowFactor / Omega` (atten = the light's PunctualFalloff x spot cone x the
area light's one-sided cosine, exactly as the diffuse term of that shader computes it; shadowFactor via the same shadow
ray / ShadowTransmission the diffuse term of that light uses — reuse its visibility when the diffuse term already traced
one for the same sample point, otherwise trace one). You may soften the cone's edge (e.g. a smoothstep over the outer
~20 % of the angle) as long as the lobe stays normalized to the same total. Area lights: use the point sampled on the
rectangle for this sample (same as the diffuse term).
- Skip lights with `affectSpecular <= 0.5`.
- Do not add highlights where no specular continuation exists (pSpec == 0): characters' toon materials keep their look.
- The existing glass glint (`GlassGlint`) stays as it is; don't double count on glass.
- The light-selection scheme stays as each shader has it (pathtrace picks one light at random and scales by nl: do the
  same for the specular term with the same picked light).

## No contamination
- A scene with no punctual lights must render bit-identically in offline GI and identically (up to noise) in PT.
- Raster / RT (mmd.hlsl) unchanged. Shader packs' pt_surface hooks (pack_api / pt pack variants) must keep compiling:
  if a pack path bypasses the code you change, leave it as is and say so.

## Constraints
- Do not commit or change git state. NEVER stop/kill/close any running process. No C++ changes are expected; if you
  think one is needed, keep it minimal and explain.
- App runs: one at a time, at most 4. Use relative paths in UI scripts.

## Verification
1. Build (PowerShell): `$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;C:\Users\admin\AppData\Roaming\Python\Python314\Scripts;" + $env:PATH; cmd /c "E:\repos\MMDX12\build.cmd build_dev"`; grep build_dev\bin\mmdx12.log for `[E]` after every run (shaders compile at runtime).
2. Projects ready in build_dev\captures_pt\: p_base.mmdxproj (one white point light, specular on) and p_nospec.mmdxproj
   (same, specular off). From build_dev\bin:
   `MMDX12.exe --project ..\captures_pt\p_base.mmdxproj --offline-video ..\captures_pt\n_base.mp4 --offline-range 20 20.1 --offline-renderer pt --offline-size 960 540`
   (same for p_nospec -> n_nospec.mp4), extract frame 1 with ffmpeg `-vf "select=eq(n\,1)" -frames:v 1` (ffmpeg is in
   %LOCALAPPDATA%\Microsoft\WinGet\Packages). And GI stills:
   `MMDX12.exe --project ..\captures_pt\p_base.mmdxproj --offline-still ..\captures_pt\h_base.png --seek 20 --offline-size 960 540 --offline-spp 64`
   (same for p_nospec -> h_nospec.png). base must now show a floor highlight and differ from nospec; nospec must equal the
   old frames f_nospec.png / g_nospec.png (same folder) up to noise. Report the numbers (PIL mean abs diff).

## Start here
shaders/pathtrace.hlsl (CSPathTrace ~150-340), shaders/offline_gi.hlsl (Surf, PunctualIrradiance, GlassGlint ~570,
CameraDirect ~607, the path loop ~920-1040), shaders/common.hlsli (PunctualFalloff, ShadowTransmission),
shaders/mmd.hlsl `PunctualSpecular` (raster reference only).

## Off limits
external/, library/, captures/, build/, build_au/, build_release/, docs/, progress.md, CLAUDE.local.md, src/ (unless
justified). Don't touch mmd.hlsl.
