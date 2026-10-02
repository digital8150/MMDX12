#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace mmdx {

class GpuModel;

// Temporal upscalers behind IUpscaler (see Upscaler.h).
enum class UpscalerKind : uint8_t { None = 0, DLSS, FSR, XeSS };
// Per-dimension ratio output / render: NativeAA 1.0, Quality 1.5, Balanced 1.7,
// Performance 2.0, UltraPerformance 3.0.
enum class UpscalerQuality : uint8_t { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance };

// Raster: shadow maps, SSAO, SSR. RayTraced: the raster G-buffer with DXR (inline ray query)
// shadows, ambient occlusion and reflections. PathTraced: compute path tracer + denoiser
// (no MMD outlines). Both DXR paths need raytracing tier 1.1; otherwise Raster is used.
enum class RenderPath : uint8_t { Raster = 0, RayTraced, PathTraced };

struct RenderSettings {
    uint32_t msaaSamples = 4;      // 1/2/4/8, clamped to what the device supports (1 with an upscaler or PathTraced)
    float renderScale = 1.0f;      // internal resolution = output size * scale (0.25..2.0); ignored with an upscaler
    bool fixedResolution = false;  // benchmark: the output is exactly fixedWidth x fixedHeight
    uint32_t fixedWidth = 1920, fixedHeight = 1080;
    bool vsync = true;
    bool drawEdges = true;
    UpscalerKind upscaler = UpscalerKind::None;
    UpscalerQuality upscalerQuality = UpscalerQuality::Quality;
    RenderPath renderPath = RenderPath::Raster;
    uint32_t ptSamples = 1;          // path tracer samples per pixel per frame (1..4)
    uint32_t ptBounces = 3;          // path tracer bounces after the primary hit (1..6)

    // --- lighting / effects (every one can be switched off for an honest benchmark)
    bool shadows = true;
    uint32_t shadowMapSize = 2048;   // per cascade (3 cascades)
    float shadowDistance = 160.0f;   // MMD units covered by the cascades
    bool ssao = true;
    float ssaoRadius = 2.2f;         // MMD units (1 unit ~ 8 cm)
    float ssaoIntensity = 0.85f;     // 0..1 blend toward the occluded value
    bool ssr = true;
    float floorGloss = 0.35f;        // reflectivity of upward-facing stage surfaces
    bool taa = false;                // temporal AA (camera jitter + history), works with MSAA
    bool bloom = true;
    float bloomIntensity = 0.35f;
    float bloomThreshold = 1.1f;    // linear HDR luminance where bloom starts
    bool bloomConvolution = false;  // FFT convolution bloom with a starburst kernel (falls back to the mip chain)
    bool dof = false;               // depth of field (bokeh gather), focus from FrameView::focusDistance
    float dofAperture = 1.0f;       // 0..3, scales the circle of confusion (1 = f/2-ish look)
    float dofMaxRadius = 14.0f;     // max CoC radius in output pixels at 1080p (scaled with output height)
    bool volumetric = false;        // ray-marched sun shafts + spotlight cones (height fog medium)
    float volumetricDensity = 1.0f; // 0..4, scales the medium density
    float lutIntensity = 1.0f;      // 0..1 blend toward the colour LUT (Renderer::SetColorLut); no LUT = off
    float exposure = 1.0f;
    float contrast = 1.06f;
    float saturation = 1.06f;
    float vignette = 0.22f;
    float fog = 0.35f;               // 0 = off; distance haze toward the horizon colour
    bool transparentBackground = false;  // thumbnails: no sky, alpha = coverage
};

struct CameraParams {
    DirectX::XMFLOAT4X4 view{};   // LH view matrix (row-vector convention)
    DirectX::XMFLOAT3 eye{};
    float fovYRadians = DirectX::XMConvertToRadians(30.0f);
    float nearZ = 0.5f, farZ = 3000.0f;  // projection aspect comes from the internal render target
};

// Point light (spotCosOuter <= -1) or spot light, in MMD world space. Not shadowed.
struct PunctualLight {
    DirectX::XMFLOAT3 position{};
    float range = 60.0f;
    DirectX::XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    DirectX::XMFLOAT3 direction{0, -1, 0};  // spot axis
    float spotCosOuter = -2.0f;             // <= -1: point light
    float spotCosInner = 1.0f;
};

struct LightParams {
    DirectX::XMFLOAT3 direction{-0.5f, -1.0f, 0.5f};  // MMD default light direction (toward scene)
    DirectX::XMFLOAT3 color{0.6f, 0.6f, 0.6f};        // MMD default light color (154/255)
    float sunIntensity = 1.0f;                        // scales the toon-lit result (linear)
    // Sky / environment (linear RGB).
    DirectX::XMFLOAT3 skyZenith{0.32f, 0.55f, 0.85f};
    DirectX::XMFLOAT3 skyHorizon{0.78f, 0.86f, 0.92f};
    DirectX::XMFLOAT3 groundColor{0.42f, 0.44f, 0.47f};
    float hemiStrength = 0.18f;  // hemispheric tint on top of the MMD ambient
    float rimStrength = 0.35f;   // anime rim light on characters
    DirectX::XMFLOAT3 rimColor{1.0f, 0.97f, 0.92f};
    std::vector<PunctualLight> punctual;  // up to Renderer::kMaxPunctualLights used
};

struct FrameView {
    CameraParams camera;
    LightParams light;
    std::vector<GpuModel*> models;  // drawn in this order (stage parts first, then characters)
    bool studioFloor = false;       // draw the procedural studio floor at y = 0
    bool cameraCut = false;         // discard temporal history (seek, VMD camera cut)
    float focusDistance = 0.0f;     // DoF focus plane as view-space z (MMD units); <= 0: autofocus on the screen centre
};

struct RenderStats {
    uint32_t internalWidth = 0, internalHeight = 0;   // render resolution
    uint32_t outputWidth = 0, outputHeight = 0;       // after the upscaler (== internal without one)
    RenderPath renderPath = RenderPath::Raster;       // path actually used this frame (after fallback)
    UpscalerKind upscaler = UpscalerKind::None;       // upscaler actually used this frame (None if unavailable)
    uint32_t drawCalls = 0;
    uint64_t triangles = 0;
    float gpuFrameMs = 0;  // from timestamp queries, ~kFramesInFlight frames old
};

} // namespace mmdx
