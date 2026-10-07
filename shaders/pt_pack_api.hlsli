// Path tracer / offline GI shader pack API (docs/shader_pt_api.md). Included by offline_gi.hlsl and pathtrace.hlsl under MMDX_PT_PACK.
// Authors implement:
//     PtPackOut PackEvaluate(PtPackIn i);
#ifndef PT_PACK_API_HLSLI
#define PT_PACK_API_HLSLI

// Material classes: pack.json "classes" rules map PMX material names to these ids.
#ifndef PACK_BODY
#define PACK_BODY 0u
#define PACK_SKIN 1u
#define PACK_FACE 2u
#define PACK_EYE  3u
#define PACK_HAIR 4u
#define PACK_WEAPON 5u
#endif

struct PtPackIn {
    float3 pos;           // world position
    float3 normal;        // shading normal
    float3 V;             // towards the camera
    float2 uv;
    float3 L;             // towards the light
    float sunVis;         // 0..1, from the traced shadow ray
    float3 baseColor;     // linear texture * material diffuse, no lighting
    uint materialClass;   // the existing PackClass of the material
    float params[16];     // the pack's 16 sliders, as float
    // head frame
    float3 headPos;       // world position of the head bone (only when headValid)
    float headScale;      // world units per model unit (character display scale)
    float3 headRight;
    float3 headUp;
    float3 headForward;
    bool headValid;
};

struct PtPackOut {
    float3 albedo;        // linear, used for diffuse bounces and GI
    float3 shadowTint;    // linear colour multiplying the shaded side
    float shadowBias;     // shifts the terminator, -1..1
    float3 specular;      // additive linear radiance, already includes any rim / matcap the author wants
    float2 terminator;    // N.L edges (lo, hi) of the shadow terminator smoothstep; hi <= lo = engine default (-0.12, 0.22)
    bool flatFace;        // bool: use the engine's existing flat-face handling = no GI gradient on this surface
};

// Pack texture helpers (docs/shader_pt_api.md). Implemented in pt_pack_glue.hlsli.
// Explicit-LOD sampling only. Indices are 0-based in pack.json "textures" order.
// Out-of-range index samples as white (1, 1, 1, 1) / returns size (1, 1).
float4 PtPackSampleTex(uint i, float2 uv);
float4 PtPackSampleTexLevel(uint i, float2 uv, float lod);
uint PtPackTexCount();
float2 PtPackTexSize(uint i);

#endif // PT_PACK_API_HLSLI
