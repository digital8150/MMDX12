// Skinning for the ray-traced scene: current + previous bones/morphs into the RtVertex
// raw buffer read by the BLAS builds and ray queries. gTime < 1 places the position inside the
// interval previous -> current (offline motion blur: blended bone matrices and morphs).
#pragma pack_matrix(row_major)
#include "skinning.hlsli"
struct BoneMatrix { row_major float4x4 m; };
cbuffer SkinCB : register(b0) { uint gVertexCount; float gTime; uint gHasSdef; uint _skpad; };
ByteAddressBuffer gVertices : register(t0);
StructuredBuffer<BoneMatrix> gBones : register(t1);
StructuredBuffer<BoneMatrix> gPrevBones : register(t2);
ByteAddressBuffer gMorph : register(t3);
ByteAddressBuffer gPrevMorph : register(t4);
ByteAddressBuffer gSdef : register(t5);   // GpuSdef[vertex count] (40 B) when gHasSdef, else one element
RWByteAddressBuffer gOut : register(u0);

[numthreads(64, 1, 1)]
void CSSkin(uint3 id : SV_DispatchThreadID) {
    uint v = id.x;
    if (v >= gVertexCount) return;
    uint base = v * 60;
    float3 pos = asfloat(gVertices.Load3(base));
    float3 nrm = asfloat(gVertices.Load3(base + 12));
    float2 uv = asfloat(gVertices.Load2(base + 24));
    uint b01 = gVertices.Load(base + 32), b23 = gVertices.Load(base + 36);
    uint bones[4] = { b01 & 0xFFFF, b01 >> 16, b23 & 0xFFFF, b23 >> 16 };
    float4 weights = asfloat(gVertices.Load4(base + 40));
    float3 morph = asfloat(gMorph.Load3(v * 12));
    float3 prevMorph = asfloat(gPrevMorph.Load3(v * 12));

    float sdef = 0.0;
    float3 sc = 0, scr0 = 0, scr1 = 0;
    if (gHasSdef != 0) {
        uint s = v * 40;
        sc = asfloat(gSdef.Load3(s));
        scr0 = asfloat(gSdef.Load3(s + 12));
        scr1 = asfloat(gSdef.Load3(s + 24));
        sdef = asfloat(gSdef.Load(s + 36));
    }
    float4x4 c0 = gBones[bones[0]].m, c1 = gBones[bones[1]].m, c2 = gBones[bones[2]].m, c3 = gBones[bones[3]].m;
    float4x4 p0 = gPrevBones[bones[0]].m, p1 = gPrevBones[bones[1]].m, p2 = gPrevBones[bones[2]].m, p3 = gPrevBones[bones[3]].m;
    float3 pwp, pwn;
    SkinVertex(p0, p1, p2, p3, weights, pos + prevMorph, nrm, sdef, sc, scr0, scr1, pwp, pwn);
    if (gTime < 1.0) {   // per-bone blend (also correct for SDEF, whose rotation is not linear in the blend)
        c0 = lerp(p0, c0, gTime);
        c1 = lerp(p1, c1, gTime);
        c2 = lerp(p2, c2, gTime);
        c3 = lerp(p3, c3, gTime);
        morph = lerp(prevMorph, morph, gTime);
    }
    float3 wp, wn;
    SkinVertex(c0, c1, c2, c3, weights, pos + morph, nrm, sdef, sc, scr0, scr1, wp, wn);
    wn = length(wn) > 1e-12 ? normalize(wn) : float3(0, 1, 0);

    // RtVertex layout: position @0, normal @12, uv @24, prevPosition @32, pad @44
    uint o = v * 48;
    gOut.Store3(o, asuint(wp));
    gOut.Store3(o + 12, asuint(wn));
    gOut.Store2(o + 24, asuint(uv));
    gOut.Store3(o + 32, asuint(pwp));
    gOut.Store(o + 44, 0);
}
