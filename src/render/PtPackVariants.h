#pragma once

#include "render/RenderPass.h"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mmdx {

class GpuModel;

class PtPackVariants {
public:
    void Clear(Dx12Context* ctx = nullptr);

    // Resolves and returns the compute pipeline to use: either &defaultPipe or a cached/compiled
    // pack variant under MMDX_PT_PACK.
    ComputePipeline* Resolve(Dx12Context& ctx,
                             const std::filesystem::path& shaderDir,
                             const std::vector<GpuModel*>& models,
                             ComputePipeline& defaultPipe,
                             const char* shaderFile,
                             const char* entryPoint,
                             const char* logTag);

private:
    uint32_t generation_ = 0;
    std::map<std::string, ComputePipeline> pipes_;
    std::set<std::string> failed_;
};

} // namespace mmdx
