#pragma once

#include "render/Dx12Context.h"
#include "render/ShaderPack.h"
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mmdx {

class PackTextures {
public:
    struct Set {
        bool loaded = false;
        uint32_t srv = DescriptorHeap::kInvalid;   // kPackMaxTextures consecutive SRVs in ctx.SrvHeap()
        ComPtr<ID3D12Resource> white;              // 1x1 neutral fallback (kept alive with the SRVs)
        std::vector<ComPtr<ID3D12Resource>> tex;
    };

    PackTextures() = default;
    ~PackTextures() = default;
    PackTextures(const PackTextures&) = delete;
    PackTextures& operator=(const PackTextures&) = delete;

    // Uploads pack textures into `set` (allocates kPackMaxTextures SRVs in ctx.SrvHeap()).
    // Used by Acquire and by ScenePass::EnsurePackTextures.
    static bool Upload(Dx12Context& ctx, const ShaderPack& pack, Set& set,
                       const std::filesystem::path& folder);

    // Acquires a texture set for (pack id, resolved folder). Returns nullptr when the pack
    // has no textures or upload failed. Invalidates cache on ShaderPacks().Generation() change.
    const Set* Acquire(Dx12Context& ctx, const ShaderPack& pack, const std::filesystem::path& userFolder);

    // Waits for the GPU and frees all allocated SRVs.
    void Clear(Dx12Context& ctx);
    void Clear(Dx12Context* ctx = nullptr);

private:
    uint32_t generation_ = 0;
    std::map<std::pair<std::string, std::string>, Set> sets_;   // (pack id, resolved folder utf-8) -> Set
};

} // namespace mmdx
