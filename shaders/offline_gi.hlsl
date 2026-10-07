// Offline GI renderer, part 1: irradiance cache + path tracer (see src/render/OfflineRenderer.h for
// the dispatch order). Indirect diffuse at camera hits comes from the irradiance cache built by the
// prepass (Cinema 4D style, see the cache section); everything else is path traced:
//  * Hits the cache does not cover, and every vertex after a mirror bounce, are brute force.
//  * Every vertex takes next-event estimation (soft sun through a cone sample, one punctual light
//    picked by its unshadowed contribution) and continues with a Fresnel-weighted mirror or cosine
//    diffuse bounce, up to kMaxDepth bounces with Russian roulette.
//  * Characters keep the MMD toon look: the camera-facing key light goes through the toon ramp
//    (ToonSun, as in pathtrace.hlsl) and GI adds coloured fill on top. Faces (MAT_FLAT) take an even
//    fill gathered around the view direction that ignores nearby geometry (nose, fringe).
//  * Skin (character texels with a skin tone, SkinWeight) gets subsurface scattering: sun
//    visibility diffused over per-channel scatter radii (soft, red-fringed shadow edges), light
//    leaking past the toon terminator, and translucency of thin backlit parts (ears, fingers)
//    from a traced thickness with per-channel extinction.
//  * Camera: thin lens focused on gP2.y (per-pixel lens samples) and a shutter time gP2.x shared by
//    the whole dispatch: the host rebuilds the character geometry at that time (RtScene::Build) and
//    the camera is interpolated from the shutter-open camera (offline_common.hlsli).
//  * Adaptive sampling: a pixel stops once the standard error of its perceptual luminance drops
//    below a threshold (after a minimum sample count).
//
// UAV table (identical for every kernel in this file):
//   u0 accum         RGBA32F image: radiance sum rgb, a = samples
//   u1 albedo accum  RGBA32F image: primary albedo sum rgb (1 for sky), a = primary hit count
//   u2 moments       RG32F image: sum of t, sum of t^2 (t = perceptual luminance of a sample)
//   u3 gbuffer       RGBA32F image: world normal of sample 0, w = view depth (1e6 = sky)
//   u4 counter       R32_UINT 1x1: pixels that traced in this dispatch (CSRender adds)
//   u5 outline accum RGBA32F image: sum of the per-iteration outline layers (only cleared here)
// Root constants:
//   gP0 = (seed, studioFloor, sampleIndex, minSamples)
//   gP1 = (imageWidth, imageHeight, 0, error threshold)
//   gP2 = (shutter time 0..1, focus distance (view z, <= 0: pinhole), lens radius, 0)
#include "rt_common.hlsli"   // includes common.hlsli
#include "offline_common.hlsli"
#include "offline_glass.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);
#ifdef MMDX_PT_PACK
#include "pt_pack_api.hlsli"
#include "pt_pack_glue.hlsli"
#include MMDX_PT_PACK
#endif

RWTexture2D<float4> gAccum : register(u0);
RWTexture2D<float4> gAlbedoAccum : register(u1);
RWTexture2D<float2> gMoments : register(u2);
RWTexture2D<float4> gGbuffer : register(u3);
RWTexture2D<uint> gCounter : register(u4);
RWTexture2D<float4> gEdgeAccum : register(u5);

struct PtLight { float3 pos; float invRange; float3 color; float cosOuter; float3 dir; float cosInner; float4 pad; };
StructuredBuffer<PtLight> gPtLights : register(t2, space1);

static const float kSunCosMax = 0.99978;      // ~1.2 degree sun: soft, contact-hardening shadows
static const float kFloorExtent = 900.0;      // same as mmd.hlsl
static const float kAmbientEmission = 0.7;    // stage ambient emitted as light (raster unlit stage ~0.62 of lit)
static const float kToonIndirect = 0.6;       // GI fill on characters, on top of the toon key light
static const float kFlatGatherSkip = 1.5;     // faces: fill rays ignore geometry closer than this
static const uint kMaxDepth = 12u;
static const float kFirefly = 24.0;           // per-sample luminance clamp
static const float kHaze = 0.35;
// Skin SSS (MMD units: 1 ~ 8 cm). Scatter radii diffuse the incident sun (red travels furthest);
// transmission depths drive the translucency of thin parts.
static const float3 kSkinRadius = float3(0.16, 0.07, 0.04);
static const float3 kSkinDepth = float3(0.18, 0.06, 0.035);
static const float3 kSkinScatterTint = float3(1.0, 0.36, 0.22);  // light leaking past the terminator
static const float kSkinThicknessMax = 0.8;     // thicker than this transmits nothing
static const float kSkinTranslucency = 0.6;              // aerial perspective, the raster default fog (composite.hlsl)

float3 SunIrradiance() { return SrgbToLinear(gLightColor) * (PI * 3.0) * gSunIntensity; }

// Radiance of rays that leave the scene. With the studio floor the world below the horizon is
// the faded-out cyclorama, i.e. the horizon colour (mmd.hlsl PSFloor fades to it).
float3 Background(float3 d) {
    if (gP0.y > 0.5 && d.y < 0.0) return SkyColor(normalize(float3(d.x, 0.0, d.z) + float3(0, 1e-4, 0)));
    return SkyColor(d);
}

float Perceptual(float3 c) {
    float l = Luminance(max(c, 0.0));
    return pow(l / (1.0 + l), 1.0 / 2.2);
}

// Uniform unit-disk sample (Shirley-Chiu concentric map).
float2 ConcentricDisk(float2 u) {
    float2 o = u * 2.0 - 1.0;
    if (o.x == 0.0 && o.y == 0.0) return 0.0;
    float r, phi;
    if (abs(o.x) > abs(o.y)) {
        r = o.x;
        phi = (PI / 4.0) * (o.y / o.x);
    } else {
        r = o.y;
        phi = (PI / 2.0) - (PI / 4.0) * (o.x / o.y);
    }
    return r * float2(cos(phi), sin(phi));
}

// ---- analytic props: glass box and softboxes (render benchmark scene) ------------------------
// The glass box (geometry in offline_glass.hlsli) is a smooth dielectric with dispersion (one hero
// colour channel per path once it refracts) and Beer-Lambert absorption inside.

float FresnelDielectric(float cosI, float eta) {   // eta = n(transmitted side) / n(incident side)
    float sinT2 = (1.0 - cosI * cosI) / (eta * eta);
    if (sinT2 >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sinT2);
    float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// Index of refraction for the path's hero channel (-1: none chosen yet, green).
float GlassIor(int ch) { return gGlassParams.z + gGlassParams.w * 0.5 * (float)((ch < 0 ? 1 : ch) - 1); }

// Smooth dielectric scattering at a glass crossing (n faces the incoming ray): Fresnel-weighted
// choice between mirror reflection and refraction. The first refraction of a path picks its hero
// colour channel (dispersion): the throughput keeps that channel only, x3.
float3 GlassScatter(float3 d, float3 n, bool inside, inout int ch, inout float3 T, inout uint rng, out bool refracted) {
    refracted = false;
    float cosI = saturate(dot(-d, n));
    float eta = inside ? 1.0 / GlassIor(ch) : GlassIor(ch);
    if (Rand(rng) < FresnelDielectric(cosI, eta)) return reflect(d, n);
    if (ch < 0 && gGlassParams.w > 0.0) {
        ch = min((int)(Rand(rng) * 3.0), 2);
        T *= float3(ch == 0, ch == 1, ch == 2) * 3.0;
        eta = inside ? 1.0 / GlassIor(ch) : GlassIor(ch);
    }
    float3 t = refract(d, n, 1.0 / eta);
    if (dot(t, t) < 1e-8) return reflect(d, n);   // total internal reflection
    refracted = true;
    return normalize(t);
}

// Transparent shadow of the glass along a shadow ray: interface transmission at both crossings and
// Beer-Lambert absorption over the chord (the light is attenuated, not bent).
float3 GlassShadow(float3 o, float3 d, float tMax) {
    if (!GlassOn()) return 1.0;
    float tE, tX;
    float3 nE, nX;
    bool inE, inX;
    float3 tr = 1.0;
    if (!GlassIntersect(o, d, 0.0, tMax, tE, nE, inE)) return 1.0;
    float ior = gGlassParams.z;
    if (inE) {   // starts inside: only the exit
        tr *= 1.0 - FresnelDielectric(saturate(dot(d, nE)), 1.0 / ior);
        return tr * exp(-gGlassAbsorb.xyz * tE);
    }
    tr *= 1.0 - FresnelDielectric(saturate(-dot(d, nE)), ior);
    float3 p = o + d * (tE + kGlassEps * 4.0);
    if (GlassIntersect(p, d, 0.0, max(tMax - tE, 0.0), tX, nX, inX)) {
        tr *= 1.0 - FresnelDielectric(saturate(dot(d, nX)), 1.0 / ior);
        tr *= exp(-gGlassAbsorb.xyz * tX);
    } else {
        tr *= exp(-gGlassAbsorb.xyz * max(tMax - tE, 0.0));
    }
    return tr;
}

// Closest softbox (one-sided emissive rectangle, emitting on the side of cross(U, V)) in (tMin, tMax).
bool SoftboxIntersect(float3 o, float3 d, float tMin, float tMax, out float tHit, out float3 L) {
    tHit = tMax;
    L = 0;
    bool hit = false;
    [unroll] for (int i = 0; i < 2; ++i) {
        float4 c = gSoftbox[i * 4];
        if (c.w < 0.5) continue;
        float3 U = gSoftbox[i * 4 + 1].xyz, V = gSoftbox[i * 4 + 2].xyz;
        float3 n = cross(U, V);
        float dn = dot(d, n);
        if (dn >= 0.0) continue;
        float th = dot(c.xyz - o, n) / dn;
        if (th <= tMin || th >= tHit) continue;
        float3 p = o + d * th - c.xyz;
        if (abs(dot(p, U)) > dot(U, U) || abs(dot(p, V)) > dot(V, V)) continue;
        tHit = th;
        L = gSoftbox[i * 4 + 3].xyz;
        hit = true;
    }
    return hit;
}

// ---- scene ------------------------------------------------------------------------------

struct Surf {
    float3 pos, n, faceN;   // normals face the incoming ray
    float3 albedo;          // linear
    float3 emission;        // linear radiance (stage ambient, softboxes)
    float refl, rough;
    bool character, flat, receive;
    bool glass, inside;     // glass crossing; inside = leaving the glass body
    bool emitter;           // softbox: the path ends here with `emission`
    RtGeometry g;           // undefined for the studio floor and the props
    float4 tex;
    float2 uv;
};

// Closest surface along a ray: meshes (stochastic alpha), the analytic studio floor, the glass box
// (unless skipGlass: the prepass sees straight through it) and the softboxes.
bool TraceScene(float3 o, float3 d, float tMin, inout uint rng, float lod, out Surf s, out float t,
                bool skipGlass = false) {
    s = (Surf)0;
    t = 1e5;
    float tMax = 1e5;
    bool floorHit = false;
    float tProp;
    float3 propN, propL;
    bool propInside;
    int prop = 0;   // 1 glass, 2 softbox
    if (!skipGlass && GlassIntersect(o, d, tMin, tMax, tProp, propN, propInside)) {
        tMax = tProp;
        prop = 1;
    }
    float tBox;
    if (SoftboxIntersect(o, d, tMin, tMax, tBox, propL)) {
        tMax = tBox;
        tProp = tBox;
        prop = 2;
    }
    if (gP0.y > 0.5 && d.y < -1e-5) {
        float tf = -o.y / d.y;
        // The floor dissolves into the sky with distance like the raster cyclorama (mmd.hlsl
        // PSFloor): stochastic coverage, so camera rays and GI see the same seamless fade.
        float r = length((o + d * tf).xz);
        if (tf > tMin && tf < tMax && r < kFloorExtent && Rand(rng) >= smoothstep(260.0, 820.0, r)) {
            tMax = tf;
            floorHit = true;
        }
    }
    RtHit hit;
    bool mesh = TraceClosest(o, d, tMin, tMax, max(Rand(rng), 0.004), hit);
    if (!mesh && !floorHit && prop == 0) return false;
    if (!mesh && !floorHit) {
        t = tProp;
        s.pos = o + d * t;
        if (prop == 2) {
            s.emitter = true;
            s.emission = propL;
            s.n = -d;
            s.faceN = -d;
            return true;
        }
        s.glass = true;
        s.inside = propInside;
        s.n = dot(propN, d) > 0.0 ? -propN : propN;   // toward the incoming ray
        s.faceN = s.n;
        s.albedo = 1.0;
        s.refl = 1.0;
        return true;
    }
    if (mesh) {
        t = hit.t;
        RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
        RtSurface sf = FetchSurface(g, hit.prim, hit.bary);
        if (dot(sf.faceNormal, d) > 0) sf.faceNormal = -sf.faceNormal;
        if (dot(sf.normal, sf.faceNormal) < 0) sf.normal = -sf.normal;
        float4 tex = SampleBaseTexture(g, sf.uv, lod);
        s.g = g;
        s.tex = tex;
        s.uv = sf.uv;
        s.pos = sf.pos;
        s.n = sf.normal;
        s.faceN = sf.faceNormal;
        s.albedo = SrgbToLinear(saturate(MaterialAlbedo(g, tex.rgb)));
        s.character = (g.flags & MAT_STAGE) == 0;
        s.flat = (g.flags & MAT_FLAT) != 0;
        s.receive = (g.flags & MAT_RECEIVE) != 0;
        float r = g.reflectivity;
        if (!s.character) r = saturate(r + gFloorGloss * smoothstep(0.82, 0.97, sf.normal.y));
        s.refl = r;
        s.rough = clamp(sqrt(2.0 / (g.specularPower + 2.0)), 0.03, 0.6);
        s.emission = s.character ? 0.0 : SrgbToLinear(saturate(g.ambient * tex.rgb)) * gSunIntensity * kAmbientEmission;
    } else {
        t = tMax;
        s.pos = o + d * t;
        s.n = float3(0, 1, 0);
        s.faceN = s.n;
        float r = length(s.pos.xz);
        const bool custom = gFloorParams.w >= 0.0;
        s.albedo = lerp(custom ? gFloorParams.xyz : float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
        s.refl = (custom ? gFloorParams.w : 0.42) * (1.0 - smoothstep(120.0, 600.0, r));
        s.rough = 0.12;
        s.receive = true;
    }
    return true;
}

// Camera ray through `pix` (pixel units) at this dispatch's shutter time, from a point on the lens.
void CameraRay(float2 pix, float2 lensU, out float3 o, out float3 d) {
    float2 uv = pix * gInvViewportSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 v = mul(float4(ndc, 1.0, 1.0), gInvProj);
    float3 vd = normalize(v.xyz / v.w);
    float3x3 basis;
    float3 eye;
    OfflineCameraAt(gP2.x, basis, eye);
    float3 lens = 0;
    if (gP2.y > 0.0 && gP2.z > 0.0) {
        lens = float3(ConcentricDisk(lensU) * gP2.z, 0.0);
        vd = normalize(vd * (gP2.y / vd.z) - lens);   // toward the in-focus point
    }
    o = eye + mul(lens, basis);
    d = normalize(mul(vd, basis));
}

// ---- lighting ---------------------------------------------------------------------------

// Unshadowed irradiance from punctual light `li` (pi * colour * attenuation * N.L, so that Lambert
// albedo / pi * E matches the raster albedo * colour * attenuation * N.L). Characters use the raster
// soft toon terminator (faces: almost no N.L).
float3 PunctualUnshadowed(Surf s, uint li, bool toon, out float3 ld, out float dist) {
    PtLight l = gPtLights[li];
    float3 d = l.pos - s.pos;
    dist = length(d);
    ld = d / max(dist, 1e-4);
    float x = saturate(1.0 - pow(dist * l.invRange, 4.0));
    float atten = x * x / (1.0 + dist * dist * 0.0004);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
    float ndl = dot(s.n, ld);
    float flat = s.flat ? 1.0 : 0.0;
    float diff = toon ? lerp(smoothstep(-0.05, 0.25, ndl), saturate(ndl * 0.3 + 0.7), flat) : ndl;
    if (atten <= 0.0 || diff <= 0.0) return 0.0;
    return PI * l.color * atten * diff;
}

// Punctual irradiance with a single shadow ray: one light is picked in proportion to its unshadowed
// contribution (unbiased; lights out of range or outside their cone cost nothing). Preset lights
// hang on a virtual truss outside the stage, so only characters occlude them (pathtrace.hlsl).
float3 PunctualIrradiance(Surf s, bool toon, inout uint rng) {
    uint nl = (uint)gNumLights;
    float total = 0.0;
    for (uint i = 0; i < nl; ++i) {
        float3 ld;
        float dist;
        total += Luminance(PunctualUnshadowed(s, i, toon, ld, dist));
    }
    if (total <= 0.0) return 0.0;
    float u = Rand(rng) * total;
    for (uint j = 0; j < nl; ++j) {
        float3 ld;
        float dist;
        float3 e = PunctualUnshadowed(s, j, toon, ld, dist);
        float w = Luminance(e);
        if (w <= 0.0) continue;
        u -= w;
        if (u <= 0.0 || j == nl - 1u) {
            float3 po = OffsetRayOrigin(s.pos, s.faceN);
            float3 vis = TraceShadowRayMasked(po, ld, max(dist - 0.05, 0.0), RT_MASK_CHARACTER) *
                         GlassShadow(po, ld, max(dist - 0.05, 0.0));
            return e * (total / w) * vis;
        }
    }
    return 0.0;
}

// Sun visibility (soft cone sample), tinted by the glass it passes through.
float3 SunVisibility(Surf s, inout uint rng) {
    if (!s.receive) return 1.0;
    float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, kSunCosMax);
    float3 po = OffsetRayOrigin(s.pos, s.faceN);
    float v = TraceShadowRay(po, sd, 1e5);
    return v > 0.0 ? GlassShadow(po, sd, 1e5) : 0.0;
}

// Lambert irradiance from the sun and the punctual lights (one shadow ray each).
float3 DirectIrradiance(Surf s, inout uint rng) {
    float3 E = 0;
    float3 L = -gLightDir;
    float ndl = dot(s.n, L);
    if (ndl > 0.0 && dot(s.faceN, L) > 0.0) E += SunIrradiance() * ndl * SunVisibility(s, rng);
    if (gNumLights >= 1.0) E += PunctualIrradiance(s, false, rng);
    return E;
}

// ---- skin subsurface scattering -----------------------------------------------------------

// 0..1 skin likelihood of a character texel from its fully lit colour (gamma space): warm
// low-saturation tones with r >= g >= b (peach to pale pink). Eyes, brows, hair and clothes in the
// same texture atlas fall outside the range.
float SkinWeight(Surf s) {
    if (!s.character) return 0.0;
    float3 c = saturate(s.g.ambient + s.g.diffuse.rgb) * s.tex.rgb;
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    if (mx < 0.3 || c.r < c.g || c.g < c.b - 0.02) return 0.0;
    float sat = (mx - mn) / mx;
    float hue = 60.0 * (c.g - c.b) / max(mx - mn, 1e-4);   // degrees within the red sector
    float w = smoothstep(0.04, 0.10, sat) * (1.0 - smoothstep(0.45, 0.60, sat));
    w *= smoothstep(-3.0, 5.0, hue) * (1.0 - smoothstep(30.0, 40.0, hue));
    return w * smoothstep(0.30, 0.50, mx);
}

// Sun visibility per colour channel for skin: each channel's shadow ray starts from a point
// jittered on the surface within that channel's scatter radius, so the averaged image shows the
// incident light diffused under the skin (soft, red-fringed shadow edges).
float3 SkinSunVisibility(Surf s, float skin, inout uint rng) {
    if (!s.receive) return 1.0;
    float3 t, b;
    BuildBasis(s.faceN, t, b);
    float3 vis;
    [unroll] for (int c = 0; c < 3; ++c) {
        float r = kSkinRadius[c] * skin * sqrt(Rand(rng));
        float phi = 6.2831853 * Rand(rng);
        float3 p = s.pos + (t * cos(phi) + b * sin(phi)) * r;
        float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, kSunCosMax);
        float3 po = OffsetRayOrigin(p, s.faceN);
        vis[c] = TraceShadowRay(po, sd, 1e5);
        if (vis[c] > 0.0) vis[c] *= GlassShadow(po, sd, 1e5)[c];
    }
    return vis;
}

// Closest character surface along a ray from inside the mesh (back faces included): the exit
// distance of light passing through a thin part. Returns -1 when nothing is hit within tMax.
float ExitDistance(float3 origin, float3 dir, float tMax) {
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, RT_MASK_CHARACTER, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= 0.5)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
}

// Light arriving from `dir` (unit, toward the light) through the skin behind the shaded point:
// per-channel transmittance exp(-thickness / depth) times the light's visibility at the exit point.
float3 SkinTransmittance(Surf s, float3 dir, float lightDist) {
    float3 inside = s.pos - s.faceN * 0.004;   // just beneath the surface
    float d = ExitDistance(inside, dir, kSkinThicknessMax);
    if (d < 0.0) return 0.0;
    float3 trans = exp(-d / kSkinDepth);
    if (Luminance(trans) < 1e-3) return 0.0;
    float3 exitP = inside + dir * (d + 0.01);
    float vis = TraceShadowRay(exitP, dir, max(lightDist - d, 0.0));
    return vis > 0.0 ? trans * GlassShadow(exitP, dir, max(lightDist - d, 0.0)) : 0.0;
}

// Translucency of thin skin (ears, fingers) lit from behind by the sun and one punctual light
// (picked uniformly), as radiance toward the camera.
float3 SkinTranslucency(Surf s, float skin, inout uint rng) {
    float3 c = 0;
    float3 L = -gLightDir;
    if (dot(s.faceN, L) < 0.2) {
        float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), L, kSunCosMax);
        c += SunIrradiance() * SkinTransmittance(s, sd, 1e5);
    }
    if (gNumLights >= 1.0) {
        uint nl = (uint)gNumLights;
        uint li = min((uint)(Rand(rng) * (float)nl), nl - 1u);
        PtLight l = gPtLights[li];
        float3 d = l.pos - s.pos;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float x = saturate(1.0 - pow(dist * l.invRange, 4.0));
        float atten = x * x / (1.0 + dist * dist * 0.0004);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        if (atten > 0.0 && dot(s.faceN, ld) < 0.2)
            c += PI * l.color * atten * (float)nl * SkinTransmittance(s, ld, dist);
    }
    return s.albedo / PI * c * kSkinTranslucency * skin;
}

// Character materials keep the raster toon model (mmd.hlsl PSMain) for the sun, with the traced
// soft-shadow visibility `sh`. Same as pathtrace.hlsl ToonSun. Linear output.
float3 ToonSun(Surf s, float3 V, float3 sh, float skin) {
    RtGeometry g = s.g;
    float3 n = s.n;
    float3 L = -gLightDir;
    float3 lit = saturate(g.ambient + g.diffuse.rgb * gLightColor) * s.tex.rgb;
    if (g.flags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 sp = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.sphereSrv)].SampleLevel(gLinear, suv, 0).rgb, g.sphereMul, g.sphereAdd, (g.flags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
        if (g.flags & MAT_SPHERE_MUL) lit *= sp; else lit += sp;
    }
    float ndl = dot(n, L);
    float flat = s.flat ? 1.0 : 0.0;
    float3 c;
    if (g.flags & MAT_TOON_MAP) {
        float3 shadowTex = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gRtWrap, s.uv, 0).rgb, g.toonMul, g.toonAdd, 1.0);
        float3 term = sh * lerp(smoothstep(-0.03, 0.06, ndl), 1.0, flat);
        c = lerp(saturate(g.ambient + g.diffuse.rgb * gLightColor) * shadowTex, lit, term);
    } else if (g.flags & MAT_HAS_TOON) {
        // cast shadows push the lookup to the dark end of the ramp (per channel for skin)
        Texture2D toon = gBindlessTex[NonUniformResourceIndex(g.toonSrv)];
        float3 rampLit = ApplyTexFactor3(toon.SampleLevel(gLinear, float2(0.5, saturate(0.5 - 0.5 * ndl)), 0).rgb, g.toonMul, g.toonAdd, 1.0);
        float3 rampShadow = ApplyTexFactor3(toon.SampleLevel(gLinear, float2(0.5, 1.0), 0).rgb, g.toonMul, g.toonAdd, 1.0);
        c = lit * lerp(rampShadow, rampLit, sh);
    } else {
        c = lit;
    }
    if (!(g.flags & MAT_TOON_MAP)) {
        float3 term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        // the shaded side of skin stays warm (light scattered through it), other materials cool
        float3 tint = lerp(float3(0.90, 0.90, 0.96), float3(1.0, 0.88, 0.84), skin);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * tint;
        c = lerp(shade, c, term);
        // light leaking past the terminator into the shadow side
        float band = smoothstep(-0.35, -0.02, ndl) * (1.0 - smoothstep(-0.02, 0.25, ndl)) * (1.0 - flat);
        c += lit * kSkinScatterTint * band * skin * 0.3 * sh;
    }
    if (g.specularPower > 0.0)
        c += pow(saturate(dot(normalize(L + V), n)), g.specularPower) * g.specular * gLightColor * sh;
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
    float side = smoothstep(-0.2, 0.5, ndl + 0.25);
    color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    return color;
}

// Glass crossing seen by the camera (or a mirror): delta lights have no mirror image in a smooth
// dielectric, so the sun and the spots add a tight normalized Blinn-Phong glint weighted by Fresnel.
float3 GlassGlint(Surf s, float3 V, inout uint rng) {
    const float e = 1200.0, norm = (e + 8.0) / (8.0 * PI);
    float3 c = 0;
    float3 L = -gLightDir;
    float3 h = normalize(L + V);
    float ndl = dot(s.n, L);
    if (ndl > 0.0) {
        float spec = norm * pow(saturate(dot(s.n, h)), e) * FresnelDielectric(saturate(dot(h, V)), gGlassParams.z);
        if (spec > 1e-3) {
            float3 po = OffsetRayOrigin(s.pos, s.faceN);
            c += spec * SunIrradiance() * ndl * TraceShadowRay(po, L, 1e5);
        }
    }
    uint nl = (uint)gNumLights;
    for (uint i = 0; i < nl; ++i) {
        PtLight l = gPtLights[i];
        float3 dl = l.pos - s.pos;
        float dist = length(dl);
        float3 ld = dl / max(dist, 1e-4);
        float x = saturate(1.0 - pow(dist * l.invRange, 4.0));
        float atten = x * x / (1.0 + dist * dist * 0.0004);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        float nl2 = dot(s.n, ld);
        if (atten <= 0.0 || nl2 <= 0.0) continue;
        float3 hh = normalize(ld + V);
        c += norm * pow(saturate(dot(s.n, hh)), e) * FresnelDielectric(saturate(dot(hh, V)), gGlassParams.z) *
             PI * l.color * atten * nl2;
    }
    return c;
}

// Direct light toward the camera (or a mirror): toon key for characters, Lambert sun for the
// stage, the punctual lights. `pSpec` is the probability of the specular continuation.
float3 CameraDirect(Surf s, float3 V, float pSpec, inout uint rng) {
    float3 c = s.emission;
    if (s.character) {
        float skin = SkinWeight(s);
        if (skin > 0.01) {
            c += ToonSun(s, V, SkinSunVisibility(s, skin, rng), skin);
            c += SkinTranslucency(s, skin, rng);
        } else {
            c += ToonSun(s, V, SunVisibility(s, rng), 0.0);
        }
    } else {
        float3 L = -gLightDir;
        float ndl = dot(s.n, L);
        if (ndl > 0.0 && dot(s.faceN, L) > 0.0)
            c += (1.0 - pSpec) * s.albedo / PI * SunIrradiance() * ndl * SunVisibility(s, rng);
    }
    if (gNumLights >= 1.0) c += (1.0 - pSpec) * s.albedo / PI * PunctualIrradiance(s, s.character, rng);
    return c;
}

// Faces (MAT_FLAT): even environment fill gathered around the view direction, ignoring nearby
// geometry (nose, fringe), like the raster flat fill but lit by the scene.

// ---- irradiance cache (Cinema 4D-style IC prepass, used by the render) --------------------------
// Six passes place irradiance samples on screen grids from coarse to fine (stride halves per pass).
// A point at a finer level gets its own sample only where the geometry changes (normal / depth
// against the parent level) or the parent irradiance varies; elsewhere it inherits the parent's
// interpolated value. Every sample is the INDIRECT irradiance of its surface point, gathered with
// multi-bounce diffuse paths (faces: the even fill around the view direction). An edge-aware smoothing
// of the finest level gives the cache CSRender reads: indirect diffuse at camera hits comes from it
// (looked up by projecting the hit into the frame's camera, so it serves the lens / shutter samples
// too); direct light, reflections, depth of field and motion blur stay per-pixel ray traced, and hits
// the cache does not cover fall back to brute-force paths.
//   CSPrepassLevel   gP0 = (seed, studioFloor, level, stride), gP1 = (w, h, parent stride, rays)
//                    t0..t5 level indirect irradiance (a = 1 where sampled), t6 parent level geometry
//                    u6 this level's irradiance, u7 this level's geometry (oct view normal, view z, hit)
//   CSPrepassDisplay gP0 = (seed, studioFloor, level, stride), gP1 = (w, h, 0, 0), gP3.x = dot radius
//                    t0..t5 level irradiance, t6 this level's geometry; writes u0 (accum, 1 sample):
//                    the scene lit by direct light + the cache, sample points as white dots
//   CSPrepassSmooth  t5 finest level irradiance, t6 its geometry; u6 = the final cache
//   CSRender         t6 finest level geometry, t7 final cache, gP2.w = 1 when the cache is valid,
//                    gP3.x = max path depth (0: kMaxDepth), gP3.y = finest level stride
Texture2D<float4> gLevelE[6] : register(t0);
Texture2D<float4> gLevelG : register(t6);
Texture2D<float4> gIcFinal : register(t7);
RWTexture2D<float4> gPreE : register(u6);
RWTexture2D<float4> gPreG : register(u7);

static const uint kGatherDepth = 4u;   // diffuse bounces of a cache sample's gather paths

// Radiance arriving along a ray from diffusely lit surroundings: a brute-force diffuse path with
// next-event estimation at every vertex (Russian roulette from the second bounce).
float3 GatherPath(float3 o, float3 d, float tMin, inout uint rng) {
    float3 L = 0, T = 1;
    int ch = -1;
    uint glassEvents = 0u;
    [loop] for (uint depth = 0; depth < kGatherDepth; ++depth) {
        Surf y;
        float t;
        if (!TraceScene(o, d, depth == 0u ? tMin : 0.0, rng, 1.0, y, t)) {
            L += T * Background(d);
            break;
        }
        if (y.emitter) {
            L += T * y.emission;
            break;
        }
        if (y.glass) {   // crossings do not count as diffuse bounces (at most 8 per path)
            if (y.inside) T *= exp(-gGlassAbsorb.xyz * t);
            bool refr;
            d = GlassScatter(d, y.n, y.inside, ch, T, rng, refr);
            o = OffsetRayOrigin(y.pos, refr ? -y.faceN : y.faceN);
            if (++glassEvents < 8u) --depth;
            continue;
        }
        L += T * (y.emission + y.albedo / PI * DirectIrradiance(y, rng));
        float3 nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), y.n);
        if (dot(nd, y.faceN) <= 0.0) break;
        T *= y.albedo;
        o = OffsetRayOrigin(y.pos, y.faceN);
        d = nd;
        if (depth >= 1u) {
            float q = min(max(T.r, max(T.g, T.b)), 0.95);
            if (Rand(rng) > q) break;
            T /= q;
        }
    }
    return L;
}

// Irradiance at `q` (pixel units) interpolated from a grid (stride s) with geometry-aware bilinear
// weights; `cover` = bilinear weight on samples of the same surface (0..1).
float3 InterpolateGrid(Texture2D<float4> etex, float s, float2 q, float3 n, float z, float normalMin,
                       float depthTol, out float cover, out float lmin, out float lmax, out bool discontinuity) {
    uint lw, lh;
    gLevelG.GetDimensions(lw, lh);
    float2 pc = q / s - 0.5;
    int2 p0 = (int2)floor(pc);
    float2 f = pc - (float2)p0;
    float3 E = 0;
    lmin = 1e9;
    lmax = 0;
    discontinuity = false;
    cover = 0;
    [unroll] for (int c = 0; c < 4; ++c) {
        int2 off = int2(c & 1, c >> 1);
        int2 pp = clamp(p0 + off, int2(0, 0), int2(lw - 1, lh - 1));
        float4 g = gLevelG[pp];
        float wb = (off.x ? f.x : 1.0 - f.x) * (off.y ? f.y : 1.0 - f.y) + 1e-4;
        bool same = g.w > 0.5 && dot(OctDecode(g.xy), n) > normalMin && abs(g.z - z) < depthTol * z;
        if (!same) {
            discontinuity = true;
            continue;
        }
        float3 e = etex[pp].rgb;
        float l = Luminance(e);
        lmin = min(lmin, l);
        lmax = max(lmax, l);
        E += e * wb;
        cover += wb;
    }
    return cover > 1e-3 ? E / cover : 0.0;
}

// Indirect irradiance of a camera hit from the final cache (false when not covered).
bool IcLookup(Surf s, out float3 E) {
    E = 0;
    float4 c = mul(float4(s.pos, 1.0), gViewProjNoJitter);
    if (c.w <= 0.0) return false;
    float2 ndc = c.xy / c.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0)) return false;
    float3 vn = normalize(mul(s.n, (float3x3)gView));
    float z = mul(float4(s.pos, 1.0), gView).z;
    float cover, lmin, lmax;
    bool disc;
    E = InterpolateGrid(gIcFinal, gP3.y, uv * gViewportSize, vn, z, 0.85, 0.03, cover, lmin, lmax, disc);
    return cover > 0.5;
}

[numthreads(8, 8, 1)]
void CSPrepassLevel(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)gP1.x, H = (uint)gP1.y;
    uint level = (uint)gP0.z;
    float s = gP0.w, ps = gP1.z;
    uint2 dims = (uint2(W, H) + (uint)s - 1u) / (uint)s;
    if (id.x >= dims.x || id.y >= dims.y) return;
    float2 q = min((float2)id.xy * s + s * 0.5, float2(W, H) - 0.5);
    uint rng = RngSeed(id.xy, (uint)gP0.x, 17u);
    float3 o, d;
    CameraRay(q, float2(0.5, 0.5), o, d);
    Surf sf;
    float t;
    if (!TraceScene(o, d, gNearZ, rng, 0.0, sf, t, true) || sf.emitter) {
        gPreG[id.xy] = float4(0, 0, 1e6, 0);
        gPreE[id.xy] = 0;
        return;
    }
    float3 vn = normalize(mul(sf.n, (float3x3)gView));
    float z = mul(float4(sf.pos, 1.0), gView).z;
    gPreG[id.xy] = float4(OctEncode(vn), z, 1.0);

    bool need = level == 0u;
    float3 Ei = 0;
    if (!need) {
        float cover, lmin, lmax;
        bool disc;
        Ei = InterpolateGrid(gLevelE[level - 1u], ps, q, vn, z, 0.75, 0.06, cover, lmin, lmax, disc);
        if (cover <= 1e-3 || disc || lmax > lmin * 1.8 + 0.05) need = true;
    }
    if (!need) {
        gPreE[id.xy] = float4(Ei, 0.0);
        return;
    }
    // indirect irradiance: multi-bounce diffuse gather (faces: around the view direction, nearby
    // geometry skipped, like the render's face fill)
    bool face = sf.character && sf.flat;
    float3 axis = face ? -d : sf.n;
    float tMin = face ? kFlatGatherSkip : 0.0;
    uint rays = max((uint)gP1.w, 1u);
    float3 ind = 0;
    [loop] for (uint i = 0; i < rays; ++i) {
        float3 dir = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), axis);
        if (!face && dot(dir, sf.faceN) <= 0.0) continue;
        ind += GatherPath(OffsetRayOrigin(sf.pos, sf.faceN), dir, tMin, rng);
    }
    gPreE[id.xy] = float4(PI * ind / (float)rays, 1.0);
}

[numthreads(8, 8, 1)]
void CSPrepassDisplay(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)gP1.x, H = (uint)gP1.y;
    if (id.x >= W || id.y >= H) return;
    uint level = (uint)gP0.z;
    float s = gP0.w;
    float2 q = (float2)id.xy + 0.5;
    uint rng = RngSeed(id.xy, (uint)gP0.x, 19u);
    float3 o, d;
    CameraRay(q, float2(0.5, 0.5), o, d);
    Surf sf;
    float t;
    float3 color;
    if (!TraceScene(o, d, gNearZ, rng, 0.0, sf, t, true)) {
        color = Background(d);
    } else if (sf.emitter) {
        color = sf.emission;
    } else {
        float3 vn = normalize(mul(sf.n, (float3x3)gView));
        float z = mul(float4(sf.pos, 1.0), gView).z;
        float cover, lmin, lmax;
        bool disc;
        float3 E = InterpolateGrid(gLevelE[level], s, q, vn, z, 0.75, 0.06, cover, lmin, lmax, disc);
        if (cover <= 1e-3) {
            uint lw, lh;
            gLevelG.GetDimensions(lw, lh);
            E = gLevelE[level][min((uint2)(q / s), uint2(lw - 1, lh - 1))].rgb;   // nearest sample
        }
        color = sf.emission + sf.albedo / PI * (DirectIrradiance(sf, rng) + E);
    }
    // sample points of every finished level as white dots
    [loop] for (uint l = 0; l <= level; ++l) {
        float sl = s * (float)(1u << (level - l));
        uint lw, lh;
        gLevelE[l].GetDimensions(lw, lh);
        uint2 cell = min((uint2)(q / sl), uint2(lw - 1, lh - 1));
        float2 c = min((float2)cell * sl + sl * 0.5, float2(W, H) - 0.5);
        float radius = clamp(0.22 * sl, 0.75, gP3.x);   // finer passes leave smaller specks
        if (length(q - c) < radius && gLevelE[l][cell].a > 0.5) color = float3(1.4, 1.4, 1.4);
    }
    gAccum[id.xy] = float4(color, 1.0);
}

// Edge-aware smoothing of the finest level (the cache's interpolation noise), into the final cache.
[numthreads(8, 8, 1)]
void CSPrepassSmooth(uint3 id : SV_DispatchThreadID) {
    uint lw, lh;
    gLevelG.GetDimensions(lw, lh);
    if (id.x >= lw || id.y >= lh) return;
    float4 g0 = gLevelG[id.xy];
    float4 e0 = gLevelE[5][id.xy];
    if (g0.w < 0.5) {
        gPreE[id.xy] = e0;
        return;
    }
    float3 n0 = OctDecode(g0.xy);
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            int2 p = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(lw - 1, lh - 1));
            float4 g = gLevelG[p];
            if (g.w < 0.5) continue;
            float w = exp(-(float)(dx * dx + dy * dy) / 4.5) * pow(saturate(dot(n0, OctDecode(g.xy))), 8.0) *
                      exp(-abs(g.z - g0.z) / (0.02 * g0.z + 1e-3));
            sum += gLevelE[5][p].rgb * w;
            wsum += w;
        }
    }
    gPreE[id.xy] = float4(sum / max(wsum, 1e-6), e0.a);
}

// ---- kernels ----------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSClearImage(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gP1.x || id.y >= (uint)gP1.y) return;
    gAccum[id.xy] = 0.0;
    gAlbedoAccum[id.xy] = 0.0;
    gMoments[id.xy] = 0.0;
    gGbuffer[id.xy] = float4(0, 0, 0, 1e6);
    gEdgeAccum[id.xy] = 0.0;
}

[numthreads(1, 1, 1)]
void CSClearCounter(uint3 id : SV_DispatchThreadID) {
    gCounter[uint2(0, 0)] = 0u;
}

// One camera path per pixel (pixels that already converged return early).
[numthreads(8, 8, 1)]
void CSRender(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gP1.x || id.y >= (uint)gP1.y) return;
    uint2 pix = id.xy;
    uint sampleIndex = (uint)gP0.z;
    float4 acc = gAccum[pix];
    if (sampleIndex > 0u && acc.a >= gP0.w) {
        float2 m = gMoments[pix] / acc.a;
        float err = sqrt(max(m.y - m.x * m.x, 0.0) / acc.a);
        if (err < gP1.w) return;
    }
    uint active = WaveActiveCountBits(true);
    if (WaveIsFirstLane()) InterlockedAdd(gCounter[uint2(0, 0)], active);

    uint rng = RngSeed(pix, (uint)gP0.x, 3u);
    float2 sub = sampleIndex == 0u ? float2(0.5, 0.5) : float2(Rand(rng), Rand(rng));
    float2 lensU = float2(Rand(rng), Rand(rng));
    float3 o, d;
    CameraRay(pix + sub, lensU, o, d);
    const float3 d0 = d;

    float3 radiance = 0, T = 1;
    bool diffuseChain = false;   // a diffuse bounce happened: plain Lambert NEE from here on
    bool specChain = true;       // only glass crossings so far: the next opaque hit is the "primary" one
    int ch = -1;                 // hero colour channel once the path refracted (dispersion)
    float tMinNext = 0.0;
    bool primHit = false;
    float3 primAlbedo = 1, primN = 0;
    float primZ = 1e6, primT = 0.0, chainT = 0.0;
    const uint maxDepth = gP3.x >= 1.0 ? min((uint)gP3.x, kMaxDepth) : kMaxDepth;
    [loop] for (uint depth = 0; depth <= maxDepth; ++depth) {
        Surf s;
        float t;
        if (!TraceScene(o, d, depth == 0u ? gNearZ : tMinNext, rng, specChain ? 0.0 : 1.0, s, t)) {
            radiance += T * Background(d);
            break;
        }
        if (s.emitter) {   // softboxes are seen by rays only (no light sampling), so every hit counts
            radiance += T * s.emission;
            break;
        }
        if (specChain) chainT += t;
        if (s.glass) {
            if (s.inside) T *= exp(-gGlassAbsorb.xyz * t);
            else if (!diffuseChain) radiance += T * GlassGlint(s, -d, rng);
            if (depth == maxDepth) break;
            bool refr;
            d = GlassScatter(d, s.n, s.inside, ch, T, rng, refr);
            o = OffsetRayOrigin(s.pos, refr ? -s.faceN : s.faceN);
            tMinNext = 0.0;
            continue;
        }
        const bool viaGlass = specChain && depth > 0u;
#ifdef MMDX_PT_PACK
        PtPackOut ptPackOut = (PtPackOut)0;
        bool isPtPackHit = (s.g.flags & RTG_PT_PACK) != 0;
        float ptSunVis = 0.0;
        if (isPtPackHit) {
            RtPtPackRecord rec = LoadPtPackRecord(s.g.packSrv);
            PtPackBindRecord(rec);
            PtPackIn packIn;
            packIn.pos = s.pos;
            packIn.normal = s.n;
            packIn.V = -d;
            packIn.uv = s.uv;
            packIn.L = -gLightDir;
            if (!diffuseChain) {
                float3 sv = SunVisibility(s, rng);
                ptSunVis = Luminance(sv);
            }
            packIn.sunVis = ptSunVis;
            packIn.baseColor = SrgbToLinear(saturate(s.tex.rgb * s.g.diffuse.rgb));
            packIn.materialClass = rec.materialClass;
            [unroll] for (int p = 0; p < 16; ++p)
                packIn.params[p] = rec.params[p >> 2][p & 3];
            packIn.headRight = rec.headRight.xyz;
            packIn.headUp = rec.headUp.xyz;
            packIn.headForward = rec.headForward.xyz;
            packIn.headValid = (rec.headValid != 0);

            ptPackOut = PackEvaluate(packIn);
            s.albedo = ptPackOut.albedo;
            if (ptPackOut.flatFace) s.flat = true;
        }
#endif
        if (specChain) {
            primHit = true;
            primAlbedo = s.albedo;
            primN = s.n;
            // virtual depth along the glass chain (a reflection may hit something behind the camera)
            primZ = max(chainT * dot(d0, gInvView[2].xyz), gNearZ);
            primT = chainT;
            specChain = false;
        }

        float3 V = -d;
        float cosV = saturate(dot(s.n, V));
        float pSpec = s.refl > 0.001 ? saturate(s.refl + (1.0 - s.refl) * pow(1.0 - cosV, 5.0) * s.refl) : 0.0;
        if (!diffuseChain) {
#ifdef MMDX_PT_PACK
            if (isPtPackHit) {
                float3 sunDirect = PtPackComposeSunDirect(ptPackOut, s.n, ptSunVis, s.flat);
                if (gNumLights >= 1.0)
                    sunDirect += (1.0 - pSpec) * s.albedo / PI * PunctualIrradiance(s, s.character, rng);
                radiance += T * (s.emission + sunDirect);
            } else
#endif
            {
                radiance += T * CameraDirect(s, V, pSpec, rng);
            }
        } else {
            radiance += T * (s.emission + (1.0 - pSpec) * s.albedo / PI * DirectIrradiance(s, rng));
        }
        if (depth == maxDepth) break;

        // indirect diffuse from the irradiance cache (the prepass); mirror lobe still traced
        if ((depth == 0u || viaGlass) && gP2.w > 0.5) {
            float3 Eic;
            if (IcLookup(s, Eic)) {
                if (s.character && s.flat) {
                    radiance += T * s.albedo * kToonIndirect * Eic / PI;
                    break;
                }
                radiance += T * (1.0 - pSpec) * s.albedo * (s.character ? kToonIndirect : 1.0) * Eic / PI;
                if (Rand(rng) >= pSpec) break;
                float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), reflect(d, s.n), cos(s.rough * 0.5));
                if (dot(sd, s.faceN) <= 0.0) break;
                o = OffsetRayOrigin(s.pos, s.faceN);
                d = sd;
                tMinNext = 0.0;
                continue;
            }
        }

        float3 nd;
        tMinNext = 0.0;
        if (s.character && s.flat && !diffuseChain) {
            // faces: even fill around the view direction, skipping nearby geometry (nose, fringe)
            nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), V);
            T *= s.albedo * kToonIndirect;
            diffuseChain = true;
            tMinNext = kFlatGatherSkip;
        } else {
            if (Rand(rng) < pSpec) {
                nd = SampleCone(float2(Rand(rng), Rand(rng)), reflect(d, s.n), cos(s.rough * 0.5));
            } else {
                nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), s.n);
                T *= s.albedo * ((s.character && !diffuseChain) ? kToonIndirect : 1.0);
                diffuseChain = true;
            }
            if (dot(nd, s.faceN) <= 0.0) break;
        }
        o = OffsetRayOrigin(s.pos, s.faceN);
        d = nd;
        if (depth >= 3u) {
            float q = min(max(T.r, max(T.g, T.b)), 0.95);
            if (Rand(rng) > q) break;
            T /= q;
        }
    }

    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = 0;   // also guards the firefly clamp (inf * 0)
    if (primHit) {
        // aerial perspective toward the horizon colour, same curve as the raster haze
        float f = (1.0 - exp(-primT * kHaze * 0.0011)) * 0.85;
        radiance = lerp(radiance, SkyColor(float3(d0.x, max(d0.y, 0.0) * 0.3, d0.z)), f);
    }
    float lum = Luminance(radiance);
    if (lum > kFirefly) radiance *= kFirefly / lum;
    gAccum[pix] = acc + float4(radiance, 1.0);
    gAlbedoAccum[pix] = gAlbedoAccum[pix] + float4(primAlbedo, primHit ? 1.0 : 0.0);
    float tl = Perceptual(radiance);
    gMoments[pix] = gMoments[pix] + float2(tl, tl * tl);
    if (sampleIndex == 0u) gGbuffer[pix] = primHit ? float4(primN, primZ) : float4(0, 0, 0, 1e6);
}

