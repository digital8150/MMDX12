// Shader pack API (docs/shader-packs.md). Included by mmd.hlsl before the pack's surface.hlsl, only when a pack is
// compiled (MMDX_PACK). A pack implements one function:
//
//     PackResult PackShade(PackSurface s);
//
// It runs in the scene pass for every pixel of the model's materials (raster and ray-traced paths) instead of the
// default MMD shading. Vertex shading, edges and the shadow pass stay the app's. Everything in mmd.hlsl / common.hlsli
// is visible as well (gLightColor, gSunIntensity, Hemisphere(n), PunctualDiffuse(...), SrgbToLinear, ...), but only the
// names below are a stable contract.
#ifndef PACK_API_HLSLI
#define PACK_API_HLSLI

// Must equal kPackApiVersion (render/ShaderPack.h).
#define PACK_API_VERSION 3

// Material classes: pack.json "classes" rules map PMX material names to these ids.
#define PACK_BODY 0u
#define PACK_SKIN 1u
#define PACK_FACE 2u
#define PACK_EYE  3u
#define PACK_HAIR 4u
#define PACK_WEAPON 5u

struct PackSurface {
    float3 worldPos;      // world space (MMD units, +Y up)
    float3 N;             // world normal, facing the viewer on back faces
    float3 V;             // to the eye
    float3 L;             // to the key light (sun)
    float2 uv;
    float2 pixel;         // SV_Position.xy
    float viewZ;          // view-space depth
    float4 tex;           // base texture (gamma space) with material-morph factors; white without a texture
    float alpha;          // material alpha * texture alpha (pixels below 0.004 are already discarded)
    float shadow;         // cast shadow from the sun: 0 shadowed .. 1 lit (cascades or RT; 1 if the material
                          // does not receive shadows)
    uint materialClass;   // PACK_BODY / SKIN / FACE / EYE / HAIR
    // The head bone's current world frame (bone 頭, else 首). hasHead = false: no such bone, the axes are world axes.
    bool hasHead;
    float3 headPos;       // world position of the head bone
    float3 headRight;     // model's right (+X), unit length
    float3 headUp;        // +Y
    float3 headForward;   // the way the face looks (MMD models face -Z)
    float headScale;      // world units per model unit (character display scale)
};

struct PackResult {
    float3 color;         // linear HDR radiance (the default shading multiplies by gSunIntensity itself)
    float alpha;
    float reflectivity;   // 0..1, SSR / RT reflection strength
    bool noAo;            // true: SSAO / RTAO do not darken this pixel (faces); also disables reflections
};

// Pack parameter i (0..15), in pack.json order; the user's slider value or the manifest default.
float PackParam(uint i) { return gPackParams[i >> 2][i & 3]; }

// MMD material values (gamma space, after material morphs).
float4 PackDiffuse() { return gDiffuse; }
float3 PackAmbient() { return gAmbient; }
float3 PackSpecular() { return gSpecular; }
float PackSpecularPower() { return gSpecularPower; }
float PackEdgeReflectivity() { return gReflectivity; }

// The MMD colour model, as the default shading starts from it: saturate(ambient + diffuse * light) * texture.
float3 PackMmdLit(PackSurface s) { return saturate(gAmbient + gDiffuse.rgb * gLightColor) * s.tex.rgb; }

// Sphere map (view-space normal lookup) with its morph factors. mode: 0 none, 1 multiply, 2 add.
uint PackSphereMode() { return (gFlags & MAT_SPHERE_MUL) ? 1u : ((gFlags & MAT_SPHERE_ADD) ? 2u : 0u); }
float3 PackSampleSphere(float3 N) {
    float3 nv = normalize(mul(N, (float3x3)gView));
    float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
    return ApplyTexFactor3(gSphere.Sample(gClamp, suv).rgb, gSphereMul, gSphereAdd, (gFlags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
}

// MMD toon ramp: v = 0 lit end, 1 shadow end. Without a toon the ramp is white.
bool PackHasToon() { return (gFlags & MAT_HAS_TOON) != 0 && (gFlags & MAT_TOON_MAP) == 0; }
float3 PackSampleToon(float v) {
    if (!PackHasToon()) return float3(1, 1, 1);
    return ApplyTexFactor3(gToon.Sample(gClamp, float2(0.5, v)).rgb, gToonMul, gToonAdd, 1.0);
}

// ---- pack textures (pack.json "textures") ------------------------------------------------------------
// The engine uploads at most 16 textures per pack (DEFAULT heap, shared by every model using the
// pack) into a fixed table (t0, space5, below) and compiles PSPack with the texture state as
// defines; without textures declared the table is not bound and everything samples as white:
//   PACK_TEX_COUNT       number of textures (0..16)
//   PACK_TEX_CLAMP_MASK  bit i set: texture i uses clamp addressing (else wrap)
//   PACK_TEX_SRGB_MASK   bit i set: texture i is sRGB; these SRVs use the _SRGB format, so
//                        sampling returns LINEAR values - do not pass them through SrgbToLinear
//                        again (s.tex / PackMmdLit stay gamma space as before). "srgb": false
//                        textures return the stored values (data maps: light maps, SDF, LUTs).
// Indices are 0-based in pack.json "textures" order. Textures missing on disk (the manager shows
// the count) and out-of-range indices sample as white.

#ifndef PACK_TEX_COUNT
#define PACK_TEX_COUNT 0
#endif
#ifndef PACK_TEX_CLAMP_MASK
#define PACK_TEX_CLAMP_MASK 0u
#endif
#ifndef PACK_TEX_SRGB_MASK
#define PACK_TEX_SRGB_MASK 0u
#endif

Texture2D gPackTex[16] : register(t0, space5);

// Texture i (0..15 in pack.json order) with its declared address mode. Returns LINEAR values for
// sRGB textures (do not apply SrgbToLinear again), the stored values otherwise.
float4 PackSampleTex(uint i, float2 uv) {
    if (i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 15u;
    float4 c;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) c = gPackTex[i].Sample(gClamp, uv);
    else c = gPackTex[i].Sample(gWrap, uv);
    return c;
}

// Explicit-LOD variant (ramps and other data maps: sample at lod 0).
float4 PackSampleTexLevel(uint i, float2 uv, float lod) {
    if (i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 15u;
    float4 c;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) c = gPackTex[i].SampleLevel(gClamp, uv, lod);
    else c = gPackTex[i].SampleLevel(gWrap, uv, lod);
    return c;
}

// Level-0 size in pixels; a missing texture (0, 0).
uint2 PackTexSize(uint i) {
    if (i >= (uint)PACK_TEX_COUNT) return uint2(0, 0);
    uint2 s;
    gPackTex[i & 15u].GetDimensions(s.x, s.y);
    return s;
}

uint PackTexCount() { return (uint)PACK_TEX_COUNT; }

void PackHeadFrame(inout PackSurface s) {
    s.hasHead = gPackHead.w > 0.5;
    s.headPos = 0;
    s.headRight = float3(1, 0, 0);
    s.headUp = float3(0, 1, 0);
    s.headForward = float3(0, 0, -1);
    s.headScale = 1.0;
    if (!s.hasHead) return;
    // skinning matrix = bind -> posed world; MMD bones have no bind orientation, so its rows are the head's world axes
    float4x4 m = gBones[gPackHeadBone].m;
    float3 r = mul(float3(1, 0, 0), (float3x3)m);
    s.headScale = max(length(r), 1e-4);
    s.headRight = r / s.headScale;
    s.headUp = normalize(mul(float3(0, 1, 0), (float3x3)m));
    s.headForward = normalize(mul(float3(0, 0, -1), (float3x3)m));
    s.headPos = mul(float4(gPackHead.xyz, 1.0), m).xyz;
}

// ---- pack edges (optional) ---------------------------------------------------------------------------
// A pack can also draw the model's outlines: it sets
//
//     #define PACK_HAS_EDGE 1
//
// at the top of surface.hlsl and implements the one function below. The engine then compiles a pack
// variant of the edge pass: the vertex stage multiplies the outline width by widthScale, the pixel
// stage takes the colour (converted to linear and scaled by the sun, as the default edge shading).
// Without PACK_HAS_EDGE the edge pass is exactly the app's default.

struct PackEdgeResult {
    float4 color;      // outline colour, gamma space like the MMD edge colour (RGBA)
    float widthScale;  // multiplies the MMD outline width in pixels (1.0 = MMD size, 0 = no outline)
};

#endif
