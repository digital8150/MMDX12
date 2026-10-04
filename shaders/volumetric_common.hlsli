// Single-scattering integrator shared by the real-time (volumetric.hlsl) and offline
// (offline_volumetric.hlsl) volumetric passes. Include common.hlsli (or rt_common.hlsli) first
// and define, before including this file:
//   float VolSunVisibility(float3 wp)                  0..1 sun visibility at wp
//   float VolSpotVisibility(VolLight l, float3 wp)     0..1 visibility of spot / point light l at wp
//
// Medium: exponential height fog sigma(y) = sigma0 * exp(-falloff * max(y, 0)) (scattering =
// extinction, white). Transmittance is integrated in closed form, so every light sample is
// weighted by the exact T(t) and the unshadowed part of the sun term is exact:
//   integral sigma(t) T(t) dt over [a, b] = T(a) - T(b).
// The medium is bounded by a sphere around the stage (VolParams::mediumRadius).
// Light shafts come from the visibility terms: the sun is marched only where shadow data exists
// (shadow-cascade range, or the whole ray with ray queries); each spot light is marched only
// inside its cone (analytic ray / cone intersection), so a beam gets all of its samples
// regardless of the camera distance, and occluders cut dark shafts out of it.
#ifndef MMDX_VOLUMETRIC_COMMON_HLSLI
#define MMDX_VOLUMETRIC_COMMON_HLSLI

struct VolLight {
    float3 pos;   float invRange;
    float3 color; float cosOuter;   // cosOuter <= -1: point light
    float3 dir;   float cosInner;
    float4 pad;                     // x = spot shadow slice (-1 = none)
};
StructuredBuffer<VolLight> gVolLights : register(t2, space1);

float VolSunVisibility(float3 wp);
float VolSpotVisibility(VolLight l, float3 wp);

struct VolParams {
    float sigma0;        // extinction at y <= 0 (per MMD unit)
    float falloff;       // height falloff (per MMD unit)
    float sunG;          // Henyey-Greenstein anisotropy for the sun
    float punctualG;     // ... for spot / point lights
    float spotBoost;     // spot light in-scattering scale
    float pointBoost;    // point light in-scattering scale (fills would only add a uniform veil)
    float sunShadowDist; // the sun is marched over [0, min(dist, sunShadowDist)], analytic beyond
    uint sunSteps;       // max steps over the sun's shadowed segment
    uint spotSteps;      // max steps per spot cone crossing
    uint pointSteps;     // per point light sphere crossing
    float sunStepLen;    // target step length (MMD units); fewer steps on short segments
    float spotStepLen;
    float jitter;        // 0..1 per pixel / frame sample offset
    float3 ambient;      // isotropic in-scattered radiance from the sky dome (already scaled)
    float3 mediumCenter; // the haze fills a sphere around the stage (none outside), so distant
    float mediumRadius;  // scenery and the sky are not fogged out
};

float VolHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Optical depth of the height fog along o + d t, t in [0, t1] (d normalized).
float VolOpticalDepthAbove(float y0, float dy, float t, VolParams p) {
    float base = p.sigma0 * exp(-p.falloff * y0);
    float k = p.falloff * dy;
    return abs(k * t) < 1e-3 ? base * t * (1.0 - 0.5 * k * t) : base * (1.0 - exp(-k * t)) / k;
}
float VolOpticalDepth(float3 o, float3 d, float t, VolParams p) {
    float tc = abs(d.y) > 1e-6 ? -o.y / d.y : -1.0;   // crossing of the y = 0 plane
    if (o.y >= 0.0) {
        if (d.y < 0.0 && tc < t) return VolOpticalDepthAbove(o.y, d.y, tc, p) + p.sigma0 * (t - tc);
        return VolOpticalDepthAbove(o.y, d.y, t, p);
    }
    if (d.y > 0.0 && tc < t) return p.sigma0 * tc + VolOpticalDepthAbove(0.0, d.y, t - tc, p);
    return p.sigma0 * t;
}
float VolSigma(float3 wp, VolParams p) { return p.sigma0 * exp(-p.falloff * max(wp.y, 0.0)); }

// Ray / sphere: [t0, t1] clipped to [0, tMax]; empty when t0 >= t1.
float2 VolRaySphere(float3 o, float3 d, float3 c, float r, float tMax) {
    float3 oc = o - c;
    float b = dot(oc, d);
    float h = b * b - (dot(oc, oc) - r * r);
    if (h <= 0.0) return float2(1, 0);
    h = sqrt(h);
    return float2(max(-b - h, 0.0), min(-b + h, tMax));
}

// Ray / forward cone (apex a, unit axis, cos of the half angle): the parameter interval where the
// ray is inside the cone, clipped to [lo, hi]; empty when x >= y.
float2 VolRayCone(float3 o, float3 d, float3 apex, float3 axis, float cosA, float lo, float hi) {
    float3 co = o - apex;
    float c2 = cosA * cosA;
    float da = dot(d, axis), ca = dot(co, axis);
    float a = da * da - c2;
    float b = 2.0 * (da * ca - dot(d, co) * c2);
    float c = ca * ca - dot(co, co) * c2;
    float t0 = -1e30, t1 = 1e30;   // the inside interval on the double cone (forward nappe picked below)
    float disc = b * b - 4.0 * a * c;
    if (abs(a) < 1e-6) {
        // ray parallel to the cone surface: one root
        if (abs(b) < 1e-8) return float2(1, 0);
        float r = -c / b;
        if (b > 0.0) t0 = r; else t1 = r;
    } else if (disc < 0.0) {
        if (a < 0.0) return float2(1, 0);   // never inside
        // always inside the double cone: the forward nappe test below decides
    } else {
        float sq = sqrt(disc);
        float r0 = (-b - sq) / (2.0 * a), r1 = (-b + sq) / (2.0 * a);
        float rl = min(r0, r1), rh = max(r0, r1);
        if (a < 0.0) {
            t0 = rl; t1 = rh;   // inside between the roots
        } else {
            // inside outside the roots: (-inf, rl] and [rh, inf), one per nappe
            float axialHi = ca + da * (rh + 1.0);
            if (axialHi > 0.0) t0 = rh; else t1 = rl;
        }
    }
    t0 = max(t0, lo);
    t1 = min(t1, hi);
    if (t0 >= t1) return float2(1, 0);
    // forward nappe only (apex side along the axis)
    float tm = 0.5 * (t0 + t1);
    if (ca + da * tm < 0.0) return float2(1, 0);
    return float2(t0, t1);
}

float VolPunctualAtten(VolLight l, float3 wp, out float3 ld) {
    float3 dv = l.pos - wp;
    float dL = length(dv);
    ld = dv / max(dL, 1e-4);
    float x = saturate(1.0 - pow(dL * l.invRange, 4.0));
    float atten = x * x / (1.0 + dL * dL * 0.0004);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
    return atten;
}

// In-scattered radiance (rgb) and transmittance (a) along eye + dir t, t in [0, dist].
float4 IntegrateVolume(float3 eye, float3 dir, float dist, uint numLights, float3 sunDir, float3 sunCol,
                       VolParams p) {
    // clip the ray to the haze volume; t is measured from its entry point from here on
    float2 medium = VolRaySphere(eye, dir, p.mediumCenter, p.mediumRadius, dist);
    if (medium.x >= medium.y) return float4(0, 0, 0, 1);
    eye += dir * medium.x;
    dist = medium.y - medium.x;
    p.sunShadowDist -= medium.x;

    float Tend = exp(-VolOpticalDepth(eye, dir, dist, p));
    float3 L = p.ambient * (1.0 - Tend);

    // ---- sun: shadowed march over the shadow range, analytic (fully lit) tail beyond
    float3 sunScale = sunCol * VolHG(dot(dir, -sunDir), p.sunG);
    float tS = min(dist, p.sunShadowDist);
    if (tS > 0.0 && p.sunSteps > 0) {
        uint n = clamp((uint)ceil(tS / p.sunStepLen), 4u, p.sunSteps);
        float stepLen = tS / (float)n;
        float Tprev = 1.0;
        float sum = 0.0;
        for (uint i = 0; i < n; ++i) {
            float tb = (float)(i + 1) * stepLen;
            float Tb = exp(-VolOpticalDepth(eye, dir, tb, p));
            float vis = VolSunVisibility(eye + dir * ((float)i + p.jitter) * stepLen);
            sum += vis * (Tprev - Tb);
            Tprev = Tb;
        }
        L += sunScale * (sum + (Tprev - Tend));
    } else {
        L += sunScale * (1.0 - Tend);
    }

    // ---- spot cones / point spheres: samples only where the light reaches
    for (uint li = 0; li < numLights; ++li) {
        VolLight l = gVolLights[li];
        float range = 1.0 / max(l.invRange, 1e-6);
        float2 seg = VolRaySphere(eye, dir, l.pos, range, dist);
        bool spot = l.cosOuter > -1.0;
        if (spot && seg.x < seg.y) seg = VolRayCone(eye, dir, l.pos, l.dir, l.cosOuter, seg.x, seg.y);
        if (seg.x >= seg.y) continue;
        uint n = spot ? clamp((uint)ceil((seg.y - seg.x) / p.spotStepLen), 3u, max(p.spotSteps, 3u)) : p.pointSteps;
        if (n == 0) continue;
        float stepLen = (seg.y - seg.x) / (float)n;
        float g = spot ? p.spotBoost : p.pointBoost;
        float3 acc = 0;
        for (uint i = 0; i < n; ++i) {
            float t = seg.x + ((float)i + p.jitter) * stepLen;
            float3 wp = eye + dir * t;
            float3 ld;
            float atten = VolPunctualAtten(l, wp, ld);
            if (atten <= 0.0) continue;
            float vis = spot ? VolSpotVisibility(l, wp) : 1.0;
            float T = exp(-VolOpticalDepth(eye, dir, t, p));
            acc += atten * vis * VolHG(dot(dir, ld), p.punctualG) * VolSigma(wp, p) * T;
        }
        L += l.color * g * acc * stepLen;
    }
    return float4(L, Tend);
}

#endif
