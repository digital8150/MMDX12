# My shader pack

A starting point for your own shader pack (created with the app's "새 팩 만들기" button).

## What to edit

- `pack.json` — the manifest: id, name, description, material class rules (`classes`), parameter sliders (`params`).
- `surface.hlsl` — the shader. It implements `PackShade(PackSurface)`, the contract in
  `shaders/pack_api.hlsli` (`PackSurface` in, `PackResult` out).
- `preview.png` (optional, add it yourself) — the card image in the shader tab.

Edit the files and press the app's reload button (or just save — the app re-reads changed packs automatically);
the pack recompiles on its next draw.

## Docs

The full guide (manifest fields, the `PackSurface` / `PackResult` contract, compiling and debugging) lives at
https://mmdx.codingbot.kr/en/docs/shader-packs/. The built-in `hoyo_toon` pack (`shaders/packs/hoyo_toon/`) is a
larger, commented example.
