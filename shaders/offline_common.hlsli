// Offline GI renderer: the camera inside the shutter interval, shared by offline_gi.hlsl (traced
// camera rays) and offline_edge.hlsl (rasterised outlines), so both see the same camera.
// Include common.hlsli first.
#ifndef MMDX_OFFLINE_COMMON_HLSLI
#define MMDX_OFFLINE_COMMON_HLSLI

// Camera-to-world rotation (rows: right, up, forward) and eye at shutter time t: 0 = the camera at
// shutter open (gPrevInvView), 1 = the frame's camera (gInvView).
void OfflineCameraAt(float t, out float3x3 basis, out float3 eye) {
    float3 f = normalize(lerp(gPrevInvView[2].xyz, gInvView[2].xyz, t));
    float3 r = lerp(gPrevInvView[0].xyz, gInvView[0].xyz, t);
    r = normalize(r - f * dot(r, f));
    basis = float3x3(r, cross(f, r), f);
    eye = lerp(gPrevInvView[3].xyz, gInvView[3].xyz, t);
}

// Thin lens: a view-space point seen from the lens position (lens.x, lens.y, 0), sheared so the
// focal plane z = focus stays put (focus <= 0: pinhole).
float3 OfflineLensView(float3 vp, float2 lens, float focus) {
    if (focus > 0.0) vp.xy -= lens * (1.0 - vp.z / focus);
    return vp;
}

#endif
