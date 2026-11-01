#pragma once
// Settings of the offline (non-real-time) video render, chosen in the lobby's render dialog and
// persisted in AppSettings. Stills keep the fixed 4K format.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/I18n.h"
#include "render/OfflineRenderer.h"

namespace mmdx {

struct VideoResolution { uint32_t width, height; const char* label; };
inline constexpr VideoResolution kVideoResolutions[] = {
    {1280, 720, "720p"}, {1920, 1080, "1080p"}, {2560, 1440, "1440p"}, {3840, 2160, "4K"}};
inline constexpr int kVideoResolutionCount = 4;
inline constexpr uint32_t kVideoFpsChoices[] = {24, 30, 60};
inline constexpr int kVideoFpsCount = 3;

// Compute studio output size for a given short-side pixel count and aspect ratio.
// Rounded to even integers for video encoding.
inline void ComputeStudioRenderSize(uint32_t shortSide, float aspect, uint32_t& outW, uint32_t& outH) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if (aspect >= 1.0f) {
        outH = (shortSide + 1u) & ~1u;
        outW = (uint32_t)std::lround(shortSide * aspect * 0.5f) * 2u;
    } else {
        outW = (shortSide + 1u) & ~1u;
        outH = (uint32_t)std::lround(shortSide / aspect * 0.5f) * 2u;
    }
}

// Compute studio render resolution from resolution preset (720p/1080p/1440p/4K) and aspect ratio.
inline void StudioVideoResolution(int resIdx, float aspect, uint32_t& outW, uint32_t& outH) {
    resIdx = std::clamp(resIdx, 0, kVideoResolutionCount - 1);
    const uint32_t shortSide = kVideoResolutions[resIdx].height;
    ComputeStudioRenderSize(shortSide, aspect, outW, outH);
}

// GI quality: the sampling budget of the path tracer and the irradiance cache.
struct VideoQualityPreset {
    const char* label;
    uint32_t minSamples, maxSamples;  // adaptive sampling range (samples per pixel)
    float errorThreshold;             // adaptive stop (standard error of perceptual luminance)
    uint32_t prepassRays;             // gather paths per irradiance cache sample
    uint32_t maxBounces;              // path depth
    float relativeCost;               // render time relative to "high" (measured at 1080p, estimates only)
};
inline constexpr VideoQualityPreset kVideoQualities[] = {
    {TrKey("초안"), 32, 128, 0.016f, 128, 4, 0.21f},
    {TrKey("표준"), 64, 384, 0.008f, 256, 8, 0.43f},
    {TrKey("고품질"), kOfflineVideoMinSamples, kOfflineVideoMaxSamples, 0.0053f, 512, 12, 1.0f},
    {TrKey("최고"), 256, 2048, 0.0033f, 1024, 12, 2.1f}};
inline constexpr int kVideoQualityCount = 4;

// Which renderer produces the video frames. The first three are the real-time renderers, driven at
// the video's pace (one frame at a time, at the video resolution); OfflineGI is the offline
// global-illumination renderer.
enum class VideoRenderer : int { Raster = 0, RayTraced = 1, PathTraced = 2, OfflineGI = 3 };
inline constexpr int kVideoRendererCount = 4;
struct VideoRendererInfo { const char* label; const char* summary; };
inline constexpr VideoRendererInfo kVideoRenderers[] = {
    {TrKey("래스터"), TrKey("가장 빠르게 완성되는 기본 화질")},
    {TrKey("실시간 RT"), TrKey("레이 트레이싱 그림자와 반사")},
    {TrKey("실시간 PT"), TrKey("빛의 경로를 추적한 사실적인 조명")},
    {TrKey("오프라인 GI"), TrKey("가장 사실적인 전역 조명, 시간이 오래 걸림")}};

// Quality of the real-time renderers (the GI renderer uses kVideoQualities).
struct VideoRealtimeQuality {
    const char* label;
    uint32_t msaa;            // raster / RT anti-aliasing samples
    uint32_t shadowMapSize;   // per cascade
    uint32_t ptSamples;       // path tracer samples per pixel per pass (1..4)
    uint32_t ptBounces;       // path tracer bounces (1..6)
    uint32_t ptPasses;        // passes for the first video frame and after a cut (warm-up)
    uint32_t ptSteadyPasses;  // passes for every other video frame (the denoiser history carries over)
    float historyMax;         // temporal history length cap
    float historyMinAlpha;    // temporal blend floor
    float historyCarry;       // history length kept across a video frame boundary
    float relativeCost;       // relative to "고품질"
    uint32_t rtaoRays;         // RTAO rays per half-res pixel
};
inline constexpr VideoRealtimeQuality kVideoRealtimeQualities[] = {
    {"초안", 2, 1024, 1, 2, 4, 1, 24.0f, 0.08f, 12.0f, 0.5f, 8},
    {"표준", 4, 2048, 4, 3, 4, 1, 32.0f, 0.05f, 12.0f, 0.75f, 16},
    {"고품질", 8, 4096, 4, 4, 16, 4, 48.0f, 0.03f, 12.0f, 1.0f, 32},
    {"최고", 8, 4096, 4, 6, 24, 8, 64.0f, 0.02f, 12.0f, 2.0f, 48}};

inline bool IsGiRenderer(VideoRenderer r) { return r == VideoRenderer::OfflineGI; }

struct VideoRenderConfig {
    int renderer = (int)VideoRenderer::OfflineGI;  // VideoRenderer
    int resolution = 3;    // index into kVideoResolutions
    int fps = 60;          // one of kVideoFpsChoices
    int bitrateMbps = 100; // H.264 video bit rate
    int quality = 2;       // index into kVideoQualities / kVideoRealtimeQualities
    // Effects. Depth of field applies to every renderer (GI: a thin lens; its motion blur stays automatic).
    bool bloom = true;
    bool bloomConvolution = false;
    bool volumetric = false;
    float volumetricDensity = 1.0f;  // 0.25 .. 4
    bool volumetricFull = true;      // S9c: GI computes the medium in the paths (off = the single-scatter pass)
    bool dof = false;
    float dofAperture = 1.0f;        // 0.2 .. 3
    // Transparent-background export (P2.3): write a straight-alpha RGBA8 PNG sequence instead of an MP4.
    bool alpha = false;

    VideoRenderer Renderer() const { return (VideoRenderer)renderer; }
    bool Gi() const { return IsGiRenderer(Renderer()); }
    bool DofApplies() const { return true; }           // whether the DoF option exists for the chosen renderer (all do)
    bool DofActive() const { return DofApplies() && dof; }

    void Clamp() {
        renderer = std::clamp(renderer, 0, kVideoRendererCount - 1);
        resolution = std::clamp(resolution, 0, kVideoResolutionCount - 1);
        if (fps != 24 && fps != 30 && fps != 60) fps = 60;
        bitrateMbps = std::clamp(bitrateMbps, 2, 300);
        quality = std::clamp(quality, 0, kVideoQualityCount - 1);
        volumetricDensity = std::clamp(volumetricDensity, 0.25f, 4.0f);
        dofAperture = std::clamp(dofAperture, 0.2f, 3.0f);
    }
};

// Bit rate that keeps H.264 visually clean for MMD footage at that size and frame rate.
inline int RecommendedBitrateMbps(uint32_t height, int fps) {
    const double base = height >= 2000 ? 100.0 : height >= 1300 ? 50.0 : height >= 1000 ? 25.0 : 12.0;  // at 60 fps
    const double f = fps >= 60 ? 1.0 : fps >= 30 ? 0.65 : 0.55;
    return std::max(2, (int)std::lround(base * f));
}

// Rough wall time of one video frame on an RTX 3060 Laptop at 4K "high", scaling with the pixel count
// and the quality preset: the estimate shown until a sample render has measured the real time.
inline double EstimatedSecondsPerFrame(const VideoRenderConfig& c, float studioAspect = 0.0f) {
    double pixels;
    if (studioAspect > 0.0f) {
        uint32_t sw = 0, sh = 0;
        StudioVideoResolution(c.resolution, studioAspect, sw, sh);
        pixels = (double)sw * sh / (3840.0 * 2160.0);
    } else {
        const VideoResolution& r = kVideoResolutions[std::clamp(c.resolution, 0, kVideoResolutionCount - 1)];
        pixels = (double)r.width * r.height / (3840.0 * 2160.0);
    }
    const int q = std::clamp(c.quality, 0, kVideoQualityCount - 1);
    double base4k, rel;
    switch (c.Renderer()) {
    case VideoRenderer::Raster: base4k = 0.35; rel = kVideoRealtimeQualities[q].relativeCost; break;
    case VideoRenderer::RayTraced: base4k = 0.6; rel = kVideoRealtimeQualities[q].relativeCost; break;
    case VideoRenderer::PathTraced:
        base4k = 0.03 * kVideoRealtimeQualities[q].ptSteadyPasses * kVideoRealtimeQualities[q].ptSamples;
        rel = 1.0;
        break;
    default: base4k = 10.0; rel = kVideoQualities[q].relativeCost; break;
    }
    double t = base4k * pixels * rel;
    if (c.volumetric) t *= c.Gi() ? 1.04 : 1.2;
    return t;
}

// A sample render's measured time per frame for one combination of scene and render options.
struct VideoProbe {
    uint64_t key = 0;
    double secondsPerFrame = 0.0;
};

// Identifies what a sample render measured: the scene assets and every option that changes the cost
// (not the frame rate or bit rate).
inline uint64_t VideoProbeKey(const VideoRenderConfig& c, const std::string& character, const std::string& stage,
                              const std::string& song) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    const auto mixStr = [&](const std::string& s) {
        mix(s.data(), s.size());
        const char sep = '';
        mix(&sep, 1);
    };
    mixStr(character);
    mixStr(stage);
    mixStr(song);
    const int fields[] = {c.renderer, c.resolution, c.quality, c.bloom, c.bloomConvolution && c.bloom, c.volumetric,
                          c.DofApplies() && c.dof};
    mix(fields, sizeof(fields));
    return h;
}

} // namespace mmdx
