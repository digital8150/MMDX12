# Shader pack screens (API 6)

A shader pack with `"type": "screen"` draws the content of an MMDX **stage screen** (`docs/stage_format.md` `screens`).
It replaces an image / video / camera feed with something a shader computes: a lyric ticker, an equalizer, a scrolling
pattern, a vector display. Surface (`"type": "surface"`, the default), effect (`"type": "effect"`), look
(`"type": "look"`) and world (`"type": "world"`) packs are unchanged.

A screen pack is bundled **inside a stage folder** (docs/stage_format.md `packs`): it is registered as
`stage:<stage id>/<folder>` while that stage is loaded, and never installed, listed in the manager / pickers / the
gallery, or hot reloaded.

## Folder

```
my_stage/
  stage.json
  packs/
    led/
      pack.json      manifest (same fields as surface packs, plus type)
      screen.hlsl    implements PackScreen
  ...
```

`pack.json` additions: `"type": "screen"`. `requires`, `textures` and `params` (at most 16) work exactly like surface
packs. `classes`, `look`, `passes`, `state` and `replaces` are not used and are a manifest problem.
`screen.hlsl` is required.

## How a screen finds its pack

Two ways, both naming the same pack folder:

```json
"screens": [
  {"id": "main", "materials": ["led_main"], "source": "program", "file": "packs/led"}
],
"packs": [
  {"folder": "packs/led", "role": "screen", "screen": "main", "params": {"bars": 10}}
]
```

* `screens[].source: "program"` + `file` = the pack folder: the screen draws that pack with the defaults of its own
  `params` (plus a `packs[]` entry on the same folder, if there is one, for its parameter values).
* a `packs[]` entry with `"role": "screen"` and `"screen": <screen id>`: the screen draws the pack in `folder`. This
  form **wins** when both exist, and it is the only way to bind stage `params` to the pack
  (`packs.<index>.params.<key>`).

The screen's `width` / `height` size the texture it is drawn into (at most 2048 px per side); the result is the
material's emission like any other screen source, so `brightness` / `emitLight` work the same way. A pack that fails to
load or compile leaves the screen black: one `[W]` at load, the stage keeps loading.

## screen.hlsl

```hlsl
// pack_api additions: see shaders/screen_api.hlsli
float4 PackScreen(PackScreenInput i);
```

| `PackScreenInput` | |
| --- | --- |
| `float2 uv` | 0..1 across the screen, `(0, 0)` at the top left |
| `float2 size` | the screen texture size in pixels |
| `float time` | seconds on the song / timeline clock. An offline render passes the rendered frame's time, so a still is exact and reproducible |
| `float frameIndex` | frame counter, wraps at 64 (animation noise seeds) |

Return **linear RGB**; alpha is ignored. The pass sRGB-encodes the result into the screen's RGBA8 texture (the material
shaders decode it again), so `float3(0.2, 0.4, 0.8)` is what a video / image screen would store as its 8-bit colour. Do
not apply `gamma` yourself.

### Parameters

`pack.json` `params`, in order, are 16 root constants (`b2`) and reach the pack through the effect API's accessors:

```hlsl
#define P_BARS  PackParamInt(0)      // int
#define P_SPEED PackParam(1)         // float
#define P_TINT  PackParamColor(2)    // colour: linear rgb (sRGB manifest values converted)
#define P_DIR   PackParamVec3(3)     // vec3
```

`PackParamInt` / `PackParamBool` / `PackParamColor` / `PackParamColor4` / `PackParamVec3` need the `params.ext`
capability (add `"requires": ["params.ext"]` to pack.json once a pack declares a non-float parameter); `PackParam(i)`
alone works for float-only packs. The values are the manifest's `packs[].params` initial values with the stage's
`packs.<index>.params.<key>` binding targets applied, clamped to the pack.json ranges.

### Audio inputs

A screen pack that declares `"requires": ["inputs.audio"]` reads the song's beat analysis with the same helpers as
surface / effect / world packs (`shaders/pack_inputs.hlsli`):

```hlsl
PackInputValid(PACK_IN_AUDIO) // true while the analysis is ready (all values 0 otherwise)
PackAudioBeat()               // phase 0..1 within the current beat
PackAudioDownbeat()           // phase 0..1 within the current bar
PackAudioBeatCount()          // continuous beat count (beat index + phase; negative before the first beat)
PackAudioOnset()              // latest onset strength, decaying (0.25 s)
PackAudioEnergy()             // the current beat's normalised RMS 0..1
PackAudioSection()            // 0 low / 0.5 mid / 1 high
PackAudioBpm()
```

The screen pass has no scene constants: the host passes the frame's two audio vectors in the pass constants `gP1` /
`gP2` (`StageScreens`), and `screen_api.hlsli` maps `gPackAudio0` / `gPackAudio1` onto them. A pack without the
capability compiles and renders exactly as before. Audio time = the song's audio cursor (play), the timeline time minus
the audio offset (studio), the rendered frame's time in offline renders.

### Pack textures

The v2 pack texture table (`pack.json` `"textures"`, at most 16) is bound at `t0, space5` and sampled with

```hlsl
PackSampleTex(i, uv)           // the i-th texture with its declared address mode
PackSampleTexLevel(i, uv, lod) // explicit LOD (ramps / data maps: 0)
PackTexSize(i)                 // uint2, level-0 size
PackTexCount()                 // declared count
```

sRGB textures sample as linear values (do not convert again); `"srgb": false` returns the stored values. Out-of-range
and missing textures sample white, so a pack with no texture keeps compiling.

## Example

`tools/stagegen/stagegen.py --only pack_test` writes the fixture stage (`captures/stages/pack_test`) with a pack of
each role; its `packs/screen/screen.hlsl` is the smallest useful screen pack:

```hlsl
#define P_BARS PackParamInt(0)
#define P_SPEED PackParam(1)

float4 PackScreen(PackScreenInput i) {
    const float bars = max((float)P_BARS, 1.0);
    const float bar = frac(i.uv.x * bars + i.time * P_SPEED * bars);
    const float lit = smoothstep(0.0, 0.08, bar) * smoothstep(1.0, 0.92, bar);
    return float4(lerp(float3(0.02, 0.03, 0.06), float3(0.35, 0.75, 1.0), lit), 1.0);
}
```

## Checking a screen pack

`python tools/stagegen/stagegen.py --out captures/stages --only pack_test` then

```
build/bin/pack_check captures/stages/pack_test/packs/screen --compile   # manifest, files, DXC compile of PSPackScreen
build/bin/stage_check captures/stages/pack_test                        # the stage as a whole
```

`pack_check --compile` compiles `screen.hlsl` through the host `shaders/stage_screen.hlsl` exactly like the app does
(vs_6_0 / ps_6_0, `MMDX_PACK` = the quoted include path), so a shader error shows up there before a capture.
