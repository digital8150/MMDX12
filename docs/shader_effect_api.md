# Shader pack effects (API 3)

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
far, alpha kept), `depth` (raw device depth, 1 = background), `normal` (world space, undefined on the background), `motion`
(uv(current) - uv(previous)).

Helpers: `gEffectSource.SampleLevel(gLinear, uv, 0)` reads any pixel of the frame so far (bilinear, clamp), `PackParam(i)`,
`PackFxSampleTex(i, uv)` / `PackFxSampleTexLevel` / `PackFxTexSize` / `PackFxTexCount` (pack textures: sRGB textures return
linear values), plus everything in `common.hlsli` (`Luminance`, `LinearZ`, `Ign`, `SrgbToLinear`, `gTime` ...). `line` is a
reserved word in HLSL.

The shader is compiled at runtime with DXC (`ps_6_0`); a compile error skips the effect (log `[E]`, toast, status in the
shader manager) and never crashes. Saving `effect.hlsl` while the app runs reloads it.

## Rules

- Effects apply to the raster / ray-traced / path-traced real-time views. Unlit / wireframe views, the quad view and the
  offline GI renderer do not apply them.
- No effects in the stack = no extra targets, no extra passes: the frame is identical to a build without the feature.
- Check a pack with `pack_check <folder> --compile`. Template: `shaders/pack_template_effect`.
- Built-in examples: `shaders/packs/chromatic_aberration`, `film_grain`, `crt_scanlines`.
- CLI: `--effect <id>[,<id>...]` replaces the stack for one run, `--effect none` clears it.
