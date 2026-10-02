// Skinning for the ray-traced scene: current + previous bones/morphs into the RtVertex
// raw buffer read by the BLAS builds and ray queries. gTime < 1 places the position inside the
// interval previous -> current (offline motion blur: blended bone matrices and morphs).
#pragma pack_matrix(row_major)
struct BoneMatrix { row_major float4x4 m; };
cbuffer SkinCB : register(b0) { uint gVertexCount; float gTime; uint2 _skpad; };
ByteAddressBuffer gVertices : register(t0);
StructuredBuffer<BoneMatrix> gBones : register(t1);
StructuredBuffer<BoneMatrix> gPrevBones : register(t2);
ByteAddressBuffer gMorph : register(t3);
ByteAddressBuffer gPrevMorph : register(t4);
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

    float4x4 m = gBones[bones[0]].m * weights.x + gBones[bones[1]].m * weights.y
               + gBones[bones[2]].m * weights.z + gBones[bones[3]].m * weights.w;
    float4x4 pm = gPrevBones[bones[0]].m * weights.x + gPrevBones[bones[1]].m * weights.y
                + gPrevBones[bones[2]].m * weights.z + gPrevBones[bones[3]].m * weights.w;

    if (gTime < 1.0) {
        m = lerp(pm, m, gTime);
        morph = lerp(prevMorph, morph, gTime);
    }
    float3 wp = mul(float4(pos + morph, 1.0), m).xyz;
    float3 pwp = mul(float4(pos + prevMorph, 1.0), pm).xyz;
    float3 wn = mul(nrm, (float3x3)m);
    wn = length(wn) > 1e-12 ? normalize(wn) : float3(0, 1, 0);

    // RtVertex layout: position @0, normal @12, uv @24, prevPosition @32, pad @44
    uint o = v * 48;
    gOut.Store3(o, asuint(wp));
    gOut.Store3(o + 12, asuint(wn));
    gOut.Store2(o + 24, asuint(uv));
    gOut.Store3(o + 32, asuint(pwp));
    gOut.Store(o + 44, 0);
}
