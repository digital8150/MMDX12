// Film grain: a hash of the grain cell and the 24 fps tick, centred noise added in display space, weighted towards the
// mid-tones (shadows and highlights keep less grain). Parameters: 0 strength, 1 grain size (pixels), 2 mid-tone weight.
float Hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 PackEffect(PackEffectInput i) {
    const float tick = floor(i.time * 24.0);
    const float2 cell = floor(i.pixel / max(PackParam(1), 1.0));
    const float2 jitter = float2(tick * 17.0, tick * 31.0);
    const float n = Hash21(cell + jitter) + Hash21(cell * 1.7 + jitter + 5.0) - 1.0;   // triangular, -1..1
    const float luma = Luminance(i.color.rgb);
    const float weight = lerp(1.0, 4.0 * luma * (1.0 - luma), PackParam(2));
    return saturate(i.color.rgb + n * PackParam(0) * weight);
}
