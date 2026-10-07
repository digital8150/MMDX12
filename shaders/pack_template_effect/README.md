# Effect pack template

A `"type": "effect"` pack: a whole-screen shader in `effect.hlsl` (function `PackEffect`, contract in
`shaders/effect_api.hlsli`) plus `pack.json`. Copy this folder into `shader_packs/<id>` (or use "New pack" in the
shader manager and pick "Effect"), then add the pack in the shader manager / the play bar's effect stack.
Check it with `pack_check <folder> --compile`.
