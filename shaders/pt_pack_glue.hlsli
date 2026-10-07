// Shared PT pack glue (docs/shader_pt_api.md). Included by offline_gi.hlsl and pathtrace.hlsl under MMDX_PT_PACK.
#ifndef PT_PACK_GLUE_HLSLI
#define PT_PACK_GLUE_HLSLI

#include "pt_pack_api.hlsli"

RtPtPackRecord LoadPtPackRecord(uint packSrv) {
    ByteAddressBuffer buf = gBindlessBuf[NonUniformResourceIndex(packSrv)];
    RtPtPackRecord rec;
    uint4 v0 = buf.Load4(0);
    rec.materialClass = v0.x;
    rec.headValid = v0.y;
    rec._pad0 = v0.zw;
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
