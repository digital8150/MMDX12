#include "render/PackTextures.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <algorithm>

namespace mmdx {

bool PackTextures::Upload(Dx12Context& ctx, const ShaderPack& pack, Set& set,
                          const std::filesystem::path& folder) {
    set.loaded = true;
    const uint32_t count = (uint32_t)std::min<size_t>(pack.textures.size(), kPackMaxTextures);
    if (count == 0) return true;   // no textures: the table is not bound (PackSampleTex returns white)
    ShaderPackRegistry& reg = ShaderPacks();
    set.tex.assign(kPackMaxTextures, {});
    set.srv = ctx.SrvHeap().Allocate(kPackMaxTextures);
    if (set.srv == DescriptorHeap::kInvalid) {
        LOG_ERROR("shader pack '%s': out of SRV descriptors for pack textures", pack.id.c_str());
        set.tex.clear();
        return false;
    }
    UploadBatch batch(ctx);
    ImageRGBA8 white;
    white.mips.push_back({1, 1, {255, 255, 255, 255}});
    set.white = batch.CreateTexture(white, L"packtex.white");   // kept alive: SRVs point at it
    const ComPtr<ID3D12Resource>& whiteRes = set.white;
    uint32_t missing = 0;
    const std::filesystem::path userFolder = folder;
    ImageRGBA8 img;
    std::string loadError;
    for (uint32_t i = 0; i < count; ++i) {
        const PackTexture& t = pack.textures[i];   // address mode / srgb are compile defines; the data is the file's
        std::filesystem::path path;
        if (!ResolvePackTextureFile(pack, t.file, userFolder, path)) {
            LOG_WARN("shader pack '%s': texture '%s' not found, white is used", pack.id.c_str(), t.file.c_str());
            ++missing;
            continue;
        }
        img = {};
        loadError.clear();
        if (LoadImageRGBA8(path, img, &loadError) && img.Width() <= kPackMaxTextureSize &&
            img.Height() <= kPackMaxTextureSize) {
            set.tex[i] = batch.CreateTextureTyped(img, DXGI_FORMAT_R8G8B8A8_TYPELESS, L"packtex.texture");
            if (!set.tex[i])
                LOG_WARN("shader pack '%s': texture '%s' upload failed, white is used", pack.id.c_str(),
                         t.file.c_str());
        } else if (!img.Empty()) {
            LOG_WARN("shader pack '%s': texture '%s' is %ux%u (max %u), white is used", pack.id.c_str(),
                     t.file.c_str(), img.Width(), img.Height(), kPackMaxTextureSize);
        } else {
            LOG_WARN("shader pack '%s': texture '%s' cannot be decoded (%s), white is used", pack.id.c_str(),
                     t.file.c_str(), loadError.c_str());
        }
        if (!set.tex[i]) ++missing;
    }
    batch.Submit();
    reg.ReportMissingTextures(pack.id, folder, missing);
    if (missing) LOG_WARN("shader pack '%s': %u of %u textures missing (white is used)", pack.id.c_str(), missing, count);
    if (!whiteRes) {
        LOG_ERROR("shader pack '%s': pack texture upload failed", pack.id.c_str());
        ctx.SrvHeap().Free(set.srv, kPackMaxTextures);
        set.srv = DescriptorHeap::kInvalid;
        set.tex.clear();
        return false;
    }

    ID3D12Device* device = ctx.Device();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = UINT(-1);
    for (uint32_t i = 0; i < kPackMaxTextures; ++i) {
        // sRGB textures sample as linear values (the _SRGB view format); others as stored (UNORM)
        srv.Format = i < count && pack.textures[i].srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        ID3D12Resource* res = i < count && set.tex[i] ? set.tex[i].Get() : whiteRes.Get();
        device->CreateShaderResourceView(res, &srv, ctx.SrvHeap().Cpu(set.srv + i));
    }
    return true;
}

const PackTextures::Set* PackTextures::Acquire(Dx12Context& ctx, const ShaderPack& pack,
                                               const std::filesystem::path& userFolder) {
    const uint32_t count = (uint32_t)std::min<size_t>(pack.textures.size(), kPackMaxTextures);
    if (count == 0) return nullptr;

    ShaderPackRegistry& reg = ShaderPacks();
    if (generation_ != reg.Generation()) {
        Clear(ctx);
        generation_ = reg.Generation();
    }

    auto key = std::make_pair(pack.id, PathToUtf8(userFolder));
    auto it = sets_.find(key);
    if (it != sets_.end()) {
        return (it->second.loaded && it->second.srv != DescriptorHeap::kInvalid) ? &it->second : nullptr;
    }

    Set& set = sets_[key];
    if (!Upload(ctx, pack, set, userFolder) || set.srv == DescriptorHeap::kInvalid) {
        return nullptr;
    }
    return &set;
}

void PackTextures::Clear(Dx12Context& ctx) {
    if (!sets_.empty()) {
        ctx.WaitForGpu();
        for (auto& [key, set] : sets_) {
            if (set.srv != DescriptorHeap::kInvalid) {
                ctx.SrvHeap().Free(set.srv, kPackMaxTextures);
                set.srv = DescriptorHeap::kInvalid;
            }
        }
        sets_.clear();
    }
}

void PackTextures::Clear(Dx12Context* ctx) {
    if (ctx) Clear(*ctx);
    else sets_.clear();
}

} // namespace mmdx
