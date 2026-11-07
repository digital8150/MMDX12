// Lighting composite: colour * AO (depth-aware upsample), blended SSR, distance haze.
//   t0 colour (HDR), t1 depth (raw), t2 normal, t3 ao (half res, may be null), t4 ssr (half res, may be null).
//   gP0.x = AO intensity (0 = off), gP0.y = SSR on, gP0.zw = half-res texel size.
//   gP1 = camera-view rect in uv (Studio quad view): AO / SSR / haze only apply inside it
//   (gP1.z <= 0: the whole screen).
#include "fullscreen.hlsli"

Texture2D<float4> gColorTex : register(t0);
Texture2D<float> gDepthTex : register(t1);
Texture2D<float4> gNormalTex : register(t2);
Texture2D<float> gAoTex : register(t3);
Texture2D<float4> gSsrTex : register(t4);
Texture2D<float4> gMirrorTex : register(t5);

// S9b-3: composite has no bindless texture table, and its only SkyColor caller is the distance haze, which wants a
// colour rather than the panorama: rebuild it from the CPU averages FillSceneConstants wrote (the image's zenith /
// horizon / ground means), the same shape as the hand-set gradient. The panorama itself is drawn by the scene passes.
float3 SkyMapRadiance(float3 dir) {
    const float y = dir.y;
    return y >= 0.0 ? lerp(gSkyHorizon, gSkyZenith, pow(saturate(y), 0.55))
                    : lerp(gSkyHorizon, gGroundColor, saturate(-y * 6.0));
}

float UpsampleAo(float2 uv, float z) {
    float2 halfTexel = gP0.zw;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float sum = 0, wsum = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) {
        float2 suv = base + float2(x, y) * halfTexel;
        float sz = LinearZ(gDepthTex.SampleLevel(gPoint, suv, 0));
        float2 f = 1.0 - abs(uv - suv) / halfTexel;
        float w = max(f.x * f.y, 1e-3) / (1e-3 + abs(sz - z));
        sum += gAoTex.SampleLevel(gPoint, suv, 0) * w;
        wsum += w;
    }
    return sum / wsum;
}

float4 PSComposite(FsOut i) : SV_Target {
    float4 c = gColorTex.SampleLevel(gPoint, i.uv, 0);
    float d = gDepthTex.SampleLevel(gPoint, i.uv, 0);
    if (d >= 1.0) return c;
    float z = LinearZ(d);
    float4 nt = gNormalTex.SampleLevel(gPoint, i.uv, 0);

    // Studio quad view: AO / SSR / haze unproject through the frame camera, so they only apply
    // inside the camera quadrant (gP1 = the rect in uv; gP1.z <= 0: the whole screen).
    const bool cam = gP1.z <= 0.0 ||
                     (i.uv.x >= gP1.x && i.uv.x < gP1.x + gP1.z && i.uv.y >= gP1.y && i.uv.y < gP1.y + gP1.w);

    // nt.z = the reflection tag (common.hlsli). "No AO": a shader pack's faces, translucent stage pixels (mmd.hlsl
    // NormalTag), world / MME surfaces.
    if (cam && gP0.x > 0.0 && !ReflTagNoAo(nt.z)) {
        float ao = UpsampleAo(i.uv, z);
        c.rgb *= lerp(1.0, ao, gP0.x);
    }
    if (cam && gP0.y > 0.0 && ReflTagIsPbr(nt.z)) {
        // PBR: the scene PS added W x sky (W = the split-sum DFG weight it tagged). Where the reflection found
        // something (alpha = confidence; a miss is 0), replace that sky by the hit: + W x a x (hit - sky). Raw sample
        // for DLSS-RR, else the same alpha-weighted 3x3 gather as below.
        const float W = ReflTagPbrWeight(nt.z);
        if (W > 0.0) {
            float4 r = 0;
            if (gP2.x > 0.5) {
                r = gSsrTex.SampleLevel(gPoint, i.uv, 0);
            } else {
                [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x) {
                    float4 sr = gSsrTex.SampleLevel(gLinear, i.uv + float2(x, y) * gP0.zw * 1.2, 0);
                    float w = (x == 0 && y == 0) ? 2.0 : 1.0;
                    r.rgb += sr.rgb * sr.a * w;
                    r.a += sr.a * w;
                }
                r.rgb /= max(r.a, 1e-4);
                r.a /= 10.0;
            }
            if (r.a > 0.0) {
                const float3 Vv = normalize(ViewPosFromDepth(i.uv, d));
                const float3 Rw = normalize(mul(reflect(Vv, OctDecode(nt.xy)), (float3x3)gInvView));
                c.rgb = max(c.rgb + W * saturate(r.a * nt.w) * (r.rgb - SkyColor(Rw)), 0.0);
            }
        }
    } else if (cam && gP0.y > 0.0 && nt.z > 0.01) {
        if (nt.z > 1.5 && gP2.y > 0.5) {
            // Mirror pixel (mmd.hlsl MAT_MIRROR wrote reflectivity 2): the full-res ray-traced mirror is the whole
            // reflection, multiplied by the tint the material already drew (no Fresnel, no alpha, no gather).
            c.rgb *= gMirrorTex.Load(int3(int2(i.pos.xy), 0)).rgb;
        } else if (gP2.x > 0.5) {
            // DLSS-RR: hand RR the raw 1-frame ray-traced reflection (no 3x3 spatial gather).
            float4 sr = gSsrTex.SampleLevel(gPoint, i.uv, 0);
            float3 V = normalize(ViewPosFromDepth(i.uv, d));
            float3 N = OctDecode(nt.xy);
            float refl = min(nt.z, 1.0);
            float fres = refl + (1.0 - refl) * pow(1.0 - saturate(dot(-V, N)), 5.0);
            float w = saturate(fres * sr.a * nt.w);
            c.rgb = lerp(c.rgb, sr.rgb, w);
        } else {
        // 3x3 alpha-weighted gather hides the per-pixel jitter of the half-res march
        float4 r = 0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float4 sr = gSsrTex.SampleLevel(gLinear, i.uv + float2(x, y) * gP0.zw * 1.2, 0);
            float w = (x == 0 && y == 0) ? 2.0 : 1.0;
            r.rgb += sr.rgb * sr.a * w;
            r.a += sr.a * w;
        }
        r.rgb /= max(r.a, 1e-4);
        r.a /= 10.0;
        float3 V = normalize(ViewPosFromDepth(i.uv, d));
        float3 N = OctDecode(nt.xy);
        float refl = min(nt.z, 1.0);
        float fres = refl + (1.0 - refl) * pow(1.0 - saturate(dot(-V, N)), 5.0);
        float w = saturate(fres * r.a * nt.w);
        c.rgb = lerp(c.rgb, r.rgb, w);
        }
    }
    if (cam && gFog > 0.0) {
        float3 vp = ViewPosFromDepth(i.uv, d);
        float dist = length(vp);
        float f = (1.0 - exp(-dist * gFog * 0.0011)) * 0.85;
        float3 dir = normalize(mul(normalize(vp), (float3x3)gInvView));
        float3 fogColor = SkyColor(float3(dir.x, max(dir.y, 0.0) * 0.3, dir.z));
        c.rgb = lerp(c.rgb, fogColor, f * c.a);
    }
    return c;
}
