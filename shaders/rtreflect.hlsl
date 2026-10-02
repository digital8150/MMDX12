// Ray-traced reflections at half resolution: one mirror ray per pixel, closest
// hit shaded with the MMD material model, sky on miss. Written linear; alpha = blend weight
// (fades with the hit distance as a stand-in for gloss).
#include "rt_common.hlsli"   // includes common.hlsli
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);

Texture2D<float> gDepthTex : register(t0);
Texture2D<float4> gNormalTex : register(t1);
RWTexture2D<float4> gReflOut : register(u0);

[numthreads(8, 8, 1)]
void CSReflect(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gReflOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    uint2 fp = p * 2;   // full-res source pixel, loaded without filtering
    float d = gDepthTex.Load(int3(fp, 0));
    float4 nt = gNormalTex.Load(int3(fp, 0));
    if (d >= 1.0 || nt.z <= 0.01) { gReflOut[p] = float4(0, 0, 0, 0); return; }   // nt.z = reflectivity

    float2 uv = (fp + 0.5) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d);
    float3 wp = mul(float4(vp, 1.0), gInvView).xyz;
    float3 wn = normalize(mul(OctDecode(nt.xy), (float3x3)gInvView));

    float3 V = normalize(wp - gEyePos);
    float3 R = reflect(V, wn);
    if (dot(R, wn) <= 0) { gReflOut[p] = float4(0, 0, 0, 0); return; }

    // One sharp ray: a jittered (glossy) ray is speckled noise without temporal accumulation.
    // Gloss is approximated instead by blurrier texture lookups and a weaker blend with distance.
    RtHit hit;
    if (!TraceClosest(OffsetRayOrigin(wp, wn), R, 0.0, gP0.z, 0.5, hit)) {
        gReflOut[p] = float4(SkyColor(R), 0.6);
        return;
    }

    RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
    RtSurface s = FetchSurface(g, hit.prim, hit.bary);
    if (dot(s.faceNormal, R) > 0) s.faceNormal = -s.faceNormal;
    if (dot(s.normal, s.faceNormal) < 0) s.normal = -s.normal;

    float4 tex = SampleBaseTexture(g, s.uv, 1.0 + log2(1.0 + hit.t / 20.0));
    float3 L = -gLightDir;
    float ndl = dot(s.normal, L);
    float sh = 1.0;
    if ((g.flags & MAT_RECEIVE) && gCascadeSplits.w > 0.5 && ndl > 0)
        sh = TraceShadowRay(OffsetRayOrigin(s.pos, s.faceNormal), L, 2000.0);

    float lightTerm = sh * smoothstep(-0.1, 0.2, ndl);
    float3 lit = MaterialLit(g, tex.rgb);
    float3 c = lerp(lit * 0.62, lit, lightTerm);
    float3 albedoLin = SrgbToLinear(saturate(MaterialAlbedo(g, tex.rgb)));
    float3 hemi = lerp(gGroundColor, gSkyZenith, saturate(s.normal.y * 0.5 + 0.5)) * gSunIntensity;
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity + albedoLin * hemi * gHemiStrength;
    gReflOut[p] = float4(color, lerp(0.8, 0.35, saturate(hit.t / 300.0)));
}
