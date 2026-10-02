// BloomPass: the mip-chain bloom (bloom.hlsl) and the FFT convolution bloom (bloom_fft.hlsl).
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "core/Log.h"
#include <string>

namespace mmdx {

// ---- BloomPass -------------------------------------------------------------------------------

bool BloomPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    const std::filesystem::path f = shaderDir / L"bloom.hlsl";
    if (!(prefilter_.Create(ctx, f, "PSPrefilter", {DXGI_FORMAT_R11G11B10_FLOAT}) &&
          down_.Create(ctx, f, "PSDown", {DXGI_FORMAT_R11G11B10_FLOAT}) &&
          up_.Create(ctx, f, "PSUp", {DXGI_FORMAT_R11G11B10_FLOAT}, {}, FullscreenPipeline::Blend::Additive)))
        return false;
    if (RtPipelinesSupported(ctx)) {
        const ShaderDefines defs = {{"FFT_N", std::to_string(kFftSize)}, {"FFT_LOG2", "9"}};
        const auto ff = shaderDir / L"bloom_fft.hlsl";
        bool ok = fftInput_.Create(ctx, ff, "CSInput", defs) && fftRows_.Create(ctx, ff, "CSFftRows", defs) &&
                  fftCols_.Create(ctx, ff, "CSFftCols", defs) && fftKernel_.Create(ctx, ff, "CSKernel", defs) &&
                  fftOutput_.Create(ctx, f, "PSFftOutput", {DXGI_FORMAT_R11G11B10_FLOAT});
        if (!ok) {
            LOG_WARN("Bloom: convolution bloom unavailable");
            fftInput_ = {};
            fftRows_ = {};
            fftCols_ = {};
            fftKernel_ = {};
            fftOutput_ = {};
        }
    }
    return true;
}

void BloomPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    uint32_t w = Half(targets.outWidth), h = Half(targets.outHeight);
    for (uint32_t i = 0; i < kMips; ++i) {
        mips_[i].Create(ctx, w, h, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv,
                        L"bloom.mip");
        w = Half(w);
        h = Half(h);
    }
    if (fftRows_) {
        gridA_.Create(ctx, kFftSize, kFftSize, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv, L"bloom.fftA");
        gridB_.Create(ctx, kFftSize, kFftSize, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv, L"bloom.fftB");
        kernelSpec_.Create(ctx, kFftSize, kFftSize, DXGI_FORMAT_R32G32B32A32_FLOAT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv, L"bloom.kernel");
        kernelReady_ = false;
    }
}

void BloomPass::ReleaseTargets(Dx12Context& ctx) {
    for (Texture& m : mips_) m.Release(ctx);
    gridA_.Release(ctx);
    gridB_.Release(ctx);
    kernelSpec_.Release(ctx);
    kernelReady_ = false;
}

void BloomPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    t.bloom = nullptr;
    if (!pc.settings.bloom || !mips_[0] || !t.hdrFinal) return;
    if (pc.settings.bloomConvolution && fftRows_ && gridA_ && !pc.offscreen) {
        ExecuteConvolution(pc);
        return;
    }
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    {
        const float c[4] = {1.0f / t.hdrFinal->width, 1.0f / t.hdrFinal->height, pc.settings.bloomThreshold, 1};
        mips_[0].Transition(cmd, kRt);
        prefilter_.Draw(pc, {&mips_[0]}, pc.transient.SrvTable(pc.ctx, {t.hdrFinal}), c, 4);
        mips_[0].Transition(cmd, kSrv);
    }
    for (uint32_t i = 1; i < kMips; ++i) {
        const float c[4] = {1.0f / mips_[i - 1].width, 1.0f / mips_[i - 1].height, 0, 1};
        mips_[i].Transition(cmd, kRt);
        down_.Draw(pc, {&mips_[i]}, pc.transient.SrvTable(pc.ctx, {&mips_[i - 1]}), c, 4);
        mips_[i].Transition(cmd, kSrv);
    }
    for (uint32_t i = kMips - 1; i > 0; --i) {
        const float c[4] = {1.0f / mips_[i].width, 1.0f / mips_[i].height, 0, 1.0f};
        mips_[i - 1].Transition(cmd, kRt);
        up_.Draw(pc, {&mips_[i - 1]}, pc.transient.SrvTable(pc.ctx, {&mips_[i]}), c, 4);
        mips_[i - 1].Transition(cmd, kSrv);
    }
    t.bloom = &mips_[0];
}

// FFT convolution bloom: the thresholded image goes into a 512x512 grid, is transformed with a 2D
// FFT, multiplied by the spectrum of a procedural starburst kernel, inverse-transformed and written
// into mips_[0]. The kernel spectrum is cached in kernelSpec_.
void BloomPass::ExecuteConvolution(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t n = kFftSize;

    // prefilter into mips_[0], then down into mips_[1], as in the mip chain
    {
        const float c[4] = {1.0f / t.hdrFinal->width, 1.0f / t.hdrFinal->height, pc.settings.bloomThreshold, 1};
        mips_[0].Transition(cmd, kRt);
        prefilter_.Draw(pc, {&mips_[0]}, pc.transient.SrvTable(pc.ctx, {t.hdrFinal}), c, 4);
        mips_[0].Transition(cmd, kSrv);
    }
    {
        const float c[4] = {1.0f / mips_[0].width, 1.0f / mips_[0].height, 0, 1};
        mips_[1].Transition(cmd, kRt);
        down_.Draw(pc, {&mips_[1]}, pc.transient.SrvTable(pc.ctx, {&mips_[0]}), c, 4);
        mips_[1].Transition(cmd, kSrv);
    }

    // build the kernel spectrum once (kernel, then rows/cols forward FFT)
    if (!kernelReady_) {
        const float kernelC[8] = {0.003f, 36.0f, 0.03f, 64.0f, 0.6f, 3.0f, 0.5236f, 0.9f};
        gridA_.Transition(cmd, kUav);
        fftKernel_.Dispatch(pc, {}, pc.transient.UavTable(pc.ctx, {&gridA_}), kernelC, 8, Groups(n), Groups(n));
        gridA_.Transition(cmd, kSrvAll);
        gridB_.Transition(cmd, kUav);
        const float zero[4] = {0, 0, 0, 0};
        fftRows_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&gridA_}),
                          pc.transient.UavTable(pc.ctx, {&gridB_}), zero, 4, 1, n);
        gridB_.Transition(cmd, kSrvAll);
        kernelSpec_.Transition(cmd, kUav);
        fftCols_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&gridB_}),
                          pc.transient.UavTable(pc.ctx, {&kernelSpec_}), zero, 4, 1, n);
        kernelSpec_.Transition(cmd, kSrvAll);
        gridA_.Transition(cmd, kSrv);
        gridB_.Transition(cmd, kSrv);
        kernelReady_ = true;
    }

    // content rect: 75% of the grid, centred, preserving the output aspect ratio
    const float aspect = (float)t.outWidth / (float)t.outHeight;
    float cw, ch;
    if (aspect >= 1.0f) {
        cw = 0.75f * n;
        ch = cw / aspect;
    } else {
        ch = 0.75f * n;
        cw = ch * aspect;
    }
    const float ox = (n - cw) * 0.5f, oy = (n - ch) * 0.5f;

    // thresholded image into the grid
    mips_[1].Transition(cmd, kSrvAll);
    gridA_.Transition(cmd, kUav);
    const float inC[4] = {ox, oy, cw, ch};
    fftInput_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&mips_[1]}),
                       pc.transient.UavTable(pc.ctx, {&gridA_}), inC, 4, Groups(n), Groups(n));
    mips_[1].Transition(cmd, kSrv);

    // rows forward
    gridA_.Transition(cmd, kSrvAll);
    gridB_.Transition(cmd, kUav);
    const float zero[4] = {0, 0, 0, 0};
    fftRows_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&gridA_}),
                      pc.transient.UavTable(pc.ctx, {&gridB_}), zero, 4, 1, n);
    gridA_.Transition(cmd, kSrv);

    // columns: multiply by the kernel spectrum and transform back
    gridB_.Transition(cmd, kSrvAll);
    gridA_.Transition(cmd, kUav);
    const float convC[4] = {1, 0, 0, 0};
    fftCols_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&gridB_, &kernelSpec_}),
                      pc.transient.UavTable(pc.ctx, {&gridA_}), convC, 4, 1, n);
    gridB_.Transition(cmd, kSrv);

    // rows inverse
    gridA_.Transition(cmd, kSrvAll);
    gridB_.Transition(cmd, kUav);
    fftRows_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&gridA_}),
                      pc.transient.UavTable(pc.ctx, {&gridB_}), convC, 4, 1, n);
    gridA_.Transition(cmd, kSrv);

    // write the convolved result into mips_[0]
    gridB_.Transition(cmd, kSrv);
    mips_[0].Transition(cmd, kRt);
    const float outC[8] = {ox / n, oy / n, cw / n, ch / n, (float)kMips, 0, 0, 0};
    fftOutput_.Draw(pc, {&mips_[0]}, pc.transient.SrvTable(pc.ctx, {&gridB_}), outC, 8);
    mips_[0].Transition(cmd, kSrv);
    t.bloom = &mips_[0];
}

} // namespace mmdx
