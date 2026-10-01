// ScenePass / ResolvePass / PresentPass implementations.
#include "render/Passes.h"
#include "render/ShaderInterop.h"
#include "render/GpuModel.h"
#include "core/Log.h"
#include <directx/d3dx12.h>

namespace mmdx {

namespace {

struct MmdVertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint16_t bones[4];
    float weights[4];
    float edgeScale;
};

} // namespace

// ---- ScenePass ---------------------------------------------------------------

bool ScenePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    ID3D12Device* device = ctx.Device();
    if (!device) return false;

    // Root signature (version 1.0).
    CD3DX12_ROOT_PARAMETER params[4];
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);   // SceneConstants
    params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);   // MaterialConstants
    params[2].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX); // bone matrices
    CD3DX12_DESCRIPTOR_RANGE table[1];
    table[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    params[3].InitAsDescriptorTable(1, table, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0].Init(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 8);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 16,
                     D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
                     D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(4, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("ScenePass: root signature serialization failed");
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSig_)),
                 "ScenePass: CreateRootSignature"))
        return false;

    ComPtr<ID3DBlob> vs = CompileShader(shaderDir / L"mmd.hlsl", "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderDir / L"mmd.hlsl", "PSMain", "ps_5_1");
    ComPtr<ID3DBlob> vsEdge = CompileShader(shaderDir / L"mmd.hlsl", "VSEdge", "vs_5_1");
    ComPtr<ID3DBlob> psEdge = CompileShader(shaderDir / L"mmd.hlsl", "PSEdge", "ps_5_1");
    if (!vs || !ps || !vsEdge || !psEdge) return false;

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    UINT numElements = (UINT)(sizeof(layout) / sizeof(layout[0]));
    UINT msaaCount = msaa > 1 ? msaa : 1;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {layout, numElements};
    pso.pRootSignature = rootSig_.Get();
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    {
        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = RenderTargets::kColorFormat;
    pso.DSVFormat = RenderTargets::kDepthFormat;
    pso.SampleDesc.Count = msaaCount;
    pso.RasterizerState.MultisampleEnable = msaa > 1 ? TRUE : FALSE;

    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoCullBack_)),
                 "ScenePass: CreateGraphicsPipelineState(back)"))
        return false;

    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoNoCull_)),
                 "ScenePass: CreateGraphicsPipelineState(no cull)"))
        return false;

    pso.VS = {vsEdge->GetBufferPointer(), vsEdge->GetBufferSize()};
    pso.PS = {psEdge->GetBufferPointer(), psEdge->GetBufferSize()};
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoEdge_)),
                 "ScenePass: CreateGraphicsPipelineState(edge)"))
        return false;
    return true;
}

void ScenePass::Execute(PassContext& pc) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& targets = pc.targets;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.RtvHeap().Cpu(targets.colorMsaaRtv);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(targets.depthMsaaDsv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float clearColor[4] = {pc.settings.clearColor.x, pc.settings.clearColor.y,
                                 pc.settings.clearColor.z, pc.settings.clearColor.w};
    cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)targets.width, (float)targets.height, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)targets.width, (LONG)targets.height};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    uint32_t slot = ctx.FrameSlot();
    for (GpuModel* model : pc.view.models) {
        if (!model) continue;
        D3D12_VERTEX_BUFFER_VIEW vbs[2] = {model->VertexBufferView(), model->MorphBufferView(slot)};
        cmd->IASetVertexBuffers(0, 2, vbs);
        cmd->IASetIndexBuffer(&model->IndexBufferView());
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(slot));

        for (const GpuModel::Material& m : model->Materials()) {
            if (m.indexCount == 0) continue;
            cmd->SetPipelineState(m.doubleSided ? psoNoCull_.Get() : psoCullBack_.Get());
            cmd->SetGraphicsRootConstantBufferView(1, m.constants);
            cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
            cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
            pc.stats.drawCalls++;
            pc.stats.triangles += m.indexCount / 3;
        }

        if (pc.settings.drawEdges) {
            cmd->SetPipelineState(psoEdge_.Get());
            for (const GpuModel::Material& m : model->Materials()) {
                if (!m.drawEdge || m.indexCount == 0) continue;
                cmd->SetGraphicsRootConstantBufferView(1, m.constants);
                cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
                cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
                pc.stats.drawCalls++;
            }
        }
    }
    // colorMsaa stays in RENDER_TARGET state (its resting state).
}

// ---- ResolvePass ---------------------------------------------------------------

bool ResolvePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    return true;
}

void ResolvePass::Execute(PassContext& pc) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    RenderTargets& targets = pc.targets;
    if (!targets.colorMsaa || !targets.colorResolved) return;

    if (targets.msaa > 1) {
        D3D12_RESOURCE_BARRIER toResolve[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(targets.colorMsaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(targets.colorResolved.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_RESOLVE_DEST),
        };
        cmd->ResourceBarrier(2, toResolve);
        cmd->ResolveSubresource(targets.colorResolved.Get(), 0, targets.colorMsaa.Get(), 0,
                                RenderTargets::kColorFormat);
    } else {
        D3D12_RESOURCE_BARRIER toCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(targets.colorMsaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(targets.colorResolved.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmd->ResourceBarrier(2, toCopy);
        cmd->CopyResource(targets.colorResolved.Get(), targets.colorMsaa.Get());
    }
    D3D12_RESOURCE_BARRIER back[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(targets.colorMsaa.Get(),
                                             targets.msaa > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                                              : D3D12_RESOURCE_STATE_COPY_SOURCE,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(targets.colorResolved.Get(),
                                             targets.msaa > 1 ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                                                              : D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    cmd->ResourceBarrier(2, back);
}

// ---- PresentPass ---------------------------------------------------------------

bool PresentPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    ID3D12Device* device = ctx.Device();
    if (!device) return false;

    CD3DX12_DESCRIPTOR_RANGE range[1];
    range[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsDescriptorTable(1, range, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler[1];
    sampler[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(1, params, 1, sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("PresentPass: root signature serialization failed");
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSig_)),
                 "PresentPass: CreateRootSignature"))
        return false;

    ComPtr<ID3DBlob> vs = CompileShader(shaderDir / L"present.hlsl", "VSFullscreen", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderDir / L"present.hlsl", "PSPresent", "ps_5_1");
    if (!vs || !ps) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // opaque
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = Dx12Context::kBackBufferFormat;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = 1;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)),
                 "PresentPass: CreateGraphicsPipelineState"))
        return false;
    return true;
}

void PresentPass::Execute(PassContext& pc) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& targets = pc.targets;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.BackBufferRtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->ClearRenderTargetView(rtv, black, 0, nullptr);

    const float bw = (float)ctx.Width();
    const float bh = (float)ctx.Height();
    float s = std::min(bw / (float)targets.width, bh / (float)targets.height);
    float vw = (float)targets.width * s;
    float vh = (float)targets.height * s;
    D3D12_VIEWPORT viewport{(bw - vw) / 2.0f, (bh - vh) / 2.0f, vw, vh, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)bw, (LONG)bh};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    if (!pso_ || !rootSig_) return;
    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->SetGraphicsRootDescriptorTable(0, ctx.SrvHeap().Gpu(targets.colorResolvedSrv));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Contract: UI draws next with the full back buffer visible.
    D3D12_VIEWPORT fullViewport{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
    cmd->RSSetViewports(1, &fullViewport);
    cmd->RSSetScissorRects(1, &scissor);
}

} // namespace mmdx
