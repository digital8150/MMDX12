#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "render/PhysicalSky.h"
#include "render/ShaderInterop.h"
#include "render/WorldTypes.h"
#include "asset/StageManifest.h"

namespace mmdx {

class GpuModel;
struct EffectStackEntry;

// The controller accessory panel of an MME effect (docs/mme/mme_design.md §2.9): CONTROLOBJECT `(self)` reads these.
// `si` is the panel size; CONTROLOBJECT `Si` returns si*10. `rx/ry/rz` are radians.
struct MmeControllerPanel {
    float x = 0, y = 0, z = 0;
    float rx = 0, ry = 0, rz = 0;   // radians
    float si = 1, tr = 1;
    bool visible = true;
};

// Convolution bloom kernel (bloom_fft.hlsl CSKernel): a bright core, a power-law glow and diffraction spikes, built
// per colour channel (dispersion scales the channels apart) and normalised per channel, so every shape keeps the
// scattered energy. The kernel spectrum is rebuilt only when this changes.
struct BloomKernel {
    int rays = 6;                 // diffraction spikes, even, 0..16 (two rays per line through the centre)
    float rotation = 15.0f;       // degrees, 0..180
    float spikeLength = 0.35f;    // 0..1, ray length (1 ~ half the screen)
    float spikeIntensity = 0.3f;  // 0..1, energy share of the rays
    float glowRadius = 0.35f;     // 0..1, reach of the glow
    float glowIntensity = 0.5f;   // 0..1, energy share of the glow (the rest stays in the core)
    float dispersion = 0.2f;      // 0..1, chromatic spread (red reaches farther than blue)
    float anamorphic = 0.0f;      // 0..1, horizontal stretch of the glow (anamorphic lens look)
    bool operator==(const BloomKernel&) const = default;
};

// Kernel presets the UI starts from: 0 cinema (core + glow + short thin spikes), 1 starburst (long rays),
// 2 soft glow (no rays), 3 anamorphic (horizontal streak). Out of range = cinema.
inline constexpr int kBloomKernelPresets = 4;
inline BloomKernel BloomKernelPreset(int preset) {
    BloomKernel k;
    switch (preset) {
    case 1: k = {6, 15.0f, 0.8f, 0.55f, 0.3f, 0.35f, 0.15f, 0.0f}; break;
    case 2: k = {0, 0.0f, 0.0f, 0.0f, 0.55f, 0.7f, 0.25f, 0.0f}; break;
    case 3: k = {2, 0.0f, 0.9f, 0.5f, 0.3f, 0.45f, 0.35f, 0.6f}; break;
    default: break;
    }
    return k;
}

// Bloom look shared by the mip chain and the convolution bloom.
struct BloomLook {
    float radius = 1.0f;     // 0..1, weight of the coarser mip levels (1 = flat, today's wide glow; mip chain only)
    int blend = 0;           // 0 additive, 1 energy-conserving (intensity = share of the bright energy that scatters)
    float depthMask = 0.0f;  // 0..1, keeps the bloom of farther lights off nearer surfaces
    BloomKernel kernel;      // convolution bloom only
    bool operator==(const BloomLook&) const = default;
};

// One MME post effect (--mme): the effect file plus the controller accessory's panel values
// (CONTROLOBJECT `(self)` / `Si`). `si` is the panel value; CONTROLOBJECT `Si` returns si*10.
struct MmeEffectEntry {
    std::filesystem::path file;
    bool enabled = true;
    float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;   // degrees
    float si = 1.0f, tr = 1.0f;
    bool visible = true;
    bool operator==(const MmeEffectEntry&) const = default;
};

// M5c: one MME effect object -- an accessory `.x` plus its same-stem `.fx`, the way MMD users load effects. The
// scene carries these next to `models`. The runtime treats each as a post/scene effect (with `panel` as its `(self)`
// controller, CONTROLOBJECT-addressable by `fileName`) whose object techniques draw `mesh` (the accessory's own
// model, world-space; the App inserts it into FrameView::models so every draw path covers it). `mesh` is null when
// the effect ships no `.x` (`.fx`-only use).
struct MmeEffectObject {
    std::filesystem::path fx;          // effect file
    std::filesystem::path meshFile;    // the `.x` file (empty when none)
    GpuModel* mesh = nullptr;          // the accessory mesh (prop role), null when none
    std::string fileName;              // `.x` file name, no folder (CONTROLOBJECT match), lower-cased
    std::string displayName;
    MmeControllerPanel panel;          // (self) controller panel
    bool enabled = true;
};

// Temporal upscalers behind IUpscaler (see Upscaler.h).
enum class UpscalerKind : uint8_t { None = 0, DLSS, FSR, XeSS };
// Per-dimension ratio output / render: NativeAA 1.0, Quality 1.5, Balanced 1.7,
// Performance 2.0, UltraPerformance 3.0.
enum class UpscalerQuality : uint8_t { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance };

// DLSS Super Resolution model, passed to NGX as a render-preset hint before feature creation.
// Auto = the driver's default; Cnn = preset E (legacy convolutional); Transformer = preset K (DLSS 4);
// Transformer2 = preset M (DLSS 4.5), with preset L for the Ultra Performance quality.
enum class DlssModel : uint8_t { Auto = 0, Cnn, Transformer, Transformer2 };
inline const char* DlssModelId(DlssModel m) {
    switch (m) {
    case DlssModel::Cnn: return "cnn";
    case DlssModel::Transformer: return "transformer";
    case DlssModel::Transformer2: return "transformer2";
    case DlssModel::Auto:
    default: return "auto";
    }
}

// Raster: shadow maps, SSAO, SSR. RayTraced: the raster G-buffer with DXR (inline ray query)
// shadows, ambient occlusion and reflections. PathTraced: compute path tracer + denoiser
// (no MMD outlines). Both DXR paths need raytracing tier 1.1; otherwise Raster is used.
enum class RenderPath : uint8_t { Raster = 0, RayTraced, PathTraced };
inline constexpr uint32_t kMaxExtraViews = 3;

// Shading mode of the real-time raster path (Studio editing views). RayTraced / PathTraced
// ignore the setting and always render Lit.
enum class ViewShading : uint8_t { Lit = 0, Unlit, Wireframe };

// Per-material PBR data source (work unit P2; CLI --material-source override, P5 per-character choice). Default
// never parses Ray data and renders as before; Ray uploads the model's parsed Ray records.
enum class MaterialSource : uint8_t { Default = 0, Ray = 1 };
// PBR debug channel (CLI --pbr-debug): a material with a PBR record outputs one channel in the lit raster PS.
enum class PbrDebug : uint8_t { None = 0, Albedo, Normal, Roughness, Metalness, Ao, Emissive, Model };

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
    DlssModel dlssModel = DlssModel::Auto;   // DLSS SR preset hint (recreates the feature when it changes)
    bool dlssRayReconstruction = false;      // DLSS-RR (ray-traced paths only): replaces the denoising/filtering + upscaler
    RenderPath renderPath = RenderPath::Raster;
    uint32_t ptSamples = 1;          // path tracer samples per pixel per frame (1..4)
    uint32_t ptBounces = 3;          // path tracer bounces after the primary hit (1..6)
    float ptHistoryMax = 24.0f;      // temporal history length cap (video quality presets may raise it)
    float ptHistoryMinAlpha = 0.08f; // temporal blend floor (1 / history length)
    float ptHistoryCarry = 24.0f;    // history length kept across a video frame boundary (no clamp when >= max)
    bool ptAccumAlbedo = false;      // offline video: accumulate the primary albedo too (alpha-tested hair, silhouette AA)
    PbrDebug pbrDebug = PbrDebug::None;                       // P2: debug channel (CLI --pbr-debug)

    // --- lighting / effects (every one can be switched off for an honest benchmark)
    bool shadows = true;
    uint32_t shadowMapSize = 2048;   // per cascade (3 cascades)
    float shadowDistance = 160.0f;   // MMD units covered by the cascades
    bool ssao = true;
    float ssaoRadius = 2.2f;         // MMD units (1 unit ~ 8 cm)
    uint32_t rtaoRays = 4;   // RTAO rays per half-res pixel per frame (1..64)
    float rtaoHistoryMax = 32.0f;   // RTAO temporal history length cap (frames)
    float ssaoIntensity = 0.85f;     // 0..1 blend toward the occluded value
    bool ssr = true;
    float floorGloss = 0.35f;        // reflectivity of upward-facing stage surfaces
    bool taa = false;                // temporal AA (camera jitter + history), works with MSAA
    bool bloom = true;
    bool autoLuminous = true;       // そぼろ AutoLuminous convention: materials marked black-specular/power>=100 glow
    // S8b: stage screens (LED walls / video surfaces) light the raster / RT scene as rectangular area lights whose
    // colour follows the screen's current content. Off, or no screen in the frame = the old image byte for byte.
    // PT / offline GI are unaffected (they already have S8a's emissive NEE).
    bool screenLights = true;
    float bloomIntensity = 0.35f;
    float bloomThreshold = 1.1f;    // linear HDR luminance where bloom starts
    bool bloomConvolution = false;  // FFT convolution bloom with a starburst kernel (falls back to the mip chain)
    BloomLook bloomLook;            // radius / blend / depth mask / convolution kernel
    bool dof = false;               // depth of field (bokeh gather), focus from FrameView::focusDistance
    float dofAperture = 1.0f;       // 0..3, scales the circle of confusion (1 = f/2-ish look)
    float dofMaxRadius = 14.0f;     // max CoC radius in output pixels at 1080p (scaled with output height)
    bool volumetric = false;        // ray-marched sun shafts + spotlight cones (height fog medium)
    float volumetricDensity = 1.0f; // 0..4, scales the medium density
    // S9d: 0 = the froxel grid (inject + light, temporal reuse, front-to-back integration, sampled per pixel), the
    // default; 1 = the per-pixel march of S9 (volumetric.hlsl), kept for A/B and as the fallback.
    int volumetricMode = 0;
    // S9d: the 3-octave multiple-scatter approximation of the froxel injector (Wrenninge 2015); off = single scattering.
    bool volumetricMultiScatter = true;
    // S9: the haze. volumetricAnisotropy is the Henyey-Greenstein g of the medium itself (the sun and punctual terms use
    // it unless a light has its own volumetricG); -1 = the built-in constants (sun 0.55, punctual 0.3) as before.
    float volumetricAnisotropy = -1.0f;
    float lutIntensity = 1.0f;      // 0..1 blend toward the colour LUT (Renderer::SetColorLut); no LUT = off
    float exposure = 1.0f;
    float contrast = 1.06f;
    float saturation = 1.06f;
    float vignette = 0.22f;
    float fog = 0.35f;               // 0 = off; distance haze toward the horizon colour
    bool transparentBackground = false;  // thumbnails: no sky, alpha = coverage
    // Transparent-background video/still export (P2.3): the scene is drawn with transparentBackground set (sky / no
    // background contributes nothing, the resolved colour is premultiplied by the pixel's coverage alpha) and the
    // default studio floor is skipped, so the read-back image gives straight RGBA with a soft coverage alpha.
    bool exportAlpha = false;
    bool headless = false;               // render without touching the back buffer (background sample renders)
    // DLSS Frame Generation multiplier: 0 off, 2 / 3 / 4 (1..3 generated frames). Ignored when unavailable or
    // outside the play scene (the app clears it elsewhere).
    int frameGen = 0;
    // Effect packs ("type": "effect"), the user's ordered stack: PackEffectPass runs the enabled entries
    // (PackEffectStage from each pack's manifest) before Bloom / after Post. Empty = no cost, no targets.
    std::vector<EffectStackEntry> packEffects;
    // MME post effects (--mme), run in list order on the LDR image (MmePostPass). Empty = no cost.
    std::vector<MmeEffectEntry> mmeEffects;
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
    // S9: how much of the light shows in the haze (0 = invisible in the medium, 1 = the physical amount) and the
    // Henyey-Greenstein anisotropy of this light's in-scattering (-0.9..0.95; -1 = the fog's own).
    float volumetric = 1.0f;
    float volumetricG = -1.0f;
    // S7b: a spot's gobo (a stencil projected through the cone). `gobo` is the texture file (absolute: the stage
    // runtime resolves a manifest's relative path before the renderer sees it); empty = no gobo. The value fields are
    // the authored rotation (degrees), uv scale, spin (degrees per second, added with the song time) and focus
    // (0..1: 1 = sharp, below = blurred). Only a spot uses them (a point light has no cone to project through).
    std::string gobo;
    float goboRotation = 0.0f;
    float goboScale = 1.0f;
    float goboSpin = 0.0f;
    float goboFocus = 1.0f;
};

// S9: a local participating-medium volume (studio LightKind::FogVolume). Its density adds to the global height fog
// inside the shape and its colour tints that part of the medium; it lights no surface.
struct FogVolume {
    DirectX::XMFLOAT3 position{0, 20, 0};  // world centre (MMD units)
    DirectX::XMFLOAT3 extent{20, 10, 20};  // half size: box half extents, sphere radii
    DirectX::XMFLOAT3 color{1, 1, 1};      // medium tint (linear)
    float density = 1.0f;                  // multiplier added to the global medium inside the volume
    float edge = 0.35f;                    // 0..1 soft falloff band at the border (of the half size)
    bool sphere = false;                   // box or ellipsoid
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
    float sunVolumetric = 1.0f;           // S9: the sun's share of the haze (LightValues::volumetric)
    float sunVolumetricG = -1.0f;         // S9: the sun's scattering anisotropy (-1 = the fog's)
    std::vector<FogVolume> fogVolumes;    // S9: local medium volumes, up to Renderer::kMaxFogVolumes used
    // S9: the global medium (a stage's environment.fog fills these; BuildSceneLighting leaves the defaults, which are the
    // constants the volumetric pass always used): sigma0 < 0 = 0.005 * RenderSettings::volumetricDensity, the exponential
    // height decay per MMD unit (< 0 = the path's own: real-time 0.012, offline GI 0.02) and the medium's scattering
    // tint (white = plain).
    float volumetricSigma0 = -1.0f;
    float volumetricFalloff = -1.0f;
    DirectX::XMFLOAT3 volumetricColor{1, 1, 1};
    // S9: the fog's Henyey-Greenstein g of this frame (a loaded stage's environment.fog.anisotropy); -1 = the
    // RenderSettings value, and with that also -1 the built-in constants (sun 0.55, punctual 0.3).
    float volumetricAnisotropy = -1.0f;
    // S9: radius of the haze sphere around the origin (a stage's environment.fog.radius); <= 0 = the built-in 180.
    float volumetricRadius = -1.0f;

    // S9b-1: the Physical Sky light (studio::LightKind::Sky, render/PhysicalSky.h). physicalSky = true only with an
    // enabled Sky light: SkyColor then renders the model instead of skyZenith / skyHorizon / groundColor, which the
    // model also writes (the hemisphere fill, the floor and the haze stay coherent with it).
    bool physicalSky = false;
    PhysicalSkyState sky;
    // S9b-3: the Sky light's image (render/SkyMap.h): `skyMap` is the file (absolute: the stage runtime / the studio
    // resolve a relative one before the renderer sees it), empty = no image and the procedural sky above draws.
    // skyMapEncoding: 0 auto, 1 linear, 2 sRGB, 3 Ray-MMD RGBT; skyMapRotationRad rotates the panorama around +y,
    // skyMapIntensity scales its radiance (and the ambient colours derived from it).
    std::string skyMap;
    int skyMapEncoding = 0;
    bool skyMapMirror = false;
    float skyMapRotationRad = 0.0f;
    float skyMapIntensity = 1.0f;
    // S9b-4: the procedural cloud layer of the physical sky (unused on an image sky). cloudCoverage is already gated
    // (0 with no physical sky); height and noise size are in km, the wind offset is the layer's drift at the scene
    // time (km). Raster / RT / PT / offline GI all read the same values through SkyColor.
    float cloudCoverage = 0.0f;
    float cloudOpacity = 1.0f;
    float cloudSoftness = 0.25f;
    float cloudHeightKm = 2.0f;
    float cloudScaleKm = 1.5f;
    DirectX::XMFLOAT2 cloudWindOffset{0.0f, 0.0f};

    // Sun shadow properties
    LightShadowType sunShadow = LightShadowType::Hard;
    float sunShadowSoftness = 0.5f;
    float sunShadowDensity = 1.0f;
    DirectX::XMFLOAT3 sunShadowColor{0, 0, 0};
};

// S9: the haze sphere's radius for the volumetric passes: the frame's (or 180), grown to enclose every fog volume.
// Without an explicit radius it also grows to hold every punctual light that shows in the haze (an arena's rigs
// hang 300+ units from the origin: outside the default sphere their beams crossed no medium at all), capped at 1000.
inline float VolumetricMediumRadius(const LightParams& light) {
    float r = light.volumetricRadius > 0.0f ? light.volumetricRadius : 180.0f;
    if (light.volumetricRadius <= 0.0f) {
        for (const PunctualLight& l : light.punctual) {
            if (l.volumetric <= 0.0f) continue;   // not the intensity: a cue dimming a light must not move the haze
            const DirectX::XMFLOAT3& p = l.position;
            r = std::max(r, std::min(1000.0f, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) + 10.0f));
        }
    }
    for (const FogVolume& f : light.fogVolumes) {
        const DirectX::XMFLOAT3& p = f.position;
        const DirectX::XMFLOAT3& e = f.extent;
        r = std::max(r, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) + std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z));
    }
    return r;
}
// The march length cap: the old 400, or far enough to cross the whole sphere from a distant eye.
inline float VolumetricMaxDistance(const DirectX::XMFLOAT3& eye, float radius) {
    return std::max(400.0f, std::sqrt(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z) + radius);
}

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

// M8b: one video layer of the frame (docs/mme/mme_design.md §5b). Filled by the App for the play scene from the
// song's resolved video package; the renderer composites it (render/PassVideo.cpp, video.hlsl) both in play mode
// and in offline renders. `time` is seconds into the video (the background layer is already looped by the caller
// for play mode; the offline renderer loops it itself).
enum class VideoLayerKind : uint8_t { Background = 0, Overlay };
enum class VideoBlend : uint8_t { BlackKey = 0, Screen, Add, Normal };
struct VideoLayerDesc {
    std::filesystem::path path;
    VideoLayerKind kind = VideoLayerKind::Background;
    VideoBlend blend = VideoBlend::BlackKey;
    double time = 0.0;
    // Feeds materials whose diffuse texture file name is screen.bmp (MMD background AVI); drawn as background only
    // when drawBackground is set (the studio's "배경에도 표시", default off).
    bool screen = false;
    bool drawBackground = true;
};

// M5a: one scene drawable the MME runtime can address through CONTROLOBJECT (§2.9) and match against an offscreen
// target's DefaultEffect table (§2.3). Filled by the App next to FrameView::models (one entry per model, in the same
// order); `gpu` correlates the entry with its FrameView::models pointer. The bone / morph name and value arrays are
// owned by the App and valid for the frame (pointers into the model instance's per-frame data).
struct MmeSceneObject {
    std::string fileName;      // UTF-8, no folder (the model's source file name), lower-case for matching
    std::string displayName;   // PMX display name
    bool visible = true;
    DirectX::XMFLOAT4X4 rootWorld{};   // model root world matrix (identity for PMX)
    const GpuModel* gpu = nullptr;     // correlation with FrameView::models
    // M5c: an accessory `.x` (an MME effect object's mesh): its vertices are model space, so the MME draw must
    // transform them by the controller panel (`panel`), not treat them as world space like a PMX model.
    bool accessory = false;
    MmeControllerPanel panel{};
    std::vector<std::string> boneNames;
    std::vector<DirectX::XMFLOAT4X4> boneWorld;
    std::vector<std::string> morphNames;
    std::vector<float> morphWeights;
};

// An extra view of the same scene drawn into a rectangle of the render target (Studio quad view). Raster only, and
// always drawn flat (Unlit / Wireframe): the view is orthographic, `height` world units from bottom to top.
struct ExtraView {
    float rect[4] = {0, 0, 0.5f, 0.5f};  // x, y, w, h as fractions of the render target (origin top left)
    DirectX::XMFLOAT4X4 view{};          // LH view matrix
    float height = 40.0f;
    float nearZ = 0.1f, farZ = 4000.0f;
};

// S5: one LED / video surface of the frame (docs/stage_format.md `screens`). The app resolves the manifest for this
// frame (screens.<id>.<field> targets included) into a ScreenView per screen; the renderer owns the textures
// (render/StageScreens.h), binds them to the listed materials of `modelUid` and renders camera feeds.
struct ScreenView {
    std::string key;                    // the screen id (a stable key across frames: the same screen keeps its texture)
    uint64_t modelUid = 0;              // the part model the materials belong to (GpuModel::Uid)
    std::vector<uint32_t> materials;    // material indices of that model this screen draws on
    StageScreenSource source = StageScreenSource::None;
    std::filesystem::path path;         // image / video file (absolute; empty = not resolved / no file)
    CameraParams camera;                // source camera: a stage camera name resolved by the app, or a studio camera
    // S6: the "program" source's pack (a bundled stage pack id, "stage:<stage id>/<folder>") and its parameter values at
    // this frame (the manifest's `packs[].params` with the packs.<index>.params.<key> targets applied). Empty pack =
    // nothing to draw (black).
    std::string pack;
    std::map<std::string, float> params;
    uint32_t width = 1920, height = 1080;   // the LED pixel grid (the texture is capped at 2048)
    float brightness = 1.0f;
    bool enabled = true;                // off = black (the surface stays dark)
};

struct FrameView {
    CameraParams camera;
    // Where `camera` is drawn (fractions of the render target). With extraViews it is one quadrant of the target.
    float mainRect[4] = {0, 0, 1, 1};
    std::vector<ExtraView> extraViews;  // at most kMaxExtraViews; non-empty: the extra views draw flat, the camera view keeps its shading; no TAA / upscaler
    LightParams light;
    std::vector<GpuModel*> models;  // drawn in this order (stage parts first, then characters)
    // M5a: MME scene objects (CONTROLOBJECT / DefaultEffect matching), parallel to `models` (same order, same count);
    // empty when the App did not fill them.
    std::vector<MmeSceneObject> mmeObjects;
    // M5c: MME effect objects (accessory `.x` + same-stem `.fx`, docs/mme/mme_design.md §2.9). Their post/scene
    // part runs in the post chain with `panel` as `(self)`; their object part draws `mesh` (the App also appends
    // `mesh` to `models` so the MME draw paths cover it). Empty when the scene has none.
    std::vector<MmeEffectObject> mmeEffectObjects;
    bool studioFloor = false;       // draw the procedural studio floor at y = 0
    bool cameraCut = false;         // discard temporal history (seek, VMD camera cut)
    float frame = 0.0f;             // current frame number / time base
    // This view may use DLSS Frame Generation (live play scene only; the lobby, studio, offline renders,
    // benchmarks and captures leave it false).
    bool frameGen = false;
    bool ptFirstPass = false;       // first pass of a new video frame (clamp the carried history length)
    bool ptStaticPass = false;      // a later pass of the same video frame: same pose and camera, accumulate in place
    int ptPassIndex = 0;            // passes of this video frame already done (0 on the first)
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
    // M8a: the MME scene clock (docs/mme/mme_design.md §5b). sceneTime = seconds since the song / motion start
    // (the frame time at the scene's fps); sceneElapsed = this frame's scene-clock delta. These feed MME TIME /
    // ELAPSEDTIME with SyncInEditMode. In studio edit mode (mmeEditMode) MME TIME without SyncInEditMode uses the
    // wall clock since effect load instead (spec draft §4.6).
    float sceneTime = 0.0f;
    float sceneElapsed = 0.0f;
    bool timelinePlaying = false;   // the timeline is playing this frame (paused = still frames, see PassEffect)
    bool mmeEditMode = false;
    // M8b: the video layers of this frame (background behind the scene, overlay over the final image). Empty when
    // the song has no video package / videos are off.
    std::vector<VideoLayerDesc> videoLayers;
    // Screen-video layers (screen == true) decode synchronously for the rendered time (offline renders): the frame
    // is exact instead of the async decode-ahead ring. Ignored without a screen layer.
    bool videoExact = false;
    // S5: the LED / video surfaces of this frame (render/StageScreens.h). Empty = no screen cost.
    std::vector<ScreenView> screens;
    // Shader pack typed scene and audio inputs
    GpuPackInputs packInputs{};
    // World pack entities (falling snow, ground cover ...): simulated and drawn by WorldPass (render/WorldTypes.h)
    std::vector<WorldInstance> worlds;
};

struct RenderStats {
    uint32_t internalWidth = 0, internalHeight = 0;   // render resolution
    uint32_t outputWidth = 0, outputHeight = 0;       // after the upscaler (== internal without one)
    RenderPath renderPath = RenderPath::Raster;       // path actually used this frame (after fallback)
    UpscalerKind upscaler = UpscalerKind::None;       // upscaler actually used this frame (None if unavailable)
    int frameGenMultiplier = 0;                       // DLSS Frame Generation multiplier used this frame (0 off)
    DlssModel dlssModel = DlssModel::Auto;            // DLSS model actually used this frame
    bool dlssRayReconstruction = false;               // DLSS-RR ran this frame
    uint32_t drawCalls = 0;
    uint64_t triangles = 0;
    float gpuFrameMs = 0;  // from timestamp queries, ~kFramesInFlight frames old
};

} // namespace mmdx
