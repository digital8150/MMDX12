// The MVP pass list: ScenePass (MMD forward shading + edges), ResolvePass
// (MSAA -> single-sample) and PresentPass (letterboxed stretch to the back buffer).
#pragma once
#include "render/RenderPass.h"
#include <wrl/client.h>

namespace mmdx {

class ScenePass final : public IRenderPass {
public:
    const char* Name() const override { return "Scene"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    ComPtr<ID3D12PipelineState> psoCullBack_;
    ComPtr<ID3D12PipelineState> psoNoCull_;
    ComPtr<ID3D12PipelineState> psoEdge_;
    ComPtr<ID3D12RootSignature> rootSig_;
};

class ResolvePass final : public IRenderPass {
public:
    const char* Name() const override { return "Resolve"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;
};

class PresentPass final : public IRenderPass {
public:
    const char* Name() const override { return "Present"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12RootSignature> rootSig_;
};

} // namespace mmdx
