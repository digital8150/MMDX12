// Screen-space reflections at half resolution: perspective-correct march along the reflected
// view ray against the resolved depth buffer, then a binary refinement.
//   t0 depth (raw), t1 normal (oct view normal, reflectivity, coverage), t2 colour (HDR).
//   gP0.x = max distance (MMD units), gP0.y = steps.
#include "fullscreen.hlsli"

Texture2D<float> gDepthTex : register(t0);
Texture2D<float4> gNormalTex : register(t1);
Texture2D<float4> gColorTex : register(t2);

float4 PSSsr(FsOut i) : SV_Target {
    float4 nt = gNormalTex.SampleLevel(gPoint, i.uv, 0);
    float refl = nt.z;
    float d = gDepthTex.SampleLevel(gPoint, i.uv, 0);
    if (refl < 0.01 || d >= 1.0) return 0;

    float3 P = ViewPosFromDepth(i.uv, d);
    float3 N = OctDecode(nt.xy);
    float3 V = normalize(P);
    float3 R = normalize(reflect(V, N));
    if (R.z < -0.6) return 0;  // pointing back at the camera: nothing on screen to hit

    float maxDist = gP0.x;
    float3 end = P + R * maxDist;
    if (end.z < gNearZ * 1.5) {
        float t = (gNearZ * 1.5 - P.z) / (end.z - P.z);
        end = lerp(P, end, t);
    }
    float4 h0 = mul(float4(P, 1.0), gProj);
    float4 h1 = mul(float4(end, 1.0), gProj);
    float k0 = 1.0 / h0.w, k1 = 1.0 / h1.w;
    float2 uv0 = float2(h0.x * k0 * 0.5 + 0.5, 0.5 - h0.y * k0 * 0.5);
    float2 uv1 = float2(h1.x * k1 * 0.5 + 0.5, 0.5 - h1.y * k1 * 0.5);

    const int steps = (int)gP0.y;
    float jitter = Ign(i.pos.xy + gFrameIndex * 7.13);
    float prevT = 0;
    float prevRayZ = P.z;
    float prevDiff = -1.0;
    bool hit = false;
    float hitT = 0;
    [loop] for (int s = 1; s <= steps; ++s) {
        float t = (s + jitter - 1.0) / steps;
        t = t * t;  // more samples near the surface
        float2 uv = lerp(uv0, uv1, t);
        if (any(uv < 0.0) || any(uv > 1.0)) break;
        float rayZ = 1.0 / lerp(k0, k1, t);
        float sceneZ = LinearZ(gDepthTex.SampleLevel(gPoint, uv, 0));
        float diff = rayZ - sceneZ;
        // A crossing from in front of the depth surface to behind it, within a thickness that
        // grows with the step so thin geometry (legs, hair) is not skipped.
        float thickness = max(max(0.6, rayZ * 0.02), abs(rayZ - prevRayZ) * 1.5);
        if (prevDiff <= 0.0 && diff > 0.0 && diff < thickness && s > 1) {
            float lo = prevT, hi = t;
            [unroll] for (int b = 0; b < 6; ++b) {
                float mid = 0.5 * (lo + hi);
                float2 muv = lerp(uv0, uv1, mid);
                float mz = 1.0 / lerp(k0, k1, mid);
                if (mz > LinearZ(gDepthTex.SampleLevel(gPoint, muv, 0))) hi = mid; else lo = mid;
            }
            hitT = hi;
            hit = true;
            break;
        }
        prevT = t;
        prevRayZ = rayZ;
        prevDiff = diff;
    }
    if (!hit) return 0;

    float2 huv = lerp(uv0, uv1, hitT);
    float3 hitN = OctDecode(gNormalTex.SampleLevel(gPoint, huv, 0).xy);
    if (dot(hitN, R) > 0.2) return 0;  // hit the back of something
    // Reject hits on the reflecting surface itself (depth precision at grazing angles).
    float3 hitP = ViewPosFromDepth(huv, gDepthTex.SampleLevel(gPoint, huv, 0));
    if (dot(hitP - P, N) < 0.02 * length(hitP - P) + 0.05) return 0;
    float2 edge = saturate(min(huv, 1.0 - huv) / 0.08);
    float conf = edge.x * edge.y * (1.0 - smoothstep(0.55, 1.0, hitT));
    float3 c = gColorTex.SampleLevel(gLinear, huv, 0).rgb;
    return float4(c, conf);
}
