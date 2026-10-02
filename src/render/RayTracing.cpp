// RtScene: GPU skinning into world-space RtVertex buffers, per-model BLAS and a per-frame
// TLAS consumed by inline ray queries (see render/RayTracing.h).
#include "render/RayTracing.h"
#include "render/GpuModel.h"
#include "render/RenderPass.h"
#include "render/ShaderInterop.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cstring>

namespace mmdx {

namespace {

// Committed buffer (DEFAULT for results/scratch, UPLOAD for the CPU-written rings).
ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                    const wchar_t* name) {
    D3D12_HEAP_PROPERTIES heap{type};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(&res)),
                 "RtScene: CreateCommittedResource"))
        return {};
    res->SetName(name);
    return res;
}

uint64_t Align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// One geometry desc per drawable material (RtScene::EnsureModelResources filtered them).
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> BuildGeometryDescs(GpuModel& model) {
    const auto& rt = model.Rt();
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    descs.reserve(rt.geometryMaterials.size());
    for (uint32_t i : rt.geometryMaterials) {
        const GpuModel::Material& m = model.Materials()[i];
        const MaterialConstants& c = model.MaterialConstantsCpu()[i];
        D3D12_RAYTRACING_GEOMETRY_DESC& d = descs.emplace_back();
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        const bool alphaTest = m.alphaTested || c.diffuse.w < 0.999f;
        d.Flags = (m.castShadow && !alphaTest) ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                               : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        d.Triangles.IndexCount = m.indexCount;
        d.Triangles.IndexBuffer = model.IndexBuffer()->GetGPUVirtualAddress() + (uint64_t)m.indexStart * 4;
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.VertexCount = model.VertexCount();
        d.Triangles.VertexBuffer.StartAddress = rt.vertices->GetGPUVirtualAddress();
        d.Triangles.VertexBuffer.StrideInBytes = sizeof(RtVertex);
        d.Triangles.Transform3x4 = 0;
    }
    return descs;
}

} // namespace

RtScene::~RtScene() {
    Shutdown();
}

bool RtScene::Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    ctx_ = &ctx;
    if (ctx.Caps().raytracingTier < D3D12_RAYTRACING_TIER_1_1 || ctx.Caps().shaderModel < D3D_SHADER_MODEL_6_5) {
        LOG_WARN("ray tracing unavailable: tier %d, shader model 0x%X",
                 (int)ctx.Caps().raytracingTier, (unsigned)ctx.Caps().shaderModel);
        ctx_ = nullptr;
        return false;
    }
    if (!CheckHr(ctx.Device()->QueryInterface(IID_PPV_ARGS(&device5_)), "RtScene: ID3D12Device5")) {
        LOG_WARN("ray tracing unavailable: no ID3D12Device5");
        ctx_ = nullptr;
        return false;
    }

    // skin.hlsl root signature: 0: 4 root constants b0, 1..5: root SRVs t0..t4, 6: root UAV u0.
    ID3D12Device* device = ctx.Device();
    CD3DX12_ROOT_PARAMETER p[7];
    p[0].InitAsConstants(4, 0);
    for (uint32_t i = 0; i < 5; ++i) p[1 + i].InitAsShaderResourceView(i);
    p[6].InitAsUnorderedAccessView(0);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(7, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("RtScene: skin root signature serialization failed");
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&skinRootSig_)),
                 "RtScene: skin CreateRootSignature"))
        return false;

    ComPtr<ID3DBlob> cs = CompileShaderDxc(shaderDir / L"skin.hlsl", "CSSkin", "cs_6_5");
    if (!cs) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = skinRootSig_.Get();
    pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (!CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&skinPso_)), "RtScene: skin PSO"))
        return false;

    LOG_INFO("ray tracing: DXR 1.1 inline ray queries enabled");
    return true;
}

void RtScene::Shutdown() {
    if (instances_ && instancesMapped_) instances_->Unmap(0, nullptr);
    if (geometries_ && geometriesMapped_) geometries_->Unmap(0, nullptr);
    instancesMapped_ = geometriesMapped_ = nullptr;
    instances_.Reset();
    geometries_.Reset();
    tlas_.Reset();
    tlasScratch_.Reset();
    tlasBytes_ = tlasScratchBytes_ = 0;
    tlasCapacity_ = instanceCapacity_ = geometryCapacity_ = 0;
    lastSlot_ = 0;
    skinPso_.Reset();
    skinRootSig_.Reset();
    device5_.Reset();
    ready_ = false;
    ctx_ = nullptr;
}

bool RtScene::EnsureModelResources(GpuModel& model) {
    auto& rt = model.Rt();
    if (rt.vertices) return true;
    ID3D12Device* device = ctx_->Device();
    const uint32_t vc = std::max(1u, model.VertexCount());
    rt.vertices = CreateBuffer(device, (uint64_t)vc * sizeof(RtVertex), D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rt.vertices");
    if (!rt.vertices) return false;
    rt.verticesState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    rt.srv = ctx_->SrvHeap().Allocate(2);
    if (rt.srv == DescriptorHeap::kInvalid) {
        LOG_ERROR("RtScene: out of SRV descriptors");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    // +0: raw SRV over the RtVertex buffer (48 bytes = 12 floats per vertex).
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.Buffer.NumElements = (UINT)(vc * (sizeof(RtVertex) / 4));
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(rt.vertices.Get(), &srv, ctx_->SrvHeap().Cpu(rt.srv));
    // +1: raw SRV over the uint32 index buffer (ByteAddressBuffer in rt_common.hlsli).
    srv.Buffer.NumElements = std::max(1u, model.IndexCount());
    device->CreateShaderResourceView(model.IndexBuffer(), &srv, ctx_->SrvHeap().Cpu(rt.srv + 1));

    // One geometry per drawable material.
    for (size_t i = 0; i < model.Materials().size(); ++i) {
        const GpuModel::Material& m = model.Materials()[i];
        const MaterialConstants& c = model.MaterialConstantsCpu()[i];
        if (m.indexCount > 0 && c.diffuse.w > 0.001f) rt.geometryMaterials.push_back((uint32_t)i);
    }
    if (rt.geometryMaterials.empty()) return true;  // the model is simply not in the TLAS

    // BLAS prebuild: size the result and scratch buffers from the prebuild info.
    const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms = BuildGeometryDescs(model);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = model.Role() == ModelRole::Stage
                   ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                   : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.NumDescs = (UINT)geoms.size();
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = geoms.data();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    rt.blasBytes = Align(info.ResultDataMaxSizeInBytes, 256);
    rt.scratchBytes = Align(info.ScratchDataSizeInBytes, 256);
    rt.blas = CreateBuffer(device, rt.blasBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"rt.blas");
    rt.scratch = CreateBuffer(device, rt.scratchBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rt.blas.scratch");
    return rt.blas && rt.scratch;
}

bool RtScene::Build(ID3D12GraphicsCommandList* cmd, const std::vector<GpuModel*>& models, uint64_t frame,
                    uint32_t slot) {
    ready_ = false;
    if (!device5_ || !skinPso_) return false;
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4)))) return false;
    ID3D12Device* device = ctx_->Device();

    const auto needsSkin = [](const GpuModel* m) {
        return m->Role() == ModelRole::Character || !const_cast<GpuModel*>(m)->Rt().blasBuilt;
    };
    const auto usable = [](GpuModel* m) { return m && m->VertexCount() > 0; };

    // ---- 1. GPU skinning into the world-space RtVertex buffers -----------
    cmd->SetComputeRootSignature(skinRootSig_.Get());
    cmd->SetPipelineState(skinPso_.Get());
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (GpuModel* m : models) {
        if (!usable(m) || !needsSkin(m)) continue;
        auto& rt = m->Rt();
        if (rt.vertices && rt.verticesState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                rt.vertices.Get(), rt.verticesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            rt.verticesState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    }
    if (!barriers.empty()) cmd->ResourceBarrier((UINT)barriers.size(), barriers.data());
    for (GpuModel* m : models) {
        if (!usable(m) || !needsSkin(m) || !EnsureModelResources(*m)) continue;
        const auto& rt = m->Rt();
        const uint32_t c[4] = {m->VertexCount(), 0, 0, 0};
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->SetComputeRootShaderResourceView(1, m->VertexBuffer()->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(2, m->BoneBuffer(frame));
        cmd->SetComputeRootShaderResourceView(3, m->PrevBoneBuffer(frame));
        cmd->SetComputeRootShaderResourceView(4, m->MorphBuffer(frame));
        cmd->SetComputeRootShaderResourceView(5, m->PrevMorphBuffer(frame));
        cmd->SetComputeRootUnorderedAccessView(6, rt.vertices->GetGPUVirtualAddress());
        cmd->Dispatch((m->VertexCount() + 63) / 64, 1, 1);
    }
    barriers.clear();
    for (GpuModel* m : models) {
        auto& rt = m->Rt();
        if (usable(m) && rt.vertices && rt.verticesState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                rt.vertices.Get(), rt.verticesState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
            rt.verticesState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }
    if (!barriers.empty()) cmd->ResourceBarrier((UINT)barriers.size(), barriers.data());

    // ---- 2. BLAS builds (characters and not-yet-built stage models) ------
    for (GpuModel* m : models) {
        if (!usable(m) || !EnsureModelResources(*m)) continue;
        auto& rt = m->Rt();
        if (!rt.blas || !(m->Role() == ModelRole::Character || !rt.blasBuilt)) continue;
        const auto geoms = BuildGeometryDescs(*m);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.Flags = m->Role() == ModelRole::Stage
                             ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                             : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        d.Inputs.NumDescs = (UINT)geoms.size();
        d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        d.Inputs.pGeometryDescs = geoms.data();
        d.DestAccelerationStructureData = rt.blas->GetGPUVirtualAddress();
        d.ScratchAccelerationStructureData = rt.scratch->GetGPUVirtualAddress();
        cmd4->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        rt.blasBuilt = true;
    }
    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    cmd->ResourceBarrier(1, &uavBarrier);

    // ---- 3. instance descs + geometry table for `slot` -------------------
    uint32_t n = 0, g = 0;
    for (GpuModel* m : models) {
        if (usable(m) && m->Rt().blas) ++n;
        if (usable(m) && m->Rt().blas) g += (uint32_t)m->Rt().geometryMaterials.size();
    }
    if (n == 0) return false;
    if (n > instanceCapacity_ || g > geometryCapacity_) {
        const uint32_t newInstances =
            std::max(std::max(n, 2 * instanceCapacity_), 16u);
        const uint32_t newGeometries =
            std::max(std::max(g, 2 * geometryCapacity_), 16u);
        auto newInstancesBuf = CreateBuffer(device,
                                            (uint64_t)kSlots * newInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, L"rt.instances");
        auto newGeometriesBuf =
            CreateBuffer(device, (uint64_t)kSlots * newGeometries * sizeof(RtGeometry),
                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, L"rt.geometries");
        if (!newInstancesBuf || !newGeometriesBuf) return false;
        void* im = nullptr;
        void* gm = nullptr;
        if (FAILED(newInstancesBuf->Map(0, nullptr, &im)) || FAILED(newGeometriesBuf->Map(0, nullptr, &gm)))
            return false;
        if (instances_) ctx_->DeferRelease(instances_);
        if (geometries_) ctx_->DeferRelease(geometries_);
        instances_ = newInstancesBuf;
        geometries_ = newGeometriesBuf;
        instancesMapped_ = (uint8_t*)im;
        geometriesMapped_ = (uint8_t*)gm;
        instanceCapacity_ = newInstances;
        geometryCapacity_ = newGeometries;
    }
    // One ring entry per frame slot; 64 / 80 are the descriptor sizes.
    auto* inst = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(
        instancesMapped_ + (size_t)slot * instanceCapacity_ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    auto* geom = reinterpret_cast<RtGeometry*>(
        geometriesMapped_ + (size_t)slot * geometryCapacity_ * sizeof(RtGeometry));
    uint32_t base = 0;
    for (GpuModel* m : models) {
        if (!usable(m) || !EnsureModelResources(*m)) continue;
        auto& rt = m->Rt();
        if (!rt.blas) continue;
        D3D12_RAYTRACING_INSTANCE_DESC id{};
        id.Transform[0][0] = id.Transform[1][1] = id.Transform[2][2] = 1;
        id.InstanceID = base;
        id.InstanceMask = 0xFF;
        id.InstanceContributionToHitGroupIndex = 0;
        id.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        id.AccelerationStructure = rt.blas->GetGPUVirtualAddress();
        *inst++ = id;
        for (uint32_t mi : rt.geometryMaterials) {
            const GpuModel::Material& m2 = m->Materials()[mi];
            const MaterialConstants& c = m->MaterialConstantsCpu()[mi];
            const bool alphaTest = m2.alphaTested || c.diffuse.w < 0.999f;
            RtGeometry e{};
            e.vertexSrv = rt.srv;
            e.indexSrv = rt.srv + 1;
            e.indexStart = m2.indexStart;
            e.flags = c.flags | (m2.castShadow ? RtGeom_CastShadow : 0) | (alphaTest ? RtGeom_AlphaTest : 0) |
                      (m->Role() == ModelRole::Character ? RtGeom_Character : 0);
            e.diffuse = c.diffuse;
            e.specular = c.specular;
            e.specularPower = c.specularPower;
            e.ambient = c.ambient;
            e.reflectivity = c.reflectivity;
            e.textureSrv = m2.srvTable;
            e.sphereSrv = m2.srvTable + 1;
            e.toonSrv = m2.srvTable + 2;
            *geom++ = e;
        }
        base += (uint32_t)rt.geometryMaterials.size();
    }

    // ---- 4. TLAS ---------------------------------------------------------
    if (n > tlasCapacity_) {
        const uint32_t cap = std::max(std::max(n, 2 * tlasCapacity_), 8u);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS pre{};
        pre.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        pre.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        pre.NumDescs = cap;
        pre.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&pre, &info);
        tlasBytes_ = Align(info.ResultDataMaxSizeInBytes, 256);
        tlasScratchBytes_ = Align(info.ScratchDataSizeInBytes, 256);
        auto newTlas = CreateBuffer(device, tlasBytes_, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"rt.tlas");
        auto newScratch = CreateBuffer(device, tlasScratchBytes_, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rt.tlas.scratch");
        if (!newTlas || !newScratch) return false;
        if (tlas_) ctx_->DeferRelease(tlas_);
        if (tlasScratch_) ctx_->DeferRelease(tlasScratch_);
        tlas_ = newTlas;
        tlasScratch_ = newScratch;
        tlasCapacity_ = cap;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    d.Inputs.NumDescs = n;
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.InstanceDescs = instances_->GetGPUVirtualAddress() +
                             (uint64_t)slot * instanceCapacity_ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    d.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = tlasScratch_->GetGPUVirtualAddress();
    cmd4->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    D3D12_RESOURCE_BARRIER tlasBarrier = CD3DX12_RESOURCE_BARRIER::UAV(tlas_.Get());
    cmd->ResourceBarrier(1, &tlasBarrier);

    lastSlot_ = slot;
    ready_ = true;
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS RtScene::Tlas() const {
    return tlas_ ? tlas_->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS RtScene::Geometries() const {
    return geometries_
               ? geometries_->GetGPUVirtualAddress() + (uint64_t)lastSlot_ * geometryCapacity_ * sizeof(RtGeometry)
               : 0;
}

} // namespace mmdx
