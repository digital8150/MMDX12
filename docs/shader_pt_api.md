# PT/GI shader pack hook (phase 1: offline GI stills, phase 2: real-time path tracer)

A surface shader pack can optionally provide `pt_surface.hlsl` next to `surface.hlsl` for the offline GI path tracer (`CSRender`) and the real-time path tracer (`CSPathTrace`, `PathTracePass`, `--render pt`).
When present, characters using the pack will evaluate `PackEvaluate` during offline GI still rendering and real-time path tracing instead of the default `ToonSun` shading.
If `pt_surface.hlsl` is omitted, the pack uses the path tracer's default shading. Real-time raster and RT are unaffected.

## File placement

```
my_pack/
  pack.json        manifest
  surface.hlsl     raster / real-time RT (PackShade)
  pt_surface.hlsl  offline GI / real-time PT (PackEvaluate)
```

No `pack.json` change or `apiVersion` bump is needed.

## pt_surface.hlsl

```hlsl
PtPackOut PackEvaluate(PtPackIn i);
```

### `PtPackIn`

- `float3 pos`: world position
- `float3 normal`: shading normal
- `float3 V`: view direction toward the camera
- `float2 uv`: texture coordinates
- `float3 L`: direction towards the sun/key light
- `float sunVis`: 0..1 sun visibility from the traced shadow ray (0 on diffuse bounces)
- `float3 baseColor`: linear base colour (`SrgbToLinear(texture * diffuse)`)
- `uint materialClass`: pack material class (`PACK_BODY`, `PACK_SKIN`, `PACK_FACE`, `PACK_EYE`, `PACK_HAIR`, `PACK_WEAPON`)
- `float params[16]`: 16 pack sliders
- Head frame: `float3 headRight`, `float3 headUp`, `float3 headForward`, `bool headValid`

### `PtPackOut`

- `float3 albedo`: linear surface albedo used for GI diffuse bounces and reflections
- `float3 shadowTint`: linear multiplier on the shaded side of the terminator
- `float shadowBias`: shifts the terminator (-1..1)
- `float3 specular`: additive linear radiance (rim, matcap, highlights)
- `bool flatFace`: whether to use the engine's flat face handling (no GI gradient)

### Helpers

Pack textures declared in `pack.json` (`"textures"`, up to 16) are accessible inside `PackEvaluate`.
Sampling uses explicit LOD (no implicit-gradient `.Sample`). Textures declared `"srgb": true` (default) sample as linear; `"srgb": false` return stored values. Out-of-range index or missing texture file on disk returns white `(1, 1, 1, 1)` and size `(1, 1)`:

- `float4 PtPackSampleTex(uint i, float2 uv)`: samples pack texture `i` at LOD 0 using its declared wrap/clamp address mode.
- `float4 PtPackSampleTexLevel(uint i, float2 uv, float lod)`: samples pack texture `i` at explicit `lod` using its declared address mode.
- `uint PtPackTexCount()`: number of declared pack textures (0 if no textures declared or upload failed).
- `float2 PtPackTexSize(uint i)`: dimensions of texture `i` in pixels; returns `(1, 1)` for out-of-range index.

Note: `ScenePass` (raster/RT) and the ray-tracing / path-tracer scene cache (`render/PackTextures`) each acquire their own texture sets independently. When both raster and PT use the same pack, duplicate GPU texture copies may exist.

## Integrator behaviour

- **Camera / specular chain (`!diffuseChain`)**: The sun direct term is evaluated as:
  `lerp(albedo * shadowTint, albedo, terminator * sunVis) * sunIntensity + specular * sunVis`
  where `terminator` is the engine's smoothstep shape shifted by `shadowBias`, and `flatFace` reuses the engine's `s.flat` path.
- **Diffuse bounces (`diffuseChain`)**: Uses `PtPackOut.albedo` as the surface albedo. Toon terms do not run on GI gather rays.
- **Cache prepass**: The irradiance cache prepass keeps default albedo.
- **Denoiser note**: In real-time path tracing, the pack's stylized direct light goes through the same temporal / a-trous filtering as the default toon light (`radiance / a` demodulation, temporal accumulation, a-trous filter, modulation). A test pack with a hard `step()` highlight band kept crisp edges in a still capture, but fast motion was not checked; a later phase may split the stylized term out of the denoiser if ghosting shows up.

## Verification & tooling

- `tools/pack_check <pack>` reports `pt_surface: yes/no` and compiles `CSRender` and `CSPathTrace` with DXC when `--compile` is passed.
- Template: `shaders/pack_template/pt_surface.hlsl`.
