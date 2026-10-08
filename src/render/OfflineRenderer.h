#pragma once
// Offline ("non-real-time") GI renderer for high-quality stills and videos. Independent of the
// real-time graphics settings and without quality options: it always renders at the maximum.
//
// Light transport (shaders/offline_gi.hlsl): brute-force path tracing (no caches, up to 12 bounces,
// next-event estimation at every vertex) with adaptive sampling: each pixel stops when its
// perceptual error is below kOfflineErrorThreshold, between minSamples and maxSamples. Thin-lens
// depth of field focused on FrameView::focusDistance and motion blur (180 degree shutter: every
// iteration rebuilds the character geometry and camera at its own shutter time from the previous
// and current pose). Characters keep the MMD toon key light with GI fill. MMD outlines come from a
// raster inverted-hull layer drawn per iteration with the same camera (shaders/offline_edge.hlsl)
// and averaged. shaders/offline_post.hlsl denoises, adds bloom, composites the outlines and grades
// into an sRGB RGBA8 image.
//
// Per image (Renderer::BeginOffline, then Renderer::RenderOffline once per app frame):
//   Begin   TLAS for the pose uploaded this frame, clears
//   Prepass (job.prepass) the irradiance cache, Cinema 4D style: kOfflinePrepassLevels passes of
//           screen-grid indirect-irradiance samples (512 multi-bounce gather paths each), coarse to
//           fine and adaptive (only where geometry or irradiance changes), one pass per frame,
//           displayed as the cache-lit scene with the sample points as white dots, then smoothed.
//           The render takes indirect diffuse at camera hits from it (brute force where it has
//           no matching surface); direct light, reflections, DoF and motion blur stay per pixel
//   Render  iterations (one sample per active pixel each; geometry/outlines at the iteration's
//           shutter time) until converged or maxSamples; the first frames run 1, 2, 3 ... iterations
//           so the preview visibly refines from noise
//   Done    CSDenoise x3, optional volumetric light (offline_volumetric.hlsl), bloom (soft glow or FFT
//           convolution), CSFinalize -> final image; Renderer::ReadOfflineImage copies it.
//           Shader packs (job.effects, PackEffectPass::RunOffline): the pre-bloom share runs on the lit HDR
//           image (CSLitCompose: albedo, volumetric light and outlines composed in) before the bloom, which
//           then reads the effect's output; the post share runs on the final sRGB image
// Work is split across app frames with a GPU-time budget so the UI stays responsive and no
// command list runs long enough to trigger a TDR. Between frames the preview shows the running
// accumulation (raw, with outlines).
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace mmdx {

class RtScene;
struct ImageRGBA8;

// Fixed output formats: stills and videos in 4K UHD, videos at 60 fps.
inline constexpr uint32_t kOfflineStillWidth = 3840, kOfflineStillHeight = 2160;
inline constexpr uint32_t kOfflineVideoWidth = 3840, kOfflineVideoHeight = 2160;
inline constexpr uint32_t kOfflineVideoFps = 60;
inline constexpr uint32_t kOfflineStillMinSamples = 256, kOfflineStillMaxSamples = 4096;
inline constexpr uint32_t kOfflineVideoMinSamples = 128, kOfflineVideoMaxSamples = 1024;
inline constexpr float kOfflineErrorThreshold = 0.004f;  // standard error of perceptual luminance (~1/255)

inline constexpr uint32_t kOfflinePrepassLevels = 6;

enum class OfflinePhase : uint8_t { Idle = 0, Prepass, Render, Done };

struct OfflineJobDesc {
    uint32_t width = kOfflineStillWidth, height = kOfflineStillHeight;
    uint32_t minSamples = kOfflineStillMinSamples, maxSamples = kOfflineStillMaxSamples;
    bool prepass = true;     // irradiance cache prepass (the render's indirect diffuse); false: brute force
    // Adaptive sampling stop (standard error of perceptual luminance). 0 = every pixel takes
    // maxSamples (fixed workload, the render benchmark).
    float errorThreshold = kOfflineErrorThreshold;
    uint32_t prepassRays = 512;   // multi-bounce gather paths per irradiance cache sample
    uint32_t maxBounces = 12;     // path depth of the render (1..12)
    // Post effects (applied once the image has converged; the preview shows the plain image).
    bool bloom = true;
    bool bloomConvolution = false;   // FFT convolution bloom with the starburst kernel instead of the soft glow
    bool volumetric = false;         // sun shafts + spotlight cones, sun shadowed with ray queries
    float volumetricDensity = 1.0f;  // 0.25 .. 4
    // The shader packs' effect stack (RenderSettings::packEffects of the real-time frame); empty = no effects.
    std::vector<EffectStackEntry> effects;
};

struct OfflineProgress {
    OfflinePhase phase = OfflinePhase::Idle;
    uint32_t width = 0, height = 0;
    uint32_t prepassStep = 0, prepassSteps = 0;  // prepass passes done / planned
    uint32_t samples = 0;                     // render iterations done (samples per active pixel)
    uint32_t minSamples = 0, maxSamples = 0;
    float activeFraction = 1.0f;              // pixels still sampling, from the latest counter readback
    float fraction = 0.0f;                    // 0..1 estimate for this image (1 when Done)
};

// Owned by Renderer (created in Renderer::Initialize when ray tracing is supported). All entry
// points are called by Renderer, which binds ctx.SrvHeap() and rewinds `transient` first.
class OfflineRenderer {
public:
    OfflineRenderer();
    ~OfflineRenderer();
    OfflineRenderer(const OfflineRenderer&) = delete;
    OfflineRenderer& operator=(const OfflineRenderer&) = delete;

    // Compiles offline_gi.hlsl / offline_post.hlsl (ComputePipeline) and offline_edge.hlsl (FXC
    // raster PSOs), creates the upload/readback buffers.
    // False (logged) on any failure.
    bool Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir);
    void Shutdown();  // the GPU must be idle

    // Starts a new image and records this frame's first share of work. The GPU must be idle (the
    // Renderer waits) and `rt` must already be built for view.models in `cmd`. With
    // view.motionBlur the models' previous ring entry (frame - 1) must hold the shutter-open pose. `sc` was filled for
    // job.width x job.height without jitter; `lights` holds `lightCount` entries. `frame` is
    // ctx.FrameNumber() (bone/morph ring entry for the outline pass).
    void Begin(ID3D12GraphicsCommandList* cmd, TransientDescriptors& transient, const BuiltinTextures* builtin,
               RtScene& rt, const FrameView& view, const SceneConstants& sc, const GpuLight* lights,
               uint32_t lightCount, const OfflineJobDesc& job, uint64_t frame);
    // Records this frame's share of work (no-op when Idle or Done).
    void Render(ID3D12GraphicsCommandList* cmd, TransientDescriptors& transient, const BuiltinTextures* builtin,
                RtScene& rt);
    // Draws the current image (preview or final) letterboxed onto the back buffer, cleared to
    // black around it. Leaves the back buffer RTV bound with a full-window viewport/scissor
    // (the Renderer::Render contract for UI drawing). Writes the image rectangle to rect[4].
    void Present(ID3D12GraphicsCommandList* cmd, TransientDescriptors& transient, float rect[4]);

    const OfflineProgress& Progress() const { return progress_; }
    void Cancel();  // phase -> Idle; resources are kept for the next job
    // Copies the final image (phase Done) into `out` (RGBA8, opaque). Waits for the GPU; must be
    // called outside frame recording.
    bool ReadImage(ImageRGBA8& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    OfflineProgress progress_;
};

} // namespace mmdx
