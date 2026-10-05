// Worker-thread part of opening a scene in the Studio. Separate from StudioDoc.cpp because
// asset/ModelImport.h and render/GpuModel.h both declare mmdx::ModelRole.
#include "studio/StudioDoc.h"
#include "app/SceneLoader.h"
#include "asset/AssetLibrary.h"
#include "asset/ModelImport.h"
#include "asset/VmdMotion.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx::studio {

namespace {
bool LoadModel(const std::filesystem::path& path, ModelRole role, const std::string& label, LoadProgress* progress,
               float f0, float f1, StudioPackageModel& out, std::string* error) {
    LoadedModelCpu cpu;
    cpu.pmx = std::make_shared<PmxModel>();
    if (!LoadModelFile(path, role, *cpu.pmx, error)) return false;
    DecodeModelTextures(cpu, progress, f0, f1, label.c_str());
    out.pmx = cpu.pmx;
    out.textures = std::move(cpu.textures);
    out.name = label;
    return true;
}
} // namespace

bool LoadStudioPackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset* song,
                       StudioPackage& out, LoadProgress* progress, std::string* error) {
    try {
        out = StudioPackage{};
        if (progress) progress->fraction.store(0.0f, std::memory_order_relaxed);

        if (stage) {
            const size_t n = stage->parts.size();
            for (size_t k = 0; k < n; ++k) {
                const std::string label =
                    stage->displayName + (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
                if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);
                StudioPackageModel part;
                part.isStage = true;
                std::string err;
                const float f0 = 0.05f + 0.35f * (float)k / (float)n, f1 = 0.05f + 0.35f * (float)(k + 1) / (float)n;
                if (!LoadModel(stage->parts[k], ModelRole::Stage, label, progress, f0, f1, part, &err)) {
                    LOG_WARN("studio: stage part load failed: %s: %s", PathToUtf8(stage->parts[k]).c_str(), err.c_str());
                    continue;
                }
                out.models.push_back(std::move(part));
            }
        }

        if (progress) progress->SetStatus(Tr("캐릭터 로드: ") + character.displayName);
        StudioPackageModel ch;
        ch.libraryId = character.id;
        std::string err;
        if (!LoadModel(character.modelPath, ModelRole::Character, character.displayName, progress, 0.4f, 0.85f, ch, &err)) {
            if (error) *error = Tr("캐릭터를 불러오지 못했습니다: ") + err;
            return false;
        }

        if (song) {
            if (progress) progress->SetStatus(Tr("모션 로드"));
            std::vector<std::filesystem::path> files;
            if (!song->danceVmd.empty()) files.push_back(song->danceVmd);
            files.insert(files.end(), song->extraVmds.begin(), song->extraVmds.end());
            for (const auto& f : files) {
                VmdMotion vmd;
                std::string vmdErr;
                if (!LoadVmd(f, vmd, &vmdErr)) {
                    LOG_WARN("studio: motion skipped: %s: %s", PathToUtf8(f).c_str(), vmdErr.c_str());
                    continue;
                }
                MotionData d = MotionData::FromVmd(vmd);
                d.camera.clear();
                d.light.clear();
                if (ch.motion.Empty()) ch.motion.modelName = d.modelName;
                ch.motion.Merge(d);
            }
            if (!song->cameraVmd.empty()) {
                VmdMotion cam;
                std::string camErr;
                if (LoadVmd(song->cameraVmd, cam, &camErr)) {
                    MotionData d = MotionData::FromVmd(cam);
                    out.camera.camera = std::move(d.camera);
                    out.camera.light = std::move(d.light);
                    out.camera.shadow = std::move(d.shadow);
                    out.camera.modelName = d.modelName;
                } else {
                    LOG_WARN("studio: camera skipped: %s: %s", PathToUtf8(song->cameraVmd).c_str(), camErr.c_str());
                }
            }
            out.audioPath = song->audioPath;
        }
        ch.motion.CanonicalizeNames(*ch.pmx);
        if (ch.motion.modelName.empty()) ch.motion.modelName = ch.pmx->name;
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        out.models.push_back(std::move(ch));
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace mmdx::studio
