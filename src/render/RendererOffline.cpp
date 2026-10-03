// Renderer offline entry points (see render/OfflineRenderer.h for the data flow).
#include "render/Renderer.h"
#include "render/RayTracing.h"
#include "asset/ImageLoader.h"
#include <cmath>

namespace mmdx {

bool Renderer::BeginOffline(ID3D12GraphicsCommandList* cmd, const FrameView& view, const OfflineJobDesc& job) {
    if (!offline_ || !rt_ || !cmd || job.width == 0 || job.height == 0) return false;
    ctx_->WaitForGpu();
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    transient_.Begin(ctx_->FrameSlot());
    SceneConstants sc{};
    FillSceneConstants(view, job.width, job.height, /*offscreen*/ true, sc);  // no jitter
    sc.floorGloss = 0.35f;     // independent of the real-time settings
    sc.fog = 0.0f;
    sc.transparentBg = 0.0f;
    if (view.motionBlur)
        DirectX::XMStoreFloat4x4(&sc.prevInvView,
                                 DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&view.prevCamera.view)));
    else
        sc.prevInvView = sc.invView;
    {
        const OfflineSceneProps& p = view.offlineProps;
        const OfflineGlassBox& g = p.glass;
        sc.glassCenter = {g.center.x, g.center.y, g.center.z, g.enabled ? 1.0f : 0.0f};
        sc.glassHalf = {g.halfExtents.x, g.halfExtents.y, g.halfExtents.z, g.cornerRadius};
        sc.glassParams = {std::cos(g.yawRadians), std::sin(g.yawRadians), g.ior, g.dispersion};
        sc.glassAbsorb = {g.absorption.x, g.absorption.y, g.absorption.z, 0.0f};
        for (int i = 0; i < 2; ++i) {
            const OfflineSoftbox& b = p.softboxes[i];
            sc.softbox[i][0] = {b.center.x, b.center.y, b.center.z, b.enabled ? 1.0f : 0.0f};
            sc.softbox[i][1] = {b.halfU.x, b.halfU.y, b.halfU.z, 0.0f};
            sc.softbox[i][2] = {b.halfV.x, b.halfV.y, b.halfV.z, 0.0f};
            sc.softbox[i][3] = {b.radiance.x, b.radiance.y, b.radiance.z, 0.0f};
        }
        sc.floorParams = p.customFloor ? DirectX::XMFLOAT4{p.floorAlbedo.x, p.floorAlbedo.y, p.floorAlbedo.z,
                                                           p.floorReflectivity}
                                       : DirectX::XMFLOAT4{0.0f, 0.0f, 0.0f, -1.0f};
    }
    GpuLight lights[kMaxPunctualLights] = {};
    const uint32_t count = FillGpuLights(view.light, lights);
    if (!rt_->Build(cmd, view.models, ctx_->FrameNumber(), ctx_->FrameSlot())) return false;
    offline_->Begin(cmd, transient_, &builtin_, *rt_, view, sc, lights, count, job, ctx_->FrameNumber());
    if (offlinePresent_) offline_->Present(cmd, transient_, presentRect_);
    sceneVisible_ = false;
    havePrev_ = false;
    return true;
}

void Renderer::RenderOffline(ID3D12GraphicsCommandList* cmd) {
    if (!offline_ || !rt_ || !cmd) return;
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    transient_.Begin(ctx_->FrameSlot());
    offline_->Render(cmd, transient_, &builtin_, *rt_);
    if (offlinePresent_) offline_->Present(cmd, transient_, presentRect_);
    sceneVisible_ = false;
    havePrev_ = false;
}

const OfflineProgress& Renderer::OfflineStatus() const {
    static OfflineProgress idle;
    return offline_ ? offline_->Progress() : idle;
}

bool Renderer::ReadOfflineImage(ImageRGBA8& out) {
    return offline_ && offline_->ReadImage(out);
}

void Renderer::CancelOffline() {
    if (offline_) offline_->Cancel();
}

} // namespace mmdx
