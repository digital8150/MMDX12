# My shader pack

A starting point for your own shader pack (created with the app's "새 팩 만들기" button).

## What to edit

- `pack.json` — the manifest: id, name, description, material class rules (`classes`), parameter sliders (`params`).
- `surface.hlsl` — the shader. It implements `PackShade(PackSurface)`, the contract in
  `shaders/pack_api.hlsli` (`PackSurface` in, `PackResult` out).
- `preview.png` (optional, add it yourself) — the card image in the shader tab.

Edit the files and press the app's reload button (or just save — the app re-reads changed packs automatically);
the pack recompiles on its next draw.

## Pack textures and edges (apiVersion 2)

A pack can sample its own textures (light maps, ramps, face SDFs, matcaps, LUTs). Declare them in `pack.json`:

```json
"textures": [
  { "file": "textures/ramp.png", "address": "clamp" },
  { "file": "textures/tint.png" }
]
```

png/jpg/jpeg, at most 16, paths inside the pack. Sample them with `PackSampleTex(i, uv)` /
`PackSampleTexLevel(i, uv, lod)` / `PackTexSize(i)` / `PackTexCount()`: sRGB textures (default) sample as **linear**
values (do not apply `SrgbToLinear` again), `"srgb": false` returns the stored values. Textures missing on disk sample
as white — game textures can't be redistributed, so point the pack's "texture folder" (shader manager) at your own rips.

A pack can also draw the model's outline: `#define PACK_HAS_EDGE 1` at the top of `surface.hlsl` and implement
`PackEdgeResult PackEdge(uint materialClass, float4 mmdEdgeColor, float mmdEdgeSize)` (colour in gamma space,
`widthScale` multiplying the MMD outline width). Commented examples are in `surface.hlsl`.

## Docs

The full guide (manifest fields, the `PackSurface` / `PackResult` contract, compiling and debugging) lives at
https://mmdx.codingbot.kr/en/docs/shader-packs/. The built-in `hoyo_toon` pack (`shaders/packs/hoyo_toon/`) is a
larger, commented example.
