#pragma once
// Render benchmark ("Cinebench style", category kBenchCategories[kBenchGiRender]): one offline GI
// image of a dedicated scene with a fixed workload, scored by the total render time.
//
// Scene "Prism": three Sour-style Miku models holding poses (frames of dance motions, with a
// physics pre-roll so hair and skirts are mid-motion) behind a large rounded glass cube
// (refraction with dispersion, absorption, transparent shadows) in a dark studio lit by a warm key
// light, two softboxes and coloured rim spots. The camera, lights and props are fixed
// (BuildRenderBenchView); the job is kRenderBenchWidth x kRenderBenchHeight with exactly
// kRenderBenchSamples per pixel (no adaptive stop) plus the irradiance cache prepass.
#include "app/Benchmark.h"
#include "render/RenderTypes.h"
#include <DirectXMath.h>
#include <cstdint>

namespace mmdx {

struct RenderBenchPerformer {
    const char* characterKey;  // case-insensitive substring of CharacterAsset::id
    const char* songKey;       // case-insensitive substring of SongAsset::id: the dance the pose comes from
    float poseFrame;           // MMD frame (30 fps) of the pose
    float x, z;                // where the performer's centre bone ends up on the floor
    float yawDeg;              // XMMatrixRotationY(yaw) applied to the model (models face -z)
};

// [0] is the centre performer (depth-of-field focus on its head).
inline constexpr RenderBenchPerformer kRenderBenchPerformers[3] = {
    {"sour_miku/white.pmx", "ura_omote", 82.0f * 30.0f, 0.0f, 0.0f, 0.0f},
    {"sour_breath/breath you.pmx", "catch_the_wave", 52.0f * 30.0f, -11.5f, 5.0f, -24.0f},
    {"sour_spring/cwl.pmx", "catch_the_wave", 97.0f * 30.0f, 11.5f, 5.0f, 22.0f},
};
inline constexpr int kRenderBenchPerformerCount = 3;
// Motion frames simulated (physics on, 60 Hz steps) before the pose frame.
inline constexpr float kRenderBenchPrerollFrames = 60.0f;

inline constexpr uint32_t kRenderBenchWidth = 3840, kRenderBenchHeight = 2160;
inline constexpr uint32_t kRenderBenchSamples = 4096;

// Camera, lighting, studio floor, depth of field and the offline props (glass cube, softboxes) of
// the scene. `centerHead` is the world position of the centre performer's head bone (focus).
// view.models is left untouched (the caller adds the performers).
void BuildRenderBenchView(const DirectX::XMFLOAT3& centerHead, FrameView& view);

// Score of a finished render: proportional to path samples per second (width * height * spp /
// seconds). Tiers use the same thresholds and titles as the real-time benchmark.
// result.durationSec = seconds, totalFrames = spp, width/height as rendered; avgFps = million
// samples per second; the remaining fps/frametime fields stay 0.
BenchmarkResult ComputeRenderBenchResult(double seconds, uint32_t width, uint32_t height, uint32_t spp);

} // namespace mmdx
