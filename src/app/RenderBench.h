#pragma once
// Render benchmark ("Cinebench style", category kBenchCategories[kBenchGiRender]): one offline GI
// image of a dedicated scene with a fixed workload, scored by the total render time.
//
// Scene: the three characters with the most vertices in the user's library holding poses (frames
// of dance motions, with a physics pre-roll so hair and skirts are mid-motion) behind a large
// rounded glass cube
// (refraction with dispersion, absorption, transparent shadows) in a dark studio lit by a warm key
// light, two softboxes and coloured rim spots. The camera, lights and props are fixed
// (BuildRenderBenchView); the job is kRenderBenchWidth x kRenderBenchHeight with exactly
// kRenderBenchSamples per pixel (no adaptive stop) plus the irradiance cache prepass.
#include "app/Benchmark.h"
#include "render/RenderTypes.h"
#include <DirectXMath.h>
#include <cstdint>

namespace mmdx {

// Where the performers stand. [0] is the centre performer (depth-of-field focus on its head).
struct RenderBenchSlot {
    float x, z;     // where the performer's centre bone ends up on the floor
    float yawDeg;   // the performer's upper body faces the camera (-z) turned by this angle (XMMatrixRotationY)
};
inline constexpr RenderBenchSlot kRenderBenchSlots[3] = {
    {0.0f, 0.0f, 0.0f}, {-11.5f, 5.0f, -24.0f}, {11.5f, 5.0f, 22.0f}};
inline constexpr int kRenderBenchPerformerCount = 3;

// The cast, picked from the user's library: the three characters with the most vertices (fewer characters repeat),
// each holding a pose from a dance in the library. Songs and pose frames come from a pseudo-random generator seeded
// by the asset ids, so the same library always renders the same scene.
struct RenderBenchCast {
    int characters[kRenderBenchPerformerCount] = {-1, -1, -1};   // library indices
    int songs[kRenderBenchPerformerCount] = {-1, -1, -1};
    uint32_t seed = 0;
};
bool PickRenderBenchCast(const LibraryScanResult& library, RenderBenchCast& out);   // false: no character or no song
// Candidate pose frame (MMD frames, 30 fps) number `attempt` (0, 1, ...) for performer `slot`: deterministic,
// inside the middle of the dance [25 %, 75 %] of danceSeconds.
float RenderBenchPoseFrame(uint32_t seed, int slot, int attempt, float danceSeconds);
inline constexpr int kRenderBenchPoseAttempts = 8;
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
