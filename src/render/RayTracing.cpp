// RtScene: GPU skinning into world-space RtVertex buffers, per-model BLAS and a per-frame
// TLAS consumed by inline ray queries (see render/RayTracing.h).
#include "render/RayTracing.h"
#include "render/GpuModel.h"
#include "render/RenderPass.h"
#include "render/PackTextures.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cmath>
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

// Geometry range [first, end) of BLAS part `part` within Rt().geometryMaterials.
std::pair<uint32_t, uint32_t> PartRange(const GpuModel::RtResources& rt, int part) {
    return part == 0 ? std::make_pair(0u, rt.partSplit)
                     : std::make_pair(rt.partSplit, (uint32_t)rt.geometryMaterials.size());
}

// One geometry desc per drawable material of one BLAS part (EnsureModelResources filtered them).
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> BuildGeometryDescs(GpuModel& model, int part) {
    const auto& rt = model.Rt();
    const auto [first, end] = PartRange(rt, part);
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    descs.reserve(end - first);
    for (uint32_t k = first; k < end; ++k) {
        const uint32_t i = rt.geometryMaterials[k];
        const GpuModel::Material& m = model.Materials()[i];
        const MaterialConstants& c = model.MaterialConstantsCpu()[i];
        D3D12_RAYTRACING_GEOMETRY_DESC& d = descs.emplace_back();
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // a stage BLAS is built once: materials a material morph targets stay non-opaque so the ray
        // queries see their run-time alpha (RtGeometry). Character BLAS are rebuilt every frame.
        const bool alphaTest = m.alphaTested || c.diffuse.w < 0.999f || (m.morphable && model.Role() == ModelRole::Stage);
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

    // skin.hlsl root signature: 0: 4 root constants b0, 1..5: root SRVs t0..t4, 6: root UAV u0,
    // 7: root SRV t5 (SDEF parameters).
    ID3D12Device* device = ctx.Device();
    CD3DX12_ROOT_PARAMETER p[8];
    p[0].InitAsConstants(4, 0);
    for (uint32_t i = 0; i < 5; ++i) p[1 + i].InitAsShaderResourceView(i);
    p[6].InitAsUnorderedAccessView(0);
    p[7].InitAsShaderResourceView(5);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(8, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
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
    if (packRecords_ && packRecordsMapped_) packRecords_->Unmap(0, nullptr);
    if (packSrvBase_ != DescriptorHeap::kInvalid && ctx_) {
        ctx_->SrvHeap().Free(packSrvBase_, kSlots * packRecordCapacity_);
        packSrvBase_ = DescriptorHeap::kInvalid;
    }
    instancesMapped_ = geometriesMapped_ = packRecordsMapped_ = nullptr;
    instances_.Reset();
    geometries_.Reset();
    packRecords_.Reset();
    tlas_.Reset();
    tlasScratch_.Reset();
    tlasBytes_ = tlasScratchBytes_ = 0;
    tlasCapacity_ = instanceCapacity_ = geometryCapacity_ = packRecordCapacity_ = 0;
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

    // One geometry per drawable material: single-sided ones first (part 0), then double-sided.
    for (int part = 0; part < 2; ++part) {
        if (part == 1) rt.partSplit = (uint32_t)rt.geometryMaterials.size();
        for (size_t i = 0; i < model.Materials().size(); ++i) {
            const GpuModel::Material& m = model.Materials()[i];
            const MaterialConstants& c = model.MaterialConstantsCpu()[i];
            if (m.indexCount > 0 && (c.diffuse.w > 0.001f || m.morphable) && m.doubleSided == (part == 1))
                rt.geometryMaterials.push_back((uint32_t)i);
        }
    }

    // BLAS prebuild per non-empty part (a model with neither is simply not in the TLAS).
    for (int part = 0; part < 2; ++part) {
        const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms = BuildGeometryDescs(model, part);
        if (geoms.empty()) continue;
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
        auto& bp = rt.parts[part];
        bp.blasBytes = Align(info.ResultDataMaxSizeInBytes, 256);
        bp.scratchBytes = Align(info.ScratchDataSizeInBytes, 256);
        bp.blas = CreateBuffer(device, bp.blasBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"rt.blas");
        bp.scratch = CreateBuffer(device, bp.scratchBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"rt.blas.scratch");
        if (!bp.blas || !bp.scratch) return false;
    }
    return true;
}

bool RtScene::Build(ID3D12GraphicsCommandList* cmd, const std::vector<GpuModel*>& models, uint64_t frame,
                    uint32_t slot, float time) {
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
        uint32_t c[4] = {m->VertexCount(), 0, m->HasSdef() ? 1u : 0u, 0};
        std::memcpy(&c[1], &time, sizeof(float));
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->SetComputeRootShaderResourceView(1, m->VertexBuffer()->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(2, m->BoneBuffer(frame));
        cmd->SetComputeRootShaderResourceView(3, m->PrevBoneBuffer(frame));
        cmd->SetComputeRootShaderResourceView(4, m->MorphBuffer(frame));
        cmd->SetComputeRootShaderResourceView(5, m->PrevMorphBuffer(frame));
        cmd->SetComputeRootUnorderedAccessView(6, rt.vertices->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(7, m->SdefBuffer()->GetGPUVirtualAddress());
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
        if (!(m->Role() == ModelRole::Character || !rt.blasBuilt)) continue;
        for (int part = 0; part < 2; ++part) {
            const auto& bp = rt.parts[part];
            if (!bp.blas) continue;
            const auto geoms = BuildGeometryDescs(*m, part);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
            d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            d.Inputs.Flags = m->Role() == ModelRole::Stage
                                 ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                                 : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            d.Inputs.NumDescs = (UINT)geoms.size();
            d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            d.Inputs.pGeometryDescs = geoms.data();
            d.DestAccelerationStructureData = bp.blas->GetGPUVirtualAddress();
            d.ScratchAccelerationStructureData = bp.scratch->GetGPUVirtualAddress();
            cmd4->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        }
        rt.blasBuilt = true;
    }
    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    cmd->ResourceBarrier(1, &uavBarrier);

    // ---- 3. instance descs + geometry table for `slot` -------------------
    uint32_t n = 0, g = 0;
    for (GpuModel* m : models) {
        if (!usable(m)) continue;
        for (const auto& bp : m->Rt().parts) n += bp.blas ? 1 : 0;
        g += (uint32_t)m->Rt().geometryMaterials.size();
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

    uint32_t packGeomCount = 0;
    for (GpuModel* m : models) {
        if (!usable(m) || m->Role() != ModelRole::Character || m->ShaderPackId().empty()) continue;
        const ShaderPack* sp = ShaderPacks().Find(m->ShaderPackId());
        if (sp && sp->hasPtSurface) {
            packGeomCount += (uint32_t)m->Rt().geometryMaterials.size();
        }
    }
    if (packGeomCount > packRecordCapacity_) {
        const uint32_t newPackCapacity = std::max(std::max(packGeomCount, 2 * packRecordCapacity_), 16u);
        auto newPackRecordsBuf = CreateBuffer(device,
                                              (uint64_t)kSlots * newPackCapacity * sizeof(RtPtPackRecord),
                                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, L"rt.packRecords");
        if (!newPackRecordsBuf) return false;
        void* prm = nullptr;
        if (FAILED(newPackRecordsBuf->Map(0, nullptr, &prm))) return false;

        uint32_t newSrvBase = ctx_->SrvHeap().Allocate(kSlots * newPackCapacity);
        if (newSrvBase == DescriptorHeap::kInvalid) {
            newPackRecordsBuf->Unmap(0, nullptr);
            return false;
        }

        for (uint32_t s = 0; s < kSlots; ++s) {
            for (uint32_t r = 0; r < newPackCapacity; ++r) {
                uint32_t idx = s * newPackCapacity + r;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Format = DXGI_FORMAT_R32_TYPELESS;
                srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                srv.Buffer.FirstElement = (UINT)(idx * (sizeof(RtPtPackRecord) / 4));
                srv.Buffer.NumElements = (UINT)(sizeof(RtPtPackRecord) / 4);
                device->CreateShaderResourceView(newPackRecordsBuf.Get(), &srv, ctx_->SrvHeap().Cpu(newSrvBase + idx));
            }
        }

        if (packRecords_) ctx_->DeferRelease(packRecords_);
        if (packSrvBase_ != DescriptorHeap::kInvalid) {
            ctx_->SrvHeap().Free(packSrvBase_, kSlots * packRecordCapacity_);
        }
        packRecords_ = newPackRecordsBuf;
        packRecordsMapped_ = (uint8_t*)prm;
        packSrvBase_ = newSrvBase;
        packRecordCapacity_ = newPackCapacity;
    }

    uint32_t curPackRecord = 0;
    const size_t slotPackBase = (size_t)slot * packRecordCapacity_;

    // One ring entry per frame slot.
    auto* inst = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(
        instancesMapped_ + (size_t)slot * instanceCapacity_ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    auto* geom = reinterpret_cast<RtGeometry*>(
        geometriesMapped_ + (size_t)slot * geometryCapacity_ * sizeof(RtGeometry));
    uint32_t base = 0;
    for (GpuModel* m : models) {
        if (!usable(m) || !EnsureModelResources(*m)) continue;
        auto& rt = m->Rt();
        for (int part = 0; part < 2; ++part) {
            if (!rt.parts[part].blas) continue;
            D3D12_RAYTRACING_INSTANCE_DESC id{};
            id.Transform[0][0] = id.Transform[1][1] = id.Transform[2][2] = 1;
            id.InstanceID = base + PartRange(rt, part).first;
            // 0x01 stage, 0x02 character (punctual-light shadow rays trace characters only)
            id.InstanceMask = m->Role() == ModelRole::Character ? 0x02 : 0x01;
            id.InstanceContributionToHitGroupIndex = 0;
            // MMD front faces are clockwise, the DXR default. Single-sided parts keep culling
            // for rays that ask for it (RAY_FLAG_CULL_BACK_FACING_TRIANGLES).
            id.Flags = part == 1 ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE
                                 : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            id.AccelerationStructure = rt.parts[part].blas->GetGPUVirtualAddress();
            *inst++ = id;
        }

        bool isPtPack = false;
        const ShaderPack* sp = nullptr;
        if (m->Role() == ModelRole::Character && !m->ShaderPackId().empty()) {
            sp = ShaderPacks().Find(m->ShaderPackId());
            if (sp && sp->hasPtSurface) isPtPack = true;
        }

        uint32_t texBase = 0;
        uint32_t texInfo = 0;
        if (isPtPack && sp && packTextures_ && ctx_) {
            const PackTextures::Set* set = packTextures_->Acquire(*ctx_, *sp, m->ShaderTextureFolder());
            if (set && set->srv != DescriptorHeap::kInvalid) {
                uint32_t count = (uint32_t)std::min<size_t>(sp->textures.size(), kPackMaxTextures);
                uint32_t clampMask = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    if (sp->textures[i].clamp) clampMask |= (1u << i);
                }
                texBase = set->srv;
                texInfo = (count & 0xFFu) | ((clampMask & 0xFFFFu) << 8);
            }
        }

        DirectX::XMFLOAT3 headRight = {1.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 headUp = {0.0f, 1.0f, 0.0f};
        DirectX::XMFLOAT3 headForward = {0.0f, 0.0f, -1.0f};
        uint32_t headValid = 0;
        DirectX::XMFLOAT4 headPosScale = {0.0f, 0.0f, 0.0f, 1.0f};   // world head bone position, headScale

        if (isPtPack) {
            const auto& matConsts = m->MaterialConstantsCpu();
            if (!matConsts.empty() && matConsts[0].packHead.w > 0.5f) {
                uint32_t headBone = matConsts[0].packHeadBone;
                const DirectX::XMFLOAT4X4* bonesCur = m->BoneMatricesCpu(frame);
                const DirectX::XMFLOAT4X4* bonesPrev = m->PrevBoneMatricesCpu(frame);
                if (bonesCur && headBone < m->BoneCount()) {
                    DirectX::XMFLOAT4X4 M = bonesCur[headBone];
                    if (time < 1.0f && bonesPrev) {
                        const DirectX::XMFLOAT4X4& P = bonesPrev[headBone];
                        float* mF = reinterpret_cast<float*>(&M);
                        const float* pF = reinterpret_cast<const float*>(&P);
                        for (int k = 0; k < 16; ++k) {
                            mF[k] = pF[k] + (mF[k] - pF[k]) * time;
                        }
                    }
                    DirectX::XMFLOAT3 r = {M._11, M._12, M._13};
                    float scale = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
                    if (scale < 1e-4f) scale = 1.0f;
                    float invScale = 1.0f / scale;
                    headRight = {r.x * invScale, r.y * invScale, r.z * invScale};

                    DirectX::XMFLOAT3 u = {M._21, M._22, M._23};
                    float uLen = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
                    float invU = uLen > 1e-6f ? 1.0f / uLen : 1.0f;
                    headUp = {u.x * invU, u.y * invU, u.z * invU};

                    DirectX::XMFLOAT3 f = {-M._31, -M._32, -M._33};
                    float fLen = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
                    float invF = fLen > 1e-6f ? 1.0f / fLen : 1.0f;
                    headForward = {f.x * invF, f.y * invF, f.z * invF};

                    // bind head position * skinning matrix (row vectors), as PackHeadFrame does
                    const DirectX::XMFLOAT4& hp = matConsts[0].packHead;
                    headPosScale = {hp.x * M._11 + hp.y * M._21 + hp.z * M._31 + M._41,
                                    hp.x * M._12 + hp.y * M._22 + hp.z * M._32 + M._42,
                                    hp.x * M._13 + hp.y * M._23 + hp.z * M._33 + M._43, scale};
                    headValid = 1;
                }
            }
        }

        for (uint32_t mi : rt.geometryMaterials) {
            const GpuModel::Material& m2 = m->Materials()[mi];
            const MaterialConstants& c = m->MaterialConstantsCpu()[mi];
            const bool alphaTest = m2.alphaTested || c.diffuse.w < 0.999f ||
                                   (m2.morphable && m->Role() == ModelRole::Stage);
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

            if (isPtPack && curPackRecord < packRecordCapacity_) {
                uint32_t recordIdx = (uint32_t)slotPackBase + curPackRecord;
                RtPtPackRecord* rec = reinterpret_cast<RtPtPackRecord*>(
                    packRecordsMapped_ + recordIdx * sizeof(RtPtPackRecord));
                rec->materialClass = c.packClass;
                rec->headValid = headValid;
                rec->texBase = texBase;
                rec->texInfo = texInfo;
                rec->headPos = headPosScale;
                rec->headRight = {headRight.x, headRight.y, headRight.z, 0.0f};
                rec->headUp = {headUp.x, headUp.y, headUp.z, 0.0f};
                rec->headForward = {headForward.x, headForward.y, headForward.z, 0.0f};
                std::memcpy(rec->params, c.packParams, 16 * sizeof(float));

                e.packSrv = packSrvBase_ + recordIdx;
                e.flags |= RtGeom_PtPack;
                ++curPackRecord;
            } else {
                e.packSrv = 0;
            }

            e.texMul = c.texMul;
            e.texAdd = c.texAdd;
            e.sphereMul = c.sphereMul;
            e.sphereAdd = c.sphereAdd;
            e.toonMul = c.toonMul;
            e.toonAdd = c.toonAdd;
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
