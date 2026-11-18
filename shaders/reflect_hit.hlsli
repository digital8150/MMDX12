// The radiance of one reflection hit, shared by rtreflect.hlsl (compute) and mmd.hlsl's RT glass branch.
// Requires rt_common.hlsli and pbr_common.hlsli before it. The includer defines REFLECT_LIGHTS (the light buffer),
// REFLECT_LIGHT_T (its element type) and optionally REFLECT_MAX_LIGHTS (default 64).
#ifndef REFLECT_HIT_HLSLI
#define REFLECT_HIT_HLSLI

#ifndef REFLECT_LIGHTS
#error "reflect_hit.hlsli: define REFLECT_LIGHTS (and REFLECT_LIGHT_T) before including"
#endif
#ifndef REFLECT_MAX_LIGHTS
#define REFLECT_MAX_LIGHTS 64u
#endif

// Punctual-light irradiance at a reflection hit (colour * shadow * attenuation * diffuse term), summed over the
// frame's lights. The caller multiplies by the hit's linear albedo.
float3 ReflectPunctualLights(const RtSurface s) {
    float3 sum = 0;
    const uint n = min((uint)gNumLights, REFLECT_MAX_LIGHTS);
    [loop] for (uint i = 0; i < n; ++i) {
        REFLECT_LIGHT_T l = REFLECT_LIGHTS[i];
        if (l.affectDiffuse <= 0.0) continue;
        float dist;
        float3 ld;
        float atten;
        if (l.isArea > 0.5) {
            // the light centre (no random sample): a mirror reflection has no temporal accumulation to hide it
            float3 d = l.pos - s.pos;
            dist = length(d);
            ld = d / max(dist, 1e-4);
            atten = PunctualFalloff(dist, l.invRange, l.falloff) * max(0.0, dot(l.dir, -ld));
        } else {
            float3 d = l.pos - s.pos;
            dist = length(d);
            ld = d / max(dist, 1e-4);
            atten = PunctualFalloff(dist, l.invRange, l.falloff);
            if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        }
        if (atten <= 0.0) continue;

        float diff = smoothstep(-0.05, 0.25, dot(s.normal, ld));
        float3 shadowFactor = 1.0;
        if (l.shadowType > 0.5) {
            const float rawVis = Luminance(saturate(TraceShadowRayTinted(OffsetRayOrigin(s.pos, s.faceNormal), ld,
                                                                        max(dist - 0.05, 0.0), RT_MASK_CHARACTER)));
            shadowFactor = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        }
        sum += l.color * shadowFactor * atten * diff;
    }
    return sum;
}

// Radiance of one reflection hit: the MMD material model (texture / PBR albedo / emissive, the sun with its traced
// shadow, the hemisphere) plus the punctual lights. `R` is the direction of the ray that hit it.
float3 ReflectHitRadiance(RtHit hit, float3 R) {
    RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
    RtSurface s = FetchSurface(g, hit.prim, hit.bary);
    if (dot(s.faceNormal, R) > 0) s.faceNormal = -s.faceNormal;
    if (dot(s.normal, s.faceNormal) < 0) s.normal = -s.normal;

    float4 tex = SampleBaseTexture(g, s.uv, 1.0 + log2(1.0 + hit.t / 20.0));
    // A hit with a PBR record takes its PBR albedo (and emissive) instead of the PMX texture (no full PBR at hits).
    // tex.rgb stays gamma-space: the reflect shading below decodes it like MaterialAlbedo/MaterialLit expect.
    float3 pbrEmissive = 0;
    if (g.pbrIndex != 0xFFFFFFFFu) {
        GpuPbrMaterial pm = PbrLoadMaterial((int)g.pbrSrv, g.pbrIndex);
        tex.rgb = LinearToSrgb(saturate(PbrAlbedo(pm, s.uv, g.diffuse.rgb)));
        pbrEmissive = saturate(PbrEmissive(pm, s.uv));
    }
    float3 L = -gLightDir;
    float ndl = dot(s.normal, L);
    float sh = 1.0;
    if ((g.flags & MAT_RECEIVE) && gCascadeSplits.w > 0.5 && ndl > 0 && gSunShadowParams.x > 0.5)
        sh = Luminance(saturate(TraceShadowRayTinted(OffsetRayOrigin(s.pos, s.faceNormal), L, 2000.0, 0xFFu)));

    float3 shTrans = ShadowTransmission(sh, gSunShadowParams.z, gSunShadowColor.rgb);
    float3 lightTerm = shTrans * smoothstep(-0.1, 0.2, ndl);
    float3 lit = MaterialLit(g, tex.rgb);
    float3 c = lerp(lit * 0.62, lit, lightTerm);
    float3 albedoLin = SrgbToLinear(saturate(MaterialAlbedo(g, tex.rgb)));
    float3 hemi = lerp(gGroundColor, gSkyZenith, saturate(s.normal.y * 0.5 + 0.5)) * gSunIntensity;
    return SrgbToLinear(saturate(c)) * gSunIntensity + albedoLin * hemi * gHemiStrength + pbrEmissive +
           albedoLin * ReflectPunctualLights(s);
}

#endif
