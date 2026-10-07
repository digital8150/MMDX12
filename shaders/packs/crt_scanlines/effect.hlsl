// CRT look: barrel distortion, scanlines, an RGB triad mask and a corner darkening. Parameters: 0 scanline strength,
// 1 line spacing in pixels, 2 RGB mask strength, 3 curvature. Pixels pushed outside the screen go black.
float3 PackEffect(PackEffectInput i) {
    float2 p = i.uv * 2.0 - 1.0;
    p *= 1.0 + dot(p, p) * PackParam(3) * 0.5;
    const float2 uv = p * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    float3 c = gEffectSource.SampleLevel(gLinear, uv, 0).rgb;

    const float scan = 0.5 + 0.5 * cos(6.2831853 * i.pixel.y / max(PackParam(1), 2.0));
    c *= 1.0 - PackParam(0) * (1.0 - scan);

    const uint phase = (uint)i.pixel.x % 3u;
    const float3 triad = float3(phase == 0u ? 1.0 : 0.6, phase == 1u ? 1.0 : 0.6, phase == 2u ? 1.0 : 0.6);
    c *= lerp(1.0.xxx, triad, PackParam(2));

    const float2 q = abs(p);
    c *= 1.0 - 0.25 * smoothstep(0.7, 1.2, max(q.x, q.y)) * (PackParam(3) * 4.0 + 0.2);
    return c;
}
