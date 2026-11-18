# MMDX stage format (`stage.json`, formatVersion 1)

Status: S1a (2026-10-25), the asset layer; S2a (2026-10-25) adds the glTF material / light import table below;
S2b (2026-10-26) renders transmissive materials as mesh dielectrics (see “Transmission per render path”). The
direction (why stages are folders, what S1b..S13 add) is `docs/stage_ecosystem.md`; this file is the reference every
later milestone keeps current.

An **MMDX stage** is a folder holding `stage.json` plus its model parts (glTF 2.0 `.glb` / `.gltf` or PMX). The
library detects it **by content** (a `stage.json` whose `"format"` is `"mmdx-stage"` and whose `"formatVersion"` this
build reads), never by folder name, and the folder then counts as **one** stage: its model files are not listed as
characters, props or stages of their own. A `stage.json` that does not parse is a scan note and the folder is scanned
exactly as before.

```
my_stage/
  stage.json          required
  parts/room.glb      any model files, referenced relatively
  textures/led.png
  packs/floor/        optional bundled packs (pack.json + shader)
  preview.png         optional
```

Everything relative resolves against the stage folder. Strings may be plain or localized (`{"ko": .., "en": ..,
"ja": .., "zh": ..}`; fallback: the UI language, then `en`, then the plain string, then `ko`, then any entry).

Conventions: colours are **linear** `[r,g,b]` or `[r,g,b,a]` (0..1) — `#rrggbb` is accepted and read as 0..1;
positions, sizes and ranges are **MMD world units** (1 unit = 8 cm); angles in degrees; times in seconds, cue frames at
**30 fps**. Unknown keys are ignored (forward compatibility); a wrong-typed key keeps its default and the scan never
throws.

## Minimal example

```json
{
  "format": "mmdx-stage",
  "formatVersion": 1,
  "id": "my_stage",
  "version": "1.0.0",
  "name": {"ko": "내 스테이지", "en": "My stage"},
  "models": [{"file": "parts/room.glb", "role": "floor"}]
}
```

## Top level

| key | type | notes |
|---|---|---|
| `format` | string | **required**, exactly `"mmdx-stage"` |
| `formatVersion` | int | **required**, `1` (a newer version is "not an MMDX stage" for this build) |
| `id` | string | **required**, `[a-z0-9_-]{1,64}`, unique in the library (`stage_check` enforces the shape) |
| `version` | string | `"1.2.0"`, dotted numbers (the gallery compares versions with the updater's rules) |
| `name`, `description` | string or localized | `name` is required — it is the card / outliner title |
| `authors` | array | `{"name": "..", "role": "..", "url": ".."}`; `name` required, `url` http(s) only |
| `license` | string | free text / SPDX id (`"CC0-1.0"`, `"MIT"`, ...) |
| `minAppVersion` | string | optional; later milestones refuse to load a stage that needs a newer app |
| `tags` | string[] | lower-case words |
| `preview`, `previews` | string / string[] | preview images (and videos) inside the folder; a missing one is a `stage_check` error |
| `models` | array | see below, at least one |
| `materials` | array | per-material PBR overrides |
| `lights` | array | **the `.mmdxproj` editor `lights` schema**, see below |
| `sockets` | array | named transforms (pack / camera / prop anchors) |
| `cameras` | array | named static cameras (S4) |
| `screens` | array | LED / video surfaces (S5, S8) |
| `environment` | object | `fog`, `fogVolumes`, `ambient` (S9 / later) |
| `packs` | array | bundled packs with a role and initial params (S6) |
| `groups`, `params`, `variants`, `defaultVariant` | array / string | the exposed parameter surface |
| `bindings` | array | param → target assignments |
| `cues` | array | L0 timeline: keyed param tracks |
| `drivers` | array | L0 procedural: params modulated by the beat / bar / onset / energy / section or the clock |
| `gltfLights` | bool | **S2a**: take the glTF lights of the parts into the rig (see below), default `false` |

## `models`

```json
"models": [
  {"file": "parts/room.glb", "name": "Room", "role": "floor"},
  {"file": "parts/chairs.pmx", "role": "props"}
]
```

* `file` — **required**, relative to the stage folder, no `..`, no absolute / drive path.
* `name` — optional (default: the file stem). It is only a label; lights, sockets and screens are matched by material
  name / node name / id, never by part name.
* `role` — `"main"` (default), `"floor"` (the part the stage is aligned on and players stand on) or `"props"`.
  Parsed and reported now; the renderer consumes it in S1b.

Duplicated `file` entries are a validation error. A part that does not load is a scan note / `stage_check` error, and
the stage still lists the parts that do load.

### Supported glTF features (S2a / S2b)

Geometry: triangle primitives, UV0, normals, tangents (ignored), skins / joints, morph targets, node transforms, embedded
(GL) or side-car textures. Every PBR material of a glTF part becomes one `GpuPbrMaterial` record (a PMX / VRM / MToon /
`KHR_materials_unlit` material stays NPR and gets none).

| glTF feature | where it goes | render status |
|---|---|---|
| `pbrMetallicRoughness` (base colour, metallic, roughness factor + maps) | albedo / metalness / smoothness slots | rendered |
| `normalTexture` (+ `scale`), `occlusionTexture` (+ strength), `emissiveTexture` | normal / occlusion / emissive slots (occlusion strength baked at decode) | rendered |
| `alphaMode` / `doubleSided` | PMX material alpha mode, blend / cutout, culling | rendered |
| `KHR_materials_emissive_strength` | `ext.emissiveStrength` → HDR emissive slot (`flags` bit 3) | rendered (not clamped > 1, so it blooms) |
| `KHR_materials_clearcoat` (factor, roughness) | `ext.clearcoat`, `ext.clearcoatRoughness` | rendered: a second GGX lobe (F0 0.04) in raster / RT PBR shading and PT / GI direct light |
| `KHR_materials_transmission` (factor + texture) | `ext.transmission`, `ext.transmissionTex` (R) | S2b: a dielectric in every path (see “Transmission per render path” below) |
| `KHR_materials_ior` | `ext.ior` | rendered: the Fresnel / refraction index of the interface |
| `KHR_materials_volume` (thickness factor + texture, attenuation colour / distance) | `ext.volumeThickness`, `ext.volumeThicknessTex` (G), `ext.volumeAttenuationColor`, `ext.volumeAttenuationDistance` | rendered in RT / PT / GI: refraction + Beer-Lambert absorption; `thickness` 0 = thin-walled |
| `KHR_materials_specular` (factor, colour) | `ext.specularFactor`, `ext.specularColorFactor` | stored, not rendered |
| `KHR_materials_sheen` (colour, roughness) | `ext.sheenColorFactor`, `ext.sheenRoughness` | stored, not rendered |
| `KHR_texture_transform` (offset, scale, rotation) on every PBR map | the map slot's `uvScale.xy` (scale), `uvScale.zw` (offset) and the record's per-material rotation (`coat.z`) | rendered in raster / RT / PT / GI |
| `KHR_lights_punctual` | `PmxModel::importedLights` → the rig with `"gltfLights": true` | rendered as ordinary scene lights |
| multiple UV sets (`TEXCOORD_1` ...) | — | **not** supported for PBR maps: a map on a non-zero `texCoord` is dropped with one `[W]` (also for a texture transform's `texCoord`) |
| Draco / meshopt compression, `KHR_materials_unlit`, specular-glossiness materials | — | unsupported / NPR (see above) |

`KHR_texture_transform` follows the extension's own matrix order: `uv' = R(rotation) · S(scale) · uv + offset` (the
rotation is about the texture origin, as the spec defines it) — applied after the engine's loop / UV-scale conventions.
The GPU record has room for **one** rotation per material, so the base colour map's rotation is used; if another map of
the same material carries a different one, one `[W]` names the material and the first rotation wins. A material without
`KHR_texture_transform` (and every Ray-MMD material) keeps an identity transform, so its look is unchanged.

A stage that puts the same GLB in several parts gets the maps decoded once per part.

### Transmission per render path (S2b)

A material with transmission > 0 becomes a **mesh dielectric**: it refracts, reflects (Fresnel) and absorbs
(Beer-Lambert). Materials with transmission 0 are untouched. The transmission factor comes from the record times its
map's R channel (`PbrTransmissionAt`), together with `ior`, `volumeThickness`, attenuation colour / distance; a stage
rule's `transmission` / `ior` change them at run time (`GpuModel::SetPbrOverrides`), and `RtScene` rebuilds the
geometry flags when that changes which materials are transmissive. The volume thickness map and the attenuation
distance are read from the record's factor (the maps are carried, not sampled: a per-texel chord is out of scope).

| path | what a transmissive material does |
|---|---|
| raster / RT camera view | no screen-space refraction (S2b): the surface is blended with `alpha = 1 − transmission × (1 − Fresnel(N·V, ior))`, its blended colour is tinted by the base colour and its reflectivity output is the interface Fresnel, so SSR reflections show on it; on the RayTraced path the scene shader traces a ray-traced reflection of the scene on the glass itself |
| RT (ray-query shadows / reflections) and PT, offline GI shadows | a shadow ray is tinted by `transmission × baseColour × (1 − Fresnel)` per crossing (`TraceShadowRayTinted`) instead of being stopped, and by the volume's absorption over the chord travelled inside; no caustics. Glass therefore casts a coloured shadow that darkens toward its rim |
| PT, offline GI (path continuation) | at a hit, the transmission event is chosen with probability = the transmission factor, else the ordinary BSDF. The event is Fresnel-weighted reflection vs refraction with the material's ior; thin-walled (`thickness` 0) transmits straight through without bending, a volume refracts and absorbs over the segment it travels inside (`sigma = -ln(attenuationColour) / attenuationDistance`). Smooth when roughness < 0.1; above that the interface normal is a GGX-sampled microfacet normal, so a frosted panel blurs what is seen through it. The base colour tints the light entering the material (glTF), and the offline GI irradiance cache is never read across the interface (that path is brute force) |
| renders of the benchmark glass cube | unchanged: the analytic cube keeps its own dispersion path through the same shared dielectric step (`shaders/dielectric.hlsli`) |

## `materials`

```json
"materials": [
  {"match": "cove", "baseColor": [0.82, 0.82, 0.85], "roughness": 0.45, "receiveShadow": true},
  {"match": "led_*", "screen": "main", "castShadow": false},
  {"match": "glass", "transmission": 1.0, "ior": 1.5, "alpha": 0.6}
]
```

`match` is an exact material name or a `*` / `?` wildcard, ASCII-case-insensitive. The first matching rule wins per
field (a later rule may still fill fields the earlier one leaves out — consumers apply the resolved values, so author
one rule per material family). Every override is optional:

| field | type | meaning |
|---|---|---|
| `baseColor` | colour | albedo (linear); alpha is the material's opacity |
| `roughness`, `metallic` | 0..1 | PBR factors |
| `emissive` | `[r,g,b]` | emitted colour (linear, already includes the strength below) |
| `emissiveStrength` | float | multiplies `emissive` (glTF `KHR_materials_emissive_strength`) |
| `transmission` | 0..1 | dielectric transmission (`KHR_materials_transmission`); raster / RT blend the surface see-through, RT / PT / GI refract it (S2b) |
| `ior` | float | 1.0..3.0 (`KHR_materials_ior`) |
| `alpha` | 0..1 | opacity |
| `mirror` | bool | planar mirror surface (S10, see below) |
| `screen` | string | a screen id from `screens`: this material is that screen's surface |
| `castShadow`, `receiveShadow` | bool | shadow participation |

A rule that matches no material of the parts (after they load) is a `stage_check` error: the override would do nothing.

## `lights`

The array entries are **exactly the `.mmdxproj` editor `lights` entries** — one implementation
(`asset/SceneLightJson.h`, `SceneLightToJson` / `SceneLightFromJson`) reads and writes both, and
`tools/studio_project_test` keeps the project round trip byte-identical. `name` is required here (the name is the
binding key); `uid` is ignored in a stage (the stage's order is its identity).

```json
"lights": [
  {"kind": "sun", "name": "key", "values": {"direction": [-0.35, -1, 0.25], "color": [1, 0.96, 0.9], "intensity": 1.1}},
  {"kind": "ambient", "name": "ambient", "values": {"intensity": 0.18}},
  {"kind": "spot", "name": "spot_l",
   "values": {"position": [-45, 62, 40], "aim": [-22, 0, -30], "color": [1, 1, 1], "intensity": 2.4,
              "range": 140, "coneOuter": 0.26, "coneInner": 0.17},
   "spot": {"aim": "sway", "swayPhase": 0.0},
   "shadow": {"type": "soft"}}
]
```

Fields (all optional except `kind` and `name`; a missing one keeps the `SceneLight` default):
`kind` (`sun` / `point` / `spot` / `ambient` / `area` / `fog_volume` / `sky`), `enabled`, `values` (`position`, `aim`, `direction`, `color`,
`intensity`, `range`, `coneOuter`, `coneInner` radians, `size`, `extent`, `volumetric`), `sun` (`vmdLink`, `rimStrength`, `rimColor`), `spot`
(`aim` = `manual` / `target` / `sway`, `target`, `part` = `centre` / `head`, `swayPhase`), `sky` (`zenith`,
`horizon`, `ground`), `shadow` (`type` = `nocast` / `hard` / `soft`, `softness`, `density`, `color`), `falloff`
(`none` / `linear` / `inverse_square`), `affectDiffuse`, `affectSpecular`, `viewportVisible`, `keys`.
Falloff curves (all windowed to 0 at `range`): `none` = 1 / (1 + (d / 50)^2), `linear` = 1 - d / range,
`inverse_square` = (25 / max(d, 12.5))^2, i.e. `intensity` is the brightness at 25 units (2 m) for every curve, so the
same intensity values work after switching the curve (the peak near the source is capped at 4x).

S9 haze fields: `values.volumetric` (0..20, how much of the light shows in the volumetric haze; 1 = the physical share,
0 = the light is invisible in the medium) and `volumetricG` (the light's Henyey-Greenstein anisotropy, -0.9..0.95; absent
= the fog's own, from `environment.fog.anisotropy`). A `fog_volume` light is a local participating-medium volume: it lights
no surface, `values.position` is its centre, `values.extent` the half size, `values.intensity` the extra density as a multiple of the
base haze density (`fog.density`) added inside it (4 = five times as hazy there), `values.color` the medium tint, `fogShape` (`box` / `sphere`) the shape and `fogEdge` (0..1) the soft
border band. Both keys are omitted from a written file while at their defaults, so older files are unchanged.

Rules: at most one `sun` and one `ambient`; `coneInner <= coneOuter`; a `sun` cannot carry `keys` (use cues on its
`intensity` / `color` / `direction` instead). Play mode's lighting picker gains a "스테이지 조명" entry in S1b.

### `gobo` (S7b): a texture projected through a spot's cone

A **spot** may carry a gobo: a greyscale or colour stencil projected through its cone, so the lit floor / wall / haze
shows the texture's pattern instead of a plain pool. The light's colour multiplies the stencil's rgb, so a coloured
image tints the light.

```json
{"kind": "spot", "name": "floor_ring",
 "values": {"position": [-38, 105, 30], "aim": [-38, 0, 30], "coneOuter": 0.26, "coneInner": 0.16},
 "gobo": {"file": "gobos/ring.png", "rotation": 15, "scale": 1.0, "spin": 45, "focus": 0.9}}
```

| key | default | meaning |
| --- | --- | --- |
| `file` | — | the image, relative to the stage folder (a bundled PNG / JPG; `SafeRelativeFile` rules as for `screens`) |
| `rotation` | `0` | degrees: the stencil's rotation around the cone's axis |
| `scale` | `1` | uv scale: 1 = the texture fills the cone's inscribed square, 2 = magnified, 0.5 = repeated area cropped |
| `spin` | `0` | degrees per second, added to `rotation` with the song / timeline time (a rotating gobo) |
| `focus` | `1` | `1` = sharp; lower blurs the stencil (a higher mip level) |

The block is omitted from a written file when the light has no gobo, so an older file is unchanged. `stage_check`
requires the file to exist and to be a `.png` / `.jpg` image. The projection is in the spot's own frame (`+z` = the
light direction, an up vector picked stably): the pattern lands on world geometry and in the haze, in every render
path (raster, RT, path tracing, offline GI and the volumetric beams), and is the same in all of them. A missing or
undecodable file is one `[W]` and the light then shines without a gobo.

The four value fields are keyable through the usual binding / cue machinery: `lights.<name>.gobo.rotation`,
`lights.<name>.gobo.scale`, `lights.<name>.gobo.spin`, `lights.<name>.gobo.focus` (all float). The inspector shows
them read-only while a stage binding drives them, exactly like the shadow properties.

Limits: a gobo is a **spot** feature (a point / area light has no cone); the legacy per-pixel march
(`--volumetric-mode march`) keeps a plain beam (the default froxel grid and the offline march carry gobos), and the
`world.lights` world-pack API does not read gobos.

### `sky` (S9b-1): a Physical Sky light

A light of kind `sky` replaces the hand-set sky gradient with a physical clear sky (Preetham 1999, a Perez distribution in
Yxy): it is the background, the fog colour source and the sun's colour in every render path (raster, RT, path tracing and
offline GI). At most one sky light counts (the first enabled); with none the frame is exactly the one before S9b-1.

```json
{"kind": "sky", "name": "sky",
 "values": {"skyTimeOfDay": 6.5, "skyTurbidity": 3.2, "skyHaze": 0.35, "skyNorthDeg": -25,
            "skyGround": [0.34, 0.33, 0.31], "skyIntensity": 1.0, "skySunIntensity": 1.0,
            "skyMaxElevationDeg": 60}}
```

| key | default | meaning |
| --- | --- | --- |
| `skyTimeOfDay` | `12` | hours, 0..24: the sun's arc (sunrise 6:00, sunset 18:00, the peak at 12:00). Ignored while the scene has a `sun` light |
| `skyNorthDeg` | `0` | compass rotation of the arc around +y |
| `skyMaxElevationDeg` | `60` | the sun's elevation at 12:00 (1..89) |
| `skyTurbidity` | `3` | haze in the atmosphere, 1.7..10 (3 = a clear blue sky, 10 = a milky one) |
| `skyHaze` | `0.2` | 0..1: how strongly the lowest ~15 degrees blend toward the horizon colour |
| `skyGround` | `[0.35, 0.35, 0.35]` | the ground's albedo: the below-horizon blend target (the light's ground colour x the sky's irradiance) |
| `skyIntensity` | `1` | scales the sky's radiance |
| `skySunIntensity` | `1` | scales the sun's colour |

The light owns the sun: without a `sun` light its direction comes from `skyTimeOfDay` / `skyNorthDeg` and its colour from
the model's transmittance, and it casts shadows like a hard sun. With a `sun` light that light keeps its own direction
and intensity and only its colour is replaced. Below the horizon the sun goes black and the sky falls to a dim night
tone (a floor, so the scene is never pitch black). The model also writes the ambient sky / ground colours (the
hemisphere fill, the floor and the haze follow the sky); image-based sky lighting is S9b-2.

The values are **keyable** (they live in `values`, so a key's `values` carry them) and bindable:
`lights.<name>.sky.timeOfDay` / `.northDeg` / `.turbidity` / `.haze` / `.ground` (colour) / `.intensity` /
`.sunIntensity` (float). `stage_check` rejects out-of-range values, and a second sky light. A written file omits the
values that are at their defaults, so a file without a sky light keeps its bytes.

**Image skies (S9b-3).** A sky light may show an image instead of the model:
`"skyMap": "sky/helipad.hdr"` (relative to the stage folder), `"skyMapEncoding": "auto"` (`linear` | `srgb` | `rgbt`),
`"skyMapMirror": false`, and the keyable values `skyMapRotationDeg` (0) / `skyMapIntensity` (1) (targets
`lights.<name>.sky.mapRotation` / `.mapIntensity`). Supported: Radiance `.hdr`, `.png` / `.jpg` (sRGB), and uncompressed
RGBA8 `.dds` (an equirect panorama, or a cube map, which is converted); a Ray-MMD sky dome texture (RGBT-encoded, alpha
not all 255) decodes automatically. EXR and BC6H / BC7 `.dds` are not supported (a warning, the model sky stays). The
image is equirectangular: its centre column looks along +Z, u grows to the right. The image becomes the background and,
through the sky lighting, the ambient light everywhere and a real light in the path tracers. The light's own sun still
shines (set `skySunIntensity` to 0 when the image has its own sun; raster / RT then have no sharp sun shadow).

**Clouds (S9b-4).** The procedural sky (not an image sky) may carry a cloud layer, keyable like the sky values:
`cloudCoverage` (0, 0..1: 0 = a clear sky, exactly as before), `cloudOpacity` (1), `cloudSoftness` (0.25, 0.02..1: the
hardness of the cloud edges), `cloudHeightKm` (2, 0.3..12), `cloudScaleKm` (1.5, 0.2..20: the size of the noise),
`cloudWindDeg` (0, -360..360) and `cloudWindSpeed` (10, 0..100 m/s). Targets
`lights.<name>.sky.cloudCoverage` / `.cloudOpacity` / `.cloudSoftness` / `.cloudHeight` / `.cloudScale` / `.cloudWindDeg`
/ `.cloudWindSpeed`. The layer drifts with the song time (the wind offset is direction x speed x seconds) and is lit by
the sky's sun; the sun's colour dims with `coverage * opacity`. Limit: clouds cast no shadows on the ground.

### `gltfLights` (S2a)

A glTF part may carry lights with `KHR_lights_punctual`. They are imported into the model as light descriptions
(`PmxModel::importedLights`), and a stage uses them **only** when its `stage.json` says `"gltfLights": true`. They are
then appended to the `lights` rig above, after it, named after the glTF node (a name the rig already uses gets a
` 2`, ` 3`, … suffix), in manifest part order. `gltfLights` off (the default) = a plain glTF model's lights are ignored,
exactly like today; bindings can still target the appended lights by name, but a rule must know them at author time.

Only the three punctual kinds are used; the mapping to a rig entry:

| glTF light | rig entry | fields used |
|---|---|---|
| `point` | `kind: "point"` | `position` (node world translation), `color`, `intensity`, `range` |
| `spot` | `kind: "spot"`, `aimMode: "manual"`, soft shadow | the above + `aim` = position + direction, `coneInner` / `coneOuter` |
| `directional` | `kind: "sun"` (`vmdLink: false`) | `direction` (the node's -Z), `color`, `intensity` |

**Unit conversion** (the extension declares candela for point / spot and lux for directional; the engine's intensities are
unitless multipliers, like every other light in the rig — `intensity` 1 = the preset key light):

```
engine intensity (point / spot)  = candela * 4π / 683 * 0.0005     ≈ candela * 9.2e-6
engine intensity (directional)   = lux    * 1 / 683 * 0.5          ≈ lux * 7.3e-4
```

The first factor is the photometric conversion (683 lm/W, 4π sr for an isotropic point light), the trailing scale is tuned
by eye: Blender's 1000 W point light exports as 54351 cd and reads 0.5 — about the default hemisphere fill at 5 m (62.5 MMD
units). Positions and ranges are converted with the geometry (`1 m = 12.5` MMD units, the same axis mirror), so a light
stays attached to the surface it was authored with, including the stage's floor alignment shift. Spot cones are radians.
`range` 0 = unlimited; the rig gets 140 (the engine default) instead.

## `sockets`

```json
"sockets": [
  {"name": "world", "position": [0, 0, -20]},
  {"name": "truss", "node": "Truss_Front"},
  {"name": "wall", "node": "Cove_Wall", "rotation": [0, 180, 0], "scale": [1, 1, 1]}
]
```

`name` required and unique. `node` names a glTF node / PMX bone; without `node`, an explicit `position` /
`rotation` (degrees) / `scale` is required. With `node`, the explicit transform is an extra offset applied on top.

## `cameras`

```json
"cameras": [{"name": "wide", "position": [0, 20, -60], "target": [0, 10, 0], "fov": 40}]
```

`name` required and unique, `fov` vertical in degrees (1..179), `position != target`. S4 turns these into studio camera
entities: the studio's "스테이지 카메라" button (`studiocamimport`) makes one extra camera per entry with its
`stageUid` set, a single key at frame 0 holding the pose, and its name as the camera's name. A camera that is imported
twice gets a numbered copy ("wide", "wide 2"). S5 shows them on LED screens (the screen source `"camera"` names one).

## `screens`

```json
"screens": [
  {"id": "main", "materials": ["led_main"], "source": "image", "file": "textures/led_main.png",
   "width": 2048, "height": 1024, "brightness": 1.6, "emitLight": true},
  {"id": "feed", "materials": ["led_side"], "source": "camera", "camera": "stage"},
  {"id": "fx", "materials": ["led_*"], "source": "program", "file": "packs/led"}
]
```

* `id` — required, unique.
* `materials` — required, at least one name / glob of the surfaces this screen draws on (also reachable through a
  material rule's `"screen"`).
* `source` — `none` (default), `image` (`file`), `video` (`file`), `camera` (`camera` = a camera name) or `program`
  (`file` = a bundled pack folder that draws the content).
* `width` / `height` — the LED pixel grid (default 1920x1080), `brightness`, `emitLight` (S8: the screen lights the
  scene).

Only parsed and validated in S1a; S5 / S8 render them. A `program` pack is validated structurally (the folder exists
and holds `pack.json`).

**Rendered since S5 (2026-10-26).** A screen is one GPU texture (RGBA8 sRGB, at most 2048 px per side) shown on every
material of the stage's parts that matches the screen's `materials` globs or a `material` rule whose `"screen"` names
it. The texture is the material's *emission* (`MatFlag_Screen`: HDR radiance x `brightness`; the material itself is
black, no diffuse / specular / AO) in raster, RT, PT and the offline GI, and `source: "none"` is black (an off LED).
| `source` | what it shows |
|---|---|
| `none` | black |
| `image` | `file`, decoded once (downscaled to <= 2048 when larger) |
| `video` | `file`, one decoder per video screen, looped by the clip's duration; an offline render decodes the exact frame |
| `camera` | a live feed: the scene re-rendered from the `camera` (see below) |
| `program` | a bundled screen pack (`packs`, role `screen`): its `screen.hlsl` draws the content (S6, see below) |

A **camera feed** is the scene rendered from that camera into the screen texture *before* the main scene: raster and
lit, with the user's shading and shadow maps but without TAA, upscaler, DoF, volumetrics, effects or MME post, and at
most 960x540 (min(screen size, 960x540)). Screens shown *inside* a feed keep their previous frame's texture (no
recursion), and at most two feeds re-render per frame (round robin beyond that). Every render path shows the feeds.
`camera` names a `cameras` entry in play mode and in the studio; in the studio `uid:<n>` also names an S4 studio camera
(its evaluated pose at the timeline time).

A **program** screen is drawn by a bundled screen pack (S6): `render/StageScreens` makes the screen's texture a render
target and draws the pack's `screen.hlsl` into it once per frame, before the scene passes, with a fullscreen pass
(DXC `ps_6_0`). The pack implements `float4 PackScreen(PackScreenInput)` (contract `shaders/screen_api.hlsli`, guide
docs/shader_screen_api.md) and gets the uv, the screen texture size, the song / timeline seconds (so an offline render
is exact), its `packs[]` parameters and the pack's `textures`. The result is linear RGB and becomes the screen's
emission like any other source, in every render path. `file` names the pack folder; a `packs[]` entry of role `screen`
naming the same screen wins over it. A broken or missing pack leaves the screen black (one `[W]` at load).

`screens.<id>.brightness` / `.enabled` / `.source` / `.file` / `.camera` binding targets override the manifest per
frame (play mode and the studio), and the studio's stage panel writes per-screen overrides into the project
(`"screens": {id: {source, file, camera, brightness}}`, project version 6).

**Screens light the scene in every path (S8).** The path tracers sample the emissive triangles directly (S8a, an
emissive-triangle next-event-estimation table built per frame). Raster and RT get one rectangular area light per screen
material (S8b): the rectangle the material's rest-pose triangles fit, transformed by the part's root each frame, one
sided (the side the surface is drawn from). Its diffuse light is the screen texture's average colour this frame (a GPU
reduction, no readback) times the screen's `brightness`, so a video / camera feed / program screen lights the performer
and the floor with whatever it is showing; a glossy surface's reflection reads the texture itself (the image shows the
right way round). Limits: one rectangle and one average per screen material (a curved LED wall is approximated by one
rectangle; a two-colour image lights the floor with the mix, where the path tracers show each half), unshadowed. At
most 16 screen lights per frame (the largest emitters win, by area x brightness).
`RenderSettings::screenLights` (ini `screenLights=`, CLI `--screen-lights 0|1`) switches it off; a frame with no screen
is unchanged either way.

## Planar mirrors

**Rendered since S10 (2026-10-26).** A material rule with `"mirror": true` makes that material a true planar mirror in
every render path. The plane is fitted once at load from the material's rest-pose triangles (area-weighted centroid
and normal); a material whose vertices stray further than 1 % of its own extent from that plane is not a planar mirror
and shows its reflection tint only (`[I] ... mirror material N is not planar`). Keep a mirror wall as one flat,
coplanar group: split panels with different planes belong in separate materials (only the two largest on-screen planes
get a reflection per frame).

* The material's `baseColor` is the **reflection tint** (default white) and its `roughness` the blur: 0.02 or less is a
  sharp reflection, higher values blur it (a 3x3 tap blur in raster, the cone / GGX lobe in RT / PT / GI).
* **Raster** renders the scene once more per kept plane from the camera reflected about it, at half the render
  resolution, with an oblique clipping plane on the mirror (nothing behind the mirror leaks in) and without TAA,
  upscaler, DoF, volumetrics, effects or MME post. The scene PS samples that texture for mirror pixels by their own
  world position, so the reflected image is exact. A second mirror plane is reflected the same way (two slots); mirrors
  inside the reflection show their tint only (no recursion).
* **RT / PT / offline GI** reflect at the hit as a delta event (weight = the tint; a rough mirror uses the cone lobe),
  so a mirror reflects what the ray tracer sees, including other mirrors.
* A quad / ortho view, a camera feed and `RenderToImage` (lobby thumbnails) show the tint only, since they would need
  their own reflection render.
* Zero cost without a mirror material: no textures are created, no extra scene render runs, and the frame is identical.
  Mirror materials are excluded from the shadow-map casters by default in `mirror_room` (`"castShadow": false`).

## `environment`

```json
"environment": {
  "fog": {"density": 0.02, "color": [0.55, 0.6, 0.72], "anisotropy": 0.55, "heightFalloff": 0.03},
  "fogVolumes": [
    {"id": "beam", "shape": "box", "position": [0, 20, 0], "extent": [30, 20, 30],
     "density": 2.0, "color": [0.8, 0.9, 1.0], "edge": 0.4}
  ],
  "ambient": {"intensity": 0.08, "skyZenith": [0.03, 0.04, 0.08], "skyHorizon": [0.1, 0.12, 0.2],
              "ground": [0.05, 0.05, 0.06]}
}
```

`fog.density` is the participating-media density (S9), `anisotropy` the Henyey-Greenstein `g`, `heightFalloff` the
exponential height decay per unit, `radius` the haze sphere around the origin (default 180 units; grown automatically
to hold every fog volume, so an arena that spans 500 units sets e.g. 450). While a stage with an `environment.fog` block is loaded it turns the volumetric pass
on and sets its values (in play mode and in the studio; the user's volumetric switch still turns the pass off). The
user's density slider scales `density`.

`environment.fogVolumes` (S9) are local medium volumes: each becomes a `fog_volume` light of the stage's rig (appended
after `lights`, so a light index keeps its meaning). `shape` is `box` (default) or `sphere`, `position` the centre,
`extent` the half size (sphere: the three radii), `density` the extra density as a multiple of `fog.density`, `color` the tint and `edge`
(0..1) the soft border band; `id` names the light (default `안개 <n>`). `ambient` overrides the ambient light's sky /
ground colours and hemisphere strength.

## `packs`

```json
"packs": [
  {"folder": "packs/floor", "role": "surface", "materials": ["cove*"], "params": {"tint": 0.6}},
  {"folder": "packs/haze", "role": "world", "socket": "world"},
  {"folder": "packs/led", "role": "screen", "screen": "main"}
]
```

`folder` is relative and must hold a `pack.json`. `role` decides what the pack attaches to: `surface` (needs
`materials` globs), `world` (needs an existing `socket`), `screen` (needs an existing `screen` id). `params` are the
initial parameter values (float); a binding reaches them as `packs.<index>.params.<key>` (the index is the array
position). S6 wires them into the renderer.

**Rendered since S6 (2026-10-26).** Every entry is registered in the shader pack registry under
`stage:<stage id>/<folder>` (never installed, never listed in the manager / pickers / gallery, no hot reload; a broken
pack is one `[W]` and the stage keeps loading). A stage with no `packs` entry costs nothing: no registry entries, no
PSOs, no render targets.

| role | what it does |
|---|---|
| `surface` | the FIRST entry whose `materials` globs match a material of a stage part shades that part's materials with the pack (`surface.hlsl`); the part's other materials keep the default shading. The pack's class comes from its own class rules, else its first rule's class (a stage material rarely matches a character pack's rules), else `body`. Raster, RT, PT and offline GI all draw the pack: a stage part's record reaches the PT / GI `PackEvaluate` hook exactly like a character's (`render/PtPackVariants.*`, `RayTracing.cpp` `TakesPtPack`), and the pack owns the surface there (the engine's ambient-only stage term is skipped; `headPos` / `headScale` are 0 because a part has no head bone) |
| `world` | a `WorldInstance` at the `socket`'s transform (the socket node's world matrix x the socket's explicit offset, resolved after the parts are posed), appended after the user's world stack in play mode and after the project's world entities in the studio. Not an editable outliner entity |
| `screen` | the content of the `screen` with that id, drawn by the pack's `screen.hlsl` (docs/shader_screen_api.md); wins over the screen's own `source: "program"` / `file` |

`packs.<index>.params.<key>` binding targets override the entry's `params` per frame for all three roles (play mode and
the studio). The mapped pack classes are the pack.json `classes` values; a pack with no class rule at all classifies
stage materials as `body`.

`tools/stagegen/stagegen.py --only pack_test` writes the fixture (`captures/stages/pack_test`): a `program` screen
drawing animated bars from the time and a param, a surface pack tinting `cove_wall` (`surface.hlsl` for raster / RT and
the matching `pt_surface.hlsl` for PT / offline GI), and the built-in `snow` pack copied as the world pack at the
`world` socket. `tools/stage_check` passes on it.

## `params`, `groups`, `variants`

```json
"groups": [{"id": "look", "label": {"en": "Look"}, "collapsed": false}],
"params": [
  {"key": "coveColor", "type": "color", "label": {"ko": "배경 색", "en": "Cove colour"},
   "group": "look", "default": [0.82, 0.82, 0.85, 1.0]},
  {"key": "keyIntensity", "type": "float", "label": {"en": "Key intensity"},
   "default": 1.1, "min": 0.0, "max": 3.0},
  {"key": "mirrorAmount", "type": "enum", "default": "on",
   "options": [{"value": "off", "label": {"en": "Off"}}, {"value": "on"}]}
],
"variants": [
  {"id": "studio", "label": {"en": "Studio"}, "values": {"coveColor": [0.82, 0.82, 0.85, 1.0]}},
  {"id": "night", "label": {"en": "Night"}, "values": {"keyIntensity": 0.5}}
],
"defaultVariant": "studio"
```

* `param.key` — required, unique; `type` — `float` (default), `int`, `bool`, `color`, `enum`.
* `float` / `int` use `min` / `max` (default 0..1 / 0..100) and `default` (clamped); `bool`: `default` true / false;
  `color`: `default` = `[r,g,b]` or `[r,g,b,a]` (linear); `enum`: `options` (required, each `value` unique, optional
  `label`), `default` = an option's `value` or its index (default: the first option).
* `group` — a `groups` id (unknown = a `stage_check` error); the UI nests params under it.
* `label` — localized; an empty label falls back to the key.

A **variant** is a named set of param values. `defaultVariant` names the initial one (empty = the first variant; no
variants = the plain defaults). A variant that names an unknown param is an error.

## `bindings`

```json
"bindings": [
  {"param": "keyIntensity", "target": "lights.key.intensity"},
  {"param": "fillLevel", "target": "lights.fill.intensity", "scale": 2.0, "offset": 0.25},
  {"param": "mirrorAmount", "target": "materials.mirror_wall.roughness", "remap": [0, 1, 0.5, 0.02]},
  {"param": "beamChase1", "target": "lights.beam_01.intensity", "scale": 3.6, "gain": "beamLevel"}
]
```

`out = (remap(value) * scale + offset) * gain`, where `remap: [inMin, inMax, outMin, outMax]` maps the param range onto
the target range first (an empty input range is an error). `gain` is optional: the key of a `float` / `int` param whose
resolved value (after the drivers) multiplies the result: a number target `out * g`, a colour / vec3 target its rgb
(xyz) `* g` with alpha kept. A gain on a bool / string target, an unknown gain param or a gain param of another type is
a validation error; without `gain` nothing changes. Several bindings may hit one target: they are emitted in list
order and applied in that order. Target paths:

| path | value type |
|---|---|
| `lights.<name>.<field>` | see the light fields: `enabled` (bool), `intensity` / `range` / `coneOuter` / `coneInner` / `shadowSoftness` / `shadowDensity` / `rimStrength` / `swayPhase` / `gobo.rotation` / `gobo.scale` / `gobo.spin` / `gobo.focus` / `sky.timeOfDay` / `sky.northDeg` / `sky.turbidity` / `sky.haze` / `sky.intensity` / `sky.sunIntensity` / `sky.mapRotation` / `sky.mapIntensity` / `sky.cloudCoverage` / `sky.cloudOpacity` / `sky.cloudSoftness` / `sky.cloudHeight` / `sky.cloudScale` / `sky.cloudWindDeg` / `sky.cloudWindSpeed` (float), `color` / `rimColor` / `shadowColor` / `sky.ground` (colour), `position` / `aim` / `direction` / `size` (vec3), `vmdLink` (bool) |
| `materials.<rule match>.<field>` | `baseColor` (colour), `roughness` / `metallic` / `emissiveStrength` / `transmission` / `ior` / `alpha` (float), `emissive` (vec3), `mirror` / `castShadow` / `receiveShadow` (bool), `screen` (string) |
| `screens.<id>.<field>` | `brightness` (float), `width` / `height` (int), `emitLight` / `enabled` (bool), `source` / `file` / `camera` (string) |
| `packs.<index>.params.<key>` | float |
| `environment.<field>` | `fog.density` / `fog.anisotropy` / `fog.heightFalloff` / `ambient.intensity` (float), `fog.color` / `ambient.skyZenith` / `ambient.skyHorizon` / `ambient.ground` (colour) |

A `float` / `int` / `bool` param drives number / bool / vec3 targets (a float drives every component of a vec3 unless
the target is a colour); a `color` param drives colour / vec3 targets; an `enum` param drives a string target by the
**option's value** and number targets by its index. Everything else is a validation error, and so is an unknown kind,
field, owner, material rule or pack index.

The evaluator never crashes on such a target: it reports it in its warnings and skips the assignment.

## `cues` (L0 timeline)

```json
"cues": [
  {"param": "keyIntensity", "keys": [{"frame": 0, "value": 1.1}, {"frame": 300, "value": 1.5}]},
  {"param": "fogDensity", "stepped": true, "keys": [{"frame": 0, "value": 0.004}, {"frame": 900, "value": 0.012}]}
]
```

One track per param (two tracks for one param is an error), `keys` sorted by the parser, one key per frame (a repeated
frame keeps the last). `stepped: true` holds the last key at or before the frame; otherwise the value is linear between
the two neighbours (colours component-wise). `int` / `bool` / `enum` keys always hold (no interpolation). Before the
first key and after the last, that key holds (as MMD does). Frames are 30 fps song frames: `frame = time * 30`.

The studio keys stage params itself in S3; these cue tracks are the stage's own light show, and a project track
overrides them (project overrides beat cues, see below).

## `drivers` (L0 procedural)

```json
"drivers": [
  {"param": "beamLevel", "input": "audio.beat", "shape": "pulse", "every": 1, "phase": 0, "power": 2,
   "min": 0.35, "max": 1.0, "blend": "mul", "amount": "beatReact"},
  {"param": "hazeDrift", "input": "time", "shape": "sine", "every": 8, "min": 0.8, "max": 1.2, "blend": "mul"}
]
```

A driver modulates one param from the song's beat analysis or the clock, with no keys. Drivers run **last**: they
modulate the value defaults < variant < cues < overrides resolved (in the studio that includes the user's own value or
key), in list order, each on the previous driver's result.

| field | type | default | meaning |
|---|---|---|---|
| `param` | string | (required) | a `float`, `int` or `color` param (`bool` / `enum` = validation error) |
| `input` | string | (required) | `audio.beat`, `audio.bar`, `audio.onset`, `audio.energy`, `audio.section` or `time` |
| `shape` | string | `pulse` | `pulse`, `saw`, `sine`, `triangle`, `square`, `ramp` (cyclic inputs only) |
| `every` | float > 0 | 1 | beats / bars per cycle (`time`: the cycle length in seconds) |
| `phase` | float | 0 | cycles added to the count |
| `power` | float > 0 | 2 | pulse sharpness; level inputs: an exponent, applied only when given (else 1) |
| `duty` | 0..1 | 0.5 | `square`: the on share of a cycle |
| `min`, `max` | float | 0, 1 | the output range: `v = min + (max - min) * s` |
| `blend` | string | `set` | `set` (v), `add` (base + v), `mul` (base * v); a colour param takes `mul` only (rgb * v, alpha kept) |
| `amount` | string | none | a `float` param key: `result = lerp(base, driven, clamp(amount, 0, 1))` |

Signal `s`:

* cyclic inputs (`audio.beat`, `audio.bar`, `time`): `u = count / every + phase`, `count` = the continuous beat count
  (beat index + phase; negative before the first beat), the bar count (downbeat index + bar phase) or the song seconds
  (clamped to 0, like cues). `f = u - floor(u)`. `saw` = f, `pulse` = (1 - f)^power, `sine` = 0.5 - 0.5 cos(2 pi f),
  `triangle` = 1 - |2f - 1|, `square` = f < duty ? 1 : 0, `ramp` = u (unbounded, not wrapped).
* level inputs (`audio.onset` = the decaying onset envelope, `audio.energy` = the current beat's normalised RMS,
  `audio.section` = 0 low / 0.5 mid / 1 high): `s = clamp(value, 0, 1)^power`; `shape` / `every` / `phase` are ignored.

`amount` reads its param's value **before** any driver (an amount param cannot itself be driven: validation error).
The result is clamped to the param's range (`int` rounds).

**Audio unavailable**: an `audio.*` driver is skipped entirely (the param keeps its base) when there is no song audio
or its analysis is not ready / failed (`StageSignals::audioValid` false; the analysis runs on a worker thread, and
offline renders wait for it). `time` drivers always apply. The analysis is the same one the packs' `inputs.audio` read
(`audio/BeatAnalysis`, sampled at the frame's audio time: play = the audio cursor / the motion clock in offline
renders, studio = timeline time - audio offset).

Validation: unknown param / amount param, a `bool` / `enum` param, `set` / `add` on a colour, `every` <= 0, `power` <=
0, `duty` outside 0..1, an amount param that is not a float or is itself driven. Parse problems: a wrong-typed field,
an unknown `input` / `shape` / `blend` string, a missing `param` / `input`.

## Evaluation (pure)
`EvaluateStage(manifest, variantId, overrides, timeSeconds, skipCues, signals) -> StageEvalResult`
(`asset/StageEval.h`) is the single implementation used by play, studio, GI and the tools. Order per param:

1. the param's default,
2. the chosen variant's value (`variantId` empty = `defaultVariant`; an unknown id warns and uses the defaults),
3. the cue track's value at `time` (clamped to 0),
4. the caller's overrides (a project's saved values),
5. the drivers, in list order (`signals` = `StageSignals`, the song's beat analysis at the frame's audio time; nullptr or
   `audioValid` false skips the `audio.*` drivers).

Values are clamped to their param's range / option list. Then every binding turns its param's resolved value into one
assignment (`target` + typed value: float, vec3, colour, bool or string; times its `gain` param's resolved value). The
function is deterministic and does no I/O, so seeks, reverse playback, video and GI shutter samples agree.

## Runtime (S1b: loading, materials, lights, variants)

Play mode (`app/SceneLoader`, `LoadScenePackage`) and the studio (`studio/StudioLoader`, `LoadStudioStage` /
`LoadStudioPackage` / `LoadStudioProjectPackage`) load an MMDX stage **part by part from `models`** (manifest order,
glTF / GLB / PMX through `LoadModelFile`), not from the library's flat part list. A part that fails stays out with a
warning; the stage still loads its other parts.

* **Material rules** are resolved on the loader worker thread before the GPU upload (`asset/StageMaterials.h`): a
  material a rule matches gets the rule's values in its PBR record (a material without a record gets one whose albedo
  reads the PMX diffuse map times the factor, smoothness 0.5, metalness 0), `alpha` overwrites the PMX diffuse alpha,
  and `castShadow` / `receiveShadow` set the PMX material's shadow flags. "First matching rule wins per field" is
  `StageEffectiveMaterialValues`. The linear stage colours are written the way the shader reads the slots (an
  sRGB-encoded constant, or a linear factor on a sampled map).
* **Runtime targets** (`materials.<rule>.<field>` from bindings / cues): the App evaluates the stage every frame at the
  song time (`app/StageRuntime`), replays the light assignments onto a copy of `lights` for `BuildSceneLighting`, and
  hands the material assignments to `GpuModel::SetPbrOverrides` (a 3-entry ring of UPLOAD-heap PBR records, one SRV
  each, so raster / RT / PT / offline GI see the same values; only changed values are re-uploaded). A model that never
  receives an override keeps its single DEFAULT-heap record buffer.
* **Lighting in play mode**: the lighting picker gains "스테이지 조명" while the loaded stage has lights, and that is the
  scene's default then (an ordinary preset chosen for other stages is kept). It renders `lights` through
  `BuildSceneLighting` exactly like the studio does. Without a stage rig the entry is hidden, and the Studio preset is
  the fallback.
* **Studio**: adding an MMDX stage creates ONE stage entity (`studio::StudioStage`, `StudioDoc::stages`); its parts stay
  models (`StudioModel::stageManifest / stagePart / stageUid`) and the outliner shows the stage as one row with its parts
  as children (collapsed by default; remove / visibility act on the whole stage). Adding a stage that has lights replaces
  the project's scene lights with its rig as ONE undoable step (`LightsCommand`, toast); those lights are tagged with the
  stage (`SceneLight::stageUid` / `stageLightName`, saved in the project) and the fields a binding drives (`lights.<name>.
  <field>`) are shown read-only in the light inspector ("스테이지 파라미터가 제어").
  Every frame (editor, offline still / video) the studio evaluates each stage at the timeline time — base values (the
  user's overriding the variant), the project's keyed params at that frame, then the manifest's cues for the params it
  did not key (`StageOverridesAt` → `EvaluateStage(.., skipCues)`) — and replays the material targets into the parts'
  GPU models (`ApplyStageMaterialList`) and the light targets onto its tagged lights (`ApplyStageLightField`).
  The inspector's "스테이지" panel edits the variant, every param (per the manifest `groups`), imports the manifest cues
  as keys ("큐를 키로 가져오기": those params leave the cue layer, `StudioStage::cuesOff`) and resets one param; params
  are keyed on the timeline (`RowKind::StageParam`, category "스테이지" next to "월드").
  The installed stage is never written: the variant, param base values, keyed params and `cuesOff` are saved in the
  project as `"stages"` (version 4) and the parts / lights refer to them by `stageUid`.
* **Variant**: the lobby's select screen ("셰이더" tab) shows a combo when the selected stage has >= 2 variants, saved as
  `stageVariant=<variant id>|<stage id>` in `mmdx12.ini`; `--stage-variant <id>` wins for one run. Empty = the
  manifest's `defaultVariant`.
* Ignored silently until their milestones: `packs.*` and `environment.*` targets (S6 / S9). They are parsed, validated
  and carried, never warned about at run time. S2a gives clearcoat its own specular lobe, S2b renders transmission as a
  mesh dielectric (see the glTF feature table and “Transmission per render path”), S5 renders `screen` and S10 renders
  `mirror` (see “Planar mirrors”).
* Installed stages live in `<exe>/stages/<id>/` (next to the executable, like `shader_packs`): their library ids are
  prefixed `stages:<id>` so they never collide with a user-library id. A missing folder is fine. The user's library can
  hold MMDX stages too (detection stays by content).

## Validation
`ValidateStageManifest` (`asset/StageManifest.h`) is everything checkable from the JSON alone, and `tools/stage_check`
adds the file / model checks:

* identity: `id` shape, `name` present, unique model files / light names / socket names / camera names / screen ids /
  param keys / variant ids / group ids;
* references: groups, screens (from material rules and packs), sockets (from packs), cameras (from screens), params
  (from variants, bindings, cues), the default variant, pack indices;
* binding targets and type compatibility, binding `gain` params, cue track shape, `remap` ranges, drivers (see
  `drivers`);
* pack roles and their required fields, screen sources and their required fields, sun / ambient counts, cone angles;
* `stage_check` only: declared files exist, every model part loads (`LoadModelFile`), material names / globs in
  `materials` / `screens` / surface packs match a loaded material, previews exist, and a size budget report.

## Tools

```
python tools/stagegen/stagegen.py --out captures/stages    # the procedural test stages
python tools/stagegen/stagegen.py --out build/bin/stages   # install them next to the exe (<exe>/stages/<id>/)
build/bin/stage_check captures/stages/cyclorama           # errors -> exit code != 0
build/bin/stage_check --selftest                          # evaluator / validator unit checks
build/bin/gltf_pbr_test                                   # glTF PBR records, incl. tools/testdata/ext_test.glb (S2a / S2b)
```

`stagegen` writes GLB by hand (JSON chunk + BIN chunk, 4-byte padded; named nodes and materials, normals and UVs) with
numpy only — no Blender, no glTF library. The stages are `cyclorama` (cove, 3 colour variants, 7 lights with 4
spots), `mirror_room` (box room, `mirror: true` wall, wood floor PBR), `led_stage` (raised stage, 3 screen quads,
truss, 8 swaying spots, fog values), `pack_test` (one bundled pack of each role) and `gobo_test` (S7b: two spots with a
ring gobo straight down on the floor — one of them spinning a coloured stripe stencil — and a third with the stripe
stencil across the back wall, haze on; `gobos/ring.png` / `gobos/stripes.png` are written with the stage), plus
`sky_test` (S9b-1: an open floor with four pillars under one `sky` light whose `skyTimeOfDay` is keyed 6.5 -> 12 -> 18.5
over the first 30 s — a low orange dawn sun, a blue noon, a low sunset — and no fog).

`tools/testdata/make_ext_test.py` is the one Blender-made test asset (S2a / S2b): it writes `tools/testdata/ext_test.glb`,
a committed GLB with a transmissive / volume glass sphere, a thick tinted glass block, a thin-walled window pane, a
frosted (roughness 0.4) acrylic panel, an emissive-strength-15 panel, a clearcoat sphere, a texture-transformed checker
plane and one spot + one point light. `gltf_pbr_test` checks the imported records and `stage_check` the stage; no test
runs Blender.

## Decisions taken in S1a (open points of the plan, resolved here)

1. **`lights` schema**: the project's `editor.lights` entry verbatim, moved into `asset/SceneLightJson.*` so the
   project file and the stage share one implementation (`studio/SceneLight.cpp` moved to `mmdx_core`, which the asset
   layer links). `uid` is carried but unused in a stage; `name` becomes required.
2. **Material targets bind to a rule, not to a model material name** (`materials.<rule match>.<field>`): a stage's
   materials are addressed by the rule the author wrote, so one binding covers a whole glob, and the rule name is
   stable even when a Blender re-export renames a material. S1b applies the resolved rule values on top of the parsed
   rules.
3. **`remap` + `scale` + `offset`** are all optional and compose in that order (`remap` first) — one binding can
   therefore both clamp-map a normalised slider and scale it into a light's units.
4. **Enum → string targets** use the option's `value` (not its index); enum → number targets use the index. This makes
   `screens.<id>.source` drivable from a dropdown and keeps numeric targets interpolatable.
5. **Cues are param tracks only** (no per-light animation curves): lights are animated by binding light fields to
   params and cueing those params, which is the same shape the studio's timeline rows will use in S3.
6. **Sockets need a `node` or an explicit transform**; a socket with neither is an error (an anchor at the origin is
   `"position": [0, 0, 0]`).
7. **A sun light cannot carry `keys`** — the project format allows it, but a stage's light show is the cue layer; a
   keyed sun would be a second, hidden animation system.
8. **`materials` matches are case-insensitive globs**, first rule wins per field; the material / screen / pack-surface
   globs must match at least one loaded material (`stage_check` error, never a hard load failure).
9. **Invalid `stage.json` = not a stage**: the folder is scanned as before and a scan note explains why. Semantic
   errors (bad targets, missing files) do not hide a stage from the library — the stage still loads and
   `stage_check` reports the problems.
10. **Budgets**: `stage_check` reports the file size per extension and errors above 512 MB (a gallery zip is expected
    to stay under 256 MB).

## Distribution (S12: stage gallery)

A stage is distributed as a **zip of one stage folder**: `stage.json` at the archive root, or inside a single top
folder (`cyclorama/stage.json` — the shape "compress this folder" makes). Its `id` must match the gallery entry's id
(the store refuses a zip whose `stage.json` names another id) and `models` must list at least one part.

```
https://mmdx.codingbot.kr/stages/index.json
{
  "format": "mmdx-stages",           // informational
  "formatVersion": 1,
  "stages": [
    {"id": "cyclorama", "version": "1.0.0",
     "name": {"ko": "사이클로라마", "en": "Cyclorama"},
     "author": "…", "license": "CC0-1.0",
     "description": {"en": "…"}, "tags": ["dance", "studio"],
     "preview": "prev/cyclorama.png",   // relative to the index, or absolute
     "download": "https://cdn.example.com/cyclorama-1.0.0.zip",   // absolute; big zips never live on the home server
     "size": 30720, "sha256": "…64 hex…", "minEngine": "2.0.0"}
  ]
}
```

`download`, `sha256` and a valid `id` / `version` are required; an entry missing them is skipped. `revoked: true`
(with an optional `reason`, plain or localized) keeps the entry listed — the card shows the reason and installation is
disabled — and an installed revoked stage gets a warning badge but is **never auto-deleted**. `minEngine` is compared
with the app version (a newer requirement marks the entry incompatible).

The app's gallery (`Screen::Gallery`, `app/StageStore`) fetches the index, verifies the zip's size and SHA-256, then
installs it into `<exe>/stages/<id>/` (never into the user's library) and triggers a background library rescan, so the
stage appears in the lobby without a restart. Downloads are **resumable**: the transfer writes
`<exe>/cache/stage_downloads/<id>-<version>.zip.part` and a later attempt asks the server for `Range: bytes=<size>-`
(a `206` appends, a `200` restarts, `416` restarts); the `.part` is renamed onto the zip only when it is complete.
Install extracts into `<exe>/stages/.incoming/`, requires exactly one `stage.json` with `"format": "mmdx-stage"` and the
index's id, and renames the folder into place; an update keeps the previous folder until the new one is in place and
restores it when the rename fails. Uninstall deletes the folder (refused while a scene or the studio uses the stage).
`--gallery-index <url|path>` (with `--screen gallery`) points one run at another index; local paths / `file:` URLs work.

```
python tools/stagegen/stagegen.py --out build/bin/stages      # a stage to zip for the index
build/bin/stage_store_test                                    # index / install / resume unit checks
```
