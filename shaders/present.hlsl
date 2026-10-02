// Letterboxed stretch of the final LDR image (already sRGB-encoded) to the back buffer.
Texture2D gSrc : register(t0);
SamplerState gLinear : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSFullscreen(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float4 PSPresent(VSOut i) : SV_Target { return float4(gSrc.Sample(gLinear, i.uv).rgb, 1.0); }
