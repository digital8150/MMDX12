// Texture, TransientDescriptors, FullscreenPipeline and ComputePipeline (see RenderPass.h).
#include "render/RenderPass.h"
#include "render/RayTracing.h"
#include "core/Log.h"
#include <directx/d3dx12.h>

namespace mmdx {

// ---- Texture ----------------------------------------------------------------

bool Texture::Create(Dx12Context& ctx, uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES initial, const wchar_t* name, uint32_t sampleCount,
                     uint32_t arrayCount, const float* clearColor) {
    Release(ctx);
    ID3D12Device* device = ctx.Device();
    const bool isDepth = (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const bool isRt = (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(fmt, w, h, (UINT16)arrayCount, 1, sampleCount, 0, flags);
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clearPtr = nullptr;
    if (isDepth) {
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        clearPtr = &clear;
    } else if (isRt) {
        clear.Format = fmt;
        if (clearColor) memcpy(clear.Color, clearColor, sizeof(float) * 4);
        clearPtr = &clear;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    if (!CheckHr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, clearPtr,
                                                 IID_PPV_ARGS(&res)),
                 "Texture::Create"))
        return false;
    if (name) res->SetName(name);
    state = initial;
    format = fmt;
    srvFormat = isDepth ? DXGI_FORMAT_R32_FLOAT : fmt;
    width = w;
    height = h;
    samples = sampleCount;
    arraySize = arrayCount;

    if (isRt) {
        rtv = ctx.RtvHeap().Allocate(arrayCount);
        for (uint32_t i = 0; i < arrayCount; ++i) {
            D3D12_RENDER_TARGET_VIEW_DESC d{};
            d.Format = fmt;
            if (arrayCount > 1) {
                d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                d.Texture2DArray.FirstArraySlice = i;
                d.Texture2DArray.ArraySize = 1;
            } else {
                d.ViewDimension = sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
            }
            device->CreateRenderTargetView(res.Get(), &d, ctx.RtvHeap().Cpu(rtv + i));
        }
    }
    if (isDepth) {
        dsv = ctx.DsvHeap().Allocate(arrayCount);
        for (uint32_t i = 0; i < arrayCount; ++i) {
            D3D12_DEPTH_STENCIL_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_D32_FLOAT;
            if (arrayCount > 1) {
                d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                d.Texture2DArray.FirstArraySlice = i;
                d.Texture2DArray.ArraySize = 1;
            } else {
                d.ViewDimension = sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            }
            device->CreateDepthStencilView(res.Get(), &d, ctx.DsvHeap().Cpu(dsv + i));
        }
    }
    return true;
}

void Texture::Release(Dx12Context& ctx) {
    if (rtv != DescriptorHeap::kInvalid) ctx.RtvHeap().Free(rtv, arraySize);
    if (dsv != DescriptorHeap::kInvalid) ctx.DsvHeap().Free(dsv, arraySize);
    rtv = dsv = DescriptorHeap::kInvalid;
    if (res) ctx.DeferRelease(res);
    res.Reset();
    width = height = 0;
}

void Texture::Transition(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES to) {
    if (!res || state == to) return;
    D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(res.Get(), state, to);
    cmd->ResourceBarrier(1, &b);
    state = to;
}

void Texture::WriteSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = srvFormat;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (arraySize > 1) {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.ArraySize = arraySize;
    } else if (samples > 1) {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipLevels = 1;
    }
    device->CreateShaderResourceView(res.Get(), &d, cpu);
}

// ---- TransientDescriptors ---------------------------------------------------------

bool TransientDescriptors::Create(Dx12Context& ctx, uint32_t slots) {
    base_ = ctx.SrvHeap().Allocate(kPerSlot * slots);
    if (base_ == DescriptorHeap::kInvalid) {
        LOG_ERROR("TransientDescriptors: out of descriptors");
        return false;
    }
    slots_ = slots;
    return true;
}

void TransientDescriptors::Release(Dx12Context& ctx) {
    if (base_ != DescriptorHeap::kInvalid) ctx.SrvHeap().Free(base_, kPerSlot * slots_);
    base_ = DescriptorHeap::kInvalid;
    slots_ = 0;
}

void TransientDescriptors::Begin(uint32_t slot) {
    slot_ = slot % (slots_ ? slots_ : 1);
    used_ = 0;
}

uint32_t TransientDescriptors::Alloc(uint32_t count) {
    if (base_ == DescriptorHeap::kInvalid || used_ + count > kPerSlot) {
        LOG_ERROR("TransientDescriptors: window exhausted");
        return DescriptorHeap::kInvalid;
    }
    uint32_t index = base_ + slot_ * kPerSlot + used_;
    used_ += count;
    return index;
}

D3D12_GPU_DESCRIPTOR_HANDLE TransientDescriptors::SrvTable(Dx12Context& ctx,
                                                           std::initializer_list<const Texture*> textures) {
    // Always 8 slots so every FullscreenPipeline table range (t0..t7) is fully populated.
    const uint32_t count = 8;
    uint32_t first = Alloc(count);
    if (first == DescriptorHeap::kInvalid) return {};
    ID3D12Device* device = ctx.Device();
    uint32_t i = 0;
    for (const Texture* t : textures) {
        if (i >= count) break;
        if (t && t->res) {
            t->WriteSrv(device, ctx.SrvHeap().Cpu(first + i));
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &d, ctx.SrvHeap().Cpu(first + i));
        }
        ++i;
    }
    for (; i < count; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &d, ctx.SrvHeap().Cpu(first + i));
    }
    return ctx.SrvHeap().Gpu(first);
}

void Texture::WriteUav(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = srvFormat;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(res.Get(), nullptr, &d, cpu);
}

D3D12_GPU_DESCRIPTOR_HANDLE TransientDescriptors::UavTable(Dx12Context& ctx,
                                                           std::initializer_list<const Texture*> textures) {
    // Same structure as SrvTable: always 8 slots so the range u0..u7 is fully populated.
    const uint32_t count = 8;
    uint32_t first = Alloc(count);
    if (first == DescriptorHeap::kInvalid) return {};
    ID3D12Device* device = ctx.Device();
    const auto nullUav = [&](D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(nullptr, nullptr, &d, cpu);
    };
    uint32_t i = 0;
    for (const Texture* t : textures) {
        if (i >= count) break;
        if (t && t->res) t->WriteUav(device, ctx.SrvHeap().Cpu(first + i));
        else nullUav(ctx.SrvHeap().Cpu(first + i));
        ++i;
    }
    for (; i < count; ++i) nullUav(ctx.SrvHeap().Cpu(first + i));
    return ctx.SrvHeap().Gpu(first);
}

// ---- FullscreenPipeline ---------------------------------------------------------

bool FullscreenPipeline::Create(Dx12Context& ctx, const std::filesystem::path& file, const char* psEntry,
                                std::initializer_list<DXGI_FORMAT> rtvFormats, const ShaderDefines& defines,
                                Blend blend) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_DESCRIPTOR_RANGE range;
    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0);
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsConstants(16, 1);
    params[2].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC samplers[3];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(3, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("FullscreenPipeline: root signature serialization failed");
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSig_)),
                 "FullscreenPipeline: CreateRootSignature"))
        return false;

    ComPtr<ID3DBlob> vs = CompileShader(file, "VSFullscreen", "vs_5_1", defines);
    ComPtr<ID3DBlob> ps = CompileShader(file, psEntry, "ps_5_1", defines);
    if (!vs || !ps) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    if (blend != Blend::Opaque) {
        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = blend == Blend::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = blend == Blend::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha = D3D12_BLEND_OP_MAX;
    }
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = (UINT)rtvFormats.size();
    UINT i = 0;
    for (DXGI_FORMAT f : rtvFormats) pso.RTVFormats[i++] = f;
    pso.SampleDesc.Count = 1;
    return CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)),
                   "FullscreenPipeline: CreateGraphicsPipelineState");
}

void FullscreenPipeline::Draw(PassContext& pc, std::initializer_list<Texture*> targets,
                              D3D12_GPU_DESCRIPTOR_HANDLE srvTable, const float* constants,
                              uint32_t constantCount) const {
    if (!pso_ || targets.size() == 0) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
    UINT n = 0;
    const Texture* first = *targets.begin();
    for (Texture* t : targets) rtvs[n++] = pc.ctx.RtvHeap().Cpu(t->rtv);
    cmd->OMSetRenderTargets(n, rtvs, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, (float)first->width, (float)first->height, 0, 1};
    D3D12_RECT sc{0, 0, (LONG)first->width, (LONG)first->height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    if (constants && constantCount) cmd->SetGraphicsRoot32BitConstants(1, constantCount, constants, 0);
    if (srvTable.ptr) cmd->SetGraphicsRootDescriptorTable(2, srvTable);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

// ---- ComputePipeline ---------------------------------------------------------

bool ComputePipeline::Create(Dx12Context& ctx, const std::filesystem::path& file, const char* entry,
                             const ShaderDefines& defines) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_DESCRIPTOR_RANGE srv, uav, tex, buf;
    srv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0);
    uav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0);
    tex.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0);
    buf.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, 0);
    CD3DX12_ROOT_PARAMETER p[9];
    p[0].InitAsConstantBufferView(0);
    p[1].InitAsConstants(16, 1);
    p[2].InitAsDescriptorTable(1, &srv);
    p[3].InitAsDescriptorTable(1, &uav);
    p[4].InitAsShaderResourceView(0, 1);
    p[5].InitAsShaderResourceView(1, 1);
    p[6].InitAsShaderResourceView(2, 1);
    p[7].InitAsDescriptorTable(1, &tex);
    p[8].InitAsDescriptorTable(1, &buf);
    CD3DX12_STATIC_SAMPLER_DESC samplers[4];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[3].Init(4, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    // 1.0 keeps the descriptor tables volatile; required because the unbounded ranges cover
    // unused heap slots.
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(9, p, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("ComputePipeline: root signature serialization failed");
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSig_)),
                 "ComputePipeline: CreateRootSignature"))
        return false;

    ComPtr<ID3DBlob> cs = CompileShaderDxc(file, entry, "cs_6_5", defines);
    if (!cs) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    return CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)),
                   "ComputePipeline: CreateComputePipelineState");
}

void ComputePipeline::Dispatch(PassContext& pc, D3D12_GPU_DESCRIPTOR_HANDLE srvTable,
                               D3D12_GPU_DESCRIPTOR_HANDLE uavTable, const float* constants,
                               uint32_t constantCount, uint32_t groupsX, uint32_t groupsY) const {
    if (!pso_) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const D3D12_GPU_DESCRIPTOR_HANDLE heap0 = pc.ctx.SrvHeap().Gpu(0);
    cmd->SetComputeRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
    if (constants && constantCount) cmd->SetComputeRoot32BitConstants(1, constantCount, constants, 0);
    cmd->SetComputeRootDescriptorTable(2, srvTable.ptr ? srvTable : heap0);
    cmd->SetComputeRootDescriptorTable(3, uavTable.ptr ? uavTable : heap0);
    cmd->SetComputeRootShaderResourceView(4, pc.rt ? pc.rt->Tlas() : 0);
    cmd->SetComputeRootShaderResourceView(5, pc.rt ? pc.rt->Geometries() : 0);
    cmd->SetComputeRootShaderResourceView(6, pc.lights);
    cmd->SetComputeRootDescriptorTable(7, heap0);
    cmd->SetComputeRootDescriptorTable(8, heap0);
    cmd->Dispatch(groupsX, groupsY, 1);
}

} // namespace mmdx
