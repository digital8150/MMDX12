// Chromatic aberration: red and blue are sampled along the direction away from the screen centre; the offset grows
// with the distance from the centre (falloff = exponent). Parameters: 0 strength, 1 edge falloff.
float3 PackEffect(PackEffectInput i) {
    const float2 dir = i.uv - 0.5;
    const float r = length(dir) * 1.41421356;   // 0 at the centre, 1 in the corners
    const float2 offset = dir * (PackParam(0) * 0.03 * pow(r, PackParam(1)));
    float3 c;
    c.r = gEffectSource.SampleLevel(gLinear, i.uv + offset, 0).r;
    c.g = i.color.g;
    c.b = gEffectSource.SampleLevel(gLinear, i.uv - offset, 0).b;
    return c;
}
