// DofPass (dof.hlsl): CoC from depth, half-res golden-angle bokeh gather, tent filter, full-res blend.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "core/Log.h"
#include <algorithm>

namespace mmdx {

bool DofPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    const auto f = shaderDir / L"dof.hlsl";
    return prepare_.Create(ctx, f, "PSPrepare", {DXGI_FORMAT_R16G16B16A16_FLOAT}) &&
           gather_.Create(ctx, f, "PSGather", {DXGI_FORMAT_R16G16B16A16_FLOAT}) &&
           tent_.Create(ctx, f, "PSTent", {DXGI_FORMAT_R16G16B16A16_FLOAT}) &&
           combine_.Create(ctx, f, "PSCombine", {RenderTargets::kColorFormat});
}

void DofPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    half_.Create(ctx, Half(targets.outWidth), Half(targets.outHeight), DXGI_FORMAT_R16G16B16A16_FLOAT,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"dof.half");
    blurA_.Create(ctx, Half(targets.outWidth), Half(targets.outHeight), DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"dof.blurA");
    blurB_.Create(ctx, Half(targets.outWidth), Half(targets.outHeight), DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"dof.blurB");
    out_.Create(ctx, targets.outWidth, targets.outHeight, RenderTargets::kColorFormat,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"dof.out");
}

void DofPass::ReleaseTargets(Dx12Context& ctx) {
    half_.Release(ctx);
    blurA_.Release(ctx);
    blurB_.Release(ctx);
    out_.Release(ctx);
}

void DofPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    if (!pc.settings.dof || pc.offscreen || !out_ || !pc.targets.hdrFinal) return;

    const float maxCocPx = pc.settings.dofMaxRadius * static_cast<float>(out_.height) / 1080.0f;
    const float r = maxCocPx * 0.5f;   // max radius in half-res pixels
    const float radScale = std::max(0.5f, r * r / (2.0f * 64.0f));   // keeps the gather at ~64 samples
    // Quad view: the orthographic views are not a lens image, only the camera quadrant gets depth of field (gP2 = its uv rect)
    const bool quad = !pc.view.extraViews.empty();
    const float c[12] = {pc.view.focusDistance, pc.settings.dofAperture, maxCocPx, radScale,
                         1.0f / static_cast<float>(out_.width), 1.0f / static_cast<float>(out_.height),
                         1.0f / static_cast<float>(half_.width), 1.0f / static_cast<float>(half_.height),
                         quad ? pc.view.mainRect[0] : 0.0f, quad ? pc.view.mainRect[1] : 0.0f,
                         quad ? pc.view.mainRect[2] : 0.0f, quad ? pc.view.mainRect[3] : 0.0f};

    t.hdrFinal->Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);

    half_.Transition(cmd, kRt);
    prepare_.Draw(pc, {&half_}, pc.transient.SrvTable(pc.ctx, {t.hdrFinal, nullptr, &t.depth}), c, 12);
    half_.Transition(cmd, kSrv);

    blurA_.Transition(cmd, kRt);
    gather_.Draw(pc, {&blurA_}, pc.transient.SrvTable(pc.ctx, {&half_}), c, 12);
    blurA_.Transition(cmd, kSrv);

    blurB_.Transition(cmd, kRt);
    tent_.Draw(pc, {&blurB_}, pc.transient.SrvTable(pc.ctx, {&blurA_}), c, 12);
    blurB_.Transition(cmd, kSrv);

    out_.Transition(cmd, kRt);
    combine_.Draw(pc, {&out_}, pc.transient.SrvTable(pc.ctx, {t.hdrFinal, &blurB_, &t.depth}), c, 12);
    out_.Transition(cmd, kSrv);

    t.hdrFinal = &out_;
}

} // namespace mmdx
