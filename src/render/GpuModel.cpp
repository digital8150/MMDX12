// GPU-side representation of one PMX model.
#include "render/GpuModel.h"
#include "asset/DdsLoader.h"
#include "asset/PmxModel.h"
#include "asset/ImageLoader.h"
#include "asset/ModelTraits.h"
#include "asset/PbrMaterial.h"
#include "render/ShaderInterop.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mmdx {

namespace {

DirectX::XMFLOAT4X4 Identity4x4() {
    DirectX::XMFLOAT4X4 r;
    DirectX::XMStoreFloat4x4(&r, DirectX::XMMatrixIdentity());
    return r;
}

uint64_t Rgba8BytesOf(const ImageRGBA8& img) {
    uint64_t bytes = 0;
    for (const ImageRGBA8::Level& l : img.mips) bytes += l.pixels.size();
    return bytes;
}

// S11: the block formats a material texture slot can sample as colour. BC4 / BC5 are one / two channel data maps and
// BC6H is HDR; none of them has an RGBA8 decoder here either, so such a file keeps the pre-S11 behaviour (it counts as
// missing, white is bound) rather than turning up as a red / green / clipped surface.
bool IsMaterialBlockFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC2_UNORM ||
           format == DXGI_FORMAT_BC3_UNORM || format == DXGI_FORMAT_BC7_UNORM;
}

// Bytes of a w x h RGBA8 texture with the full halving mip chain the loader builds (what S11 replaces for a
// block-compressed file whose level 0 has no CPU decode, e.g. BC7).
uint64_t Rgba8ChainBytes(uint32_t w, uint32_t h) {
    uint64_t bytes = 0;
    while (w > 0 && h > 0) {
        bytes += (uint64_t)w * h * 4;
        if (w == 1 && h == 1) break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return bytes;
}

// Face-like material names of MME game rigs (a Face.fx with an SDF face map we
// don't have; flat shading is the closest match).
bool IsFlatFaceMaterial(const std::string& name) {
    std::string n = name;
    for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
    return n == "face" || n == "eyelash" || n == "eyebrow";
}

// The MMD background AVI (背景AVI) feeds materials whose diffuse texture file name is screen.bmp (file name only,
// any directory, case-insensitive).
bool IsScreenTextureName(const std::string& path) {
    if (path.empty()) return false;
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
    return name == "screen.bmp";
}

} // namespace

GpuModel::GpuModel() {
    // Process-wide model id. Never 0 (0 means "no model" in the MME cache keys).
    static std::atomic<uint64_t> sNextUid{1};
    uid_ = sNextUid.fetch_add(1, std::memory_order_relaxed);
}

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
    static const PbrModelData kNoPbr;
    return Create(ctx, batch, pmx, textures, builtin, role, kNoPbr);
}

bool GpuModel::Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                      const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin,
                      ModelRole role, const PbrModelData& pbr) {
    return Create(ctx, batch, pmx, textures, builtin, role, pbr, nullptr);
}

// The material blends (its pixels depend on the draw order) or may: diffuse alpha, a texture with alpha, a dielectric.
static bool MaterialBlends(const PmxModel& pmx, size_t mi, const std::vector<ImageRGBA8>& textures,
                           const PbrModelData& pbr, const std::vector<CompressedImage>* compressed) {
    const PmxMaterial& m = pmx.materials[mi];
    if (m.diffuse.w < 0.999f) return true;
    const int32_t t = m.textureIndex;
    if (t >= 0 && (size_t)t < textures.size() && textures[(size_t)t].hasAlpha) return true;
    if (compressed && t >= 0 && (size_t)t < compressed->size() && (*compressed)[(size_t)t].hasAlpha) return true;
    if (mi < pbr.materials.size() && pbr.materials[mi].valid) {
        const PbrMaterial& p = pbr.materials[mi];
        if (p.ext.transmission > 0.0f || p.ext.transmissionTex.kind != PbrTexKind::None) return true;
    }
    return false;
}

// Sorts the triangles of a large static stage's opaque materials by world xz cell (the draw order inside an opaque
// material does not matter: the depth test decides) and returns the cells. Only a model with one bone and no vertex
// morphs: its cells' bounds stay valid under the root transform. Blending materials keep their authored order and draw
// whole. Empty `sorted` = nothing to do.
static void BuildDrawCells(const PmxModel& pmx, ModelRole role, const std::vector<ImageRGBA8>& textures,
                           const PbrModelData& pbr, const std::vector<CompressedImage>* compressed,
                           std::vector<uint32_t>& sorted, std::vector<GpuModel::DrawCell>& cells,
                           std::vector<std::pair<uint32_t, uint32_t>>& perMaterial) {
    constexpr size_t kMinTriangles = 50000;      // below this a whole stage is cheap: the draw calls would cost more
    constexpr float kCellSize = 50.0f;           // MMD units (about 4 m)
    constexpr float kMaxCellsPerAxis = 64.0f;
    if (role != ModelRole::Stage || pmx.bones.size() > 1 || pmx.indices.size() / 3 < kMinTriangles ||
        pmx.vertices.empty())
        return;
    for (const PmxMorph& mo : pmx.morphs)
        if (mo.type == PmxMorphType::Vertex) return;
    float minX = FLT_MAX, maxX = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
    for (const PmxVertex& v : pmx.vertices) {
        minX = std::min(minX, v.position.x);
        maxX = std::max(maxX, v.position.x);
        minZ = std::min(minZ, v.position.z);
        maxZ = std::max(maxZ, v.position.z);
    }
    const float cellSize = std::max(kCellSize, std::max(maxX - minX, maxZ - minZ) / kMaxCellsPerAxis);
    sorted = pmx.indices;
    perMaterial.assign(pmx.materials.size(), {0u, 0u});
    struct Tri { uint64_t key; uint32_t index; };
    std::vector<Tri> tris;
    uint32_t start = 0;
    size_t splitTriangles = 0;
    for (size_t mi = 0; mi < pmx.materials.size(); ++mi) {
        const uint32_t count = pmx.materials[mi].indexCount;
        const uint32_t materialStart = start;
        start += count;
        if (count < 3 || materialStart + count > pmx.indices.size() ||
            MaterialBlends(pmx, mi, textures, pbr, compressed))
            continue;
        tris.resize(count / 3);
        for (uint32_t t = 0; t < count / 3; ++t) {
            const DirectX::XMFLOAT3& a = pmx.vertices[pmx.indices[materialStart + t * 3 + 0]].position;
            const DirectX::XMFLOAT3& b = pmx.vertices[pmx.indices[materialStart + t * 3 + 1]].position;
            const DirectX::XMFLOAT3& c = pmx.vertices[pmx.indices[materialStart + t * 3 + 2]].position;
            const uint64_t cx = (uint64_t)std::max(0.0f, ((a.x + b.x + c.x) / 3.0f - minX) / cellSize);
            const uint64_t cz = (uint64_t)std::max(0.0f, ((a.z + b.z + c.z) / 3.0f - minZ) / cellSize);
            tris[t] = {(cz << 32) | cx, t};
        }
        std::stable_sort(tris.begin(), tris.end(), [](const Tri& x, const Tri& y) { return x.key < y.key; });
        const uint32_t firstCell = (uint32_t)cells.size();
        uint64_t key = UINT64_MAX;
        for (uint32_t i = 0; i < count / 3; ++i) {
            if (tris[i].key != key) {
                key = tris[i].key;
                GpuModel::DrawCell cell;
                cell.indexStart = materialStart + i * 3;
                cell.boundsMin = {FLT_MAX, FLT_MAX, FLT_MAX};
                cell.boundsMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
                cells.push_back(cell);
            }
            GpuModel::DrawCell& cell = cells.back();
            for (uint32_t v = 0; v < 3; ++v) {
                const uint32_t index = pmx.indices[materialStart + tris[i].index * 3 + v];
                sorted[materialStart + i * 3 + v] = index;
                const DirectX::XMFLOAT3& p = pmx.vertices[index].position;
                cell.boundsMin = {std::min(cell.boundsMin.x, p.x), std::min(cell.boundsMin.y, p.y),
                                  std::min(cell.boundsMin.z, p.z)};
                cell.boundsMax = {std::max(cell.boundsMax.x, p.x), std::max(cell.boundsMax.y, p.y),
                                  std::max(cell.boundsMax.z, p.z)};
            }
            cell.indexCount += 3;
        }
        perMaterial[mi] = {firstCell, (uint32_t)cells.size() - firstCell};
        splitTriangles += count / 3;
    }
    LOG_INFO("STAGECELLS model=%s cell=%.0f cells=%zu triangles=%zu/%zu", pmx.name.c_str(), cellSize, cells.size(),
             splitTriangles, pmx.indices.size() / 3);
    if (cells.empty()) sorted.clear();
}

bool GpuModel::Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                      const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin,
                      ModelRole role, const PbrModelData& pbr, const std::vector<CompressedImage>* compressed) {
    ctx_ = &ctx;
    role_ = role;
    name_ = pmx.name;
    additionalUvCount_ = pmx.additionalUvCount;
    boneCount_ = (uint32_t)pmx.bones.size();
    vertexCount_ = (uint32_t)pmx.vertices.size();
    drawCells_.clear();
    std::vector<uint32_t> sortedIndices;
    std::vector<std::pair<uint32_t, uint32_t>> cellsPerMaterial;
    BuildDrawCells(pmx, role, textures, pbr, compressed, sortedIndices, drawCells_, cellsPerMaterial);
    const std::vector<uint32_t>& indices = sortedIndices.empty() ? pmx.indices : sortedIndices;
    indexCount_ = (uint32_t)indices.size();

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
    // The casters' live bounds. A skinned vertex is a weighted sum of (bone + rotated offset), so it stays within
    // sum(weight x |vertex - bone|) of the bones' hull, whatever the pose. SDEF rotates about C and moves C with the
    // blend of cr0 / cr1 (skinning.hlsli), and vertex morphs add their offsets before skinning (all at weight 1).
    if (role_ == ModelRole::Character && vertexCount_ > 0) {
        const auto distance = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        std::vector<float> morphReach(vertexCount_, 0.0f);
        float morphMax = 0.0f;
        for (const PmxMorph& morph : pmx.morphs) {
            if (morph.type != PmxMorphType::Vertex) continue;
            for (const PmxMorph::VertexOffset& o : morph.vertexOffsets) {
                if (o.vertex < 0 || (uint32_t)o.vertex >= vertexCount_) continue;
                float& sum = morphReach[(size_t)o.vertex];
                sum += distance(o.offset, DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});
                morphMax = std::max(morphMax, sum);
            }
        }
        const DirectX::XMFLOAT3 mid{(boundsMin_.x + boundsMax_.x) * 0.5f, (boundsMin_.y + boundsMax_.y) * 0.5f,
                                    (boundsMin_.z + boundsMax_.z) * 0.5f};
        const float restRadius = distance(mid, boundsMax_) + morphMax;
        boneRest_.clear();
        for (const PmxBone& bone : pmx.bones) boneRest_.push_back(bone.position);
        if (boneRest_.empty()) {
            boneRest_.push_back(mid);
            skinMargin_ = restRadius;
        } else {
            for (uint32_t i = 0; i < vertexCount_; ++i) {
                const PmxVertex& v = pmx.vertices[i];
                const int32_t b0 = v.boneIndex[0], b1 = v.boneIndex[1];
                const auto valid = [&](int32_t b) { return b >= 0 && (size_t)b < boneRest_.size(); };
                float reach = 0.0f;
                if (v.deform == PmxDeform::SDEF && valid(b0) && valid(b1)) {
                    const float w0 = v.boneWeight[0], w1 = 1.0f - w0;
                    const DirectX::XMFLOAT3 rw{v.sdefR0.x * w0 + v.sdefR1.x * w1, v.sdefR0.y * w0 + v.sdefR1.y * w1,
                                               v.sdefR0.z * w0 + v.sdefR1.z * w1};
                    const DirectX::XMFLOAT3 cr0{v.sdefC.x + (v.sdefR0.x - rw.x) * 0.5f, v.sdefC.y + (v.sdefR0.y - rw.y) * 0.5f,
                                                v.sdefC.z + (v.sdefR0.z - rw.z) * 0.5f};
                    const DirectX::XMFLOAT3 cr1{v.sdefC.x + (v.sdefR1.x - rw.x) * 0.5f, v.sdefC.y + (v.sdefR1.y - rw.y) * 0.5f,
                                                v.sdefC.z + (v.sdefR1.z - rw.z) * 0.5f};
                    reach = distance(v.position, v.sdefC) + w0 * distance(cr0, boneRest_[(size_t)b0]) +
                            w1 * distance(cr1, boneRest_[(size_t)b1]);
                } else {
                    for (int w = 0; w < 4; ++w) {
                        const int32_t b = v.boneIndex[w];
                        if (valid(b) && v.boneWeight[w] > 0.0f)
                            reach += v.boneWeight[w] * distance(v.position, boneRest_[(size_t)b]);
                    }
                }
                skinMargin_ = std::max(skinMargin_, reach + morphReach[i]);
            }
        }
        // Until the first pose (and for a boneless model given no matrices), the rest pose: the bones hold identity.
        liveCenter_ = mid;
        liveRadius_ = restRadius + 0.01f;
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
    ib_ = batch.CreateBuffer(indices.data(), indices.size() * sizeof(uint32_t),
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
    ibv_.SizeInBytes = (UINT)(indices.size() * sizeof(uint32_t));
    ibv_.BufferLocation = ib_->GetGPUVirtualAddress();

    // S8a: rest-pose triangle areas (the emissive-triangle CDF weighs a triangle by area x emission estimate).
    triAreas_.assign(indices.size() / 3, 0.0f);
    for (size_t t = 0; t < triAreas_.size(); ++t) {
        const DirectX::XMFLOAT3& a = pmx.vertices[indices[t * 3 + 0]].position;
        const DirectX::XMFLOAT3& b = pmx.vertices[indices[t * 3 + 1]].position;
        const DirectX::XMFLOAT3& c = pmx.vertices[indices[t * 3 + 2]].position;
        const float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
        const float e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1];
        const float cy = e1[2] * e2[0] - e1[0] * e2[2];
        const float cz = e1[0] * e2[1] - e1[1] * e2[0];
        triAreas_[t] = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
    }

    // SDEF parameters (slot 3), precomputed like saba / MMD: with w1 = 1 - w0 and rw = R0 w0 + R1 w1,
    // R0' = C + R0 - rw, R1' = C + R1 - rw, stored as (C + R0') / 2 and (C + R1') / 2.
    std::vector<GpuSdef> sdef;
    for (uint32_t v = 0; v < vertexCount_ && boneCount_ > 0; ++v) {
        const PmxVertex& src = pmx.vertices[v];
        if (src.deform != PmxDeform::SDEF || src.boneIndex[0] < 0 || src.boneIndex[1] < 0) continue;
        if (sdef.empty()) sdef.assign(vertexCount_, GpuSdef{});
        const float w0 = src.boneWeight[0], w1 = 1.0f - w0;
        const DirectX::XMFLOAT3 &c = src.sdefC, &r0 = src.sdefR0, &r1 = src.sdefR1;
        const float rw[3] = {r0.x * w0 + r1.x * w1, r0.y * w0 + r1.y * w1, r0.z * w0 + r1.z * w1};
        const float cc[3] = {c.x, c.y, c.z}, a0[3] = {r0.x, r0.y, r0.z}, a1[3] = {r1.x, r1.y, r1.z};
        GpuSdef& d = sdef[v];
        for (int k = 0; k < 3; ++k) {
            d.c[k] = cc[k];
            d.cr0[k] = (cc[k] + (cc[k] + a0[k] - rw[k])) * 0.5f;
            d.cr1[k] = (cc[k] + (cc[k] + a1[k] - rw[k])) * 0.5f;
        }
        d.sdef = 1.0f;
    }
    hasSdef_ = !sdef.empty();
    if (!hasSdef_) sdef.assign(1, GpuSdef{});
    sdef_ = batch.CreateBuffer(sdef.data(), sdef.size() * sizeof(GpuSdef),
                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               L"model.sdef");
    if (!sdef_) {
        LOG_ERROR("model '%s': SDEF buffer creation failed", name_.c_str());
        return false;
    }
    sdefVbv_.BufferLocation = sdef_->GetGPUVirtualAddress();
    sdefVbv_.SizeInBytes = (UINT)(sdef.size() * sizeof(GpuSdef));
    sdefVbv_.StrideInBytes = hasSdef_ ? sizeof(GpuSdef) : 0;

    // Additional UVs (slot 4): uploaded only when the model has any (PMX header count > 0).
    hasAdditionalUv_ = pmx.additionalUvCount > 0 && vertexCount_ > 0;
    std::vector<GpuAddUv> addUvs;
    if (hasAdditionalUv_) {
        addUvs.resize(vertexCount_);
        for (uint32_t v = 0; v < vertexCount_; ++v) {
            const PmxVertex& src = pmx.vertices[v];
            addUvs[v].uv1[0] = src.addUv[0].x; addUvs[v].uv1[1] = src.addUv[0].y;
            addUvs[v].uv1[2] = src.addUv[0].z; addUvs[v].uv1[3] = src.addUv[0].w;
            addUvs[v].uv2[0] = src.addUv[1].x; addUvs[v].uv2[1] = src.addUv[1].y;
            addUvs[v].uv2[2] = src.addUv[1].z; addUvs[v].uv2[3] = src.addUv[1].w;
        }
    } else {
        addUvs.assign(1, GpuAddUv{});
    }
    addUv_ = batch.CreateBuffer(addUvs.data(), addUvs.size() * sizeof(GpuAddUv),
                                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                L"model.adduv");
    if (!addUv_) {
        LOG_ERROR("model '%s': additional UV buffer creation failed", name_.c_str());
        return false;
    }
    addUvVbv_.BufferLocation = addUv_->GetGPUVirtualAddress();
    addUvVbv_.SizeInBytes = (UINT)(addUvs.size() * sizeof(GpuAddUv));
    addUvVbv_.StrideInBytes = hasAdditionalUv_ ? sizeof(GpuAddUv) : 0;

    // Bindless raw SRV over the add-UV buffer (ray tracing reads it as a ByteAddressBuffer).
    addUvSrv_ = DescriptorHeap::kInvalid;
    if (hasAdditionalUv_) {
        const uint32_t alloc = ctx.SrvHeap().Allocate(1);
        if (alloc == DescriptorHeap::kInvalid) {
            LOG_ERROR("model '%s': out of SRV descriptors (additional UV)", name_.c_str());
            return false;
        }
        addUvSrv_ = alloc;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.Buffer.NumElements = (UINT)(addUvs.size() * (sizeof(GpuAddUv) / 4));
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        ctx.Device()->CreateShaderResourceView(addUv_.Get(), &srv, ctx.SrvHeap().Cpu(alloc));
    }

    // Referenced texture indices.
    // S11: an index is "present" when it has either the RGBA8 decode or the DDS blocks (a BC4/5/6H/7 file has no
    // CPU decode here, so only the blocks exist; the CPU-side checks below then see an empty image).
    auto compressedAt = [&](int32_t i) -> const CompressedImage* {
        if (!compressed || i < 0 || (size_t)i >= compressed->size()) return nullptr;
        const CompressedImage& c = (*compressed)[(size_t)i];
        if (c.Empty() || c.faces != 1 || !IsMaterialBlockFormat(c.format)) return nullptr;
        // D3D12 needs level 0 of a block-compressed texture in whole blocks: other sizes keep the RGBA8 path.
        if ((c.width & 3) != 0 || (c.height & 3) != 0) return nullptr;
        return &c;
    };
    auto hasTexture = [&](int32_t i) {
        return i >= 0 && (size_t)i < textures.size() && (!textures[(size_t)i].Empty() || compressedAt(i) != nullptr);
    };
    // MME data layout: a toon slot holding a UV-mapped shadow colour (_S) makes the sphere slot a
    // UV-space parameter mask (_H), so the sphere texture is bound whatever its PMX sphere mode.
    auto ToonMapMaterial = [&](const PmxMaterial& m) {
        if (m.sharedToon || !hasTexture(m.toonIndex)) return false;
        return !IsToonRampImage(textures[m.toonIndex]);
    };
    auto SphereUsed = [&](const PmxMaterial& m) {
        const bool mode = m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add;
        return mode || (ToonMapMaterial(m) && m.sphereTextureIndex >= 0);
    };
    auto CollectTextureIndex = [&](std::vector<int32_t>& out, int32_t i) {
        if (!hasTexture(i)) return;
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    };
    std::vector<int32_t> referenced;
    for (const PmxMaterial& m : pmx.materials) {
        CollectTextureIndex(referenced, m.textureIndex);
        if (SphereUsed(m)) CollectTextureIndex(referenced, m.sphereTextureIndex);
        if (!m.sharedToon) CollectTextureIndex(referenced, m.toonIndex);
    }
    // PBR file maps (work unit P2): texture indices the Ray records reference through a decoded file.
    std::vector<int32_t> pbrFileUsed;
    const auto usePbrFile = [&](const PbrTextureRef& ref) {
        if (ref.kind != PbrTexKind::File || ref.fileIndex < 0 || (size_t)ref.fileIndex >= pbr.textures.size() ||
            pbr.textures[(size_t)ref.fileIndex].Empty())
            return;
        if (std::find(pbrFileUsed.begin(), pbrFileUsed.end(), ref.fileIndex) == pbrFileUsed.end())
            pbrFileUsed.push_back(ref.fileIndex);
    };
    for (size_t mi = 0; mi < pbr.materials.size() && mi < pmx.materials.size(); ++mi) {
        if (!pbr.materials[mi].valid) continue;
        for (int si = 0; si < kPbrSlotCount; ++si) usePbrFile(pbr.materials[mi].slots[si].tex);
        // S2b: the extension maps ride outside the slots (KHR_materials_transmission R, KHR_materials_volume G);
        // without this they would decode but never reach the GPU, and the record's SRV would stay -1.
        usePbrFile(pbr.materials[mi].ext.transmissionTex);
        usePbrFile(pbr.materials[mi].ext.volumeThicknessTex);
    }
    textures_.assign(textures.size(), {});
    // S11: a block-compressed DDS is uploaded as its blocks (below), so the VRAM saving is real. The RGBA8 image is
    // still around: it supplies the CPU-side checks (toon ramp, alpha test) and the PBR single-channel maps.
    textureFormats_.assign(textures.size(), DXGI_FORMAT_UNKNOWN);
    textureCount_ = 0;
    textureCompressedBytes_ = 0;
    textureRgba8Bytes_ = 0;
    textureWouldBeRgba8Bytes_ = 0;
    for (int32_t i : referenced) {
        if (const CompressedImage* c = compressedAt(i)) {
            // DECISION: the RGBA8 model path uploads R8G8B8A8_UNORM and the shaders convert to linear themselves
            // (SrgbToLinear), so the BC texture is uploaded UNORM too (not the _SRGB variant): the hardware gamma
            // decode would convert twice. The data maps (toon / sphere) sample the same way.
            const DXGI_FORMAT fmt = c->format;
            textures_[i] = batch.CreateTextureCompressed(*c, fmt, L"model.texture.bc");
            if (textures_[i]) {
                textureFormats_[i] = fmt;
                textureCompressedBytes_ += c->data.size();
                textureWouldBeRgba8Bytes_ += textures[i].Empty() ? Rgba8ChainBytes(c->width, c->height)
                                                                 : Rgba8BytesOf(textures[i]);
            }
        }
        if (!textures_[i]) {
            textures_[i] = batch.CreateTexture(textures[i], L"model.texture");
            if (textures_[i]) {
                textureFormats_[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
                textureRgba8Bytes_ += Rgba8BytesOf(textures[i]);
            }
        }
        if (!textures_[i]) {
            LOG_ERROR("model '%s': texture creation failed", name_.c_str());
            return false;
        }
        ++textureCount_;
    }
    // S11: the VRAM saving of this model's material textures, one line per loaded model (tools/asset_probe --full
    // prints the same totals for the whole library).
    if (textureCount_ > 0) {
        const uint64_t all = textureRgba8Bytes_ + textureWouldBeRgba8Bytes_;
        const uint64_t used = textureRgba8Bytes_ + textureCompressedBytes_;
        const uint64_t saved = all > used ? all - used : 0;
        LOG_INFO("TEXTURES %s: n=%u rgba8=%.1f MB bcn=%.1f MB all_rgba8=%.1f MB saved=%.1f%%", name_.c_str(),
                 textureCount_, (double)textureRgba8Bytes_ / (1024.0 * 1024.0),
                 (double)textureCompressedBytes_ / (1024.0 * 1024.0), (double)all / (1024.0 * 1024.0),
                 all ? 100.0 * (double)saved / (double)all : 0.0);
    }
    // PBR file map textures (R8 for single-channel slots, RGBA8 otherwise).
    pbrIndex_.assign(pmx.materials.size(), DescriptorHeap::kInvalid);
    pbrTransmission_.assign(pmx.materials.size(), 0.0f);
    pbrTextures_.assign(pbr.textures.size(), {});
    for (int32_t i : pbrFileUsed) {
        const int channel = i < (int)pbr.textureChannels.size() ? pbr.textureChannels[(size_t)i] : -1;
        pbrTextures_[(size_t)i] = channel >= 0 ? batch.CreateTextureChannel8(pbr.textures[(size_t)i], channel,
                                                                             L"model.pbrTex")
                                               : batch.CreateTexture(pbr.textures[(size_t)i], L"model.pbrTex");
        if (!pbrTextures_[(size_t)i]) LOG_WARN("model '%s': a PBR map texture upload failed", name_.c_str());
    }

    // Material constants.
    std::vector<MaterialConstants> consts(pmx.materials.size(), MaterialConstants{});
    // A stage that never sets the PMX "receive self-shadow" flag on any of its materials is treated as
    // unconfigured: ripped stages leave every flag at 0, so without this the stage never showed the
    // character's cast shadow (only SSAO darkened it). Hand-made stages set the flag deliberately (on some of
    // their materials) and keep their own choice.
    bool stageAnyReceive = false;
    if (role == ModelRole::Stage) {
        for (const PmxMaterial& m : pmx.materials) {
            if (m.flags & PmxMat_ReceiveShadow) {
                stageAnyReceive = true;
                break;
            }
        }
    }
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
        // A real sphere-slot texture is bound (Mode Multiply/Add, or the toon-map data layout).
        const bool sphereBound = sphereRef || (SphereUsed(m) && m.sphereTextureIndex >= 0 &&
                                               (size_t)m.sphereTextureIndex < textures_.size() &&
                                               textures_[m.sphereTextureIndex]);
        if (hasTex) c.flags |= MatFlag_HasTexture;
        if (sphereBound) c.flags |= MatFlag_HasSphereTex;
        if (sphereRef) {
            if (m.sphereMode == PmxSphereMode::Multiply) c.flags |= MatFlag_SphereMul;
            if (m.sphereMode == PmxSphereMode::Add) c.flags |= MatFlag_SphereAdd;
        }
        if (m.sharedToon) {
            c.flags |= MatFlag_HasToon;
        } else if (m.toonIndex >= 0 && (size_t)m.toonIndex < textures_.size() && textures_[m.toonIndex]) {
            c.flags |= MatFlag_HasToon;
            if (!IsToonRampImage(textures[m.toonIndex])) {
                // MME toon-map layout: toon = shadow colour (_S), sphere = UV-space parameter mask (_H),
                // which must not be added as an environment sphere.
                c.flags |= MatFlag_ToonMap;
                c.flags &= ~(MatFlag_SphereMul | MatFlag_SphereAdd);
                if (IsFlatFaceMaterial(m.name)) c.flags |= MatFlag_FlatShade;
            }
        }
        // No toon on a character material: the author wants it unmodelled (typically the face,
        // which often comes with painted hair shadows).
        if (!(c.flags & MatFlag_HasToon) && role != ModelRole::Stage) c.flags |= MatFlag_FlatShade;
        if ((m.flags & PmxMat_ReceiveShadow) || (role == ModelRole::Stage && !stageAnyReceive))
            c.flags |= MatFlag_ReceiveShadow;
        if (role == ModelRole::Stage) c.flags |= MatFlag_Stage;
        // MMD has no roughness: treat a tight, bright specular as a hint of gloss.
        const float specLum = 0.299f * m.specular.x + 0.587f * m.specular.y + 0.114f * m.specular.z;
        const float tightness = std::clamp((m.specularPower - 5.0f) / 60.0f, 0.0f, 1.0f);
        c.reflectivity = std::clamp(specLum * tightness, 0.0f, 1.0f) * (role == ModelRole::Stage ? 0.5f : 0.12f);
        // そぼろ AutoLuminous convention: the author marks a glowing material with a black specular colour
        // (reflection colour 0,0,0) and a specular power (反射強度) of 100 or more (110 = the documented standard).
        // The shaders then add the material colour x texture as HDR radiance (AutoLumEmission). A PBR record that
        // already carries an emissive, or a Ray/glTF material, must not glow twice, so skip those.
        const bool hasPbrRecord = mIdx < pbr.materials.size() && pbr.materials[mIdx].valid;
        // S10: a planar mirror (a stage rule's "mirror" with a PBR record). The flag reaches the shaders through the
        // material constants; the planes are fitted below from the rest-pose triangles.
        if (hasPbrRecord && pbr.materials[mIdx].ext.mirror) c.flags |= MatFlag_Mirror;
        if (m.specularPower > 100.0f && specLum <= 1e-4f && !hasPbrRecord)
            c.flags |= MatFlag_AutoLuminous;
    }
    for (MaterialConstants& c : consts) {
        c.texMul = c.sphereMul = c.toonMul = {1, 1, 1, 1};
        c.texAdd = c.sphereAdd = c.toonAdd = {0, 0, 0, 0};
    }
    // Load-time AutoLuminous classification (kept so a shader pack turned off can restore it).
    autoLum_.assign(pmx.materials.size(), 0);
    for (size_t i = 0; i < consts.size(); ++i)
        if (consts[i].flags & MatFlag_AutoLuminous) autoLum_[i] = 1;
    // Per-frame ring (material morphs rewrite the constants at run time), every entry starts at the base.
    const size_t matCount = std::max<size_t>(consts.size(), 1);
    materialCb_ = CreateUploadBuffer(ctx.Device(), (uint32_t)(matCount * sizeof(MaterialConstants) * kRing),
                                     L"model.materialCb");
    if (!materialCb_ || FAILED(materialCb_->Map(0, nullptr, (void**)&materialMapped_))) {
        LOG_ERROR("model '%s': material constant buffer creation failed", name_.c_str());
        return false;
    }
    for (uint32_t r = 0; r < kRing; ++r) {
        if (!consts.empty())
            memcpy(materialMapped_ + r * matCount * sizeof(MaterialConstants), consts.data(),
                   consts.size() * sizeof(MaterialConstants));
        materialEntryVersion_[r] = 0;
    }
    materialVersion_ = 0;
    materialConsts_ = consts;
    baseConsts_ = consts;
    materialNames_.clear();
    materialTextures_.clear();
    // MME TOONCOLOR (spec §4.2): the bottom-left texel of the material's toon texture. A shared (built-in
    // toon01..10) toon has no decoded image here, so it stays white (logged by the object pass when used).
    mmeToonColor_.assign(pmx.materials.size(), DirectX::XMFLOAT3{1, 1, 1});
    for (size_t mi = 0; mi < pmx.materials.size(); ++mi) {
        const PmxMaterial& m = pmx.materials[mi];
        materialNames_.emplace_back(m.name, m.nameEn);
        const bool tex = m.textureIndex >= 0 && (size_t)m.textureIndex < pmx.textures.size();
        materialTextures_.push_back(tex ? pmx.textures[(size_t)m.textureIndex] : std::string());
        if (!m.sharedToon && m.toonIndex >= 0 && (size_t)m.toonIndex < textures.size() && !textures[m.toonIndex].Empty()) {
            const ImageRGBA8& img = textures[(size_t)m.toonIndex];
            if (!img.mips.empty() && !img.mips[0].pixels.empty() && img.mips[0].width > 0 && img.mips[0].height > 0) {
                const ImageRGBA8::Level& lv = img.mips[0];
                // Bottom-left corner = the last row's first texel.
                const size_t idx = ((size_t)lv.height - 1) * (size_t)lv.width * 4;
                if (idx + 3 < lv.pixels.size()) {
                    mmeToonColor_[mi] = {lv.pixels[idx + 0] / 255.0f, lv.pixels[idx + 1] / 255.0f,
                                         lv.pixels[idx + 2] / 255.0f};
                }
            }
        }
    }
    headBone_ = -1;
    for (const char* name : {"\xE9\xA0\xAD", "\xE9\xA6\x96"}) {  // 頭, else 首
        for (size_t b = 0; b < pmx.bones.size() && headBone_ < 0; ++b)
            if (pmx.bones[b].name == name) {
                headBone_ = (int32_t)b;
                headPos_ = pmx.bones[b].position;
            }
        if (headBone_ >= 0) break;
    }
    packId_.clear();
    packTextureFolder_.clear();    materialBase_.assign(pmx.materials.size(), {});
    std::vector<bool> morphable(pmx.materials.size(), false);
    for (const PmxMorph& mo : pmx.morphs) {
        if (mo.type != PmxMorphType::Material) continue;
        for (const auto& o : mo.materialOffsets) {
            if (o.material < 0) morphable.assign(morphable.size(), true);
            else if ((size_t)o.material < morphable.size()) morphable[(size_t)o.material] = true;
        }
    }

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
        materialBase_[mIdx].edgeFlag = (m.flags & PmxMat_Edge) != 0;
        materialBase_[mIdx].castFlag = (m.flags & (PmxMat_CastShadow | PmxMat_GroundShadow)) != 0;
        mat.visible = m.diffuse.w > 0.0f;
        mat.morphable = morphable[mIdx];
        mat.drawEdge = materialBase_[mIdx].edgeFlag && m.edgeSize > 0 && m.edgeColor.w > 0 && mat.visible;
        mat.castShadow = materialBase_[mIdx].castFlag && m.diffuse.w > 0.01f;
        mat.alphaTested = m.textureIndex >= 0 && (size_t)m.textureIndex < textures.size() &&
                          (textures[m.textureIndex].hasAlpha ||
                           (compressedAt(m.textureIndex) && compressedAt(m.textureIndex)->hasAlpha));
        mat.sphereMode = (uint8_t)m.sphereMode;
        if (mIdx < cellsPerMaterial.size()) {
            mat.cellFirst = cellsPerMaterial[mIdx].first;
            mat.cellCount = cellsPerMaterial[mIdx].second;
        }
        {
            // PBR_FLAG_CUTOUT (GpuModel's record below) discards by the ALPHA slot; a material morph can change the alpha
            const bool hasRecord = mIdx < pbr.materials.size() && pbr.materials[mIdx].valid;
            const float threshold = hasRecord ? pbr.materials[mIdx].alphaThreshold : 0.0f;
            const bool cutout = threshold > 0.0f && threshold < 0.999f;
            mat.opaqueAtLoad = !cutout && !morphable[mIdx] && !MaterialBlends(pmx, mIdx, textures, pbr, compressed);
        }
        mat.edgeSize = m.edgeSize;
        mat.constants = materialCb_->GetGPUVirtualAddress() + mIdx * sizeof(MaterialConstants);

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
        // S11: a BCn texture needs its own format in the SRV (the view format must match the resource).
        auto srvFor = [&](int32_t index) -> D3D12_SHADER_RESOURCE_VIEW_DESC {
            D3D12_SHADER_RESOURCE_VIEW_DESC d = srv;
            if (index >= 0 && (size_t)index < textureFormats_.size() && textureFormats_[(size_t)index] != DXGI_FORMAT_UNKNOWN)
                d.Format = textureFormats_[(size_t)index];
            return d;
        };
        auto hasTexAt = [&](int32_t index) {
            return index >= 0 && (size_t)index < textures_.size() && textures_[(size_t)index];
        };

        // +0 texture
        ID3D12Resource* texRes = builtin.white.Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC texSrv = srv;
        if (m.textureIndex >= 0 && (size_t)m.textureIndex < textures_.size() && textures_[m.textureIndex]) {
            texRes = textures_[m.textureIndex].Get();
            texSrv = srvFor(m.textureIndex);
        }
        device->CreateShaderResourceView(texRes, &texSrv, ctx.SrvHeap().Cpu(alloc + 0));
        // +1 sphere
        if ((m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add) &&
            hasTexAt(m.sphereTextureIndex)) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC d = srvFor(m.sphereTextureIndex);
            device->CreateShaderResourceView(textures_[m.sphereTextureIndex].Get(), &d, ctx.SrvHeap().Cpu(alloc + 1));
        } else if ((consts[mIdx].flags & MatFlag_HasSphereTex) && hasTexAt(m.sphereTextureIndex)) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC d = srvFor(m.sphereTextureIndex);
            device->CreateShaderResourceView(textures_[m.sphereTextureIndex].Get(), &d, ctx.SrvHeap().Cpu(alloc + 1));
        } else
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 1));
        // +2 toon
        if (m.sharedToon) {
            int32_t t = m.toonIndex < 0 ? 0 : (m.toonIndex > 9 ? 9 : m.toonIndex);
            device->CreateShaderResourceView(builtin.toon[t].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        } else if (hasTexAt(m.toonIndex)) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC d = srvFor(m.toonIndex);
            device->CreateShaderResourceView(textures_[m.toonIndex].Get(), &d, ctx.SrvHeap().Cpu(alloc + 2));
        } else {
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        }
        materials_.push_back(mat);
        // The material's own diffuse texture (background AVI / S5 screen bindings restore it).
        materialOrigTex_.push_back(texRes);
        materialOrigTexFormat_.push_back(texSrv.Format);
        materialOrigHasTex_.push_back((char)((consts[mIdx].flags & MatFlag_HasTexture) != 0));
        materialOrigReflect_.push_back(consts[mIdx].reflectivity);
        // Background AVI: remember the material whose diffuse texture file name is screen.bmp
        if (m.textureIndex >= 0 && (size_t)m.textureIndex < pmx.textures.size() &&
            IsScreenTextureName(pmx.textures[(size_t)m.textureIndex])) {
            screenMaterials_.push_back((uint32_t)mIdx);
        }
    }

    // S8b: the light rectangle of every stage material that has rest-pose triangles, fitted once: the area-weighted
    // centroid and normal (from the vertex normals, like the S10 mirror plane below), then the two in-plane axes from
    // the principal axis of the vertices' in-plane scatter, and the half extents of the vertices on them. A screen
    // claims a material only at frame time (StageScreens::SetMaterialScreen sets MatFlag_Screen), so every stage material
    // is fitted here and the renderer filters the list by the current flags. A non-planar screen (a curved LED wall)
    // keeps this one rectangle: the area light is an approximation there, close to the emitter's own extent. A character
    // never carries a screen, so character models skip the fit (an extra per-material pass over their triangles).
    for (size_t mIdx = 0; role == ModelRole::Stage && mIdx < pmx.materials.size() && mIdx < consts.size(); ++mIdx) {
        const uint32_t first = materials_[mIdx].indexStart;
        const uint32_t count = std::min(materials_[mIdx].indexCount, (uint32_t)(indices.size() - first));
        double cx = 0, cy = 0, cz = 0;
        // area-weighted normal and centroid (the same fit as the S10 mirror plane below)
        double nx = 0, ny = 0, nz = 0, area = 0;
        for (uint32_t t = 0; t + 2 < count; t += 3) {
            const DirectX::XMFLOAT3& a = pmx.vertices[indices[first + t + 0]].position;
            const DirectX::XMFLOAT3& b = pmx.vertices[indices[first + t + 1]].position;
            const DirectX::XMFLOAT3& c = pmx.vertices[indices[first + t + 2]].position;
            const double p[3][3] = {{a.x, a.y, a.z}, {b.x, b.y, b.z}, {c.x, c.y, c.z}};
            const double e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const double e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            const double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                  e1[0] * e2[1] - e1[1] * e2[0]};
            const double tri = 0.5 * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
            if (!(tri > 0.0)) continue;
            nx += tri * (pmx.vertices[indices[first + t + 0]].normal.x +
                         pmx.vertices[indices[first + t + 1]].normal.x +
                         pmx.vertices[indices[first + t + 2]].normal.x) / 3.0;
            ny += tri * (pmx.vertices[indices[first + t + 0]].normal.y +
                         pmx.vertices[indices[first + t + 1]].normal.y +
                         pmx.vertices[indices[first + t + 2]].normal.y) / 3.0;
            nz += tri * (pmx.vertices[indices[first + t + 0]].normal.z +
                         pmx.vertices[indices[first + t + 1]].normal.z +
                         pmx.vertices[indices[first + t + 2]].normal.z) / 3.0;
            cx += tri * (a.x + b.x + c.x) / 3.0;
            cy += tri * (a.y + b.y + c.y) / 3.0;
            cz += tri * (a.z + b.z + c.z) / 3.0;
            area += tri;
        }
        const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(area > 0.0) || !(nl > 1e-12)) continue;
        ScreenRect rect;
        rect.material = mIdx;
        rect.area = (float)area;
        rect.center = {(float)(cx / area), (float)(cy / area), (float)(cz / area)};
        rect.normal = {(float)(nx / nl), (float)(ny / nl), (float)(nz / nl)};
        // The two in-plane axes: the principal axis of the vertices' in-plane scatter (PCA of a rectangle's corners
        // is exactly its axes, whatever the tessellation), the second axis = normal x A. A world axis that is not the
        // normal starts the basis, so a degenerate / single-point material still gets an orthonormal frame.
        const double n[3] = {rect.normal.x, rect.normal.y, rect.normal.z};
        double a0[3] = {std::abs(n[0]) < 0.9 ? 1.0 : 0.0, std::abs(n[0]) < 0.9 ? 0.0 : 1.0, 0.0};
        const double a0d = a0[0] * n[0] + a0[1] * n[1] + a0[2] * n[2];
        for (int k = 0; k < 3; ++k) a0[k] -= a0d * n[k];
        const double a0l = std::sqrt(a0[0] * a0[0] + a0[1] * a0[1] + a0[2] * a0[2]);
        if (!(a0l > 1e-9)) continue;
        for (int k = 0; k < 3; ++k) a0[k] /= a0l;
        const double b0[3] = {n[1] * a0[2] - n[2] * a0[1], n[2] * a0[0] - n[0] * a0[2], n[0] * a0[1] - n[1] * a0[0]};
        double saa = 0, sbb = 0, sab = 0;
        for (uint32_t t = 0; t + 2 < count; t += 3) {
            for (int v = 0; v < 3; ++v) {
                const DirectX::XMFLOAT3& p = pmx.vertices[indices[first + t + (uint32_t)v]].position;
                const double d[3] = {p.x - rect.center.x, p.y - rect.center.y, p.z - rect.center.z};
                const double da = d[0] * a0[0] + d[1] * a0[1] + d[2] * a0[2];
                const double db = d[0] * b0[0] + d[1] * b0[1] + d[2] * b0[2];
                saa += da * da;
                sbb += db * db;
                sab += da * db;
            }
        }
        // the principal angle of the 2 x 2 scatter matrix (0 when it is isotropic)
        const double theta = 0.5 * std::atan2(2.0 * sab, saa - sbb);
        const double cs = std::cos(theta), sn = std::sin(theta);
        double a[3], b[3];
        for (int k = 0; k < 3; ++k) {
            a[k] = a0[k] * cs + b0[k] * sn;
            b[k] = b0[k] * cs - a0[k] * sn;
        }
        rect.axisA = {(float)a[0], (float)a[1], (float)a[2]};
        rect.axisB = {(float)b[0], (float)b[1], (float)b[2]};
        // Orient the axes by the texture: A along +u and B along +v (the rectangle's uv in screen_light.hlsli is
        // 0.5 + 0.5 x the axis projections), so the specular reflection shows the image the right way round.
        {
            double au = 0, av = 0, bu = 0, bv = 0;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                for (int v = 0; v < 3; ++v) {
                    const PmxVertex& pv = pmx.vertices[indices[first + t + (uint32_t)v]];
                    const double d[3] = {pv.position.x - rect.center.x, pv.position.y - rect.center.y,
                                         pv.position.z - rect.center.z};
                    const double da = d[0] * a[0] + d[1] * a[1] + d[2] * a[2];
                    const double db = d[0] * b[0] + d[1] * b[1] + d[2] * b[2];
                    au += da * pv.uv.x; av += da * pv.uv.y;
                    bu += db * pv.uv.x; bv += db * pv.uv.y;
                }
            }
            if (std::abs(bu) > std::abs(au)) {   // u runs along B: swap the axes
                std::swap(rect.axisA, rect.axisB);
                std::swap(au, bu);
                std::swap(av, bv);
            }
            if (au < 0) rect.axisA = {-rect.axisA.x, -rect.axisA.y, -rect.axisA.z};
            if (bv < 0) rect.axisB = {-rect.axisB.x, -rect.axisB.y, -rect.axisB.z};
        }
        // half extents: the largest vertex distance along each in-plane axis from the centroid
        float halfA = 0.0f, halfB = 0.0f;
        for (uint32_t t = 0; t + 2 < count; t += 3) {
            for (int v = 0; v < 3; ++v) {
                const DirectX::XMFLOAT3& p = pmx.vertices[indices[first + t + (uint32_t)v]].position;
                const float dx = p.x - rect.center.x, dy = p.y - rect.center.y, dz = p.z - rect.center.z;
                halfA = std::max(halfA, std::abs(dx * rect.axisA.x + dy * rect.axisA.y + dz * rect.axisA.z));
                halfB = std::max(halfB, std::abs(dx * rect.axisB.x + dy * rect.axisB.y + dz * rect.axisB.z));
            }
        }
        rect.halfA = halfA;
        rect.halfB = halfB;
        rect.valid = halfA > 1e-4f && halfB > 1e-4f;
        if (rect.valid) screenRects_.push_back(rect);
    }

    // S10: the plane of every mirror material (MatFlag_Mirror), fitted once from its rest-pose triangles. A mirror
    // material is an authored flat surface, so the area-weighted centroid and normal of its triangles are the plane;
    // a material whose vertices do not stay within 1 % of its own extent of that plane (a curved or scattered group)
    // gets valid = false and shows its tint only.
    for (size_t mIdx = 0; mIdx < pmx.materials.size() && mIdx < consts.size(); ++mIdx) {
        if ((consts[mIdx].flags & MatFlag_Mirror) == 0) continue;
        MirrorPlane plane;
        plane.material = mIdx;
        const uint32_t first = materials_[mIdx].indexStart;
        const uint32_t count = std::min(materials_[mIdx].indexCount, (uint32_t)(indices.size() - first));
        double cx = 0, cy = 0, cz = 0, nx = 0, ny = 0, nz = 0, area = 0;
        for (uint32_t t = 0; t + 2 < count; t += 3) {
            const DirectX::XMFLOAT3& a = pmx.vertices[indices[first + t + 0]].position;
            const DirectX::XMFLOAT3& b = pmx.vertices[indices[first + t + 1]].position;
            const DirectX::XMFLOAT3& c = pmx.vertices[indices[first + t + 2]].position;
            const double e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
            const double e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
            const double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                  e1[0] * e2[1] - e1[1] * e2[0]};
            const double tri = 0.5 * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
            if (!(tri > 0.0)) continue;
            // The plane's orientation is the surface's own: the area-weighted vertex normals (the importer mirrors the
            // axis and flips the winding, so the winding cross product is not the visible side).
            nx += tri * (pmx.vertices[indices[first + t + 0]].normal.x +
                         pmx.vertices[indices[first + t + 1]].normal.x +
                         pmx.vertices[indices[first + t + 2]].normal.x) / 3.0;
            ny += tri * (pmx.vertices[indices[first + t + 0]].normal.y +
                         pmx.vertices[indices[first + t + 1]].normal.y +
                         pmx.vertices[indices[first + t + 2]].normal.y) / 3.0;
            nz += tri * (pmx.vertices[indices[first + t + 0]].normal.z +
                         pmx.vertices[indices[first + t + 1]].normal.z +
                         pmx.vertices[indices[first + t + 2]].normal.z) / 3.0;
            cx += tri * (a.x + b.x + c.x) / 3.0;
            cy += tri * (a.y + b.y + c.y) / 3.0;
            cz += tri * (a.z + b.z + c.z) / 3.0;
            area += tri;
        }
        const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (area > 0.0 && nl > 1e-12) {
            plane.area = (float)area;             // model-space triangle area: ranks the frame's two mirror slots
            plane.centroid = {(float)(cx / area), (float)(cy / area), (float)(cz / area)};
            plane.normal = {(float)(nx / nl), (float)(ny / nl), (float)(nz / nl)};
            // coplanar test: every vertex within 1 % of the material's extent of the fitted plane
            double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
            double maxDist = 0.0;
            const auto addVertex = [&](const DirectX::XMFLOAT3& v) {
                const double p[3] = {v.x, v.y, v.z};
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], p[k]);
                    hi[k] = std::max(hi[k], p[k]);
                }
                const double d = (p[0] - plane.centroid.x) * plane.normal.x +
                                 (p[1] - plane.centroid.y) * plane.normal.y +
                                 (p[2] - plane.centroid.z) * plane.normal.z;
                maxDist = std::max(maxDist, std::abs(d));
            };
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                addVertex(pmx.vertices[indices[first + t + 0]].position);
                addVertex(pmx.vertices[indices[first + t + 1]].position);
                addVertex(pmx.vertices[indices[first + t + 2]].position);
            }
            const double extent = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
            plane.valid = maxDist <= std::max(extent * 0.01, 1e-4);
            if (!plane.valid)
                LOG_INFO("model '%s': mirror material %zu is not planar, showing its tint only", name_.c_str(), mIdx);
        }
        mirrorPlanes_.push_back(plane);
    }

    // PBR records and their SRVs (work unit P2): one GpuPbrMaterial per material with a valid Ray record. The
    // file-map SRVs are allocated first, so the record can store their absolute heap index (one field read by the
    // raster bindless table and the RT table alike).
    const bool anyPbr = !pbr.materials.empty() &&
                        std::any_of(pbr.materials.begin(), pbr.materials.end(),
                                    [](const PbrMaterial& m) { return m.valid; });
    if (anyPbr) {
        pbrTextureSrvs_.assign(pbrTextures_.size(), DescriptorHeap::kInvalid);
        bool outOfDescriptors = false;
        for (size_t i = 0; i < pbrTextures_.size(); ++i) {
            if (!pbrTextures_[i]) continue;
            const uint32_t ta = ctx.SrvHeap().Allocate(1);
            if (ta == DescriptorHeap::kInvalid) {
                outOfDescriptors = true;
                pbrTextures_[i].Reset();
                continue;
            }
            pbrTextureSrvs_[i] = ta;
            D3D12_SHADER_RESOURCE_VIEW_DESC ts{};
            ts.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ts.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            ts.Format = i < pbr.textureChannels.size() && pbr.textureChannels[i] >= 0 ? DXGI_FORMAT_R8_UNORM
                                                                                       : DXGI_FORMAT_R8G8B8A8_UNORM;
            ts.Texture2D.MipLevels = UINT(-1);
            ctx.Device()->CreateShaderResourceView(pbrTextures_[i].Get(), &ts, ctx.SrvHeap().Cpu(ta));
        }
        if (outOfDescriptors) LOG_WARN("model '%s': ran out of SRV descriptors for PBR maps", name_.c_str());

        std::vector<GpuPbrMaterial> records;
        for (size_t mi = 0; mi < pmx.materials.size(); ++mi) {
            if (mi >= pbr.materials.size() || !pbr.materials[mi].valid) continue;
            const PbrMaterial& src = pbr.materials[mi];
            const PmxMaterial& pm = pmx.materials[mi];
            GpuPbrMaterial r{};
            r.flags = 1u | (src.alphaThreshold > 0.0f && src.alphaThreshold < 0.999f ? 2u : 0u) |
                      (src.emissiveEnable ? 4u : 0u);
            // S2a: an emissive whose KHR_materials_emissive_strength exceeds 1 is not clamped (HDR glow / bloom).
            if (src.emissiveEnable && src.ext.emissiveStrength > 1.0f) r.flags |= 8u;
            r.shadingModel = (uint32_t)std::max(0, src.shadingModel);
            r.alphaThreshold = src.alphaThreshold;
            r.diffuseAlpha = pm.diffuse.w;
            // S2b: the transmission block drives the dielectric in every path (raster / RT Fresnel alpha in
            // shaders/mmd.hlsl, refraction in pathtrace.hlsl / offline_gi.hlsl, tinted shadows in pbr_common.hlsli).
            // The material's own diffuse alpha stays untouched: the coverage is the shader's business.
            const float transmission = std::clamp(src.ext.transmission, 0.0f, 1.0f);
            r.params.x = (float)src.albedoSubEnable;
            r.params.y = (float)src.albedoApplyDiffuse;
            r.transmission = {transmission, src.ext.ior, src.ext.volumeThickness, src.ext.volumeAttenuationDistance};
            r.attenuation = {src.ext.volumeAttenuationColor.x, src.ext.volumeAttenuationColor.y,
                             src.ext.volumeAttenuationColor.z, src.ext.clearcoat};
            r.coat = {src.ext.clearcoatRoughness, src.ext.specularFactor, src.ext.uvRotation, src.ext.sheenRoughness};
            r.sheen = {src.ext.sheenColorFactor.x, src.ext.sheenColorFactor.y, src.ext.sheenColorFactor.z, -1.0f};
            if (src.ext.transmissionTex.kind == PbrTexKind::File && src.ext.transmissionTex.fileIndex >= 0 &&
                (size_t)src.ext.transmissionTex.fileIndex < pbrTextureSrvs_.size() &&
                pbrTextureSrvs_[(size_t)src.ext.transmissionTex.fileIndex] != DescriptorHeap::kInvalid)
                r.sheen.w = (float)pbrTextureSrvs_[(size_t)src.ext.transmissionTex.fileIndex];
            for (int si = 0; si < kPbrSlotCount; ++si) {
                const PbrSlot& s = src.slots[si];
                GpuPbrSlot& d = r.slots[si];
                d.type = (uint32_t)std::max(0, s.type);
                d.swizzle = (uint32_t)std::clamp(s.swizzle, 0, 3);
                d.uvFlip = (uint32_t)std::max(0, s.uvFlip);
                d.applyScale = (uint32_t)std::max(0, s.applyScale);
                d.colorFlip = (uint32_t)std::max(0, s.colorFlip);
                d.loopU = s.loopU;
                d.loopV = s.loopV;
                d.value = {s.value[0], s.value[1], s.value[2], s.hasScale ? s.scale : 0.0f};
                d.uvScale = {s.uvScaleU, s.uvScaleV, s.uvOffsetU, s.uvOffsetV};
                if (s.tex.kind == PbrTexKind::File) {
                    d.textureSrv = (s.tex.fileIndex >= 0 && (size_t)s.tex.fileIndex < pbrTextureSrvs_.size() &&
                                    pbrTextureSrvs_[(size_t)s.tex.fileIndex] != DescriptorHeap::kInvalid)
                                       ? (int)pbrTextureSrvs_[(size_t)s.tex.fileIndex]
                                       : -1;
                    // A single-channel File map is uploaded R8 with the selected channel baked into R, so the
                    // shader reads channel 0; PMX-sourced maps stay RGBA and keep the declared swizzle.
                    if (s.tex.fileIndex >= 0 && (size_t)s.tex.fileIndex < pbr.textureChannels.size() &&
                        pbr.textureChannels[(size_t)s.tex.fileIndex] >= 0)
                        d.swizzle = 0;
                } else if (s.tex.kind == PbrTexKind::PmxDiffuse) {
                    d.textureSrv = (int)materials_[mi].srvTable + 0;
                } else if (s.tex.kind == PbrTexKind::PmxSphere) {
                    d.textureSrv = (int)materials_[mi].srvTable + 1;
                } else if (s.tex.kind == PbrTexKind::PmxToon) {
                    d.textureSrv = (int)materials_[mi].srvTable + 2;
                } else {
                    d.textureSrv = -1;
                }
            }
            pbrIndex_[mi] = (uint32_t)records.size();
            pbrTransmission_[mi] = transmission;   // S2b: RtGeom_Transmissive (the ray paths refract through it)
            records.push_back(r);
        }
        pbrBuf_ = batch.CreateBuffer(records.data(), records.size() * sizeof(GpuPbrMaterial),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     L"model.pbr");
        pbrRecords_ = records;   // the base values SetPbrOverrides restores / rings copy from
        if (!pbrBuf_) {
            LOG_WARN("model '%s': PBR record buffer creation failed", name_.c_str());
            pbrIndex_.assign(pmx.materials.size(), DescriptorHeap::kInvalid);
            pbrTransmission_.assign(pmx.materials.size(), 0.0f);
            pbrRecords_.clear();
        } else {
            const uint32_t alloc = ctx.SrvHeap().Allocate(1);
            if (alloc != DescriptorHeap::kInvalid) {
                pbrSrv_ = alloc;
                pbrStaticSrv_ = alloc;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Format = DXGI_FORMAT_R32_TYPELESS;
                srv.Buffer.NumElements = (UINT)(records.size() * sizeof(GpuPbrMaterial) / 4);
                srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                ctx.Device()->CreateShaderResourceView(pbrBuf_.Get(), &srv, ctx.SrvHeap().Cpu(alloc));
            } else {
                pbrBuf_.Reset();
                pbrIndex_.assign(pmx.materials.size(), DescriptorHeap::kInvalid);
                pbrTransmission_.assign(pmx.materials.size(), 0.0f);
                pbrRecords_.clear();
            }
        }
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

void GpuModel::SetMaterialBlendHints(const std::vector<uint8_t>& hints, uint8_t def) {    if (materials_.empty()) return;
    materialBlend_.assign(materials_.size(), 0);
    for (size_t i = 0; i < materials_.size(); ++i) {
        const uint8_t h = i < hints.size() ? hints[i] : def;  // slots past the vector use the bare Obj default
        materialBlend_[i] = h;
        Material& mat = materials_[i];
        mat.additive = h == 2;
        mat.noDepthWrite = h >= 1;
        // A hinted material draws no edge (outline) and casts no shadow-map shadow. The base flags are cleared too,
        // so UpdateMaterials (which re-derives drawEdge / castShadow each frame) keeps them off. An ordinary (h == 0)
        // material is left untouched: models without .emd or with only ordinary effects render exactly as before.
        if (h >= 1) {
            mat.drawEdge = false;
            mat.castShadow = false;
            materialBase_[i].edgeFlag = false;
            materialBase_[i].castFlag = false;
        }
        for (MaterialConstants* c : {&baseConsts_[i], &materialConsts_[i]}) {
            c->flags &= ~(MatFlag_Additive | MatFlag_NoDepthWrite);
            if (h == 2) c->flags |= MatFlag_Additive | MatFlag_NoDepthWrite;   // additive implies no depth write
            else if (h == 1) c->flags |= MatFlag_NoDepthWrite;
        }
    }
    for (uint64_t& v : materialEntryVersion_) v = ~0ull;   // re-upload the constants on the next UpdateMaterials
    for (size_t i = 0; i < materials_.size(); ++i) materials_[i].constants = materialCb_->GetGPUVirtualAddress() + i * sizeof(MaterialConstants);
}

bool GpuModel::BuildCullFrustum(const DirectX::XMFLOAT4X4& viewProj, bool depthPlanes, CullFrustum& out) const {
    if (drawCells_.empty()) return false;
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&rootSkin_),
                                                           DirectX::XMLoadFloat4x4(&viewProj)));
    // clip = (p, 1) * m, so each clip coordinate is the dot product with a column of m
    const auto plane = [&](float sign, int column) {
        return DirectX::XMFLOAT4{m.m[0][3] + sign * m.m[0][column], m.m[1][3] + sign * m.m[1][column],
                                 m.m[2][3] + sign * m.m[2][column], m.m[3][3] + sign * m.m[3][column]};
    };
    out.count = 0;
    out.planes[out.count++] = plane(1.0f, 0);    // x >= -w
    out.planes[out.count++] = plane(-1.0f, 0);   // x <= w
    out.planes[out.count++] = plane(1.0f, 1);    // y >= -w
    out.planes[out.count++] = plane(-1.0f, 1);   // y <= w
    if (depthPlanes) {
        out.planes[out.count++] = {m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2]};   // z >= 0
        out.planes[out.count++] = plane(-1.0f, 2);                                // z <= w
    }
    return true;
}

uint32_t GpuModel::DrawMaterial(ID3D12GraphicsCommandList* cmd, const Material& m, const CullFrustum* frustum) const {
    if (!frustum || m.cellCount == 0) {
        cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
        return 1;
    }
    uint32_t draws = 0, start = 0, count = 0;
    for (uint32_t c = 0; c < m.cellCount; ++c) {
        const DrawCell& cell = drawCells_[m.cellFirst + c];
        bool outside = false;
        for (uint32_t p = 0; p < frustum->count && !outside; ++p) {
            const DirectX::XMFLOAT4& pl = frustum->planes[p];
            // the box corner farthest along the plane's normal
            const float d = pl.x * (pl.x > 0.0f ? cell.boundsMax.x : cell.boundsMin.x) +
                            pl.y * (pl.y > 0.0f ? cell.boundsMax.y : cell.boundsMin.y) +
                            pl.z * (pl.z > 0.0f ? cell.boundsMax.z : cell.boundsMin.z) + pl.w;
            outside = d < 0.0f;
        }
        if (outside) continue;
        if (count && start + count == cell.indexStart) {
            count += cell.indexCount;
            continue;
        }
        if (count) {
            cmd->DrawIndexedInstanced(count, 1, start, 0, 0);
            ++draws;
        }
        start = cell.indexStart;
        count = cell.indexCount;
    }
    if (count) {
        cmd->DrawIndexedInstanced(count, 1, start, 0, 0);
        ++draws;
    }
    return draws;
}

bool GpuModel::DepthPrepassMaterial(const Material& m) const {
    if (!m.opaqueAtLoad || !m.visible || m.indexCount == 0 || m.additive || m.noDepthWrite || m.screen ||
        opacity_ != 1.0f)
        return false;
    return PbrTransmission((size_t)(&m - materials_.data())) <= 0.0f;   // a stage rule may turn a material into glass
}

void GpuModel::UpdateSkinning(uint64_t frame, const std::vector<DirectX::XMFLOAT4X4>& skin) {
    if (!skin.empty()) rootSkin_ = skin[0];
    const size_t count = std::min(skin.size(), (size_t)(boneCount_ == 0 ? 1u : boneCount_));
    const uint32_t r = (uint32_t)(frame % kRing);
    for (uint32_t i = 0; i < kRing; ++i) {
        if ((skinInitialized_ && i != r) || !boneMapped_[i]) continue;
        memcpy(boneMapped_[i], skin.data(), count * 64);
    }
    skinInitialized_ = true;
    if (!boneRest_.empty() && !skin.empty()) {
        // Rest position x skin matrix = the bone's position in this pose. The largest axis length is the pose's scale
        // (the display scale and a placement's scale, possibly non-uniform), which also scales the margin.
        DirectX::XMFLOAT3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        float scaleSq = 0.0f;
        const size_t n = std::min(boneRest_.size(), skin.size());
        for (size_t b = 0; b < n; ++b) {
            const DirectX::XMFLOAT3& p = boneRest_[b];
            const DirectX::XMFLOAT4X4& m = skin[b];
            const float x = p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41;
            const float y = p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42;
            const float z = p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43;
            lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
            hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
            scaleSq = std::max({scaleSq, m._11 * m._11 + m._12 * m._12 + m._13 * m._13,
                                m._21 * m._21 + m._22 * m._22 + m._23 * m._23,
                                m._31 * m._31 + m._32 * m._32 + m._33 * m._33});
        }
        const float ex = (hi.x - lo.x) * 0.5f, ey = (hi.y - lo.y) * 0.5f, ez = (hi.z - lo.z) * 0.5f;
        liveCenter_ = {lo.x + ex, lo.y + ey, lo.z + ez};
        liveRadius_ = std::sqrt(ex * ex + ey * ey + ez * ez) + skinMargin_ * std::sqrt(scaleSq) + 0.01f;
    }
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

void GpuModel::UpdateMaterials(uint64_t frame, const std::vector<PmxMorph::MaterialOffset>& mul,
                               const std::vector<PmxMorph::MaterialOffset>& add, uint64_t version, float opacity) {
    if (!materialMapped_ || materials_.empty()) return;
    opacity_ = opacity;
    const size_t n = materials_.size();
    const bool identity = mul.size() != n || add.size() != n;
    if (identity) version = 0;
    // a stage receiveShadow override changed: fold it into the version so the constants rebuild and re-upload
    if (stageFlagsGen_) version ^= (stageFlagsGen_ << 24);
    const bool alpha = opacity != 1.0f;
    if (alpha) version |= (1ull << 63);  // fold the opacity into the version so a change re-uploads (1 stays untouched)
    if (version != materialVersion_) {
        // final = base * mul + add (PMX material morph); the texture factors go to the shaders as is
        for (size_t i = 0; i < n; ++i) {
            const MaterialConstants& b = baseConsts_[i];
            MaterialConstants& c = materialConsts_[i];
            c = b;
            if (!identity) {
                const PmxMorph::MaterialOffset& m = mul[i];
                const PmxMorph::MaterialOffset& a = add[i];
                c.diffuse = {b.diffuse.x * m.diffuse.x + a.diffuse.x, b.diffuse.y * m.diffuse.y + a.diffuse.y,
                             b.diffuse.z * m.diffuse.z + a.diffuse.z,
                             std::clamp(b.diffuse.w * m.diffuse.w + a.diffuse.w, 0.0f, 1.0f)};
                c.specular = {b.specular.x * m.specular.x + a.specular.x, b.specular.y * m.specular.y + a.specular.y,
                              b.specular.z * m.specular.z + a.specular.z};
                c.specularPower = std::max(0.0f, b.specularPower * m.specularPower + a.specularPower);
                c.ambient = {b.ambient.x * m.ambient.x + a.ambient.x, b.ambient.y * m.ambient.y + a.ambient.y,
                             b.ambient.z * m.ambient.z + a.ambient.z};
                c.edgeColor = {b.edgeColor.x * m.edgeColor.x + a.edgeColor.x, b.edgeColor.y * m.edgeColor.y + a.edgeColor.y,
                               b.edgeColor.z * m.edgeColor.z + a.edgeColor.z,
                               std::clamp(b.edgeColor.w * m.edgeColor.w + a.edgeColor.w, 0.0f, 1.0f)};
                c.edgeSize = std::max(0.0f, b.edgeSize * m.edgeSize + a.edgeSize);
                c.texMul = m.textureFactor;
                c.texAdd = a.textureFactor;
                c.sphereMul = m.sphereFactor;
                c.sphereAdd = a.sphereFactor;
                c.toonMul = m.toonFactor;
                c.toonAdd = a.toonFactor;
            }
            if (alpha) c.diffuse.w = std::clamp(c.diffuse.w * opacity, 0.0f, 1.0f);  // studio accessory opacity
            if (i < stageReceive_.size() && stageReceive_[i] >= 0) {
                // a stage material rule's receiveShadow (a runtime target keeps driving it)
                if (stageReceive_[i] == 1) c.flags |= MatFlag_ReceiveShadow;
                else c.flags &= ~MatFlag_ReceiveShadow;
            }
            if (i < stageMirror_.size() && stageMirror_[i] >= 0) {
                // S10: a stage material rule's mirror (a runtime target keeps driving it)
                if (stageMirror_[i] == 1) c.flags |= MatFlag_Mirror;
                else c.flags &= ~MatFlag_Mirror;
            }
            Material& mat = materials_[i];
            mat.visible = c.diffuse.w > 0.0f;
            mat.drawEdge = materialBase_[i].edgeFlag && c.edgeSize > 0 && c.edgeColor.w > 0 && mat.visible;
            mat.castShadow = materialBase_[i].castFlag && c.diffuse.w > 0.01f;
            mat.edgeSize = c.edgeSize;
        }
        materialVersion_ = version;
    }
    const uint32_t r = (uint32_t)(frame % kRing);
    const size_t stride = n * sizeof(MaterialConstants);
    if (materialEntryVersion_[r] != materialVersion_) {
        memcpy(materialMapped_ + r * stride, materialConsts_.data(), stride);
        materialEntryVersion_[r] = materialVersion_;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS base = materialCb_->GetGPUVirtualAddress() + r * stride;
    for (size_t i = 0; i < n; ++i) materials_[i].constants = base + i * sizeof(MaterialConstants);
    if (hasPackParamsExt_ && packParamsExtMapped_ && packExtEntryVersion_[r] != packParamsExtVersion_) {
        uint8_t* dst = packParamsExtMapped_ + r * kPackExtCbBytes;
        const size_t bytes = std::min(packParamsExtSlots_.size() * sizeof(DirectX::XMFLOAT4), kPackExtCbBytes);
        if (bytes) memcpy(dst, packParamsExtSlots_.data(), bytes);
        memset(dst + bytes, 0, kPackExtCbBytes - bytes);
        packExtEntryVersion_[r] = packParamsExtVersion_;
    }
}

void GpuModel::SetPbrOverrides(uint64_t frame, const std::vector<PbrOverride>& list, uint64_t version) {
    if (!ctx_) return;
    // The shadow flags do not live in the PBR record: apply them first, so a stage rule that only sets castShadow /
    // receiveShadow also works on a material without a record.
    if (stageReceive_.size() < materials_.size()) stageReceive_.resize(materials_.size(), -1);
    if (stageMirror_.size() < materials_.size()) stageMirror_.resize(materials_.size(), -1);
    bool flagsChanged = false;
    for (const PbrOverride& o : list) {
        if (o.material >= materials_.size()) continue;
        if (o.castShadow && materialBase_[o.material].castFlag != *o.castShadow) {
            materialBase_[o.material].castFlag = *o.castShadow;   // UpdateMaterials re-derives Material::castShadow
            flagsChanged = true;
        }
        if (o.receiveShadow) {
            const int8_t v = *o.receiveShadow ? 1 : 0;
            if (stageReceive_[o.material] != v) {
                stageReceive_[o.material] = v;
                flagsChanged = true;
            }
        }
        if (o.mirror) {   // S10: MatFlag_Mirror (the plane itself is fitted at load, the flag is per frame)
            const int8_t v = *o.mirror ? 1 : 0;
            if (stageMirror_[o.material] != v) {
                stageMirror_[o.material] = v;
                flagsChanged = true;
            }
        }
    }
    if (flagsChanged) ++stageFlagsGen_;   // UpdateMaterials rebuilds / re-uploads the constants next frame
    if (pbrRecords_.empty()) return;      // the model has no PBR records to drive
    const uint32_t r = (uint32_t)(frame % kRing);
    if (list.empty()) {
        if (!pbrOverridesActive_) return;
        // no overrides: everything returns to the model's own records (the DEFAULT buffer).
        pbrOverridesActive_ = false;
        for (uint32_t s = 0; s < kRing; ++s) pbrRingVersion_[s] = 0;
        pbrSrv_ = pbrStaticSrv_;
        for (size_t mi = 0; mi < pbrTransmission_.size(); ++mi) {   // S2b: back to the base transmission
            const uint32_t rec = PbrIndex(mi);
            pbrTransmission_[mi] = rec != DescriptorHeap::kInvalid && rec < pbrRecords_.size()
                                       ? std::clamp(pbrRecords_[rec].transmission.x, 0.0f, 1.0f)
                                       : 0.0f;
        }
        return;
    }
    if (!pbrOverridesActive_) {
        // first override: one UPLOAD-heap copy of the records per ring entry, each with its own SRV (PbrSrv() must
        // follow the ring, and the DEFAULT buffer is not CPU-writable). Created once; an empty list only switches
        // PbrSrv() back to the static records.
        if (!pbrRingBuf_) {
            pbrRingBytes_ = pbrRecords_.size() * sizeof(GpuPbrMaterial);
            ComPtr<ID3D12Resource> ring = CreateUploadBuffer(ctx_->Device(), (uint32_t)(pbrRingBytes_ * kRing),
                                                             L"model.pbrRing");
            if (!ring || FAILED(ring->Map(0, nullptr, &pbrRingMapped_)) || !pbrRingMapped_) {
                LOG_WARN("model '%s': PBR override ring creation failed", name_.c_str());
                return;
            }
            pbrRingBuf_ = ring;
            for (uint32_t s = 0; s < kRing; ++s) {
                memcpy((uint8_t*)pbrRingMapped_ + s * pbrRingBytes_, pbrRecords_.data(), pbrRingBytes_);
                pbrRingVersion_[s] = 0;
                const uint32_t alloc = ctx_->SrvHeap().Allocate(1);
                if (alloc == DescriptorHeap::kInvalid) {
                    LOG_WARN("model '%s': out of SRV descriptors for the PBR override ring", name_.c_str());
                    for (uint32_t f = 0; f < s; ++f)
                        if (pbrRingSrvs_[f] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(pbrRingSrvs_[f], 1);
                    for (uint32_t& f : pbrRingSrvs_) f = DescriptorHeap::kInvalid;
                    pbrRingBuf_->Unmap(0, nullptr);
                    pbrRingMapped_ = nullptr;
                    pbrRingBuf_.Reset();
                    return;
                }
                pbrRingSrvs_[s] = alloc;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Format = DXGI_FORMAT_R32_TYPELESS;
                srv.Buffer.FirstElement = (UINT)(s * pbrRingBytes_ / 4);
                srv.Buffer.NumElements = (UINT)(pbrRingBytes_ / 4);
                srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                ctx_->Device()->CreateShaderResourceView(pbrRingBuf_.Get(), &srv, ctx_->SrvHeap().Cpu(alloc));
            }
        }
        pbrOverridesActive_ = true;
    }
    pbrSrv_ = pbrRingSrvs_[r];
    if (pbrRingVersion_[r] == version) return;   // this ring entry already holds these values
    // start from the model's own records, then the overrides (later entries win, like the evaluator's assignment order)
    GpuPbrMaterial* dst = reinterpret_cast<GpuPbrMaterial*>((uint8_t*)pbrRingMapped_ + r * pbrRingBytes_);
    memcpy(dst, pbrRecords_.data(), pbrRingBytes_);
    const auto encode = [](float linear) {
        linear = std::clamp(linear, 0.0f, 1.0f);
        return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    };
    for (const PbrOverride& o : list) {
        // the override indexes a material, the record buffer holds one record per material that has one
        if (o.material >= pbrIndex_.size()) continue;
        const uint32_t rec = pbrIndex_[o.material];
        if (rec == DescriptorHeap::kInvalid || rec >= pbrRecords_.size()) continue;
        GpuPbrMaterial& m = dst[rec];
        if (o.baseColor) {
            GpuPbrSlot& albedo = m.slots[(int)PbrSlotId::Albedo];
            m.params.y = 0.0f;   // ALBEDO_MAP_APPLY_DIFFUSE off: the override is the albedo
            if (albedo.textureSrv < 0) {
                albedo.applyScale = 0;   // no map: the sRGB-encoded constant is the albedo
                albedo.value.x = encode(o.baseColor->x);
                albedo.value.y = encode(o.baseColor->y);
                albedo.value.z = encode(o.baseColor->z);
            } else {
                albedo.applyScale = 1;   // a map: the LINEAR colour tints it
                albedo.value.x = std::clamp(o.baseColor->x, 0.0f, 1.0f);
                albedo.value.y = std::clamp(o.baseColor->y, 0.0f, 1.0f);
                albedo.value.z = std::clamp(o.baseColor->z, 0.0f, 1.0f);
            }
        }
        if (o.roughness) {
            GpuPbrSlot& s = m.slots[(int)PbrSlotId::Smoothness];
            s.textureSrv = -1;   // the override replaces a map: one value for the whole material
            s.swizzle = 0;
            s.type = 2;          // linear roughness: the shader's roughness = value^2 (glTF / stage convention)
            s.applyScale = 0;
            s.value.x = std::clamp(*o.roughness, 0.0f, 1.0f);
        }
        if (o.metallic) {
            GpuPbrSlot& s = m.slots[(int)PbrSlotId::Metalness];
            s.textureSrv = -1;
            s.swizzle = 0;
            s.applyScale = 0;
            s.value.x = std::clamp(*o.metallic, 0.0f, 1.0f);
        }
        if (o.emissive) {
            GpuPbrSlot& s = m.slots[(int)PbrSlotId::Emissive];
            const bool hdr = o.emissive->x > 1.0f || o.emissive->y > 1.0f || o.emissive->z > 1.0f;
            if (s.textureSrv < 0 && !hdr) {
                s.applyScale = 0;   // no map: the sRGB-encoded constant
                s.value.x = encode(o.emissive->x);
                s.value.y = encode(o.emissive->y);
                s.value.z = encode(o.emissive->z);
            } else if (s.textureSrv < 0) {
                s.applyScale = 0;   // no map, HDR colour: PbrEmissive reads a linear value
                s.value.x = std::max(0.0f, o.emissive->x);
                s.value.y = std::max(0.0f, o.emissive->y);
                s.value.z = std::max(0.0f, o.emissive->z);
            } else {
                s.applyScale = 1;   // a map: the LINEAR colour multiplies the decoded map
                s.value.x = std::max(0.0f, o.emissive->x);
                s.value.y = std::max(0.0f, o.emissive->y);
                s.value.z = std::max(0.0f, o.emissive->z);
            }
            const bool on = o.emissive->x > 0.0f || o.emissive->y > 0.0f || o.emissive->z > 0.0f;
            m.flags = on ? (m.flags | 4u) : (m.flags & ~4u);   // GpuPbrFlags bit 2: emissive
            m.flags = (on && hdr) ? (m.flags | 8u) : (m.flags & ~8u);   // bit 3: HDR (not clamped)
        }
        if (o.alpha) m.diffuseAlpha = std::clamp(*o.alpha, 0.0f, 1.0f);
        if (o.transmission) m.transmission.x = std::clamp(*o.transmission, 0.0f, 1.0f);
        if (o.ior) m.transmission.y = std::clamp(*o.ior, 1.0f, 3.0f);
        pbrTransmission_[o.material] = m.transmission.x;   // S2b: the ray paths' RtGeom_Transmissive follows too
    }
    pbrRingVersion_[r] = version;
}

void GpuModel::SetShaderPack(const ShaderPack* pack, const PackParamValues& params,
                             const std::filesystem::path& textureFolder,
                             const MaterialClassOverrides& materials,
                             const std::map<std::string, float>& rawParams) {
    if (role_ == ModelRole::Stage && pack && pack->type != PackType::Surface)
        pack = nullptr;   // S6: a stage part takes surface packs only (screen / world packs are never a material pack)
    const std::string id = pack ? pack->id : std::string();
    const uint32_t generation = ShaderPacks().Generation();
    // params.ext packs: one float4 per declared param (typed values); uploaded per frame slot in UpdateMaterials
    std::vector<DirectX::XMFLOAT4> extSlots;
    if (pack && pack->HasCap("params.ext")) BuildPackParamSlots(*pack, rawParams, extSlots);
    const bool sameExt = extSlots.size() == packParamsExtSlots_.size() &&
                         (extSlots.empty() ||
                          std::memcmp(extSlots.data(), packParamsExtSlots_.data(), extSlots.size() * sizeof(extSlots[0])) == 0);
    if (id == packId_ && (!pack || (generation == packGeneration_ && params == packParams_ &&
                                    textureFolder == packTextureFolder_ && packOverrides_ == materials && sameExt)))
        return;
    packId_ = id;
    packGeneration_ = generation;
    packTextureFolder_ = pack ? textureFolder : std::filesystem::path();
    packParams_ = params;
    packOverrides_ = pack ? materials : MaterialClassOverrides{};
    packParamsExtSlots_ = std::move(extSlots);
    hasPackParamsExt_ = pack && pack->HasCap("params.ext");
    if (hasPackParamsExt_ && !packParamsExtCb_ && ctx_) {
        packParamsExtCb_ = CreateUploadBuffer(ctx_->Device(), (uint32_t)(kPackExtCbBytes * kRing), L"model.packParamsExt");
        if (packParamsExtCb_ && FAILED(packParamsExtCb_->Map(0, nullptr, (void**)&packParamsExtMapped_)))
            packParamsExtMapped_ = nullptr;
    }
    ++packParamsExtVersion_;   // every ring slot re-uploads on its next UpdateMaterials (in-flight slots stay intact)
    if (!pack) {
        for (auto& m : materials_) m.packOff = false;
        // no pack = no class and no alpha key (clears the encoding of a pack switched off). A material the pack had
        // claimed gets its AutoLuminous flag back (the packs own / replace shading; below clears it again).
        for (size_t i = 0; i < baseConsts_.size(); ++i) {
            baseConsts_[i].packClass = 0;
            materialConsts_[i].packClass = 0;
            if (i < autoLum_.size() && autoLum_[i]) {
                baseConsts_[i].flags |= MatFlag_AutoLuminous;
                materialConsts_[i].flags |= MatFlag_AutoLuminous;
            }
        }
        for (uint64_t& v : materialEntryVersion_) v = ~0ull;
        return;
    }
    for (size_t i = 0; i < baseConsts_.size(); ++i) {
        const auto& names = materialNames_[i];
        uint32_t clsVal = 0;
        bool isOff = false;
        if (const auto it = packOverrides_.find(names.first); it != packOverrides_.end()) {
            if (it->second == kMaterialPackOff) {
                isOff = true;
                clsVal = kPackClassOff;
            } else if (it->second >= 0 && it->second <= 5) {
                clsVal = static_cast<uint32_t>(it->second);
            } else {
                clsVal = static_cast<uint32_t>(pack->Classify(names.first, names.second, materialTextures_[i]));
            }
        } else {
            // S6: a stage material's class comes from ClassifyStage (its first class rule when no rule matches), so a
            // surface pack bundled with a stage shades it even when the pack knows nothing about stage materials
            clsVal = static_cast<uint32_t>(role_ == ModelRole::Stage
                                               ? pack->ClassifyStage(names.first, names.second, materialTextures_[i])
                                               : pack->Classify(names.first, names.second, materialTextures_[i]));
        }
        // The alpha key follows the material's FINAL class (per-material override included); kPackClassOff = no key.
        const float key = isOff ? 0.0f : PackAlphaKeyForClass(pack->alphaKey, clsVal);
        const uint32_t encodedClass = PackClassEncode(clsVal, key);
        if (i < materials_.size()) materials_[i].packOff = isOff;
        for (MaterialConstants* c : {&baseConsts_[i], &materialConsts_[i]}) {
            c->packClass = encodedClass;
            // A pack owns the shading of the materials it shades, so their AutoLuminous flag is cleared (no double
            // glow); a per-material "pack off" override (isOff) keeps the default shading and its glow.
            if (!isOff) c->flags &= ~MatFlag_AutoLuminous;
            else if (i < autoLum_.size() && autoLum_[i]) c->flags |= MatFlag_AutoLuminous;
            c->packHeadBone = headBone_ >= 0 ? (uint32_t)headBone_ : 0u;
            c->packHead = {headPos_.x, headPos_.y, headPos_.z, headBone_ >= 0 ? 1.0f : 0.0f};
            for (uint32_t k = 0; k < kPackMaxParams; ++k) (&c->packParams[0].x)[k] = params[k];
        }
    }
    // every ring entry is rewritten by the next UpdateMaterials calls
    for (uint64_t& v : materialEntryVersion_) v = ~0ull;
}

void GpuModel::BindScreenTexture(Dx12Context& ctx, ID3D12Resource* tex) {
    if (!ctx_ || screenMaterials_.empty()) return;
    for (uint32_t mi : screenMaterials_) BindMaterialTexture(ctx, mi, tex);
}

void GpuModel::BindMaterialTexture(Dx12Context& ctx, size_t materialIndex, ID3D12Resource* tex) {
    if (!ctx_ || materialIndex >= materials_.size() || materialIndex >= materialOrigTex_.size()) return;
    ID3D12Device* device = ctx.Device();
    ID3D12Resource* res = tex ? tex : materialOrigTex_[materialIndex];
    if (!res) return;   // no own texture and no bound one: the white fallback stays
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = tex ? DXGI_FORMAT_R8G8B8A8_UNORM
                     : (materialIndex < materialOrigTexFormat_.size() ? materialOrigTexFormat_[materialIndex]
                                                                     : DXGI_FORMAT_R8G8B8A8_UNORM);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = tex ? 1 : UINT(-1);
    device->CreateShaderResourceView(res, &srv, ctx.SrvHeap().Cpu(materials_[materialIndex].srvTable + 0));
    const bool hasTex = tex != nullptr || materialOrigHasTex_[materialIndex] != 0;
    for (MaterialConstants* c : {materialIndex < baseConsts_.size() ? &baseConsts_[materialIndex] : nullptr,
                                 materialIndex < materialConsts_.size() ? &materialConsts_[materialIndex] : nullptr}) {
        if (!c) continue;
        c->flags &= ~(uint32_t)MatFlag_HasTexture;
        if (hasTex) c->flags |= MatFlag_HasTexture;
    }
    for (uint64_t& v : materialEntryVersion_) v = ~0ull;   // re-upload the constants on the next UpdateMaterials
}

// S8a: the material's emission estimate, in the same terms the shaders' emission functions add at an average texel
// (ScreenEmission / AutoLumEmission / PbrEmissive). The path tracers weigh a triangle by area x this value to build
// their emissive-triangle pick CDF; the exact radiance is evaluated in the shader at the sampled uv, so this only
// decides where the samples go. A bound emission map is assumed to average one.
float GpuModel::EmitEstimate(size_t material) const {
    if (material >= materialConsts_.size()) return 0.0f;
    const MaterialConstants& c = materialConsts_[material];
    // S5 screen: the bound texture IS the emission, scaled by the brightness in the reflectivity slot.
    if ((c.flags & MatFlag_Screen) != 0) return std::max(c.reflectivity, 0.0f);
    float e = 0.0f;
    // そぼろ AutoLuminous: luminance(diffuse) x the shader's strength ramp x its HDR gain.
    if ((c.flags & MatFlag_AutoLuminous) != 0) {
        const float lum = 0.2126f * std::max(c.diffuse.x, 0.0f) + 0.7152f * std::max(c.diffuse.y, 0.0f) +
                          0.0722f * std::max(c.diffuse.z, 0.0f);
        e += lum * std::clamp((c.specularPower - 100.0f) * 0.1f, 0.0f, 20.0f) * 4.0f;
    }
    // PBR record emissive (PbrEmissive): a map contributes its slot colour (the map averages one), a constant its
    // sRGB-decoded value (HDR records carry a linear value, see GpuPbrMaterial::flags bit 3).
    const uint32_t rec = PbrIndex(material);
    if (rec != DescriptorHeap::kInvalid && rec < pbrRecords_.size()) {
        const GpuPbrMaterial& r = pbrRecords_[rec];
        if ((r.flags & 4u) != 0) {
            const GpuPbrSlot& s = r.slots[(int)PbrSlotId::Emissive];
            DirectX::XMFLOAT3 v = {std::max(s.value.x, 0.0f), std::max(s.value.y, 0.0f), std::max(s.value.z, 0.0f)};
            if (s.textureSrv < 0 && (r.flags & 8u) == 0) {
                const auto srgb = [](float x) {
                    x = std::clamp(x, 0.0f, 1.0f);
                    return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
                };
                v = {srgb(v.x), srgb(v.y), srgb(v.z)};
            }
            e += 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
        }
    }
    return e;
}

void GpuModel::SetMaterialScreen(size_t materialIndex, bool on, float brightness) {
    if (materialIndex >= materials_.size()) return;
    materials_[materialIndex].screen = on;
    if (materialOrigReflect_.size() < materials_.size()) return;   // the model was not created through Create
    for (MaterialConstants* c : {materialIndex < baseConsts_.size() ? &baseConsts_[materialIndex] : nullptr,
                                 materialIndex < materialConsts_.size() ? &materialConsts_[materialIndex] : nullptr}) {
        if (!c) continue;
        if (on) {
            c->flags |= MatFlag_Screen;
            // A screen draws only its texture: the brightness rides in the reflectivity slot (unused by a screen's
            // shading, read as its HDR gain by ScreenEmission; the ray paths read it the same way).
            c->reflectivity = std::max(0.0f, brightness);
        } else {
            c->flags &= ~(uint32_t)MatFlag_Screen;
            c->reflectivity = materialOrigReflect_[materialIndex];
        }
    }
    for (uint64_t& v : materialEntryVersion_) v = ~0ull;
}

void GpuModel::FlushMaterialConstants(uint64_t frame) {
    if (!materialMapped_ || materials_.empty()) return;
    const size_t stride = materials_.size() * sizeof(MaterialConstants);
    const uint32_t r = (uint32_t)(frame % kRing);
    memcpy(materialMapped_ + (size_t)r * stride, materialConsts_.data(), stride);
    materialEntryVersion_[r] = materialVersion_;
}

void GpuModel::SetMmeEffects(std::vector<std::filesystem::path> perMaterial) {
    mmeEffects_ = std::move(perMaterial);
}

bool GpuModel::HasMmeEffects() const {
    for (const auto& fx : mmeEffects_)
        if (!fx.empty()) return true;
    return false;
}

std::vector<std::filesystem::path> GpuModel::MmeEffectFiles() const {
    std::vector<std::filesystem::path> out;
    for (const auto& fx : mmeEffects_) {
        if (fx.empty()) continue;
        bool dup = false;
        for (const auto& e : out)
            if (e == fx) { dup = true; break; }
        if (!dup) out.push_back(fx);
    }
    return out;
}

void GpuModel::SetMmeEffect(const std::filesystem::path& fx, std::vector<int> materialMask) {
    mmeEffects_.assign(materials_.size(), std::filesystem::path());
    if (fx.empty()) return;
    if (materialMask.empty()) {
        for (auto& p : mmeEffects_) p = fx;
        return;
    }
    for (int m : materialMask)
        if (m >= 0 && (size_t)m < mmeEffects_.size()) mmeEffects_[(size_t)m] = fx;
}

ModelTraits GpuModel::Traits() const {
    ModelTraits t;
    t.additionalUvCount = hasAdditionalUv_ ? additionalUvCount_ : 0;
    t.materials = (int)materialConsts_.size();
    for (const MaterialConstants& c : materialConsts_)
        if (c.flags & MatFlag_ToonMap) ++t.toonMapMaterials;
    return t;
}

bool GpuModel::PackActive() const {
    if (packId_.empty()) return false;
    for (const Material& m : materials_)
        if (!m.packOff) return true;
    return false;
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
    if (addUvSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(addUvSrv_, 1);
    addUvSrv_ = DescriptorHeap::kInvalid;
    if (pbrStaticSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(pbrStaticSrv_, 1);
    pbrSrv_ = DescriptorHeap::kInvalid;
    pbrStaticSrv_ = DescriptorHeap::kInvalid;
    for (uint32_t s = 0; s < kRing; ++s)   // the ring outlives a return to the static records (empty list)
        if (pbrRingSrvs_[s] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(pbrRingSrvs_[s], 1);
    for (uint32_t& s : pbrRingSrvs_) s = DescriptorHeap::kInvalid;
    if (pbrRingBuf_ && pbrRingMapped_) pbrRingBuf_->Unmap(0, nullptr);
    pbrRingMapped_ = nullptr;
    pbrRingBuf_.Reset();
    pbrRecords_.clear();
    for (uint32_t s : pbrTextureSrvs_)
        if (s != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(s, 1);
    pbrTextureSrvs_.clear();
    pbrTextures_.clear();
    pbrIndex_.clear();
    pbrTransmission_.clear();
    pbrBuf_.Reset();
    rt_ = RtResources{};
    materialConsts_.clear();
    baseConsts_.clear();
    materialBase_.clear();
    stageReceive_.clear();
    stageMirror_.clear();
    mirrorPlanes_.clear();
    screenRects_.clear();
    if (materialCb_ && materialMapped_) materialCb_->Unmap(0, nullptr);
    materialMapped_ = nullptr;
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
    sdef_.Reset();
    sdefVbv_ = {};
    hasSdef_ = false;
    addUv_.Reset();
    addUvVbv_ = {};
    if (packParamsExtCb_ && packParamsExtMapped_) packParamsExtCb_->Unmap(0, nullptr);
    packParamsExtMapped_ = nullptr;
    packParamsExtCb_.Reset();
    hasPackParamsExt_ = false;
    hasAdditionalUv_ = false;
    packParamsExtSlots_.clear();
    materialCb_.Reset();
    textures_.clear();
    materials_.clear();
    screenMaterials_.clear();
    materialOrigTex_.clear();
    materialOrigTexFormat_.clear();
    materialOrigHasTex_.clear();
    materialOrigReflect_.clear();
    textureFormats_.clear();
    textureCount_ = 0;
    textureRgba8Bytes_ = textureCompressedBytes_ = textureWouldBeRgba8Bytes_ = 0;
    ctx_ = nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuModel::PackParamsExtBuffer(uint64_t frame) const {
    if (!packParamsExtCb_ || !hasPackParamsExt_) return 0;
    return packParamsExtCb_->GetGPUVirtualAddress() + (frame % kRing) * kPackExtCbBytes;
}

GpuModel::~GpuModel() {
    Destroy();
}

} // namespace mmdx
