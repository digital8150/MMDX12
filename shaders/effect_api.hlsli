// Shader pack effect API (docs/shader_effect_api.md). Included by effect.hlsl, which a "type": "effect" pack ships;
// the app compiles effect.hlsl with the pack's own file appended (MMDX_PACK), like the surface packs. A pack
// implements one function:
//
//     float3 PackEffect(PackEffectInput i);
//
// It runs in PackEffectPass over the whole screen, in the user's stack order, at output resolution. The injection
// point comes from pack.json "stage": "pre-bloom" (linear HDR, before BloomPass) or "post" (default; after PostPass /
// tonemap, display-referred sRGB). Everything in fullscreen.hlsli / common.hlsli is visible as well (gTime,
// gFrameIndex, gNearZ / gFarZ, LinearZ, OctDecode, SrgbToLinear, Luminance, Ign, ...), but only the names below are
// a stable contract.
#ifndef EFFECT_API_HLSLI
#define EFFECT_API_HLSLI

// Must equal kPackApiVersion (render/ShaderPack.h). One manifest version for surface and effect packs.
#define PACK_API_VERSION 3

// gP0.xy (fullscreen.hlsli's PassCB, b1) = the output size in pixels; time / frame come from SceneConstants.
#define gEffectOutputSize float2(gP0.x, gP0.y)
#define gEffectTime gTime
#define gEffectFrameIndex gFrameIndex

// The pack's 16 parameters (b2, 16 root constants), pack.json order; the user's slider value or the
// manifest default.
#define PackParam(i) gPackEffectParams[i >> 2][i & 3]

struct PackEffectInput {
    float2 uv;            // texel centre of this pixel, 0..1 (0, 0 = top left)
    float2 pixel;         // SV_Position.xy (pixels)
    float2 outputSize;    // output resolution in pixels
    float time;           // seconds since the scene started
    float frameIndex;     // frame counter, wraps at 64
    float4 color;         // the frame so far at this pixel: linear HDR ("pre-bloom") or display-referred
                          // sRGB with alpha ("post"; alpha < 1 only with a transparent background)
    float depth;          // raw device depth at this pixel (1 = background; LinearZ() converts to view z)
    float3 normal;        // world-space normal, oct-encoded in the G-buffer (OctDecode); decodes to a stray
                          // direction on background pixels
    float2 motion;        // motion vector, uv(current) - uv(prev); (0, 0) when the frame has no motion
};

// The frame so far (ping-ponged through the pass; reading a neighbouring pixel of it is the way lens
// effects see around the current one).
// The G-buffer inputs are the frame's own targets, bound directly: background pixels read depth 1 and an
// empty normal / zero motion. Do not write them.
Texture2D<float4> gEffectSource : register(t0, space6);
Texture2D<float> gEffectDepth : register(t1, space6);        // raw device depth (R32_FLOAT)
Texture2D<float2> gEffectVelocity : register(t2, space6);    // uv(cur) - uv(prev) (R16G16_FLOAT)
Texture2D<float4> gEffectNormal : register(t3, space6);      // oct normal .xy (RGBA16_FLOAT)

cbuffer EffectParams : register(b2) { float4 gPackEffectParams[4]; }

// ---- pack textures (pack.json "textures", the v2 surface-pack API, same semantics) -------------------
// sRGB textures return LINEAR values (do not apply SrgbToLinear again), others the stored values;
// missing textures and out-of-range indices sample white. The same 16-SRV table as the surface
// packs (t0, space5), bound by PackEffectPass.
#ifndef PACK_TEX_COUNT
#define PACK_TEX_COUNT 0
#endif
#ifndef PACK_TEX_CLAMP_MASK
#define PACK_TEX_CLAMP_MASK 0u
#endif
#ifndef PACK_TEX_SRGB_MASK
#define PACK_TEX_SRGB_MASK 0u
#endif

Texture2D gPackFxTex[16] : register(t0, space5);

// Texture i (0..15 in pack.json order) with its declared address mode. Returns LINEAR values for
// sRGB textures, the stored values otherwise.
float4 PackFxSampleTex(uint i, float2 uv) {
    if (i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 15u;
    float4 c;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) c = gPackFxTex[i].Sample(gLinear, uv);
    else c = gPackFxTex[i].Sample(gLinearWrap, uv);
    return c;
}

// Explicit-LOD variant (ramps and other data maps: sample at lod 0).
float4 PackFxSampleTexLevel(uint i, float2 uv, float lod) {
    if (i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 15u;
    float4 c;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) c = gPackFxTex[i].SampleLevel(gLinear, uv, lod);
    else c = gPackFxTex[i].SampleLevel(gLinearWrap, uv, lod);
    return c;
}

// Level-0 size in pixels; a missing texture (0, 0).
uint2 PackFxTexSize(uint i) {
    if (i >= (uint)PACK_TEX_COUNT) return uint2(0, 0);
    uint2 s;
    gPackFxTex[i & 15u].GetDimensions(s.x, s.y);
    return s;
}

uint PackFxTexCount() { return (uint)PACK_TEX_COUNT; }

#endif
