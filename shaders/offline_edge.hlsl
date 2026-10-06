// Offline GI renderer, outline layer: MMD inverted-hull edges rasterised at the offline image
// size (4x MSAA), occluded by a depth pre-pass of the same skinned meshes. Drawn once per render
// iteration with that iteration's shutter time and lens position (the tracer uses the same camera,
// offline_common.hlsli), so the averaged layer carries the same motion blur and depth of field.
// The resolved layers (premultiplied linear colour, a = coverage) are accumulated and composited
// over the path-traced image by offline_post.hlsl CSFinalize.
// Root signature (OfflineRenderer.cpp): b0 SceneConstants, b1 MaterialConstants, b2 EdgeCB (root
// constants), t0 bones, t4 previous bones (root SRVs), t1..t3 material table, s0 anisotropic wrap.
#include "common.hlsli"
#include "offline_common.hlsli"
#include "skinning.hlsli"

cbuffer MaterialCB : register(b1) {
    float4 gDiffuse; float3 gSpecular; float gSpecularPower; float3 gAmbient; float gEdgeSize;
    float4 gEdgeColor; uint gFlags; float gReflectivity; uint2 _mp;
    // material morph factors (ApplyTexFactor): texture, sphere, toon
    float4 gTexMul; float4 gTexAdd; float4 gSphereMul; float4 gSphereAdd; float4 gToonMul; float4 gToonAdd;
};
// time: shutter time 0..1 (0 = previous pose / camera); lens: view-space lens offset; focus: view z
cbuffer EdgeCB : register(b2) { float gShutter; float2 gLens; float gFocus; };
#define MAT_HAS_TEXTURE 1u

struct BoneMatrix { row_major float4x4 m; };
StructuredBuffer<BoneMatrix> gBones : register(t0);
StructuredBuffer<BoneMatrix> gPrevBones : register(t4);
Texture2D gTexture : register(t1);
SamplerState gWrap : register(s0);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; float edge : TEXCOORD1;
    float3 morph : TEXCOORD2; float3 prevMorph : TEXCOORD3;
    float3 sdefC : TEXCOORD4; float3 sdefR0 : TEXCOORD5; float3 sdefR1 : TEXCOORD6; float sdef : TEXCOORD7;
};

// Skinning at the shutter time: per-bone blended matrices and morphs (as skin.hlsl does for the BLAS).
void SkinAt(VSIn v, out float3 wp, out float3 wn) {
    float4x4 b0 = gBones[v.bones.x].m, b1 = gBones[v.bones.y].m, b2 = gBones[v.bones.z].m, b3 = gBones[v.bones.w].m;
    float3 morph = v.morph;
    if (gShutter < 1.0) {
        b0 = lerp(gPrevBones[v.bones.x].m, b0, gShutter);
        b1 = lerp(gPrevBones[v.bones.y].m, b1, gShutter);
        b2 = lerp(gPrevBones[v.bones.z].m, b2, gShutter);
        b3 = lerp(gPrevBones[v.bones.w].m, b3, gShutter);
        morph = lerp(v.prevMorph, v.morph, gShutter);
    }
    SkinVertex(b0, b1, b2, b3, v.weights, v.pos + morph, v.nrm, v.sdef, v.sdefC, v.sdefR0, v.sdefR1, wp, wn);
    wn = normalize(wn);
}

float4 ToClip(float3 wp) {
    float3x3 basis;
    float3 eye;
    OfflineCameraAt(gShutter, basis, eye);
    float3 vp = OfflineLensView(mul(basis, wp - eye), gLens, gFocus);
    return mul(float4(vp, 1.0), gProj);
}

struct DepthOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

DepthOut VSDepth(VSIn v) {
    float3 wp, wn;
    SkinAt(v, wp, wn);
    DepthOut o;
    o.pos = ToClip(wp);
    o.uv = v.uv;
    return o;
}

// Coverage test like the ray tracer's binary alpha (>= 0.5).
void PSDepth(DepthOut i) {
    float a = gDiffuse.a * ((gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv).a : 1.0);
    clip(a - 0.5);
}

float4 VSEdge(VSIn v) : SV_Position {
    float3 wp, wn;
    SkinAt(v, wp, wn);
    float px = gEdgeSize * v.edge * gEdgeScale;   // outline width in pixels (scaled with height / 1080)
    float4 clip = ToClip(wp);
    float4 clipN = ToClip(wp + wn * 0.01);        // screen direction of the normal
    float2 dirPx = (clipN.xy / clipN.w - clip.xy / clip.w) * gViewportSize;
    float len = length(dirPx);
    dirPx = len > 1e-6 ? dirPx / len : float2(0, 0);
    clip.xy += dirPx * px * 2.0 / gViewportSize * clip.w;
    return clip;
}

// Premultiplied output (blend ONE / INV_SRC_ALPHA). Same colour as mmd.hlsl PSEdge.
float4 PSEdge(float4 pos : SV_Position) : SV_Target {
    float a = gEdgeColor.a;
    return float4(SrgbToLinear(gEdgeColor.rgb) * gSunIntensity * 0.85 * a, a);
}
