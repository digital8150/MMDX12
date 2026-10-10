# Shader pack effects (API 3, 4, 5)

A shader pack with `"type": "effect"` is a whole-screen effect (chromatic aberration, film grain, CRT, glitch, custom
vignette ...). Users stack effects in the shader manager and the play bar's effect button; the stack runs top to bottom on
the final image and is used by the play view, the lobby preview and real-time video / still renders. Surface packs
(`"type": "surface"`, the default) are unchanged.

## Folder

```
my_effect/
  pack.json      manifest (same fields as surface packs, plus type / stage)
  effect.hlsl    implements PackEffect
  preview.png    optional
  textures/      optional, pack.json "textures" (same rules as surface packs, API 2)
```

`pack.json` additions: `"apiVersion": 3`, `"type": "effect"`, `"stage": "post" | "pre-bloom"` (default `post`).
`classes` are not used. `params` (at most 16) work exactly like surface packs; values reach the shader as `PackParam(i)`.

| stage | input / output | use for |
| --- | --- | --- |
| `post` | display-referred sRGB (after tonemap and the colour LUT), 8-bit | grain, scanlines, lens fringes, vignette |
| `pre-bloom` | linear HDR RGBA16F, before the bloom | glow-friendly or exposure-aware effects |

Effects of both stages can be mixed in one stack: each stage runs its own effects in stack order.

## effect.hlsl

```hlsl
float3 PackEffect(PackEffectInput i) { return i.color.rgb; }
```

`PackEffectInput`: `uv` (0..1, top left = 0,0), `pixel`, `outputSize`, `time` (seconds), `frameIndex`, `color` (the frame so
far, alpha kept), `depth` (raw device depth, 1 = background), `normal` (view space, oct-encoded; undefined on the background), `motion`
(uv(current) - uv(previous)).

Helpers: `gEffectSource.SampleLevel(gLinear, uv, 0)` reads any pixel of the frame so far (bilinear, clamp), `PackParam(i)`,
`PackFxSampleTex(i, uv)` / `PackFxSampleTexLevel` / `PackFxTexSize` / `PackFxTexCount` (pack textures: sRGB textures return
linear values), plus everything in `common.hlsli` (`Luminance`, `LinearZ`, `Ign`, `SrgbToLinear`, `gTime` ...). `line` is a
reserved word in HLSL.

The shader is compiled at runtime with DXC (`ps_6_0`); a compile error skips the effect (log `[E]`, toast, status in the
shader manager) and never crashes. Saving `effect.hlsl` while the app runs reloads it.

## Rules

- Effects apply to the raster / ray-traced / path-traced real-time views and to the offline GI renderer (4K stills and GI
  videos). Unlit / wireframe views and the quad view do not apply them. Offline GI: a pre-bloom pack runs on the lit HDR
  image (the denoised radiance re-lit, volumetric light and outlines composed in) before the bloom, and the bloom reads its
  output; a post pack runs on the final sRGB image. `depth` and `normal` come from the offline G-buffer, `motion` is zero
  (the offline renderer has no per-pixel motion vectors; its motion blur is integrated over the shutter).
- No effects in the stack = no extra targets, no extra passes: the frame is identical to a build without the feature.
- Check a pack with `pack_check <folder> --compile`. Template: `shaders/pack_template_effect`.
- Examples (online gallery only, not bundled): chromatic_aberration, film_grain, crt_scanlines, auto_luminous (sources in the website repo `shader-packs/`).
- CLI: `--effect <id>[,<id>...]` replaces the stack for one run, `--effect none` clears it.

## API v4: persistent state

API v4 introduces optional persistent per-entry state (up to 16 floats) for temporal work like eye adaptation, exposure or focus smoothing, and cumulative effects.

### Manifest

In `pack.json`, declare `"apiVersion": 4`, `"type": "effect"`, and add the `"state"` block:

```json
{
  "apiVersion": 4,
  "type": "effect",
  "stage": "post",
  "state": {
    "floats": 4
  }
}
```

- `"floats"` must be an integer between 1 and 16.
- A pack that declares `"state"` must have `"apiVersion": 4` and `"type": "effect"`. Older versions of the app reject or ignore unknown fields, preventing stateful packs from silently running stateless.
- Stateless effects and surface packs can continue using `"apiVersion": 3`.

### HLSL Contract

State packs implement `PackEffectState` in addition to `PackEffect`:

```hlsl
void PackEffectState(PackStateInput i, out float newState[16]) {
    for (int k = 0; k < 16; ++k) newState[k] = 0;
    float centerLuma = Luminance(gEffectSource.SampleLevel(gLinear, float2(0.5, 0.5), 0).rgb);
    newState[0] = i.reset ? centerLuma : lerp(i.prevState[0], centerLuma, 1.0 - exp(-PackParam(0) * i.dt));
    newState[1] = i.reset ? 0 : (i.prevState[1] + 1);
}

float3 PackEffect(PackEffectInput i) {
    float smoothLuma = PackState(0);
    return i.color.rgb * smoothLuma;
}
```

- `PackStateInput`:
  - `float prevState[16]`: previous state values (all zeros when `reset` is true).
  - `bool reset`: true if state was reset this frame.
  - `float dt`: delta time in seconds.
  - `float time`: current time in seconds.
  - `float frameIndex`: current frame number.
  - `float2 outputSize`: dimensions of the render target.
- `PackState(i)` reads float `i` (0 <= i < stateFloats) of the state updated for this frame. For non-state packs (`PACK_STATE=0`), `PackState(i)` evaluates to `0.0`.
- `PackEffectInput` gains `float dt` appended as its last member.
- State is stored in a 4x1 RGBA32F ping-pong texture (`gEffectState`, `t0, space7`).

### Reset, dt, and advance rules

- **Reset**: `reset` is true when:
  - The stack entry is created, reordered, or its pack reloads.
  - The entry did not run in the previous executed frame of the pass instance.
  - Camera cut / teleport (`FrameView::cameraCut`).
  - Output size changed / window resized.
  - Explicitly requested by offline job or caller (`effectStateReset`).
  When `reset` is true, `prevState` is guaranteed to be all zeros.
- **dt**:
  - Interactive playback: `frameTimeMs * 0.001`, clamped to [1/240, 0.1] seconds.
  - Deterministic runs (`--frames`, `--ui-script`, MCP / headless): fixed `1/60` seconds.
  - Offline video: fixed `1/fps` seconds.
- **Advance**:
  - Interactive viewport advances once per presented frame.
  - Path-traced video: state advances only on the last of the `iterCount` passes per frame (`effectStateAdvance` false beforehand).
  - Offline GI:
    - Videos: 1 state pass per output image with `dt = 1/fps`, resetting on first frame or jump cut. State slots persist across frames of the video.
    - Stills: runs the state pass 64 times on the same image with `dt = 1/60` (reset only on step 0) to converge temporal state before running `PackEffect` once.


## API v5: intermediate passes and the camera focus

API v5 lets an effect run several passes before `PackEffect` (downsample chains, blurs, gathers: lens flares, bokeh
depth of field, ambient occlusion) and gives every effect the engine's depth-of-field focus. A pack may also take
over the engine's depth of field.

### Manifest

```json
{
  "apiVersion": 5,
  "type": "effect",
  "stage": "pre-bloom",
  "passes": [
    { "entry": "Bright", "scale": 0.25 },
    { "entry": "Blur",   "scale": 0.0625 }
  ],
  "replaces": ["dof"]
}
```

- `passes` (at most 8): each entry names an HLSL function `float4 Name(PackPassInput i)` and the size of its target as
  a fraction of the output (`scale`, 1/64 .. 1, default 1). Passes run in order; each writes its own RGBA16F target.
- `replaces` (pre-bloom packs only): `["dof"]` = while the entry is enabled, the engine's depth of field (DofPass and
  the offline GI thin lens) is skipped and the pack draws it. No other values exist yet.
- `passes`, `replaces` and the focus helpers need `"apiVersion": 5`; `state` (v4) works together with them.

### HLSL

```hlsl
float4 Bright(PackPassInput i) { return float4(max(gEffectSource.SampleLevel(gLinear, i.uv, 0).rgb - 1.0, 0.0), 1); }
float4 Blur(PackPassInput i)   { return PackPassSample(0, i.uv); }   // pass 0's target, bilinear
float3 PackEffect(PackEffectInput i) { return i.color.rgb + PackPassSample(1, i.uv).rgb; }
```

- `PackPassInput`: `uv` and `pixel` of this pass's target, `size` (its pixels), `outputSize`, `time`, `frameIndex`,
  `dt`, `pass` (its index). The frame inputs (`gEffectSource`, depth, normal, velocity), the pack textures, the
  parameters and the state are bound in every pass.
- `PackPassSample(n, uv)` (bilinear, clamp), `PackPassLoad(n, pixel)`, `PackPassSize(n)` read pass `n`'s target. A pass
  may read the passes before it; reading its own target or a later one is undefined. `PackEffect` may read all.
- Focus (the studio focus track, play mode's character head, else a five-tap autofocus on the screen centre):
  `PackFocusZ()` = view-space z of the focus plane; `PackFocusAperture()` = the focus key's aperture scale (0 = sharp,
  1 default); `PackDofAperture()` / `PackDofMaxRadius()` = the user's global DoF aperture and max blur radius (output
  px); `PackCocPx(viewZ, focusZ, aperture, maxRadius)` = the engine lens model's signed circle of confusion in output
  px (negative = in front of the focus plane).
- Pass targets are pooled by size and shared by all entries; they exist only while an entry with passes is enabled.
  A pack without `passes` keeps the v3 / v4 root signature and PSO exactly (no extra cost, identical frame).

`pack_check --compile` compiles every pass (`PSPackPass` with `PACK_PASS_ENTRY`) and prints the passes and `replaces`.
Examples (online gallery, sources in the website repo `shader-packs/`): lens_dirt, lens_reflection, lens_flare,
bokeh_dof (replaces the depth of field, follows the focus track), screen_ao.
