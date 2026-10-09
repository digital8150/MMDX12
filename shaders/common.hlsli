// Shared scene constants and helpers. Mirror of src/render/ShaderInterop.h.
#ifndef MMDX_COMMON_HLSLI
#define MMDX_COMMON_HLSLI
#pragma pack_matrix(row_major)

// PMX material morph texture factors, as MMD's default shader (full.fx) applies them:
// rgb = lerp(neutral, rgb * mul + add, mul.a + add.a); the factor alphas weight the effect and the
// texture alpha is not changed. Identity: mul 1, add 0. neutral = 1 (texture, toon, multiply
// sphere) or 0 (additive sphere), so a factor alpha of 0 turns the map off.
float3 ApplyTexFactor3(float3 c, float4 mulF, float4 addF, float neutral) {
    return saturate(lerp(neutral.xxx, c * mulF.rgb + addF.rgb, mulF.a + addF.a));
}
float4 ApplyTexFactor(float4 c, float4 mulF, float4 addF) {
    return float4(ApplyTexFactor3(c.rgb, mulF, addF, 1.0), c.a);
}

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
    float gNearZ; float gFarZ; float gFrameIndex; float gShading;   // shading: 0 Lit, 1 Unlit, 2 Wireframe
    float4x4 gPrevInvView;   // offline renderer: camera-to-world at shutter open
    // offline renderer scene extras (render benchmark): glass box, softboxes, floor
    float4 gGlassCenter;     // xyz, w = enabled
    float4 gGlassHalf;       // xyz half extents, w = corner radius
    float4 gGlassParams;     // cos yaw, sin yaw, ior (green), ior(blue) - ior(red)
    float4 gGlassAbsorb;     // xyz absorption per unit
    float4 gSoftbox[8];      // [i * 4 + 0..3]: centre (w = enabled), half U, half V, radiance
    float4 gFloorParams;     // xyz albedo, w = reflectivity (< 0: default floor)
    float4x4 gSpotViewProj[8];   // spot shadow map slices (Light.pad.x = slice)
    float4 gSpotShadowParams;    // x = slices rendered this frame, y = 1/mapSize
    float4 gSunShadowParams;     // x = shadowType (0=NoCast, 1=Hard, 2=Soft), y = softness, z = density, w = unused (sun shadows: only x/y/z are read)
    float4 gSunShadowColor;      // xyz = shadow colour (linear RGB), w = unused
    float4 gPointShadowParams;   // x = point lights shadowed this frame (0 = none), y = 1/pointMapSize
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
    // soft sun glow toward the key light (driven by sun colour so black = no glow)
    float s = saturate(dot(dir, -gLightDir));
    c += gSkyHorizon * ((gLightColor.r + gLightColor.g + gLightColor.b) > 1e-4 ? 1.0 : 0.0) * (pow(s, 24.0) * 0.35 + pow(s, 600.0) * 2.0) * (y > -0.05 ? 1.0 : 0.0);
    return c * gSunIntensity;
}

// Shared punctual light falloff curves (common.hlsli).
// falloffType: 0 = None (today's curve), 1 = Linear, 2 = InverseSquare.
float PunctualFalloff(float dist, float invRange, float falloffType) {
    float d = dist * invRange;
    if (d >= 1.0) return 0.0;
    float x = saturate(1.0 - pow(d, 4.0));
    if (falloffType > 1.5) {
        return (x * x) / max(dist * dist, 1.0);
    } else if (falloffType > 0.5) {
        return saturate(1.0 - d);
    } else {
        return (x * x) / (1.0 + dist * dist * 0.0004);
    }
}

// Sun shadow cone for the ray paths: Hard keeps `baseCos` (each path's own sun disc), Soft widens it with softness.
float SunShadowConeCos(float baseCos) {
    return (gSunShadowParams.x > 1.5) ? lerp(baseCos, 0.995, saturate(gSunShadowParams.y)) : baseCos;
}

// Shared shadow transmission factoring in density and shadow colour tint.
// Returns a 0..1 RGB multiplier for the light term.
float3 ShadowTransmission(float vis, float density, float3 shadowColor) {
    float occ = (1.0 - vis) * density;
    return (1.0 - occ) + shadowColor * occ;
}

#endif
