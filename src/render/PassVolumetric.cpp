// VolumetricPass (volumetric.hlsl, volumetric_apply.hlsl): half-res in-scattering march,
// depth-aware separable blur, additive depth-aware upsample into lit.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "core/Log.h"

namespace mmdx {

bool VolumetricPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    if (!RtPipelinesSupported(ctx)) {
        LOG_WARN("Volumetric: compute shaders unavailable");
        return true;
    }
    const bool ok = march_.Create(ctx, shaderDir / L"volumetric.hlsl", "CSMarch") &&
                    blur_.Create(ctx, shaderDir / L"volumetric.hlsl", "CSBlur") &&
                    apply_.Create(ctx, shaderDir / L"volumetric_apply.hlsl", "PSApply",
                                  {RenderTargets::kColorFormat}, {}, FullscreenPipeline::Blend::Additive);
    if (!ok) {
        LOG_WARN("Volumetric: pipelines unavailable");  // never fails the renderer
        march_ = {};
        blur_ = {};
        apply_ = {};
    }
    return true;
}

void VolumetricPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    const uint32_t w = Half(targets.width), h = Half(targets.height);
    raw_.Create(ctx, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                kSrv, L"volumetric.raw");
    temp_.Create(ctx, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 kSrv, L"volumetric.temp");
}

void VolumetricPass::ReleaseTargets(Dx12Context& ctx) {
    raw_.Release(ctx);
    temp_.Release(ctx);
}

void VolumetricPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    if (!pc.settings.volumetric || pc.offscreen || !march_ || !apply_ || !raw_) return;

    raw_.Transition(cmd, kUav);
    t.depth.Transition(cmd, kSrvAll);
    t.shadowMap.Transition(cmd, kSrvAll);
    const float c0[8] = {0.005f * pc.settings.volumetricDensity, 0.02f, 400.0f, 0.55f,
                         1.0f / raw_.width, 1.0f / raw_.height, 3.0f, 0.3f};
    march_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.shadowMap}),
                    pc.transient.UavTable(pc.ctx, {&raw_}), c0, 8, Groups(raw_.width), Groups(raw_.height));

    raw_.Transition(cmd, kSrvAll);
    temp_.Transition(cmd, kUav);
    const float ch[4] = {1, 0, 0, 0};
    blur_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&raw_, &t.depth}),
                   pc.transient.UavTable(pc.ctx, {&temp_}), ch, 4, Groups(raw_.width), Groups(raw_.height));
    temp_.Transition(cmd, kSrvAll);
    raw_.Transition(cmd, kUav);
    const float cv[4] = {0, 1, 0, 0};
    blur_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&temp_, &t.depth}),
                   pc.transient.UavTable(pc.ctx, {&raw_}), cv, 4, Groups(raw_.width), Groups(raw_.height));

    raw_.Transition(cmd, kSrv);
    temp_.Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
    t.shadowMap.Transition(cmd, kSrv);
    const float ca[4] = {1.0f / raw_.width, 1.0f / raw_.height, 0, 0};
    t.lit.Transition(cmd, kRt);
    apply_.Draw(pc, {&t.lit}, pc.transient.SrvTable(pc.ctx, {&raw_, &t.depth}), ca, 4);
    t.lit.Transition(cmd, kSrv);
}

} // namespace mmdx
