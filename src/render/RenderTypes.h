#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace mmdx {

class GpuModel;
struct EffectStackEntry;

// Temporal upscalers behind IUpscaler (see Upscaler.h).
enum class UpscalerKind : uint8_t { None = 0, DLSS, FSR, XeSS };
// Per-dimension ratio output / render: NativeAA 1.0, Quality 1.5, Balanced 1.7,
// Performance 2.0, UltraPerformance 3.0.
enum class UpscalerQuality : uint8_t { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance };

// Raster: shadow maps, SSAO, SSR. RayTraced: the raster G-buffer with DXR (inline ray query)
// shadows, ambient occlusion and reflections. PathTraced: compute path tracer + denoiser
// (no MMD outlines). Both DXR paths need raytracing tier 1.1; otherwise Raster is used.
enum class RenderPath : uint8_t { Raster = 0, RayTraced, PathTraced };
inline constexpr uint32_t kMaxExtraViews = 3;

// Shading mode of the real-time raster path (Studio editing views). RayTraced / PathTraced
// ignore the setting and always render Lit.
enum class ViewShading : uint8_t { Lit = 0, Unlit, Wireframe };

struct RenderSettings {
    uint32_t msaaSamples = 4;      // 1/2/4/8, clamped to what the device supports (1 with an upscaler or PathTraced)
    float renderScale = 1.0f;      // internal resolution = output size * scale (0.25..2.0); ignored with an upscaler
    bool fixedResolution = false;  // benchmark: the output is exactly fixedWidth x fixedHeight
    uint32_t fixedWidth = 1920, fixedHeight = 1080;
    bool vsync = true;
    bool drawEdges = true;
    ViewShading shading = ViewShading::Lit;  // Raster path only; RayTraced/PathTraced stay Lit
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
    bool headless = false;               // render without touching the back buffer (background sample renders)
    // Effect packs ("type": "effect"), the user's ordered stack: PackEffectPass runs the enabled entries
    // (PackEffectStage from each pack's manifest) before Bloom / after Post. Empty = no cost, no targets.
    std::vector<EffectStackEntry> packEffects;
    // Output area in the back buffer (Studio viewport between its panels). viewportW/H == 0: the whole window.
    // The output resolution follows the area's size; the image is letterboxed into it.
    uint32_t viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0;
};

// The back-buffer area the scene is presented into (x, y, w, h), and the image rectangle fitted into it.
inline void ViewportArea(const RenderSettings& s, float bw, float bh, float area[4]) {
    if (s.viewportW > 0 && s.viewportH > 0) {
        area[0] = (float)s.viewportX; area[1] = (float)s.viewportY;
        area[2] = (float)s.viewportW; area[3] = (float)s.viewportH;
    } else {
        area[0] = 0; area[1] = 0; area[2] = bw; area[3] = bh;
    }
}
inline void FitInArea(const float area[4], float imageW, float imageH, float rect[4]) {
    const float s = (area[2] / imageW) < (area[3] / imageH) ? area[2] / imageW : area[3] / imageH;
    rect[2] = imageW * s;
    rect[3] = imageH * s;
    rect[0] = area[0] + (area[2] - rect[2]) * 0.5f;
    rect[1] = area[1] + (area[3] - rect[3]) * 0.5f;
}

struct CameraParams {
    DirectX::XMFLOAT4X4 view{};   // LH view matrix (row-vector convention)
    DirectX::XMFLOAT3 eye{};
    float fovYRadians = DirectX::XMConvertToRadians(30.0f);
    float nearZ = 0.5f, farZ = 3000.0f;  // projection aspect comes from the internal render target
};

enum class LightShadowType : uint8_t { NoCast = 0, Hard = 1, Soft = 2 };
enum class LightFalloffType : uint8_t { None = 0, Linear = 1, InverseSquare = 2 };

// Point light (spotCosOuter <= -1) or spot light, in MMD world space.
struct PunctualLight {
    DirectX::XMFLOAT3 position{};
    float range = 60.0f;
    DirectX::XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    DirectX::XMFLOAT3 direction{0, -1, 0};  // spot axis
    float spotCosOuter = -2.0f;             // <= -1: point light
    float spotCosInner = 1.0f;
    LightShadowType shadow = LightShadowType::Hard;
    float shadowSoftness = 0.5f;
    float shadowDensity = 1.0f;
    DirectX::XMFLOAT3 shadowColor{0, 0, 0};
    LightFalloffType falloff = LightFalloffType::None;
    bool affectDiffuse = true;
    bool affectSpecular = true;
    bool castPointShadow = false;  // opt-in point-light shadow; set by BuildSceneLighting (studio only)
    DirectX::XMFLOAT2 areaSize{0, 0}; // width, height (zero = not an area light)
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

    // Sun shadow properties
    LightShadowType sunShadow = LightShadowType::Hard;
    float sunShadowSoftness = 0.5f;
    float sunShadowDensity = 1.0f;
    DirectX::XMFLOAT3 sunShadowColor{0, 0, 0};
};

// Offline renderer scene extras (the render benchmark scene). Ignored by the real-time passes.
// An analytic glass box (rounded edges, refraction with dispersion, Beer-Lambert absorption) and
// rectangular softboxes (emissive quads seen by rays, no next-event estimation).
struct OfflineGlassBox {
    bool enabled = false;
    DirectX::XMFLOAT3 center{};            // world (MMD units)
    DirectX::XMFLOAT3 halfExtents{5, 5, 5};
    float yawRadians = 0.0f;               // rotation around +Y
    float cornerRadius = 0.25f;
    float ior = 1.52f;                     // green
    float dispersion = 0.03f;              // ior(blue) - ior(red)
    DirectX::XMFLOAT3 absorption{};        // per MMD unit, linear rgb
};
struct OfflineSoftbox {
    bool enabled = false;
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfU{}, halfV{};    // half-size edge vectors; emits on the side of cross(halfU, halfV)
    DirectX::XMFLOAT3 radiance{};          // linear
};
struct OfflineSceneProps {
    OfflineGlassBox glass;
    OfflineSoftbox softboxes[2];
    bool customFloor = false;              // studio floor albedo / reflectivity override
    DirectX::XMFLOAT3 floorAlbedo{0.8f, 0.83f, 0.86f};
    float floorReflectivity = 0.42f;
};

// An extra view of the same scene drawn into a rectangle of the render target (Studio quad view). Raster only, and
// always drawn flat (Unlit / Wireframe): the view is orthographic, `height` world units from bottom to top.
struct ExtraView {
    float rect[4] = {0, 0, 0.5f, 0.5f};  // x, y, w, h as fractions of the render target (origin top left)
    DirectX::XMFLOAT4X4 view{};          // LH view matrix
    float height = 40.0f;
    float nearZ = 0.1f, farZ = 4000.0f;
};

struct FrameView {
    CameraParams camera;
    // Where `camera` is drawn (fractions of the render target). With extraViews it is one quadrant of the target.
    float mainRect[4] = {0, 0, 1, 1};
    std::vector<ExtraView> extraViews;  // at most kMaxExtraViews; non-empty: the extra views draw flat, the camera view keeps its shading; no TAA / upscaler
    LightParams light;
    std::vector<GpuModel*> models;  // drawn in this order (stage parts first, then characters)
    bool studioFloor = false;       // draw the procedural studio floor at y = 0
    bool cameraCut = false;         // discard temporal history (seek, VMD camera cut)
    float focusDistance = 0.0f;     // DoF focus plane as view-space z (MMD units); <= 0: autofocus on the screen centre
    float apertureScale = 1.0f;     // DoF strength of this frame (studio focus keys) x the settings' aperture; 0 = all sharp
    // Studio self-shadow track: turns the sun shadows off for this frame / overrides the cascade range (MMD units, > 0).
    bool shadowsOff = false;
    float shadowDistance = 0.0f;
    // Offline renderer motion blur: the camera at shutter open (the pose at shutter open is the
    // models' previous bone/morph ring entry). Ignored unless motionBlur.
    CameraParams prevCamera;
    bool motionBlur = false;
    OfflineSceneProps offlineProps;  // offline renderer only
    float effectDt = 0.0f;           // >0: override effect dt (seconds; deterministic runs, video exports)
    bool effectStateReset = false;   // force effect state reset (job start, cut)
    bool effectStateAdvance = true;  // advance effect state this frame (false: read latest without advancing)
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
