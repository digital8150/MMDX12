// world_api.hlsli: HLSL contract and helper API for MMDX12 World Packs (type: world).
// See docs/shader_world_api.md.
#ifndef WORLD_API_HLSLI
#define WORLD_API_HLSLI

// Particle representation (world space, 64 bytes)
struct PackParticle {
    float3 pos;
    float size;
    float3 vel;
    float age;
    float4 color;
    float4 data;
};

// Simulation step input for stateful particles
struct PackSimInput {
    uint id;
    float dt;
    float time;
    float3 posLocal;
};

// 2D field update input (per texel per field step, ping-pong)
struct PackFieldInput {
    float2 uv;
    float2 posLocal;   // xz in world units centred on origin
    float4 prev;
    float dt;
    float time;
    uint2 texel;
};

// Surface height evaluation input
struct PackSurfaceIn {
    float2 uv;
    float2 posLocal;   // xz in world units
    float time;
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    float2 posWorldXZ; // the vertex's world xz
    float3 groundNormal; // the ground's normal under the vertex (up when there is no ground)
#endif
};

// Surface shading output
struct PackSurfaceOut {
    float3 albedo;
    float alpha;
    float roughness;
    float3 normal;     // default from height gradient
    float3 emission;
};

// Surface shading input
struct PackSurfaceShadeIn {
    float2 uv;
    float3 posLocal;
    float3 posWorld;
    float3 normal;     // gradient normal in world space
    float3 viewDir;
    float time;
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    float2 posWorldXZ; // the fragment's world xz
    float3 groundNormal; // the ground's normal under the fragment (up when there is no ground)
#endif
};

// Particle shading input
struct PackParticleShadeIn {
    float2 uv;
    PackParticle particle;
    float3 viewDir;
    float3 posWorld;
};

// World constant buffer
cbuffer WorldCB : register(b1) {
    float4x4 gWorldToLocal;
    float4x4 gLocalToWorld;
    float4 gWorldTime;      // x: time, y: dt, z: simTime, w: stepIndex
    float4 gWorldParams;    // x: colliderCount, y: fieldCount, z: particleSystemIndex, w: particleCount
    float4 gSunDir;         // xyz: sunDir, w: hasSun
    float4 gSunColor;       // rgb: sunColor, w: shadowDistance
    float4 gAmbient;        // rgb: ambientColor, w: 0
    float4 gFieldExtent[4]; // xy: size, z: extent, w: 1/extent
    // x: the current particle system's wrap size (0 = a local-space system), y: 1 = the particles live in camera
    // space, z,w: the field's anchor centre (local xz, 0 without the "world.anchor" capability)
    float4 gWorldExtra;
    // The ground height map (the host's top-down depth render of the stage / props):
    // x: the map's size in texels (0 = no map: the flat plane at gGroundRect.y), y: 1/size, z,w: the map's origin (world xz)
    float4 gGroundMap;
    // x: the map's half extent (world units), y: the map's top y, z: the y range (top - bottom), w: 1 = a ground exists
    float4 gGroundRect;
    // The surface being drawn: x: 0 = the field's extent, 1 = camera-centred, y: the camera radius (world units),
    // z: 1 = the pack's height is above the ground, w: unused
    float4 gSurfaceParams;
    // Offline GI: the iteration's lens / shutter view-projection (gOfflineParams.x = 1 when set)
    float4x4 gOfflineViewProj;
    float4 gOfflineParams;
    // The drawn particles: x: the interpolation factor a = 1 - (simTime - time) / stepDt, y: the frame delta
    // (scene elapsed, 0 when paused), z: stepDt, w: the teleport bound (0.5 x the camera-space wrap size; 0 = none)
    float4 gParticleParams;
};

// The clip position of a world point: the offline iteration's camera (DoF / motion blur) or the frame camera.
inline float4 WorldClip(float3 posWorld) {
    return mul(float4(posWorld, 1.0), gOfflineParams.x > 0.5 ? gOfflineViewProj : gViewProj);
}

#define gTime (gWorldTime.x)

// The audio inputs ("requires": ["inputs.audio"], PACK_CAP_INPUTS_AUDIO): PackAudioBeat / Downbeat / Onset / Energy /
// Section / Bpm / BeatCount, PackInputValid(PACK_IN_AUDIO). They read gPackAudio0 / 1 of the scene constants
// (common.hlsli, b0), which every world entry point binds. A seek re-simulation sees the current frame's values in
// every step (like the camera).
#include "pack_inputs.hlsli"

// ---- particle billboards --------------------------------------------------------------------
// The unit world axes VSWorldParticle expands a particle's quad along: the frame camera's right / up (the rows of
// gInvView; every quad faces the view plane, not the eye). A quad point is
// drawn pos + (uv.x - 0.5) * size * right + (uv.y - 0.5) * size * up: size is the quad's edge length and uv.y grows
// UP (uv (0, 0) = bottom left, (1, 1) = top right). In the shade function posWorld is the shaded quad point, so the
// quad-space point is uv - 0.5; project a world direction onto (right, up) to orient a shape.
inline float3 PackBillboardRight() { return normalize(gInvView[0].xyz); }
inline float3 PackBillboardUp() { return normalize(gInvView[1].xyz); }

// Extended parameter buffer (128 slots float4)
cbuffer PackParamsExt : register(b4) {
    float4 gPackParamSlots[128];
};

#define PackParam(i) (gPackParamSlots[(i) & 127u].x)
inline int PackParamInt(uint i) { return (int)round(gPackParamSlots[i & 127u].x); }
inline bool PackParamBool(uint i) { return (gPackParamSlots[i & 127u].x > 0.5); }
inline float3 PackParamColor(uint i) { return gPackParamSlots[i & 127u].rgb; }
inline float4 PackParamColor4(uint i) { return gPackParamSlots[i & 127u]; }
inline float3 PackParamVec3(uint i) { return gPackParamSlots[i & 127u].xyz; }
inline uint PackParamCount() { return 128u; }

// Colliders
struct PackColliderData {
    float3 pos;
    float radius;
    float3 vel;
    uint flags;
};

StructuredBuffer<PackColliderData> gColliders : register(t10, space0);
inline uint PackColliderCount() { return (uint)gWorldParams.x; }
inline PackColliderData PackCollider(uint i) { return gColliders[i]; }

// Transforms
inline float3 PackWorldToLocal(float3 posWorld) {
    return mul(float4(posWorld, 1.0), gWorldToLocal).xyz;
}

inline float3 PackLocalToWorld(float3 posLocal) {
    return mul(float4(posLocal, 1.0), gLocalToWorld).xyz;
}

inline float3 PackWorldToLocalVec(float3 vecWorld) {
    return mul(float4(vecWorld, 0.0), gWorldToLocal).xyz;
}

inline float3 PackLocalToWorldVec(float3 vecLocal) {
    return mul(float4(vecLocal, 0.0), gLocalToWorld).xyz;
}

// ---- camera space (PACK_CAP_WORLD_CAMERA) ------------------------------------------------
// The frame camera's world eye position. It is the scene's gEyePos, so it is the camera of the frame being
// rendered (the render camera in an offline render). During a seek re-simulation (many steps recorded in one
// frame) every step sees the CURRENT frame's camera: the camera is not part of the simulation state.
#if defined(PACK_CAP_WORLD_CAMERA) && PACK_CAP_WORLD_CAMERA
inline float3 PackCameraPos() { return gEyePos.xyz; }

// The centre of the camera-space box of `size` world units: ahead of the camera along its horizontal view direction
// (40 % of the box), so the box covers what the camera sees (from just behind it to 0.9 x size ahead) rather than
// mostly the space behind and beside it.
inline float3 PackWrapCentre(float size) {
    float3 f = float3(gView._13, gView._23, gView._33);   // the view's forward axis in world space (row vectors)
    float2 fh = f.xz;
    float l = length(fh);
    fh = l > 1e-3 ? fh / l : float2(0.0, 0.0);              // looking straight down: no shift
    float3 c = PackCameraPos();
    return float3(c.x + fh.x * size * 0.4, c.y, c.z + fh.y * size * 0.4);
}

// The wrap size of the current particle system (0 for a local-space system): the edge length of the tile the
// particles live in, in world units.
inline float PackWrapSize() { return gWorldExtra.x; }

// Wraps a local position's xz into the tile [-W/2, W/2] around the local origin (the camera-space tile).
inline float3 PackWrapTile(float3 posLocal) {
    float size = PackWrapSize();
    if (size <= 0.0) return posLocal;
    float2 tile = frac(posLocal.xz / size + 0.5) - 0.5;
    return float3(tile.x * size, posLocal.y, tile.y * size);
}

// The world position a particle is DRAWN at this frame (the camera-space wrap applied to the local position):
// a simulation can test it against the ground (PackGroundHeight) at the place the particle will appear.
inline float3 PackParticleWorld(float3 posLocal) {
    float3 w = PackLocalToWorld(posLocal);
    float size = PackWrapSize();
    if (size > 0.0 && gWorldExtra.y > 0.5) {
        float3 cam = PackWrapCentre(size);
        w.x = cam.x + (frac((w.x - cam.x) / size + 0.5) - 0.5) * size;
        w.z = cam.z + (frac((w.z - cam.z) / size + 0.5) - 0.5) * size;
    }
    return w;
}
#endif // PACK_CAP_WORLD_CAMERA

// Lighting helpers
inline float3 PackSunDir() { return gSunDir.xyz; }
inline float3 PackSunColor() { return gSunColor.rgb; }
inline float3 PackAmbient() { return gAmbient.rgb; }
// The scene's sky / ground hemisphere light for a world-space normal (the same terms the lit scene shader uses):
// use it for ambient on particles and surfaces so they follow the stage lighting (a bright day vs a dark hall).
inline float3 PackHemiAmbient(float3 n) {
    return lerp(gGroundColor, gSkyZenith, saturate(n.y * 0.5 + 0.5)) * gHemiStrength;
}

// Forward declaration of PackSunShadow, implemented by host
float PackSunShadow(float3 worldPos);

// ---- the scene's direct lighting (PACK_CAP_WORLD_LIGHTS) ----------------------------------
// A general light sample: L = the unit vector toward the light, radiance = colour x intensity x the light's
// distance falloff x the spot cone x the light's visibility at that point (0 when the light does not reach).
// Shadows come from the cascaded / spot / point maps in raster frames and from a ray query against the scene TLAS
// in RT / PT / offline GI frames.
#if defined(PACK_CAP_WORLD_LIGHTS) && PACK_CAP_WORLD_LIGHTS
struct PackLightSample {
    float3 L;
    float3 radiance;
};

// The sun: radiance 0 when the scene has no sun. L is PackSunDir().
PackLightSample PackSunLight(float3 posWorld);
// The scene's punctual lights (spots / points), the same lights, falloff and shadows the stage floor and the world
// surfaces use.
uint PackPunctualCount();
PackLightSample PackPunctualLight(uint i, float3 posWorld);

// Lambert direct light (sun + punctual): radiance x saturate(dot(n, L)) per light, summed.
inline float3 PackDirectDiffuse(float3 pos, float3 n) {
    float3 sum = 0;
    PackLightSample sun = PackSunLight(pos);
    sum += sun.radiance * saturate(dot(n, sun.L));
    uint count = PackPunctualCount();
    for (uint i = 0; i < count; ++i) {
        PackLightSample s = PackPunctualLight(i, pos);
        sum += s.radiance * saturate(dot(n, s.L));
    }
    return sum;
}

// Two-sided thin-sheet direct light (leaves, paper, fabric, petals, snow flakes): the normal is first flipped to face
// the viewer, then each light contributes its front Lambert term plus diffuse transmission of the light arriving from
// behind: radiance x (saturate(dot(n, L)) + saturate(transmission) x saturate(dot(-n, L))). `transmission` is the
// sheet's diffuse transmission (0 = opaque, 1 = as much behind-light as front-light); the per-light weight stays
// within [0, 1], so the helper never amplifies a light. viewDir points from the surface toward the viewer.
inline float3 PackDirectThin(float3 pos, float3 n, float3 viewDir, float3 transmission) {
    if (dot(n, viewDir) < 0.0) n = -n;
    float3 t = saturate(transmission);
    float3 sum = 0;
    PackLightSample sun = PackSunLight(pos);
    sum += sun.radiance * (saturate(dot(n, sun.L)) + t * saturate(dot(-n, sun.L)));
    uint count = PackPunctualCount();
    for (uint i = 0; i < count; ++i) {
        PackLightSample s = PackPunctualLight(i, pos);
        sum += s.radiance * (saturate(dot(n, s.L)) + t * saturate(dot(-n, s.L)));
    }
    return sum;
}
#endif // PACK_CAP_WORLD_LIGHTS

// Samplers
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gWrapLinear : register(s2);

// Pack textures (up to 32)
#ifndef PACK_TEX_COUNT
#define PACK_TEX_COUNT 0
#endif
#ifndef PACK_TEX_CLAMP_MASK
#define PACK_TEX_CLAMP_MASK 0u
#endif
#ifndef PACK_TEX_SRGB_MASK
#define PACK_TEX_SRGB_MASK 0u
#endif

Texture2D gPackTex[32] : register(t0, space5);

inline float4 PackSampleTex(uint i, float2 uv) {
    if (i >= 32u || i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 31u;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) return gPackTex[i].SampleLevel(gLinear, uv, 0);
    return gPackTex[i].SampleLevel(gWrapLinear, uv, 0);
}

inline float4 PackSampleTexLevel(uint i, float2 uv, float lod) {
    if (i >= 32u || i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 31u;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) return gPackTex[i].SampleLevel(gLinear, uv, lod);
    return gPackTex[i].SampleLevel(gWrapLinear, uv, lod);
}

inline uint2 PackTexSize(uint i) {
    if (i >= 32u || i >= (uint)PACK_TEX_COUNT) return uint2(0, 0);
    uint2 s;
    gPackTex[i & 31u].GetDimensions(s.x, s.y);
    return s;
}

inline uint PackTexCount() { return (uint)PACK_TEX_COUNT; }

// Fields (up to 4 2D textures)
Texture2D<float4> gPackFields[4] : register(t0, space4);

// ---- performer-anchored fields (PACK_CAP_WORLD_ANCHOR) -----------------------------------
#if defined(PACK_CAP_WORLD_ANCHOR) && PACK_CAP_WORLD_ANCHOR
// The field's centre in local xz (the anchor): the performers' mean xz, else the origin.
inline float2 PackFieldCentre() { return gWorldExtra.zw; }
#endif // PACK_CAP_WORLD_ANCHOR

inline float4 PackFieldSample(uint fieldIndex, float2 posLocalXZ) {
    if (fieldIndex >= 4u) return 0.0;
    float extent = gFieldExtent[fieldIndex].z;
    if (extent <= 0.0) return 0.0;
    // The local xz include the field's centre (the anchor), so the sample position is relative to it.
#if defined(PACK_CAP_WORLD_ANCHOR) && PACK_CAP_WORLD_ANCHOR
    float2 uv = ((posLocalXZ - PackFieldCentre()) / extent) + 0.5;
#else
    float2 uv = (posLocalXZ / extent) + 0.5;
#endif
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    return gPackFields[fieldIndex].SampleLevel(gLinear, uv, 0);
}

inline float4 PackFieldLoad(uint fieldIndex, int2 texel) {
    if (fieldIndex >= 4u) return 0.0;
    return gPackFields[fieldIndex].Load(int3(texel, 0));
}

#if defined(PACK_CAP_WORLD_ANCHOR) && PACK_CAP_WORLD_ANCHOR
// The field's coverage at a local xz: 1 inside the field, fading smoothly to 0 over the outer 5 % of its extent,
// 0 outside it.
inline float PackFieldCoverage(uint fieldIndex, float2 posLocalXZ) {
    if (fieldIndex >= 4u) return 0.0;
    float extent = gFieldExtent[fieldIndex].z;
    if (extent <= 0.0) return 0.0;
    float fade = extent * 0.05;
    float2 q = abs(posLocalXZ - PackFieldCentre()) - (extent * 0.5 - fade);
    return saturate(1.0 - max(q.x, q.y) / fade);
}
#endif // PACK_CAP_WORLD_ANCHOR

// ---- the ground (PACK_CAP_WORLD_GROUND) --------------------------------------------------
// The host's ground height map: a top-down orthographic depth render of the stage / prop models (the top-most
// surface wins), read as R32_FLOAT. The map covers a square of 2 * gGroundRect.x world units around
// (gGroundMap.z, gGroundMap.w); the depth maps to the world height gGroundRect.y - depth * gGroundRect.z.
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
Texture2D<float> gGroundHeightMap : register(t19, space0);

// One ground texel's height (-1e9 when no surface was drawn there).
float PackGroundTexel(int2 texel) {
    int2 size = int2(gGroundMap.x, gGroundMap.x);
    float depth = gGroundHeightMap.Load(int3(clamp(texel, int2(0, 0), size - int2(1, 1)), 0));
    return depth >= 1.0 - 1e-5 ? -1e9 : gGroundRect.y - depth * gGroundRect.z;
}

// The ground height at a map uv (0..1): -1e9 when no surface was drawn there. Bilinear between the 4 texels around
// (exact on a sloped floor, so a thin layer does not sink between texel centres); next to a texel with no surface the
// nearest texel is used.
float PackGroundHeightAtUv(float2 uv) {
    if (gGroundRect.w < 0.5) return -1e9;
    if (gGroundMap.x <= 0.0) return gGroundRect.y;    // no stage model: the flat plane
    if (any(uv < 0.0) || any(uv > 1.0)) return -1e9;
    float size = gGroundMap.x;
    float2 t = uv * size - 0.5;
    int2 t0 = (int2)floor(t);
    float2 f = t - floor(t);
    float h00 = PackGroundTexel(t0), h10 = PackGroundTexel(t0 + int2(1, 0));
    float h01 = PackGroundTexel(t0 + int2(0, 1)), h11 = PackGroundTexel(t0 + int2(1, 1));
    if (min(min(h00, h10), min(h01, h11)) <= -1e8)
        return PackGroundTexel(clamp((int2)(uv * size), int2(0, 0), int2(size - 1, size - 1)));
    return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

// The ground's height spread (max - min) over the 3 x 3 texels around a map uv: large at a thin, tall feature (a
// railing, a post, a step's edge), small on a floor; 1e9 when a texel has no surface.
float PackGroundRangeAtUv(float2 uv) {
    int2 size = int2(gGroundMap.x, gGroundMap.x);
    int2 c = clamp((int2)(uv * (float)size), int2(0, 0), size - int2(1, 1));
    float lo = 1e9, hi = -1e9;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float h = PackGroundTexel(c + int2(x, y));
            if (h <= -1e8) return 1e9;
            lo = min(lo, h);
            hi = max(hi, h);
        }
    return hi - lo;
}

// World xz -> ground map uv. The map's camera looks down -Y with +Z up, so world +z is NDC +y = texture v DOWN.
inline float2 PackGroundUv(float2 xzWorld) {
    float2 d = (xzWorld - gGroundMap.zw) / (2.0 * gGroundRect.x);
    return float2(d.x + 0.5, 0.5 - d.y);
}

// The ground height at a world xz (-1e9 when there is no ground there).
inline float PackGroundHeight(float2 xzWorld) {
    if (gGroundRect.w < 0.5) return -1e9;
    if (gGroundMap.x <= 0.0) return gGroundRect.y;
    float2 uv = PackGroundUv(xzWorld);
    return PackGroundHeightAtUv(uv);
}

// The ground's height spread around a world xz (PackGroundRangeAtUv): 0 on the flat studio floor.
inline float PackGroundRange(float2 xzWorld) {
    if (gGroundRect.w < 0.5 || gGroundMap.x <= 0.0) return 0.0;
    return PackGroundRangeAtUv(PackGroundUv(xzWorld));
}

// The spread above which the ground counts as a thin / tall feature that a ground layer leaves out (a railing, a
// post): at least 0.5 world units, else 1.2 texels (a 40 degree slope over the 3 x 3 window stays below it).
inline float PackGroundStepLimit() {
    return max(0.5, 1.2 * 2.0 * gGroundRect.x / max(gGroundMap.x, 1.0));
}

// The ground at a world xz: false when the point is outside the map or no surface was drawn there,
// else the top-most surface's height and its normal (from the height map's central differences, in world space).
inline bool PackGround(float2 xzWorld, out float height, out float3 normal) {
    height = 0.0;
    normal = float3(0.0, 1.0, 0.0);
    if (gGroundRect.w < 0.5) return false;
    if (gGroundMap.x <= 0.0) {         // no stage model: the flat plane at y = 0
        height = gGroundRect.y;
        return true;
    }
    float2 uv = PackGroundUv(xzWorld);
    float h = PackGroundHeightAtUv(uv);
    if (h <= -1e8) return false;
    height = h;

    // the normal from the central differences (one texel), in world space
    float texel = 1.0 / gGroundMap.x;
    float worldPerTexel = 2.0 * gGroundRect.x * texel;
    float hl = PackGroundHeightAtUv(uv - float2(texel, 0.0));
    float hr = PackGroundHeightAtUv(uv + float2(texel, 0.0));
    float hd = PackGroundHeightAtUv(uv - float2(0.0, texel));
    float hu = PackGroundHeightAtUv(uv + float2(0.0, texel));
    if (hl <= -1e8) hl = h;
    if (hr <= -1e8) hr = h;
    if (hd <= -1e8) hd = h;
    if (hu <= -1e8) hu = h;
    float sx = (hr - hl) / (2.0 * worldPerTexel);
    float sz = (hd - hu) / (2.0 * worldPerTexel);   // +v = -z
    normal = normalize(float3(-sx, 1.0, -sz));
    return true;
}
#endif // PACK_CAP_WORLD_GROUND

// Deterministic hashing / random functions (PCG)
inline uint PackHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

inline float PackRandom(uint seed) {
    return float(PackHash(seed)) * (1.0 / 4294967296.0);
}

inline float PackRandom(uint seed, uint salt) {
    return float(PackHash(seed ^ PackHash(salt))) * (1.0 / 4294967296.0);
}

inline float PackRandom(uint seed, uint salt1, uint salt2) {
    return float(PackHash(seed ^ PackHash(salt1 ^ PackHash(salt2)))) * (1.0 / 4294967296.0);
}

inline float2 PackRandom2(uint seed, uint salt) {
    return float2(PackRandom(seed, salt), PackRandom(seed ^ 0x9e3779b9u, salt));
}

inline float3 PackRandom3(uint seed, uint salt) {
    return float3(PackRandom(seed, salt), PackRandom(seed ^ 0x9e3779b9u, salt), PackRandom(seed ^ 0x517cc1b7u, salt));
}

#endif // WORLD_API_HLSLI
