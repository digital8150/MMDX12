// Offline GI outline layer: skinning at the shutter time and the offline camera (lens, shutter), shared by
// offline_edge.hlsl (default edges) and offline_edge_pack.hlsl (shader-pack edges). The including file declares
// VSIn, gBones and gPrevBones (StructuredBuffer<BoneMatrix>) and includes offline_common.hlsli + skinning.hlsli.
#ifndef MMDX_OFFLINE_EDGE_SKIN_HLSLI
#define MMDX_OFFLINE_EDGE_SKIN_HLSLI

// time: shutter time 0..1 (0 = previous pose / camera); lens: view-space lens offset; focus: view z
cbuffer EdgeCB : register(b2) { float gShutter; float2 gLens; float gFocus; };

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

// Inverted-hull vertex: pushed `px` pixels out along the screen direction of the normal.
float4 OfflineEdgeClip(float3 wp, float3 wn, float px) {
    float4 clip = ToClip(wp);
    float4 clipN = ToClip(wp + wn * 0.01);        // screen direction of the normal
    float2 dirPx = (clipN.xy / clipN.w - clip.xy / clip.w) * gViewportSize;
    float len = length(dirPx);
    dirPx = len > 1e-6 ? dirPx / len : float2(0, 0);
    clip.xy += dirPx * px * 2.0 / gViewportSize * clip.w;
    return clip;
}

#endif
