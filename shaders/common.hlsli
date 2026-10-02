// Shared scene constants and helpers. Mirror of src/render/ShaderInterop.h.
#ifndef MMDX_COMMON_HLSLI
#define MMDX_COMMON_HLSLI
#pragma pack_matrix(row_major)

cbuffer SceneCB : register(b0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float4x4 gInvProj;
    float4x4 gInvView;
    float4x4 gViewProjNoJitter;
    float4x4 gPrevViewProjNoJitter;
    float4x4 gShadowViewProj[3];
    float4 gCascadeSplits;   // view z far of cascade 0..2, w = shadows on
    float4 gShadowParams;    // x = 1/size, y = normal offset scale, z = softness (texels)
    float4 gCascadeTexel;    // world size of one shadow texel per cascade
    float3 gEyePos;      float gTime;
    float3 gLightDir;    float gSunIntensity;
    float3 gLightColor;  float gHemiStrength;
    float3 gSkyZenith;   float gRimStrength;
    float3 gSkyHorizon;  float gNumLights;
    float3 gGroundColor; float gFloorGloss;
    float3 gRimColor;    float gFog;
    float2 gViewportSize; float gEdgeScale; float gTransparentBg;
    float2 gJitterUv;    float2 gInvViewportSize;
    float gNearZ; float gFarZ; float gFrameIndex; float _cpad;
};

static const float PI = 3.14159265;

float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(max(c, 0.0) * (1.0 / 1.055) + 0.055 / 1.055, 2.4); }
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055; }
float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Interleaved gradient noise (Jimenez 2014).
float Ign(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

float2 OctWrap(float2 v) { return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0); }
float2 OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy;
}
float3 OctDecode(float2 f) {
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0.0 ? -t : t;
    return normalize(n);
}

// Raw device depth -> positive view-space z (LH perspective).
float LinearZ(float d) { return gNearZ * gFarZ / (gFarZ - d * (gFarZ - gNearZ)); }

float3 ViewPosFromDepth(float2 uv, float d) {
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 v = mul(ndc, gInvProj);
    return v.xyz / v.w;
}

float2 ViewToUv(float3 v) {
    float4 c = mul(float4(v, 1.0), gProj);
    float2 ndc = c.xy / c.w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Linear sky radiance for a world direction (also the fog colour source).
float3 SkyColor(float3 dir) {
    float y = dir.y;
    float up = saturate(y);
    float3 sky = lerp(gSkyHorizon, gSkyZenith, pow(up, 0.55));
    float3 below = lerp(gSkyHorizon, gGroundColor, saturate(-y * 6.0));
    float3 c = y >= 0.0 ? sky : below;
    // soft sun glow toward the key light
    float s = saturate(dot(dir, -gLightDir));
    c += gSkyHorizon * (pow(s, 24.0) * 0.35 + pow(s, 600.0) * 2.0) * (y > -0.05 ? 1.0 : 0.0);
    return c * gSunIntensity;
}

#endif
