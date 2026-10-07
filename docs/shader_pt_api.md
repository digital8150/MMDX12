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

- `PtPackSampleTex(i, uv)`: reserved explicit-LOD helper for future pack texture support (phase 1 returns white).

## Integrator behaviour

- **Camera / specular chain (`!diffuseChain`)**: The sun direct term is evaluated as:
  `lerp(albedo * shadowTint, albedo, terminator * sunVis) * sunIntensity + specular * sunVis`
  where `terminator` is the engine's smoothstep shape shifted by `shadowBias`, and `flatFace` reuses the engine's `s.flat` path.
- **Diffuse bounces (`diffuseChain`)**: Uses `PtPackOut.albedo` as the surface albedo. Toon terms do not run on GI gather rays.
- **Cache prepass**: The irradiance cache prepass keeps default albedo.
- **Denoiser note**: In real-time path tracing, the pack's stylized direct light goes through the same temporal / a-trous filtering as the default toon light (`radiance / a` demodulation, temporal accumulation, a-trous filter, modulation). Hard-edged cel steps will soften; a later phase may split them out.

## Verification & tooling

- `tools/pack_check <pack>` reports `pt_surface: yes/no` and compiles `CSRender` and `CSPathTrace` with DXC when `--compile` is passed.
- Template: `shaders/pack_template/pt_surface.hlsl`.
