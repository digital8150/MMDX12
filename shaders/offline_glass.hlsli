// Offline renderer: the analytic glass box of the render benchmark scene (SceneCB gGlass*), shared
// by offline_gi.hlsl (light transport) and offline_post.hlsl (outline masking). A rounded box
// (gGlassHalf.xyz outer half extents, corner radius gGlassHalf.w) rotated by a yaw around +Y,
// intersected by sphere tracing its exact signed distance inside the slab interval of the outer box:
// forward from the slab entry for rays arriving from outside, backward from the slab exit for rays
// travelling inside it (the shape is convex, so both converge to the true crossing).
// Include common.hlsli first.
#ifndef MMDX_OFFLINE_GLASS_HLSLI
#define MMDX_OFFLINE_GLASS_HLSLI

bool GlassOn() { return gGlassCenter.w > 0.5; }

float3 GlassToLocal(float3 p) {
    float3 q = p - gGlassCenter.xyz;
    return float3(q.x * gGlassParams.x - q.z * gGlassParams.y, q.y, q.x * gGlassParams.y + q.z * gGlassParams.x);
}
float3 GlassDirToLocal(float3 v) {
    return float3(v.x * gGlassParams.x - v.z * gGlassParams.y, v.y, v.x * gGlassParams.y + v.z * gGlassParams.x);
}
float3 GlassDirToWorld(float3 v) {
    return float3(v.x * gGlassParams.x + v.z * gGlassParams.y, v.y, -v.x * gGlassParams.y + v.z * gGlassParams.x);
}

float GlassSdf(float3 p) {
    float r = gGlassHalf.w;
    float3 q = abs(p) - (gGlassHalf.xyz - r);
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

// Outward normal of the rounded box near its surface (local space).
float3 GlassNormal(float3 p) {
    float3 q = abs(p) - (gGlassHalf.xyz - gGlassHalf.w);
    float3 m = max(q, 0.0);
    float3 n;
    if (dot(m, m) > 1e-12) {
        n = m;
    } else {   // inside the core box: the nearest face
        n = q.x > q.y && q.x > q.z ? float3(1, 0, 0) : (q.y > q.z ? float3(0, 1, 0) : float3(0, 0, 1));
    }
    return normalize(n * sign(p + 1e-20));
}

// Slab interval of the outer box (local space ray).
bool GlassSlab(float3 o, float3 d, out float t0, out float t1) {
    float3 dd = abs(d) < 1e-8 ? (d >= 0.0 ? 1e-8 : -1e-8) : d;
    float3 inv = 1.0 / dd;
    float3 ta = (-gGlassHalf.xyz - o) * inv, tb = (gGlassHalf.xyz - o) * inv;
    float3 lo = min(ta, tb), hi = max(ta, tb);
    t0 = max(max(lo.x, lo.y), lo.z);
    t1 = min(min(hi.x, hi.y), hi.z);
    return t1 >= max(t0, 0.0);
}

static const float kGlassEps = 2e-4;

// First crossing of the glass surface in (tMin, tMax). `inside` = the ray travels inside the body
// (the crossing is an exit). Normal: world space, outward.
bool GlassIntersect(float3 o, float3 d, float tMin, float tMax, out float tHit, out float3 n, out bool inside) {
    tHit = tMax;
    n = float3(0, 1, 0);
    inside = false;
    if (!GlassOn()) return false;
    float3 lo = GlassToLocal(o), ld = GlassDirToLocal(d);
    float t0, t1;
    if (!GlassSlab(lo, ld, t0, t1)) return false;
    inside = GlassSdf(lo + ld * max(tMin, 0.0)) < 0.0;
    float t;
    bool hit = false;
    if (!inside) {
        t = max(t0, tMin);
        [loop] for (int i = 0; i < 64; ++i) {
            float h = GlassSdf(lo + ld * t);
            if (h < kGlassEps) { hit = true; break; }
            t += h;
            if (t > t1) break;
        }
    } else {
        t = t1;
        [loop] for (int i = 0; i < 64; ++i) {
            float h = GlassSdf(lo + ld * t);
            if (h < kGlassEps) break;
            t -= h;
        }
        hit = true;
    }
    if (!hit || t <= tMin || t >= tMax) return false;
    tHit = t;
    n = GlassDirToWorld(GlassNormal(lo + ld * t));
    return true;
}

#endif
