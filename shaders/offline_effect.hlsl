// Offline GI effect input: the raw device depth of the G-buffer (effect_api.hlsli: 1 = background), the inverse of
// LinearZ (common.hlsli). Its own file because the R32_FLOAT target needs an RWTexture2D<float> declaration.
//   t3 gbuffer (world normal, view depth; 1e6 = sky)   u0 depth output (R32_FLOAT)
//   CSEffectDepth   gP0.xy = near, far (the frame camera), gP1.xy = image size

cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };

Texture2D<float4> gGbufT : register(t3);
RWTexture2D<float> gDepthOut : register(u0);

[numthreads(8, 8, 1)]
void CSEffectDepth(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    const float z = gGbufT.Load(int3(p, 0)).w;
    float d = 1.0;
    if (z < 1e5) d = gP0.y / (gP0.y - gP0.x) * (1.0 - gP0.x / max(z, 1e-6));
    gDepthOut[p] = d;
}
