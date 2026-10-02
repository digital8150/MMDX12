// Depth of field (Dennis Gustafsson's single-pass golden-angle bokeh gather):
//   PSPrepare  half-res colour + signed CoC in a (most near-field texel of the 2x2 block)
//   PSGather   golden-angle bokeh gather into blurA_, a = effective blend radius
//   PSTent     3x3 tent filter of the gather
//   PSCombine  blend sharp hdrFinal against the blurred image by CoC radius
//   t0 source, t1 blur (PSCombine only), t2 depth (PSPrepare/PSCombine).
//   gP0 = (focusZ (<= 0: autofocus), aperture, maxCocPx (output px), radScale)
//   gP1 = (1/outWidth, 1/outHeight, 1/halfWidth, 1/halfHeight)
#include "fullscreen.hlsli"

Texture2D<float4> gTex0 : register(t0);
Texture2D<float4> gTex1 : register(t1);
Texture2D<float>  gDepthTex : register(t2);   // used by PSPrepare/PSCombine; bind depth at t2 in those draws

float FocusZ() {
    if (gP0.x > 0.0) return gP0.x;
    float z = LinearZ(gDepthTex.SampleLevel(gPoint, float2(0.5, 0.5), 0));
    z += LinearZ(gDepthTex.SampleLevel(gPoint, float2(0.47, 0.5), 0));
    z += LinearZ(gDepthTex.SampleLevel(gPoint, float2(0.53, 0.5), 0));
    z += LinearZ(gDepthTex.SampleLevel(gPoint, float2(0.5, 0.46), 0));
    z += LinearZ(gDepthTex.SampleLevel(gPoint, float2(0.5, 0.54), 0));
    return z * 0.2;
}

float SignedCoc(float z, float F) { return clamp(gP0.y * (z - F) / z, -1.0, 1.0) * gP0.z; }  // output px

float4 PSPrepare(FsOut i) : SV_Target {
    float F = FocusZ();
    // Average the 4 output-res texels of this 2x2 block, keeping the most near-field CoC
    // so near objects' blur covers the background.
    const float2 o1 = i.uv + gP1.xy * float2(-0.5, -0.5);
    const float2 o2 = i.uv + gP1.xy * float2(0.5, -0.5);
    const float2 o3 = i.uv + gP1.xy * float2(-0.5, 0.5);
    const float2 o4 = i.uv + gP1.xy * float2(0.5, 0.5);
    float3 c = gTex0.SampleLevel(gPoint, o1, 0).rgb + gTex0.SampleLevel(gPoint, o2, 0).rgb +
               gTex0.SampleLevel(gPoint, o3, 0).rgb + gTex0.SampleLevel(gPoint, o4, 0).rgb;
    float coc = min(
        min(SignedCoc(LinearZ(gDepthTex.SampleLevel(gPoint, o1, 0)), F),
            SignedCoc(LinearZ(gDepthTex.SampleLevel(gPoint, o2, 0)), F)),
        min(SignedCoc(LinearZ(gDepthTex.SampleLevel(gPoint, o3, 0)), F),
            SignedCoc(LinearZ(gDepthTex.SampleLevel(gPoint, o4, 0)), F)));
    return float4(max(c * 0.25, 0.0), coc * 0.5);   // CoC converted to half-res pixels
}

static const float kGoldenAngle = 2.39996323;

float4 PSGather(FsOut i) : SV_Target {
    float4 center = gTex0.SampleLevel(gPoint, i.uv, 0);
    float centerSize = abs(center.a);
    float maxR = gP0.z * 0.5;          // half-res pixels
    float radScale = gP0.w;
    float3 color = center.rgb;
    float tot = 1.0;
    float effective = centerSize;
    float radius = radScale;
    float ang = Ign(i.pos.xy) * 6.2831853;   // per-pixel rotation hides the spiral pattern
    [loop] for (int k = 0; k < 96 && radius < maxR; ++k) {
        float2 suv = i.uv + float2(cos(ang), sin(ang)) * radius * gP1.zw;
        float4 s = gTex0.SampleLevel(gPoint, suv, 0);
        float sSize = abs(s.a);
        if (s.a > center.a) sSize = min(sSize, centerSize * 2.0);   // farther samples may not bleed over nearer ones
        float m = smoothstep(radius - 0.5, radius + 0.5, sSize);
        color += lerp(color / tot, s.rgb, m);
        tot += 1.0;
        if (s.a < center.a) effective = max(effective, m * sSize);  // near-field samples spreading over this pixel
        radius += radScale / radius;
        ang += kGoldenAngle;
    }
    return float4(color / tot, effective);
}

float4 PSTent(FsOut i) : SV_Target {
    float4 sum = float4(0, 0, 0, 0);
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);   // 1 2 1 / 2 4 2 / 1 2 1
            sum += gTex0.SampleLevel(gPoint, i.uv + float2(float(x), float(y)) * gP1.zw, 0) * w;
        }
    }
    return sum / 16.0;
}

float4 PSCombine(FsOut i) : SV_Target {
    float4 sharp = gTex0.SampleLevel(gPoint, i.uv, 0);
    float4 blur = gTex1.SampleLevel(gLinear, i.uv, 0);
    float coc = abs(SignedCoc(LinearZ(gDepthTex.SampleLevel(gPoint, i.uv, 0)), FocusZ()));
    float blend = smoothstep(0.5, 2.0, max(coc, blur.a * 2.0));
    return float4(lerp(sharp.rgb, blur.rgb, blend), sharp.a);
}
