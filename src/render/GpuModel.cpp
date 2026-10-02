// GPU-side representation of one PMX model.
#include "render/GpuModel.h"
#include "asset/PmxModel.h"
#include "asset/ImageLoader.h"
#include "render/ShaderInterop.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace mmdx {

namespace {

DirectX::XMFLOAT4X4 Identity4x4() {
    DirectX::XMFLOAT4X4 r;
    DirectX::XMStoreFloat4x4(&r, DirectX::XMMatrixIdentity());
    return r;
}

// A toon ramp is a vertical gradient: every row is (near) one colour. Models made for the
// Project Sekai MME shader put a UV-mapped shadow-colour texture in the toon slot instead.
bool IsToonRamp(const ImageRGBA8& img) {
    if (img.Empty()) return true;
    const ImageRGBA8::Level& l = img.mips[0];
    if (l.width <= 2) return true;
    const uint32_t step = std::max(1u, l.height / 64);
    uint64_t dev = 0, n = 0;
    for (uint32_t y = 0; y < l.height; y += step) {
        const uint8_t* row = &l.pixels[(size_t)y * l.width * 4];
        for (int ch = 0; ch < 3; ++ch) {
            uint32_t sum = 0;
            for (uint32_t x = 0; x < l.width; ++x) sum += row[x * 4 + ch];
            const int mean = (int)(sum / l.width);
            for (uint32_t x = 0; x < l.width; ++x) dev += (uint32_t)std::abs(row[x * 4 + ch] - mean);
            n += l.width;
        }
    }
    return dev < n * 4;  // mean absolute deviation along x below ~1.5%
}

// Face-like material names of the Project Sekai rigs (their Face.fx uses an SDF face map we
// don't have; flat shading is the closest match).
bool IsSekaiFaceMaterial(const std::string& name) {
    std::string n = name;
    for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
    return n == "face" || n == "eyelash" || n == "eyebrow";
}

} // namespace

// Persistently mapped, CPU-written per-frame buffer (UPLOAD heap, GENERIC_READ).
static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, uint32_t bytes, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(upload)"))
        return {};
    res->SetName(name);
    return res;
}

bool GpuModel::Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                      const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin,
                      ModelRole role) {
    ctx_ = &ctx;
    role_ = role;
    name_ = pmx.name;
    boneCount_ = (uint32_t)pmx.bones.size();
    vertexCount_ = (uint32_t)pmx.vertices.size();
    indexCount_ = (uint32_t)pmx.indices.size();

    // Vertices.
    std::vector<GpuVertex> verts(vertexCount_);
    for (uint32_t v = 0; v < vertexCount_; ++v) {
        const PmxVertex& src = pmx.vertices[v];
        GpuVertex& dst = verts[v];
        dst.position[0] = src.position.x;
        dst.position[1] = src.position.y;
        dst.position[2] = src.position.z;
        dst.normal[0] = src.normal.x;
        dst.normal[1] = src.normal.y;
        dst.normal[2] = src.normal.z;
        dst.uv[0] = src.uv.x;
        dst.uv[1] = src.uv.y;
        for (int i = 0; i < 4; ++i) {
            if (src.boneIndex[i] < 0) {
                dst.bones[i] = 0;
                dst.weights[i] = 0;
            } else {
                dst.bones[i] = (uint16_t)src.boneIndex[i];
                dst.weights[i] = src.boneWeight[i];
            }
        }
        dst.edgeScale = src.edgeScale;
    }
    if (vertexCount_ > 0) {
        boundsMin_ = boundsMax_ = pmx.vertices[0].position;
        for (const PmxVertex& v : pmx.vertices) {
            boundsMin_.x = std::min(boundsMin_.x, v.position.x);
            boundsMin_.y = std::min(boundsMin_.y, v.position.y);
            boundsMin_.z = std::min(boundsMin_.z, v.position.z);
            boundsMax_.x = std::max(boundsMax_.x, v.position.x);
            boundsMax_.y = std::max(boundsMax_.y, v.position.y);
            boundsMax_.z = std::max(boundsMax_.z, v.position.z);
        }
    }
    if (boneCount_ == 0) {
        // No bones: everything bound to bone 0 which holds an identity matrix.
        for (GpuVertex& d : verts) {
            d.bones[0] = 0;
            d.weights[0] = 1;
            for (int i = 1; i < 4; ++i) {
                d.bones[i] = 0;
                d.weights[i] = 0;
            }
        }
    }

    vb_ = batch.CreateBuffer(verts.data(), verts.size() * sizeof(GpuVertex),
                             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             L"model.vb");
    if (!vb_) {
        LOG_ERROR("model '%s': vertex buffer creation failed", name_.c_str());
        return false;
    }
    ib_ = batch.CreateBuffer(pmx.indices.data(), pmx.indices.size() * sizeof(uint32_t),
                             D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             L"model.ib");
    if (!ib_) {
        LOG_ERROR("model '%s': index buffer creation failed", name_.c_str());
        return false;
    }
    vbv_.StrideInBytes = sizeof(GpuVertex); // 60
    vbv_.SizeInBytes = (UINT)(verts.size() * sizeof(GpuVertex));
    vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
    ibv_.Format = DXGI_FORMAT_R32_UINT;
    ibv_.SizeInBytes = (UINT)(pmx.indices.size() * sizeof(uint32_t));
    ibv_.BufferLocation = ib_->GetGPUVirtualAddress();

    // Referenced texture indices.
    auto CollectTextureIndex = [&](std::vector<int32_t>& out, int32_t i) {
        if (i < 0 || (size_t)i >= textures.size()) return;
        if (textures[i].Empty()) return;
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    };
    std::vector<int32_t> referenced;
    for (const PmxMaterial& m : pmx.materials) {
        CollectTextureIndex(referenced, m.textureIndex);
        if (m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add)
            CollectTextureIndex(referenced, m.sphereTextureIndex);
        if (!m.sharedToon) CollectTextureIndex(referenced, m.toonIndex);
    }
    textures_.assign(textures.size(), {});
    for (int32_t i : referenced) {
        textures_[i] = batch.CreateTexture(textures[i], L"model.texture");
        if (!textures_[i]) {
            LOG_ERROR("model '%s': texture creation failed", name_.c_str());
            return false;
        }
    }

    // Material constants.
    std::vector<MaterialConstants> consts(pmx.materials.size(), MaterialConstants{});
    for (size_t mIdx = 0; mIdx < pmx.materials.size(); ++mIdx) {
        const PmxMaterial& m = pmx.materials[mIdx];
        MaterialConstants& c = consts[mIdx];
        c.diffuse = m.diffuse;
        c.specular = m.specular;
        c.specularPower = m.specularPower;
        c.ambient = m.ambient;
        c.edgeSize = m.edgeSize;
        c.edgeColor = m.edgeColor;
        bool hasTex = (m.textureIndex >= 0 && (size_t)m.textureIndex < textures_.size() && textures_[m.textureIndex]);
        bool sphereRef =
            (m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add) &&
            m.sphereTextureIndex >= 0 && (size_t)m.sphereTextureIndex < textures_.size() &&
            textures_[m.sphereTextureIndex];
        if (hasTex) c.flags |= MatFlag_HasTexture;
        if (sphereRef) {
            if (m.sphereMode == PmxSphereMode::Multiply) c.flags |= MatFlag_SphereMul;
            if (m.sphereMode == PmxSphereMode::Add) c.flags |= MatFlag_SphereAdd;
        }
        if (m.sharedToon) {
            c.flags |= MatFlag_HasToon;
        } else if (m.toonIndex >= 0 && (size_t)m.toonIndex < textures_.size() && textures_[m.toonIndex]) {
            c.flags |= MatFlag_HasToon;
            if (!IsToonRamp(textures[m.toonIndex])) {
                // Sekai layout: toon = shadow colour (_S), sphere = UV-space parameter mask (_H),
                // which must not be added as an environment sphere.
                c.flags |= MatFlag_ToonMap;
                c.flags &= ~(MatFlag_SphereMul | MatFlag_SphereAdd);
                if (IsSekaiFaceMaterial(m.name)) c.flags |= MatFlag_FlatShade;
            }
        }
        // No toon on a character material: the author wants it unmodelled (typically the face,
        // which often comes with painted hair shadows).
        if (!(c.flags & MatFlag_HasToon) && role != ModelRole::Stage) c.flags |= MatFlag_FlatShade;
        if (m.flags & PmxMat_ReceiveShadow) c.flags |= MatFlag_ReceiveShadow;
        if (role == ModelRole::Stage) c.flags |= MatFlag_Stage;
        // MMD has no roughness: treat a tight, bright specular as a hint of gloss.
        const float specLum = 0.299f * m.specular.x + 0.587f * m.specular.y + 0.114f * m.specular.z;
        const float tightness = std::clamp((m.specularPower - 5.0f) / 60.0f, 0.0f, 1.0f);
        c.reflectivity = std::clamp(specLum * tightness, 0.0f, 1.0f) * (role == ModelRole::Stage ? 0.5f : 0.12f);
    }
    materialCb_ = batch.CreateBuffer(consts.data(), consts.size() * sizeof(MaterialConstants),
                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"model.materialCb");
    if (!materialCb_) {
        LOG_ERROR("model '%s': material constant buffer creation failed", name_.c_str());
        return false;
    }
    materialConsts_ = consts;

    // Per material: descriptor table and Material struct.
    materials_.clear();
    uint32_t indexStart = 0;
    for (size_t mIdx = 0; mIdx < pmx.materials.size(); ++mIdx) {
        const PmxMaterial& m = pmx.materials[mIdx];
        Material mat;
        mat.indexStart = indexStart;
        mat.indexCount = m.indexCount;
        indexStart += m.indexCount;
        mat.doubleSided = (m.flags & PmxMat_DoubleSided) != 0;
        mat.drawEdge = (m.flags & PmxMat_Edge) && m.edgeSize > 0 && m.edgeColor.w > 0;
        mat.castShadow = (m.flags & (PmxMat_CastShadow | PmxMat_GroundShadow)) != 0 && m.diffuse.w > 0.01f;
        mat.alphaTested = m.textureIndex >= 0 && (size_t)m.textureIndex < textures.size() &&
                          textures[m.textureIndex].hasAlpha;
        mat.edgeSize = m.edgeSize;
        mat.constants = materialCb_->GetGPUVirtualAddress() + mIdx * 256;

        uint32_t alloc = ctx.SrvHeap().Allocate(3);
        if (alloc == DescriptorHeap::kInvalid) {
            LOG_ERROR("model '%s': out of SRV descriptors", name_.c_str());
            return false;
        }
        srvAllocations_.push_back(alloc);
        mat.srvTable = alloc;

        ID3D12Device* device = ctx.Device();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = UINT(-1);

        // +0 texture
        if (m.textureIndex >= 0 && (size_t)m.textureIndex < textures_.size() && textures_[m.textureIndex])
            device->CreateShaderResourceView(textures_[m.textureIndex].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 0));
        else
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 0));
        // +1 sphere
        if ((m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add) &&
            m.sphereTextureIndex >= 0 && (size_t)m.sphereTextureIndex < textures_.size() &&
            textures_[m.sphereTextureIndex])
            device->CreateShaderResourceView(textures_[m.sphereTextureIndex].Get(), &srv,
                                             ctx.SrvHeap().Cpu(alloc + 1));
        else
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 1));
        // +2 toon
        if (m.sharedToon) {
            int32_t t = m.toonIndex < 0 ? 0 : (m.toonIndex > 9 ? 9 : m.toonIndex);
            device->CreateShaderResourceView(builtin.toon[t].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        } else if (m.toonIndex >= 0 && (size_t)m.toonIndex < textures_.size() && textures_[m.toonIndex]) {
            device->CreateShaderResourceView(textures_[m.toonIndex].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        } else {
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        }
        materials_.push_back(mat);
    }

    // Per frame slot dynamic buffers.
    const uint32_t boneBytes = (boneCount_ == 0 ? 1u : boneCount_) * 64;
    const uint32_t morphBytes = (vertexCount_ == 0 ? 1u : vertexCount_) * 12;
    for (uint32_t s = 0; s < kRing; ++s) {
        boneBuf_[s] = CreateUploadBuffer(ctx.Device(), boneBytes, L"model.bones");
        if (!boneBuf_[s]) {
            LOG_ERROR("model '%s': bone buffer creation failed", name_.c_str());
            return false;
        }
        if (FAILED(boneBuf_[s]->Map(0, nullptr, &boneMapped_[s]))) {
            LOG_ERROR("model '%s': failed to map bone buffer", name_.c_str());
            return false;
        }
        DirectX::XMFLOAT4X4 identity = Identity4x4();
        for (uint32_t b = 0; b < (boneCount_ == 0 ? 1u : boneCount_); ++b)
            memcpy((uint8_t*)boneMapped_[s] + b * 64, &identity, 64);

        morphBuf_[s] = CreateUploadBuffer(ctx.Device(), morphBytes, L"model.morphs");
        if (!morphBuf_[s]) {
            LOG_ERROR("model '%s': morph buffer creation failed", name_.c_str());
            return false;
        }
        if (FAILED(morphBuf_[s]->Map(0, nullptr, &morphMapped_[s]))) {
            LOG_ERROR("model '%s': failed to map morph buffer", name_.c_str());
            return false;
        }
        memset(morphMapped_[s], 0, morphBytes);
        morphVbv_[s].StrideInBytes = 12;
        morphVbv_[s].SizeInBytes = morphBytes;
        morphVbv_[s].BufferLocation = morphBuf_[s]->GetGPUVirtualAddress();
        morphVersion_[s] = UINT64_MAX;
    }
    return true;
}

void GpuModel::UpdateSkinning(uint64_t frame, const std::vector<DirectX::XMFLOAT4X4>& skin) {
    const size_t count = std::min(skin.size(), (size_t)(boneCount_ == 0 ? 1u : boneCount_));
    const uint32_t r = (uint32_t)(frame % kRing);
    for (uint32_t i = 0; i < kRing; ++i) {
        if ((skinInitialized_ && i != r) || !boneMapped_[i]) continue;
        memcpy(boneMapped_[i], skin.data(), count * 64);
    }
    skinInitialized_ = true;
}

void GpuModel::UpdateMorphs(uint64_t frame, const std::vector<DirectX::XMFLOAT3>& deltas, uint64_t version) {
    const size_t count = std::min(deltas.size(), (size_t)vertexCount_);
    const uint32_t r = (uint32_t)(frame % kRing);
    for (uint32_t i = 0; i < kRing; ++i) {
        if ((morphInitialized_ && i != r) || !morphMapped_[i]) continue;
        if (version == morphVersion_[i]) continue;
        memcpy(morphMapped_[i], deltas.data(), count * 12);
        morphVersion_[i] = version;
    }
    morphInitialized_ = true;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuModel::BoneBuffer(uint64_t frame) const {
    const ComPtr<ID3D12Resource>& b = boneBuf_[frame % kRing];
    return b ? b->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuModel::PrevBoneBuffer(uint64_t frame) const {
    const ComPtr<ID3D12Resource>& b = boneBuf_[(frame + kRing - 1) % kRing];
    return b ? b->GetGPUVirtualAddress() : 0;
}

void GpuModel::Destroy() {
    if (!ctx_) return;
    if (rt_.srv != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(rt_.srv, 2);
    rt_ = RtResources{};
    materialConsts_.clear();
    for (uint32_t a : srvAllocations_) ctx_->SrvHeap().Free(a, 3);
    srvAllocations_.clear();
    for (uint32_t s = 0; s < kRing; ++s) {
        if (boneBuf_[s] && boneMapped_[s]) {
            boneBuf_[s]->Unmap(0, nullptr);
            boneMapped_[s] = nullptr;
        }
        if (morphBuf_[s] && morphMapped_[s]) {
            morphBuf_[s]->Unmap(0, nullptr);
            morphMapped_[s] = nullptr;
        }
        boneBuf_[s].Reset();
        morphBuf_[s].Reset();
        morphVbv_[s] = {};
    }
    vb_.Reset();
    ib_.Reset();
    materialCb_.Reset();
    textures_.clear();
    materials_.clear();
    ctx_ = nullptr;
}

GpuModel::~GpuModel() {
    Destroy();
}

} // namespace mmdx
