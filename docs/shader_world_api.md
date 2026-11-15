# Shader Pack World Entities (World Packs)

A shader pack with `"type": "world"` defines a placed, keyable 3D world entity (such as falling snow, dust, rain, magic circles, grass, or ground cover) with an optional GPU simulation. World entities are placed and keyed in the studio and live play mode (`FrameView::worlds`), simulated via compute shaders, and rendered across all raster, ray-traced, path-traced, and offline GI render paths.

---

## 1. Folder Structure

```
my_world_pack/
  pack.json      manifest ("type": "world", "world": { ... })
  world.hlsl     entry points (simulate, spawn, update, height, shade)
  preview.png    optional preview image
  textures/      optional pack textures (up to 32 files, 64 MB total)
```

---

## 2. Manifest Schema (`pack.json`)

```json
{
  "name": "Falling Snow",
  "version": "1.0.0",
  "apiVersion": 1,
  "type": "world",
  "world": {
    "step": 0.0166667,
    "rewind": 4.0,
    "particles": [
      {
        "name": "snow_flakes",
        "count": 65536,
        "blend": "alpha",
        "simulate": "SimulateFlakes",
        "shade": "ShadeFlakes",
        "size": [0.05, 0.2]
      }
    ],
    "fields": [
      {
        "name": "snow_depth",
        "size": 512,
        "extent": 40.0,
        "format": "rgba16f",
        "persistent": true,
        "rate": 10.0,
        "update": "UpdateSnowField"
      }
    ],
    "surfaces": [
      {
        "field": "snow_depth",
        "grid": 128,
        "height": "SnowHeight",
        "shade": "SnowShade"
      }
    ]
  },
  "params": [
    {
      "key": "wind_speed",
      "label": "Wind Speed",
      "type": "float",
      "def": 1.5,
      "min": 0.0,
      "max": 10.0
    }
  ]
}
```

### Top-Level Manifest Properties
- `"type"`: Must be `"world"`. `classes` (material body/hair/face rules) are disallowed for world packs.
- `"world"`: The world entity specification (required).
- `"params"`: Extended parameter declaration. World packs support up to 128 typed parameters (`params.ext` capability is always enabled).
- `"textures"`: Up to 32 texture files (total uncompressed/compressed footprint up to 64 MB).
- `"requires"`: Optional array of capability names. The world capabilities are listed in §2.1; a pack that uses a
  manifest key of a capability it does not require is refused (the manifest problem names the capability).

### 2.1 World capabilities

`apiVersion` stays 6 for the world API: the new features are opt-in capabilities, declared in `"requires"`. A pack
without them renders exactly as before.

| Capability | What it enables | Manifest keys |
| --- | --- | --- |
| `world.camera` | The frame camera (`PackCameraPos`) and the camera-space particles | `particles[].space: "camera"`, `particles[].wrap` |
| `world.anchor` | The performer-anchored fields (the field follows the performers) | `fields[].anchor: "performers"` |
| `world.ground` | The ground height map and the camera-centred, ground-following surfaces | `surfaces[].extent: "camera"`, `surfaces[].radius`, `surfaces[].ground` |
| `world.overlay` | The overlay surfaces (a wet floor / decal over the lit scene) | `surfaces[].blend: "overlay"` |
| `world.body` | The body colliders (head, upper body, shoulders / arms, lower body) on top of the feet / hands | - |
| `world.lights` | The scene's sun and punctual direct lighting, with the scene's shadows (`PackSunLight`, `PackPunctualLight`, `PackDirectDiffuse`, `PackDirectThin`) | - |
| `inputs.audio` | The song's beat analysis (`PackAudioBeat`, `PackAudioDownbeat`, `PackAudioOnset`, `PackAudioEnergy`, `PackAudioSection`, `PackAudioBpm`, `PackAudioBeatCount`, `PackInputValid(PACK_IN_AUDIO)`) in every world function | - |

The world shaders are compiled with `PACK_CAP_WORLD_CAMERA`, `PACK_CAP_WORLD_ANCHOR`, `PACK_CAP_WORLD_GROUND`,
`PACK_CAP_WORLD_OVERLAY`, `PACK_CAP_WORLD_BODY`, `PACK_CAP_WORLD_LIGHTS` and `PACK_CAP_INPUTS_AUDIO` defined to `1`
when the pack requires the matching capability (`inputs.audio` needs no `"inputs"` list: requiring it is enough). The API functions and the struct fields below exist only under their macro, so a pack that does
not require a capability cannot use it. Packs without `world.body` see only the old (feet / toes / wrists) colliders,
in the old order and count.

### `"world"` Object Properties
- `"step"`: Fixed simulation step size in seconds. Allowed range: `1/240` (0.004167) to `1/15` (0.066667). Default: `1/60` (0.0166667).
- `"rewind"`: Duration of short-lived state re-simulation window in seconds. Allowed range: `0.0` to `20.0`. Default: `4.0`.
- `"particles"`: Array of up to 8 particle systems.
- `"fields"`: Array of up to 4 2D local-space simulation grids.
- `"surfaces"`: Array of up to 2 procedural heightfield meshes.

### Particle System Specification
Each particle system in `"particles"`:
- `"name"`: Unique identifier within the pack.
- `"count"`: Particle count between `1` and `262144` (256K).
- `"blend"`: `"alpha"` (premultiplied/interpolated alpha blend) or `"add"` (additive blend).
- `"simulate"`: Optional function name for stateful simulation (called once per fixed step). Mutually exclusive with `"spawn"`.
- `"spawn"`: Optional function name for stateless procedural evaluation ($f(id, t)$). Mutually exclusive with `"simulate"`.
- `"shade"`: Function name for shading particles in the pixel shader.
- `"size"`: 2-element array `[min, max]` specifying world-space particle radius hint for culling.
- `"space"`: `"local"` (default; today's behaviour: the particles live in the entity's world space) or `"camera"`
  (needs `"world.camera"`). A camera-space system is simulated in a local tile and drawn in a box of `"wrap"` world
  units around the camera, so the effect covers the whole map instead of a disc around the entity.
- `"wrap"`: The camera-space tile size in world units (default `120`, clamped to `1 .. 4096`). The pack keeps the
  particles inside the tile (`PackWrapTile`); the host wraps the drawn position into the box around the camera.
- `"wrapParam"` / `"wrapScale"` (optional): the tile size follows a float param instead (the instance's value, else
  the param's default, times `"wrapScale"`, default `1`), so a pack's range slider sizes the tile and the density
  stays even. Only with `"space": "camera"`.

### Field Specification
Each field in `"fields"`:
- `"name"`: Unique identifier within the pack.
- `"size"`: Square resolution in texels: `64` to `2048`.
- `"extent"`: World units across the XZ plane, centered at the entity origin ($[-extent/2, +extent/2]$).
- `"format"`: `"r16f"`, `"rg16f"`, or `"rgba16f"`.
- `"persistent"`: Boolean. If `true`, on timeline seeks this field is re-simulated from $t = 0$ at its specified `"rate"`. If `false`, it re-simulates over the entity's `"rewind"` window.
- `"rate"`: Steps per second for persistent field simulation (default: `10.0`). A persistent field always steps on
  its own grid, step k at time k / rate, both when re-simulating after a seek and during playback, so a seek and
  continuous playback reach the same field at the same time.
- `"update"`: Function name for compute shader ping-pong field updates.
- `"anchor"`: `"transform"` (default; the field is centred on the entity's origin) or `"performers"` (needs
  `"world.anchor"`). A performer-anchored field is centred on the mean xz of the foot colliders of the whole
  collider track, so it follows the performers. The centre is constant for the instance, so the re-simulation stays
  deterministic. `PackFieldInput.posLocal` and `PackSurfaceIn.posLocal` carry the local xz **including** the centre,
  so a pack that compares them with `PackWorldToLocal(collider.pos).xz` keeps working unchanged.

### Surface Specification
Each surface in `"surfaces"`:
- `"field"`: Name of the referenced field in `"fields"`.
- `"grid"`: Quads per side: `16` to `512` (generates `grid * grid * 2` triangles procedurally).
- `"height"`: Function name returning vertex local height $Y$.
- `"shade"`: Function name returning surface shading attributes (`PackSurfaceOut`).
- `"extent"`: `"field"` (default; one grid over the field's extent) or `"camera"` (needs `"world.ground"`). A
  camera-centred surface is drawn as 3 nested LOD levels around the camera (see §5.1).
- `"radius"`: The camera extent's half size in world units (default `400`, clamped to `10 .. 4096`): the grid covers a
  square of that half size around the camera.
- `"ground"`: `true` (needs `"world.ground"`): the height function returns the height **above** the ground. The host
  adds the ground height (`PackGroundHeight`) and clips the surface where there is no ground; the gradient normal
  includes the ground's slope.
- `"blend"`: `"opaque"` (default) or `"overlay"` (needs `"world.overlay"`). An overlay surface is drawn with
  premultiplied alpha over the already-lit scene colour: the depth test is on, the depth write is off, a small
  slope-scaled depth bias keeps a surface a few mm above the ground from z-fighting, and the normal / velocity
  targets keep the stage's own values. The output is `lit * alpha + emission` over `destination * (1 - alpha)`, where
  `lit` is the host's usual lighting of `sout.albedo` (sun + shadow + hemisphere + punctual). A pack darkens the
  floor with a dark albedo and `alpha` = wetness, and adds the highlights through the emission.

---

## 3. HLSL Contract (`world.hlsl` and `world_api.hlsli`)

All world shaders compile under Shader Model 6.0 (`cs_6_0`, `vs_6_0`, `ps_6_0`) using DXC. `world_api.hlsli` is automatically available.

### Data Structures

```hlsl
struct PackParticle {
    float3 pos;     // World-space position
    float size;     // World-space radius
    float3 vel;     // Units per second
    float age;      // Age in seconds; < 0 marks inactive particle
    float4 color;   // Base color and alpha
    float4 data;    // Custom user attributes
};

struct PackSimInput {
    uint id;            // Particle index (0 .. count - 1)
    float dt;           // Fixed step delta time in seconds
    float time;         // Target time in seconds
    float3 posLocal;    // Entity local-space position
};

struct PackFieldInput {
    float2 uv;          // Normalized UV in [0, 1]
    float2 posLocal;    // Local-space XZ coordinates in world units
    float4 prev;        // Previous texel value from ping-pong texture (zero on the first step of a re-simulation)
    float dt;           // Step delta time
    float time;         // Current simulation time
    uint2 texel;        // Integer texel coordinate
};

struct PackSurfaceIn {
    float2 uv;          // Grid UV in [0, 1]
    float2 posLocal;    // Local-space XZ coordinates
    float time;         // Current time
#if PACK_CAP_WORLD_GROUND
    float2 posWorldXZ;  // The vertex's world XZ
    float3 groundNormal;// The ground's normal under the vertex (up when there is no ground)
#endif
};

struct PackSurfaceOut {
    float3 albedo;      // Linear RGB albedo
    float alpha;        // Surface opacity (1.0 for opaque)
    float roughness;    // Material roughness [0, 1]
    float3 normal;      // World normal (host calculates gradient default)
    float3 emission;    // Emissive linear RGB
};

struct PackSurfaceShadeIn {
    float2 uv;          // Grid UV
    float3 posLocal;    // Local-space XYZ
    float3 posWorld;    // World-space XYZ
    float3 normal;      // Computed world-space gradient normal
    float3 viewDir;     // Normalized vector towards camera
    float time;         // Current time
#if PACK_CAP_WORLD_GROUND
    float2 posWorldXZ;  // The fragment's world XZ
    float3 groundNormal;// The ground's normal under the fragment (up when there is no ground)
#endif
};

struct PackParticleShadeIn {
    float2 uv;              // Quad billboard UV [0, 1]
    PackParticle particle;  // Particle state
    float3 viewDir;         // Normalized vector towards camera
    float3 posWorld;        // World position of the billboard vertex
};
```

### Function Signatures

1. **Stateful Particle Simulation**:
   ```hlsl
   void SimulateFlakes(inout PackParticle p, PackSimInput s);
   ```
   Particles start with `age = -1.0f`. `SimulateFlakes` is responsible for spawning and updating particles.

2. **Stateless Particle Spawn**:
   ```hlsl
   PackParticle SpawnFlakes(uint id, float t);
   ```
   Evaluates particle position and attributes procedurally as a function of index and time.

3. **Field Update**:
   ```hlsl
   float4 UpdateSnowField(PackFieldInput f);
   ```
   Computes the updated value for the texel in the field's ping-pong texture.

4. **Surface Height**:
   ```hlsl
   float SnowHeight(PackSurfaceIn i);
   ```
   Returns the local vertical displacement ($Y$) of the surface grid at $(X, Z)$.

5. **Surface Shading**:
   ```hlsl
   PackSurfaceOut SnowShade(PackSurfaceShadeIn i);
   ```
   Returns albedo, alpha, roughness, normal, and emission. The host automatically applies direct scene lighting (sun direction, shadow map visibility, sun color, and ambient light) so the surface matches the scene.

6. **Particle Shading**:
   ```hlsl
   float4 ShadeFlakes(PackParticleShadeIn i);
   ```
   Returns linear HDR color and alpha.

---

## 4. Built-in API Helpers

- **Parameters**: `PackParam(index)` (float), `PackParamInt(i)`, `PackParamBool(i)`, `PackParamColor(i)`, `PackParamVec3(i)`.
- **Transforms**:
  - `PackWorldToLocal(posWorld)` / `PackLocalToWorld(posLocal)` (points)
  - `PackWorldToLocalVec(vecWorld)` / `PackLocalToWorldVec(vecLocal)` (vectors)
- **Colliders**:
  - `PackColliderCount()`: Number of active scene colliders this frame (body colliders are included only for packs whose `requires` contains `world.body`).
  - `PackCollider(i)`: Returns `PackColliderData` with `pos` (float3), `radius` (float), `vel` (float3), and `flags` (uint). Flags indicate body parts: `kColliderFoot = 1`, `kColliderHand = 2`, `kColliderLeft = 4`, `kColliderBody = 8`.
- **Fields**: 
  - `PackFieldSample(fieldIndex, posLocalXZ)`: Bilinear sample of field `fieldIndex` (0 .. 3) at local XZ coordinates.
    With `"world.anchor"` the local XZ include the field's centre, so the sample is taken relative to it; outside the
    field's extent the sample is `float4(0,0,0,0)`.
  - `PackFieldLoad(fieldIndex, int2 texel)`: Point fetch of field texel.
  - `PackFieldCoverage(fieldIndex, posLocalXZ)` (`world.anchor`): 1 inside the field, fading smoothly to 0 over the
    outer 5 % of its extent, 0 outside it.
- **Camera** (`world.camera`):
  - `PackCameraPos()`: The frame camera's world eye position (the scene's `gEyePos`). During a seek re-simulation
    (many steps recorded in one frame) **every step sees the current frame's camera**: the camera is not part of the
    simulation state.
  - `PackWrapSize()`: The wrap size of the current particle system (0 for a local-space system).
  - `PackWrapCentre(size)`: The centre of the drawn box: the camera moved 40 % of the box ahead along its horizontal
    view direction, so the box spans from just behind the camera to 0.9 x size in front of it (what the camera sees).
  - `PackWrapTile(posLocal)`: Wraps a local position's XZ into the tile $[-W/2, +W/2]$ around the local origin.
  - `PackParticleWorld(posLocal)`: The world position the particle is **drawn** at this frame (the camera-space wrap
    applied), so a simulation can test it against the ground (`PackGroundHeight`) at the place the particle appears.
- **Ground** (`world.ground`):
  - `PackGround(xzWorld, out height, out normal)`: The ground at a world XZ: `false` when the point is outside the map
    or no surface was drawn there, else the top-most surface's height and its normal (from the height map's central
    differences, in world space).
  - `PackGroundHeight(xzWorld)`: The ground height at a world XZ, or `-1e9` when there is no ground there.
- **Pack Textures**:
  - `PackSampleTex(i, uv)` / `PackSampleTexLevel(i, uv, lod)`: Sample pack texture `i` (0 .. 31).
  - `PackTexSize(i)` / `PackTexCount()`.
- **Lighting**:
  - `PackSunDir()`: Vector pointing toward the sun.
  - `PackSunColor()`: Sun light color and intensity.
  - `PackAmbient()`: Ambient light color.
  - `PackHemiAmbient(normal)`: the scene's sky / ground hemisphere light for a world-space normal (ambient that
    follows the stage lighting; `PackAmbient()` is only the ground colour).
  - `PackSunShadow(worldPos)`: Sun shadow visibility factor in [0, 1], from the scene's cascaded shadow map in
    raster camera views and from a ray query against the scene TLAS in ray-traced / path-traced frames and offline GI;
    1 where no shadow covers the point.
  - `PackSunLight(posWorld)`, `PackPunctualCount()`, `PackPunctualLight(i, posWorld)` (`world.lights`): The scene's
    direct lights as `PackLightSample { L, radiance }`, where `radiance` already includes colour, intensity, distance
    falloff, spot cone and visibility (see §6.5).
  - `PackDirectDiffuse(pos, n)`, `PackDirectThin(pos, n, viewDir, transmission)` (`world.lights`): Lambert and
    two-sided thin-sheet direct lighting from the sun and every punctual light (see §6.5).
- **Audio** (`inputs.audio`, `shaders/pack_inputs.hlsli`; the same values surface / effect packs read, from the
  scene constants every world entry point binds — simulate, spawn, field update, surface and particle VS / PS):
  - `PackInputValid(PACK_IN_AUDIO)`: `true` while the song's analysis is ready (all values are 0 otherwise: no audio,
    analysis still running or failed).
  - `PackAudioBeat()` / `PackAudioDownbeat()`: the phase 0..1 within the current beat / bar.
  - `PackAudioBeatCount()`: the continuous beat count (beat index + phase; negative before the first beat, keeps
    counting after the last one) — use it for chases and cycles that span several beats.
  - `PackAudioOnset()`: the latest onset's strength decaying with a 0.25 s time constant. `PackAudioEnergy()`: the
    current beat's normalised RMS 0..1. `PackAudioSection()`: 0 low / 0.5 mid / 1 high. `PackAudioBpm()`.
  - In a seek re-simulation every step sees the **current frame's** audio values (like the camera): they are not part
    of the simulation state. Stateless `spawn` systems are exact; a stateful `simulate` that integrates audio over time
    is exact only during playback.
- **Particle billboards** (no capability): `PackBillboardRight()` / `PackBillboardUp()`: the unit world axes the host
  expands a particle quad along — the frame camera's right / up (every quad is parallel to the view plane; it does not
  turn toward the eye per particle). In the particle shade function the quad point is
  `posWorld = drawnPos + (uv.x - 0.5) * size * right + (uv.y - 0.5) * size * up`: `size` (`particle.size`) is the
  quad's **edge length**, `uv (0, 0)` is the bottom-left corner and `uv.y` grows **up**. In the shade function
  `i.posWorld` (and `i.particle.pos`) is the shaded point on the quad, not its centre, and `i.particle.vel` is 0: the
  point in quad space is `q = i.uv - 0.5` (x along right, y along up), and a world direction `d` (a velocity the pack
  keeps in `particle.data`, a beam axis) lands in that space as `normalize(float2(dot(d, right), dot(d, up)))`, so a
  shape can be oriented by rotating `q` (a streak, a star, a beam). The centre is
  `i.posWorld - q.x * size * right - q.y * size * up`. Offline GI uses the same axes (the frame camera's; the per-iteration lens / shutter
  camera only projects).
- **Deterministic Hashing**:
  - `PackRandom(seed)` / `PackRandom(seed, salt)` / `PackRandom(seed, salt1, salt2)`: PCG-based deterministic pseudo-random float in [0, 1).
  - `PackRandom2(seed, salt)` / `PackRandom3(seed, salt)`.

---

## 5. Determinism & Seek Rewind

World pack simulations run in fixed time increments `step` and are completely deterministic across live viewport playback, scrubbing, offline rendering, and video export:

1. **Forward Live Playback**:
   When time progresses normally ($0 \le \Delta t \le 0.25\text{ s}$), the engine steps forward by fixed `step` intervals up to the target time.
   A change of the instance's parameters while the target time is not advancing (paused, scrubbed, a repeated frame, or `inst.exact`) re-simulates exactly like a seek, so slider edits show at once; during live playback a keyed parameter change does not force a re-simulation.
   The simulation runs on one global grid: every stateful step ends at $k \cdot \text{step}$ ($k$ from timeline 0, no partial steps), and `step` is the simulation rate only — drawing is interpolated between the last two grid steps (a particle at $\text{lerp}(prevPos, pos, a)$ with $a = 1 - (simTime - time)/\text{step}$), so 120–240 fps playback no longer shows 60 Hz stairs.
2. **Timeline Seek & Re-simulation**:
   When jumping backwards or forward by $> 0.25\text{ s}$ (or when `inst.exact` is true):
   - **Persistent fields** (e.g., ground deformation tracks) re-simulate from $t = 0$ to $t_{target}$ at their declared `"rate"` (default 10 Hz).
   - **Particles and transient fields** reset and re-simulate from $\max(0, t_{target} - \text{rewind})$ to $t_{target}$ in fixed `step` increments.
   - Character interaction colliders are sampled strictly from the precomputed `WorldColliderTrack` using `track->Sample(t)`, ensuring identical state regardless of playback speed or frame rate.

---

## 6. The World Capabilities in Detail

### 6.1 Camera-space particles (`world.camera`)

A particle system with `"space": "camera"` simulates its particles in a **local tile**: the XZ of the local position
stay inside $[-W/2, +W/2]$ around the local origin (`W` = `"wrap"`), the Y is the world height. The pack keeps the
particles inside the tile itself (`PackWrapTile`).

The **host** maps every particle into the box of size `W` around `c = PackWrapCentre(W)` (the camera, shifted 0.4 W
ahead along its horizontal view direction) when it draws it:

```
x' = c.x + (frac((x - c.x) / W + 0.5) - 0.5) * W      (the same for z; y is unchanged)
```

Simulate in world-fixed tile coordinates (integrate the motion in the tile, wrap with `PackWrapTile`, spawn uniformly
over the whole tile) and let the host move the box. Do not re-anchor particles to the camera each step: they would
move with the camera (no parallax, rain "on the lens"). Ground layers that should cover the same area can centre their
falloff on `PackWrapCentre(W)` too.

The wrap happens in world space, after the pack's local -> world transform. The previous drawn position (the motion
vectors) is the interpolation of (pos, prevPos) wrapped the same way, so a wrap never leaves a velocity streak: the
particle's screen motion is its own motion, not the jump of the wrap.

### 6.2 The ground height map (`world.ground`)

When any enabled world instance requires `world.ground`, the host keeps a **ground map**: a top-down orthographic
depth render (the camera looks straight down, -Y) of the **stage and prop models only** (never the performers):

- R32_FLOAT 2048 x 2048 (a depth texture read as R32F), covering the union of the stage models' world AABB in XZ,
  clamped to 2048 x 2048 world units around its centre (the map is square). The Y range comes from the same AABB.
- Back faces are culled, so a sky dome (its triangles face inward) is not a roof.
- Materials that are invisible (diffuse alpha < 0.5), additive (`MatFlag_Additive`) or without a depth write
  (`MatFlag_NoDepthWrite`) are skipped; alpha-tested materials use the shadow pass's alpha-test path, so their
  cut-out texels do not become ground.
- The map is re-rendered when the set of stage / prop models changes or their pose changes (a stage motion, a studio
  placement), at most once per frame.
- With **no** stage model (a studio floor) the ground is the plane $y = 0$ everywhere (no render needed).

`PackGround` / `PackGroundHeight` read the map: the height comes from the map's depth, the normal from the height
map's central differences in world space.

### 6.3 The camera-centred surface grid (`world.ground`)

A surface with `"extent": "camera"` is drawn as **3 nested LOD levels**, each `grid` x `grid` quads:

| Level | Cell size | Covered half size (around the camera) |
| --- | --- | --- |
| 0 (coarse) | `2R / grid` | `R` |
| 1 (middle) | `(2R / grid) / 4` | `R / 4` |
| 2 (fine) | `(2R / grid) / 16` | `R / 16` |

`R` is `"radius"`. The cells are snapped to their level's spacing (the grid is anchored to the world, not to the
camera), so the surface does not swim when the camera moves, and a level skips the cells the next finer level covers,
so no region is drawn twice (no z-fighting). The levels tile exactly only with a grid that is a multiple of 8, so the
host rounds the grid down to one.

With `"ground": true` the pack's height function returns the height **above** the ground: the host adds
`PackGroundHeight(world xz)` to the world Y and clips the surface where there is no ground (the fragment is
discarded), and the gradient normal includes the ground's slope.

### 6.4 Overlay surfaces (`world.overlay`)

See the surface specification in §2: premultiplied alpha over the already-lit scene colour, the depth test on, the
depth write off, a slope-scaled depth bias towards the camera (standard Z: negative), and the normal / velocity targets
untouched (independent blend). The host lights the overlay like an opaque surface (sun + shadow, hemisphere, the
scene's spots / points): output = `lit * alpha + emission`.

Keep an overlay a couple of centimetres above the ground (e.g. `0.02`): `PackGroundHeight` is conservative (the
top-most surface of the 3 x 3 ground texels around the point, ~0.85 world units each on a large stage), but a layer of
a few millimetres still sinks under a sloped floor at grazing angles.

### 6.5 Scene direct lighting (`world.lights`)

A pack that requires `world.lights` can light its particles and surfaces with the same sun and punctual lights as the
lit scene. The capability adds `PackLightSample` and the sampling / shading helpers listed in §4; a pack without it
compiles and renders exactly as before (the sprite / surface lighting stays the old fixed sun + hemisphere, and the
particle pipeline keeps its old root signature and bindings).

```hlsl
struct PackLightSample {
    float3 L;         // unit vector toward the light
    float3 radiance;  // colour x intensity x falloff x spot cone x visibility (0 when the light does not reach)
};
```

- `PackSunLight(posWorld)` returns the scene's sun, or radiance 0 when the scene has no sun. `L` is `PackSunDir()`.
- `PackPunctualCount()` / `PackPunctualLight(i, posWorld)` return the scene's spots and points (the same lights, range
  falloff and area / spot terms the stage floor uses). The index order is the scene's light order, up to 256 lights
  (`Renderer::kMaxPunctualLights`; S7a).
- `PackDirectDiffuse(pos, n)` sums `radiance * saturate(dot(n, L))` over the sun and every punctual light.
- `PackDirectThin(pos, n, viewDir, transmission)` is for two-sided sheets (leaves, petals, paper, fabric). It flips
  `n` to face the viewer, then adds each light's front term plus diffuse transmission from behind:
  `radiance * (saturate(dot(n, L)) + saturate(transmission) * saturate(dot(-n, L)))`. The weight stays in [0, 1], so
  the helper never amplifies a light; `viewDir` points from the surface toward the viewer.

Visibility uses the scene's real shadows: the cascaded sun shadow map and the spot / point shadow maps in raster
frames, and one short ray query per light per pixel against the scene TLAS in ray-traced, path-traced and offline GI
frames. The API is a sample, not an emitter: a world pack does not cast shadows into the scene in v1, so it can be
lit by the scene without changing the scene's own shadowing.

A typical thin-sheet pack:

```hlsl
float4 ShadeLeaf(PackParticleShadeIn i) {
    float3 n = float3(0, 0, 1);  // the sheet's normal, in world space
    float3 direct = PackDirectThin(i.posWorld, n, i.viewDir, float3(0.65, 0.65, 0.65));
    float3 ambient = PackHemiAmbient(n) * 0.5;
    return float4(i.particle.color.rgb * (direct + ambient), 1.0);
}
```

---

## 7. Rendering Pipeline Integration

- **Raster and Ray-Traced Real-Time Views**:
  - `WorldPass` executes between `ScenePass` and `ResolvePass`.
  - Surfaces render as opaque geometry with depth write and motion vectors into MSAA scene targets (`colorMsaa`, `normalMsaa`, `velocityMsaa`, `depthMsaa`).
  - Particles render after opaque geometry with depth testing (`LESS_EQUAL`) and no depth write. Alpha and additive blending are supported. Camera-facing motion vectors are computed from $(pos - vel \cdot dt)$.
  - `ResolvePass` automatically resolves world geometry and motion vectors, ensuring full compatibility with TAA and hardware upscalers (DLSS, FSR, XeSS).
- **Path-Traced & Offline GI Views**:
  - Surfaces and particles are rendered directly into the edge/outline layer targets (`DrawPtEdges` and `OfflineRenderer::Impl::DrawEdges`), occluded by the tracer's depth prepass, and composited over the denoised radiance.
  - World surfaces in PT/GI receive direct lighting matching the scene lighting.
- **Zero-Cost Rule**:
  - When `FrameView::worlds` is empty, `WorldPass` executes zero draw calls, dispatches zero compute passes, allocates zero GPU resources, and produces an identical byte-for-byte frame.
- **Exemptions**:
  - Unlit, wireframe, and extra quad views skip world simulation and rendering.
  - Shadow casting by world particles/surfaces is not enabled in v1.

---

## 8. Verification

Validate world packs using the CLI tool:
```cmd
build_dev\bin\pack_check.exe <path_to_pack> --compile
```
This parses `pack.json`, enforces all limits, and compiles every declared entry point (`simulate`, `spawn`, `update`, `height`, `shade`) with the pack's capability macros, as the app compiles them.
