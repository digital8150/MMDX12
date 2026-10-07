#include "render/PtPackVariants.h"
#include "render/GpuModel.h"
#include "render/ShaderPack.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <algorithm>
#include <string_view>

namespace mmdx {

void PtPackVariants::Clear(Dx12Context* ctx) {
    if (ctx && !pipes_.empty()) {
        ctx->WaitForGpu();
    }
    pipes_.clear();
    failed_.clear();
}

ComputePipeline* PtPackVariants::Resolve(Dx12Context& ctx,
                                         const std::filesystem::path& shaderDir,
                                         const std::vector<GpuModel*>& models,
                                         ComputePipeline& defaultPipe,
                                         const char* shaderFile,
                                         const char* entryPoint,
                                         const char* logTag) {
    ShaderPackRegistry& reg = ShaderPacks();
    if (generation_ != reg.Generation()) {
        if (!pipes_.empty()) {
            ctx.WaitForGpu();
            pipes_.clear();
        }
        failed_.clear();
        generation_ = reg.Generation();
    }

    const ShaderPack* chosenPack = nullptr;
    bool warnedMultiple = false;
    for (GpuModel* model : models) {
        if (!model || model->Role() != ModelRole::Character || model->ShaderPackId().empty()) continue;
        const ShaderPack* pack = reg.Find(model->ShaderPackId());
        if (!pack || !pack->hasPtSurface) continue;
        if (!chosenPack) {
            chosenPack = pack;
        } else if (chosenPack->id != pack->id && !warnedMultiple) {
            if (logTag && std::string_view(logTag) == "offline") {
                LOG_WARN("offline: multiple pt packs present; using %s for GiTable", chosenPack->id.c_str());
            } else {
                LOG_WARN("%s: multiple pt packs present; using %s", logTag ? logTag : "pt", chosenPack->id.c_str());
            }
            warnedMultiple = true;
        }
    }

    if (!chosenPack) {
        return &defaultPipe;
    }

    auto it = pipes_.find(chosenPack->id);
    if (it != pipes_.end()) {
        return &it->second;
    }
    if (failed_.count(chosenPack->id)) {
        return &defaultPipe;
    }

    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(chosenPack->dir / L"pt_surface.hlsl", shaderDir, ec);
    if (ec || rel.empty()) rel = chosenPack->dir / L"pt_surface.hlsl";
    std::string inc = PathToUtf8(rel);
    std::replace(inc.begin(), inc.end(), '\\', '/');
    const std::string incDefine = "\"" + inc + "\"";
    const ShaderDefines defs = {{"MMDX_PT_PACK", incDefine}};

    ComputePipeline pipe;
    const std::filesystem::path f = shaderDir / Utf8ToWide(shaderFile);
    if (pipe.Create(ctx, f, entryPoint, defs)) {
        LOG_INFO("%s: pt pack '%s' compiled into %s", logTag ? logTag : "pt", chosenPack->id.c_str(), entryPoint);
        auto [ins, _] = pipes_.emplace(chosenPack->id, std::move(pipe));
        return &ins->second;
    } else {
        LOG_ERROR("%s: failed to compile %s with pt pack '%s'", logTag ? logTag : "pt", entryPoint, chosenPack->id.c_str());
        failed_.insert(chosenPack->id);
        return &defaultPipe;
    }
}

} // namespace mmdx
