// Unidirectional path tracer (inline ray queries): explicit sun next-event estimation,
// punctual-light NEE, analytic studio floor, stochastic alpha-tested transparency.
// u0 light (divided by albedo on surfaces), u1/u2/u3/u4 primary G-buffer for denoising.
#include "rt_common.hlsli"   // includes common.hlsli
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);
#ifdef MMDX_PT_PACK
#include "pt_pack_api.hlsli"
#include "pt_pack_glue.hlsli"
#include MMDX_PT_PACK
#endif

RWTexture2D<float4> gLightOut : register(u0);
RWTexture2D<float4> gAlbedoOut : register(u1);
RWTexture2D<float4> gNormalOut : register(u2);
RWTexture2D<float2> gVelocityOut : register(u3);
RWTexture2D<float> gDepthOut : register(u4);

struct PtLight {
    float3 pos; float invRange;
    float3 color; float cosOuter;
    float3 dir; float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice; float pad;
    float areaWidth; float areaHeight; float isArea; float pad2;
};
StructuredBuffer<PtLight> gPtLights : register(t2, space1);

static const float kSunCosMax = 0.99996;          // ~0.5 degree sun radius
static const float kSkyScale = 1.0;               // sky radiance multiplier for indirect rays
static const float kFloorExtent = 900.0;          // same as mmd.hlsl
// MMD's ambient colour is a self-illumination term (raster: saturate(ambient + diffuse * light));
// part of it is emitted so path-traced scenes keep the familiar brightness.
static const float kAmbientEmission = 0.5;

float3 SunIrradiance() { return SrgbToLinear(gLightColor) * (PI * 3.0) * gSunIntensity; }

// Character materials keep the raster toon model (mmd.hlsl PSMain) for the sun: a physical
// Lambert term models faces into 3D shading, which MMD rigs are not authored for. The sun
// visibility `sh` comes from a traced shadow ray instead of the shadow map. Linear output.
float3 ToonSun(RtGeometry g, float4 tex, float2 uv, float3 n, float3 V, float rawSh) {
    float3 L = -gLightDir;
    float3 lit = saturate(g.ambient + g.diffuse.rgb * gLightColor) * tex.rgb;
    if (g.flags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 sp = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.sphereSrv)].SampleLevel(gLinear, suv, 0).rgb, g.sphereMul, g.sphereAdd, (g.flags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
        if (g.flags & MAT_SPHERE_MUL) lit *= sp; else lit += sp;
    }
    float occ = (1.0 - rawSh) * gSunShadowParams.z;
    float sh = 1.0 - occ;
    float ndl = dot(n, L);
    float flat = (g.flags & MAT_FLAT) ? 1.0 : 0.0;
    float3 c;
    if (g.flags & MAT_TOON_MAP) {
        // Project Sekai layout: the "toon" is the painted shadow colour at the same UV.
        float3 shadowTex = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gRtWrap, uv, 0).rgb, g.toonMul, g.toonAdd, 1.0);
        float term = sh * lerp(smoothstep(-0.03, 0.06, ndl), 1.0, flat);
        c = lerp(saturate(g.ambient + g.diffuse.rgb * gLightColor) * shadowTex, lit, term);
    } else if (g.flags & MAT_HAS_TOON) {
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = lit * ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gLinear, float2(0.5, v), 0).rgb, g.toonMul, g.toonAdd, 1.0);
    } else {
        c = lit;
    }
    if (!(g.flags & MAT_TOON_MAP)) {
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += lit * gSunShadowColor.rgb * occ;
    if (g.specularPower > 0.0)
        c += pow(saturate(dot(normalize(L + V), n)), g.specularPower) * g.specular * gLightColor * sh;
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
    float side = smoothstep(-0.2, 0.5, ndl + 0.25);
    color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    return color;
}

[numthreads(8, 8, 1)]
void CSPathTrace(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gLightOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 pix = id.xy;

    uint nl = max((uint)gNumLights, 1u);   // one light is always sampled when lights exist
    uint sampleCount = max((uint)gP0.x, 1u);
    uint bounces = (uint)gP0.y;
    float3 radiance = 0;
    float3 primAlbedo = 0, primNormal = 0, primPos = 0, primPrev = 0, primRd = 0;
    float primRefl = 0;
    bool primHit = false;
    [loop] for (uint s = 0; s < sampleCount; ++s) {
        uint rng = RngSeed(pix, (uint)gP1.x, s);
        float2 sub = (s == 0) ? float2(0.5, 0.5) : float2(Rand(rng), Rand(rng));
        float2 uv = (pix + sub) * gInvViewportSize;
        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 v = mul(float4(ndc, 1.0, 1.0), gInvProj);
        float3 viewDir = normalize(v.xyz / v.w);
        float3 rd = normalize(mul(viewDir, (float3x3)gInvView));
        float3 ro = gEyePos;
        // gInvProj is the jittered projection: the jitter is already inside, do not add any.

        float3 throughput = 1.0;
        float3 dir = rd, origin = ro;
        bool primarySky = false;
#ifdef MMDX_PT_PACK
        bool diffuseChain = false;
#endif

        [loop] for (uint bounce = 0; bounce <= bounces; ++bounce) {
            float tMin = (bounce == 0) ? gNearZ : 0.0;
            float tMax = 1e5;

            // analytic studio floor plane y = 0
            float tp = 0;
            bool floorHit = false;
            if (gP0.z > 0.5 && dir.y < -1e-5) {
                float t = -origin.y / dir.y;
                if (t > tMin && length((origin + dir * t).xz) < kFloorExtent) {
                    tp = t;
                    floorHit = true;
                    tMax = t;
                }
            }

            float alphaThr = max(Rand(rng), 0.004);   // stochastic transparency
            RtHit hit;
            bool hitMesh = TraceClosest(origin, dir, tMin, tMax, alphaThr, hit);
            if (!hitMesh && !floorHit) {
                radiance += throughput * SkyColor(dir) * ((bounce == 0) ? 1.0 : kSkyScale);
                if (bounce == 0 && s == 0) { primarySky = true; primRd = rd; }
                break;
            }

            float3 pos, prevPos, n, faceN, albedo;
            float refl, rough;
            bool receive;
            bool toon = false;    // character material: raster toon sun, no Lambert NEE
            float flat = 0.0;     // MAT_FLAT (faces): even fill instead of traced indirect
            if (hitMesh) {
                RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
                RtSurface sf = FetchSurface(g, hit.prim, hit.bary);
                if (dot(sf.faceNormal, dir) > 0) sf.faceNormal = -sf.faceNormal;
                if (dot(sf.normal, sf.faceNormal) < 0) sf.normal = -sf.normal;
                float4 tex = SampleBaseTexture(g, sf.uv, bounce == 0 ? 0.0 : 2.0);
                albedo = SrgbToLinear(saturate(MaterialAlbedo(g, tex.rgb)));
                float r = g.reflectivity;
                if (g.flags & MAT_STAGE)
                    r = saturate(r + gFloorGloss * smoothstep(0.82, 0.97, sf.normal.y));
                refl = r;
                rough = clamp(sqrt(2.0 / (g.specularPower + 2.0)), 0.03, 0.6);
                receive = (g.flags & MAT_RECEIVE) != 0;
                pos = sf.pos; prevPos = sf.prevPos; n = sf.normal; faceN = sf.faceNormal;
                if (g.flags & MAT_STAGE) {
                    radiance += throughput * SrgbToLinear(saturate(g.ambient * tex.rgb)) * gSunIntensity * kAmbientEmission;
                } else {
#ifdef MMDX_PT_PACK
                    bool isPtPackHit = (g.flags & RTG_PT_PACK) != 0;
                    if (isPtPackHit) {
                        RtPtPackRecord rec = LoadPtPackRecord(g.packSrv);
                        if (rec.materialClass != PACK_CLASS_OFF) {
                            PtPackBindRecord(rec);
                            PtPackIn packIn;
                            packIn.pos = pos;
                            packIn.normal = n;
                            packIn.V = -dir;
                            packIn.uv = sf.uv;
                            packIn.L = -gLightDir;
                            float sh = 1.0;
                            if (!diffuseChain) {
                                if (receive && gShadowParams.w < 0.5) {
                                    float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, SunShadowConeCos(kSunCosMax));
                                    sh = TraceShadowRay(OffsetRayOrigin(pos, faceN), sd, 1e5);
                                }
                                packIn.sunVis = sh;
                            } else {
                                packIn.sunVis = 0.0;
                            }
                            packIn.baseColor = SrgbToLinear(saturate(tex.rgb * g.diffuse.rgb));
                            packIn.materialClass = rec.materialClass;
                            [unroll] for (int p = 0; p < 16; ++p)
                                packIn.params[p] = rec.params[p >> 2][p & 3];
                            packIn.headRight = rec.headRight.xyz;
                            packIn.headPos = rec.headPos.xyz;
                            packIn.headScale = rec.headPos.w;
                            packIn.headUp = rec.headUp.xyz;
                            packIn.headForward = rec.headForward.xyz;
                            packIn.headValid = (rec.headValid != 0);

                            PtPackOut ptPackOut = PackEvaluate(packIn);
                            albedo = ptPackOut.albedo;
                            // the pack decides: MAT_FLAT (a material without toon) would drop its terminator
                            flat = ptPackOut.flatFace ? 1.0 : 0.0;

                            if (!diffuseChain) {
                                toon = true;
                                radiance += throughput * PtPackComposeSunDirect(ptPackOut, n, sh, flat > 0.5);
                                if (flat > 0.5) {
                                    // raster flat fill: no sky/ground gradient and no occlusion modelling the face
                                    float3 fill = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength;
                                    radiance += throughput * albedo * fill;
                                }
                            } else {
                                toon = false;
                            }
                        } else {
                            isPtPackHit = false;
                        }
                    }
                    if (!isPtPackHit)
#endif
                    {
                        toon = true;
                        flat = (g.flags & MAT_FLAT) ? 1.0 : 0.0;
                        float sh = 1.0;
                        if (receive && gShadowParams.w < 0.5 && gSunShadowParams.x > 0.5) {
                            float cosMax = SunShadowConeCos(kSunCosMax);
                            float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, cosMax);
                            sh = TraceShadowRay(OffsetRayOrigin(pos, faceN), sd, 1e5);
                        }
                        radiance += throughput * ToonSun(g, tex, sf.uv, n, -dir, sh);
                        if (flat > 0.5) {
                            // raster flat fill: no sky/ground gradient and no occlusion modelling the face
                            float3 fill = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength;
                            radiance += throughput * albedo * fill;
                        }
                    }
                }
            } else {
                pos = origin + dir * tp;
                prevPos = pos;
                faceN = float3(0, 1, 0);
                n = faceN;
                float r = length(pos.xz);
                albedo = lerp(float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
                refl = 0.42 * (1.0 - smoothstep(120.0, 600.0, r));
                rough = 0.12;
                receive = true;
            }

            if (bounce == 0 && s == 0) {
                primAlbedo = albedo;
                primNormal = n;
                primRefl = refl;
                primPos = pos;
                primPrev = prevPos;
                primHit = true;
            }

            // Fresnel-weighted specular probability
            float cosV = saturate(dot(n, -dir));
            float pSpec = refl > 0.001 ? saturate(refl + (1.0 - refl) * pow(1.0 - cosV, 5.0) * refl) : 0.0;

            // sun next-event estimation (diffuse lobe)
            float3 L = -gLightDir;
            float ndl = dot(n, L);
            if (!toon && ndl > 0.0 && dot(faceN, L) > 0.0) {
                float cosMax = SunShadowConeCos(kSunCosMax);
                float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), L, cosMax);
                float rawVis = receive && gShadowParams.w < 0.5 && gSunShadowParams.x > 0.5
                                   ? TraceShadowRay(OffsetRayOrigin(pos, faceN), sd, 1e5)
                                   : 1.0;
                float occ = (1.0 - rawVis) * gSunShadowParams.z;
                float3 visFactor = (1.0 - occ) + gSunShadowColor.rgb * occ;
                radiance += throughput * (1.0 - pSpec) * albedo / PI * SunIrradiance() * ndl * visFactor;
            }

            // punctual-light NEE (one uniform light per bounce)
            if (gNumLights >= 1.0) {
                uint li = min((uint)(Rand(rng) * (float)nl), nl - 1u);
                PtLight l = gPtLights[li];
                float dist;
                float3 ld;
                float atten;
                if (l.isArea > 0.5) {
                    float3 norm = l.dir;
                    float3 c = cross(float3(0, 1, 0), norm);
                    float cLen = length(c);
                    float3 right = (cLen > 1e-4) ? (c / cLen) : float3(1, 0, 0);
                    float3 up = cross(norm, right);
                    float u = (Rand(rng) - 0.5) * l.areaWidth;
                    float v = (Rand(rng) - 0.5) * l.areaHeight;
                    float3 samplePos = l.pos + right * u + up * v;
                    float3 d = samplePos - pos;
                    dist = length(d);
                    ld = d / max(dist, 1e-4);
                    atten = PunctualFalloff(dist, l.invRange, l.falloff) * max(0.0, dot(norm, -ld));
                } else {
                    float3 d = l.pos - pos;
                    dist = length(d);
                    ld = d / max(dist, 1e-4);
                    atten = PunctualFalloff(dist, l.invRange, l.falloff);
                    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
                }

                if (atten > 0.0) {
                    float ndlp = dot(n, ld);
                    float diff = toon ? lerp(smoothstep(-0.05, 0.25, ndlp), saturate(ndlp * 0.3 + 0.7), flat) : ndlp;
                    bool evalDiff = (l.affectDiffuse > 0.0) && (diff > 0.0);

                    float3 R = reflect(dir, n);
                    float cosHalfAngle = cos(rough * 0.5);
                    float omega = max(2.0 * PI * (1.0 - cosHalfAngle), 1e-6);
                    bool evalSpec = (l.affectSpecular > 0.5) && (pSpec > 0.0) && (dot(faceN, ld) > 0.0) && (dot(R, ld) >= cosHalfAngle);

                    if (evalDiff || evalSpec) {
                        float rawVis = 1.0;
                        if (l.shadowType > 0.5) {
                            float3 rayDir = ld;
                            if (l.isArea <= 0.5 && l.shadowType > 1.5) {
                                float cosCone = SoftShadowConeCos(0.9999, l.shadowSoftness);
                                rayDir = SampleCone(float2(Rand(rng), Rand(rng)), ld, cosCone);
                            }
                            rawVis = TraceShadowRayMasked(OffsetRayOrigin(pos, faceN), rayDir, max(dist - 0.05, 0.0),
                                                          RT_MASK_CHARACTER);
                        }
                        float3 shadowFactor = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
                        if (evalDiff) {
                            radiance += throughput * (1.0 - pSpec) * albedo * l.color * shadowFactor * atten * diff * (float)nl;
                        }
                        if (evalSpec) {
                            radiance += throughput * pSpec * l.color * shadowFactor * atten * ((float)nl / omega);
                        }
                    }
                }
            }

            if (bounce == bounces || flat > 0.5) break;

            // next direction: Fresnel-weighted specular vs cosine diffuse
            float3 nextDir;
            if (Rand(rng) < pSpec) {
                nextDir = SampleCone(float2(Rand(rng), Rand(rng)), reflect(dir, n), cos(rough * 0.5));
            } else {
                nextDir = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), n);
                throughput *= albedo;
#ifdef MMDX_PT_PACK
                diffuseChain = true;
#endif
            }
            if (dot(nextDir, faceN) <= 0.0) break;
            dir = nextDir;
            origin = OffsetRayOrigin(pos, faceN);

            // Russian roulette from bounce >= 2
            if (bounce >= 2) {
                float q = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
                if (Rand(rng) > q) break;
                throughput /= q;
            }
        }
    }

    radiance /= sampleCount;
    radiance = min(radiance, 64.0);   // firefly guard

    if (!primHit) {
        // sky primary (sample 0 missed)
        gAlbedoOut[pix] = float4(1, 1, 1, gP0.w > 0.5 ? 0.0 : 1.0);
        gLightOut[pix] = float4(radiance, 0.0);
        gNormalOut[pix] = 0;
        gDepthOut[pix] = 1.0;
        float4 cur = mul(float4(primRd, 0.0), gViewProjNoJitter);
        float4 prev = mul(float4(primRd, 0.0), gPrevViewProjNoJitter);
        float2 vel = (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5, -0.5);
        gVelocityOut[pix] = vel;
        return;
    }

    float3 a = max(primAlbedo, 0.02);
    gAlbedoOut[pix] = float4(a, 1.0);
    gLightOut[pix] = float4(radiance / a, 1.0);
    float3 vn = normalize(mul(primNormal, (float3x3)gView));
    float2 enc = OctEncode(vn);
    gNormalOut[pix] = float4(enc.x, enc.y, primRefl, 1.0);
    float4 c = mul(float4(primPos, 1.0), gViewProj);
    gDepthOut[pix] = c.z / c.w;
    float4 cur = mul(float4(primPos, 1.0), gViewProjNoJitter);
    float4 prev = mul(float4(primPrev, 1.0), gPrevViewProjNoJitter);
    float2 vel = (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5, -0.5);
    gVelocityOut[pix] = vel;
}
