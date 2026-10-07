// Shared PT pack glue (docs/shader_pt_api.md). Included by offline_gi.hlsl and pathtrace.hlsl under MMDX_PT_PACK.
#ifndef PT_PACK_GLUE_HLSLI
#define PT_PACK_GLUE_HLSLI

#include "pt_pack_api.hlsli"

static uint s_PtPackTexBase = 0;
static uint s_PtPackTexCount = 0;
static uint s_PtPackClampMask = 0;

void PtPackBindRecord(RtPtPackRecord rec) {
    s_PtPackTexBase = rec.texBase;
    s_PtPackTexCount = rec.texInfo & 0xFFu;
    s_PtPackClampMask = (rec.texInfo >> 8) & 0xFFFFu;
}

uint PtPackTexCount() {
    return s_PtPackTexCount;
}

float2 PtPackTexSize(uint i) {
    if (i >= s_PtPackTexCount) return float2(1.0, 1.0);
    uint w, h;
    gBindlessTex[NonUniformResourceIndex(s_PtPackTexBase + i)].GetDimensions(w, h);
    return float2((float)w, (float)h);
}

float4 PtPackSampleTexLevel(uint i, float2 uv, float lod) {
    if (i >= s_PtPackTexCount) return float4(1.0, 1.0, 1.0, 1.0);
    uint srvIndex = s_PtPackTexBase + i;
    bool clamp = ((s_PtPackClampMask >> i) & 1u) != 0;
    if (clamp) {
        return gBindlessTex[NonUniformResourceIndex(srvIndex)].SampleLevel(gLinear, uv, lod);
    } else {
        return gBindlessTex[NonUniformResourceIndex(srvIndex)].SampleLevel(gLinearWrap, uv, lod);
    }
}

float4 PtPackSampleTex(uint i, float2 uv) {
    return PtPackSampleTexLevel(i, uv, 0.0);
}

RtPtPackRecord LoadPtPackRecord(uint packSrv) {
    ByteAddressBuffer buf = gBindlessBuf[NonUniformResourceIndex(packSrv)];
    RtPtPackRecord rec;
    uint4 v0 = buf.Load4(0);
    rec.materialClass = v0.x;
    rec.headValid = v0.y;
    rec.texBase = v0.z;
    rec.texInfo = v0.w;
    rec.headRight = asfloat(buf.Load4(16));
    rec.headUp = asfloat(buf.Load4(32));
    rec.headForward = asfloat(buf.Load4(48));
    rec.params[0] = asfloat(buf.Load4(64));
    rec.params[1] = asfloat(buf.Load4(80));
    rec.params[2] = asfloat(buf.Load4(96));
    rec.params[3] = asfloat(buf.Load4(112));
    return rec;
}

// Composes the sun direct term from PtPackOut and geometry/lighting terms.
float3 PtPackComposeSunDirect(PtPackOut ptPackOut, float3 n, float sunVis, float flatVal) {
    float ndl = dot(n, -gLightDir);
    float terminator = smoothstep(-0.12, 0.22, ndl + ptPackOut.shadowBias);
    float term = lerp(terminator * sunVis, lerp(1.0, sunVis, 0.8), flatVal);
    return lerp(ptPackOut.albedo * ptPackOut.shadowTint, ptPackOut.albedo, term) * gSunIntensity + ptPackOut.specular * sunVis;
}

float3 PtPackComposeSunDirect(PtPackOut ptPackOut, float3 n, float sunVis, bool flatSurface) {
    return PtPackComposeSunDirect(ptPackOut, n, sunVis, flatSurface ? 1.0 : 0.0);
}

#endif // PT_PACK_GLUE_HLSLI
