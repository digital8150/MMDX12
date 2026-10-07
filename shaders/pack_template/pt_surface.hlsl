// pt_surface.hlsl: offline GI shader pack template (docs/shader_pt_api.md).
//
// Optional second file in a surface pack, written for the offline GI path tracer (CSRender).
// The full contract is in shaders/pt_pack_api.hlsli.
//
// Authors implement:
//     PtPackOut PackEvaluate(PtPackIn i);
//
// PtPackIn provides:
//   i.pos, i.normal, i.V, i.uv, i.L, i.sunVis, i.baseColor (linear), i.materialClass, i.params[16],
//   head frame (i.headPos, i.headScale, i.headRight, i.headUp, i.headForward, i.headValid).
//
// PtPackOut returns:
//   o.albedo       linear surface albedo used for GI diffuse bounces
//   o.shadowTint   linear multiplier on the shaded side of the terminator
//   o.shadowBias   shifts the terminator in N.L units (a smoothed face normal = dot(Nsmooth, L) - dot(N, L))
//   o.terminator   (lo, hi) N.L edges of the terminator smoothstep; (0, 0) = engine default (-0.12, 0.22)
//   o.specular     additive linear radiance (rim, matcap, highlights)
//   o.flatFace     use engine flat-face handling (no GI gradient)

PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    o.albedo = i.baseColor;
    o.shadowTint = float3(0.7, 0.7, 0.75); // slightly cool shaded side
    o.shadowBias = 0.0;
    o.terminator = float2(0.0, 0.0);       // engine default softness
    o.specular = float3(0.0, 0.0, 0.0);
    o.flatFace = (i.materialClass == PACK_FACE);

    if (i.materialClass == PACK_SKIN) {
        o.shadowTint = float3(0.85, 0.75, 0.75); // warmer skin shadow
    }

    return o;
}
