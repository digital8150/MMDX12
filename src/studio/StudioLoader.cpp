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
                part.kind = ModelKind::Stage;
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
            MotionData dance, camera;
            LoadStudioSong(*song, dance, camera, nullptr);
            ch.motion = std::move(dance);
            if (!song->cameraVmd.empty()) {
                out.camera.camera = std::move(camera.camera);
                out.camera.light = std::move(camera.light);
                out.camera.shadow = std::move(camera.shadow);
                out.camera.modelName = camera.modelName;
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



bool LoadStudioModel(const std::filesystem::path& path, ModelKind kind, const std::string& label,
                     StudioPackageModel& out, LoadProgress* progress, std::string* error) {
    try {
        out = StudioPackageModel{};
        if (progress) progress->SetStatus(Tr("모델 로드: ") + label);
        if (!std::filesystem::exists(path)) {
            if (error) *error = Tr("모델 파일이 없습니다: ") + PathToUtf8(path);
            return false;
        }
        const ModelRole role = kind == ModelKind::Character ? ModelRole::Character
                               : kind == ModelKind::Prop    ? ModelRole::Prop
                                                            : ModelRole::Stage;
        std::string err;
        if (!LoadModel(path, role, label, progress, 0.05f, 0.95f, out, &err)) {
            if (error) *error = Tr("모델을 불러오지 못했습니다: ") + err;
            return false;
        }
        out.kind = kind;
        out.name = label;
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadStudioStage(const StageAsset& stage, std::vector<StudioPackageModel>& out, LoadProgress* progress,
                     std::string* error) {
    try {
        out.clear();
        const size_t n = stage.parts.size();
        std::string lastErr;
        for (size_t k = 0; k < n; ++k) {
            const std::string label =
                stage.displayName + (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
            if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);
            StudioPackageModel part;
            part.kind = ModelKind::Stage;
            std::string err;
            const float f0 = 0.05f + 0.9f * (float)k / (float)n;
            const float f1 = 0.05f + 0.9f * (float)(k + 1) / (float)n;
            if (!LoadModel(stage.parts[k], ModelRole::Stage, label, progress, f0, f1, part, &err)) {
                LOG_WARN("studio: stage part load failed: %s: %s", PathToUtf8(stage.parts[k]).c_str(), err.c_str());
                lastErr = err;
                continue;
            }
            out.push_back(std::move(part));
        }
        if (out.empty() && n > 0) {
            if (error) *error = Tr("모델을 불러오지 못했습니다: ") + lastErr;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadStudioSong(const SongAsset& song, MotionData& dance, MotionData& camera, std::string* error) {
    try {
        dance = {};
        camera = {};
        std::vector<std::filesystem::path> files;
        if (!song.danceVmd.empty()) files.push_back(song.danceVmd);
        files.insert(files.end(), song.extraVmds.begin(), song.extraVmds.end());
        std::string lastErr;
        int loaded = 0;
        int attempted = 0;
        for (const auto& f : files) {
            attempted++;
            VmdMotion vmd;
            std::string vmdErr;
            if (!LoadVmd(f, vmd, &vmdErr)) {
                LOG_WARN("studio: motion skipped: %s: %s", PathToUtf8(f).c_str(), vmdErr.c_str());
                lastErr = vmdErr;
                continue;
            }
            loaded++;
            MotionData d = MotionData::FromVmd(vmd);
            d.camera.clear();
            d.light.clear();
            d.shadow.clear();
            if (dance.Empty()) dance.modelName = d.modelName;
            dance.Merge(d);
        }
        if (!song.cameraVmd.empty()) {
            attempted++;
            VmdMotion cam;
            std::string camErr;
            if (LoadVmd(song.cameraVmd, cam, &camErr)) {
                loaded++;
                MotionData d = MotionData::FromVmd(cam);
                camera.camera = std::move(d.camera);
                camera.light = std::move(d.light);
                camera.shadow = std::move(d.shadow);
                camera.modelName = d.modelName;
            } else {
                LOG_WARN("studio: camera skipped: %s: %s", PathToUtf8(song.cameraVmd).c_str(), camErr.c_str());
                lastErr = camErr;
            }
        }
        if (loaded == 0 && attempted > 0) {
            if (error) *error = std::string(Tr("모션 로드")) + ": " + lastErr;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadStudioProjectPackage(const std::filesystem::path& file, bool recovery, StudioPackage& out,
                              LoadProgress* progress, std::string* error) {
    try {
        out = StudioPackage{};
        if (progress) progress->SetStatus(Tr("프로젝트 로드"));
        ProjectData pd;
        std::vector<std::string> warn;
        std::string err;
        if (!LoadProject(file, pd, &err, &warn)) {
            if (error) *error = Tr("프로젝트를 열 수 없습니다: ") + err;
            return false;
        }
        out.fromProject = true;
        out.editor = pd.editor;
        out.camera = pd.camera;
        out.audioPath = pd.audioPath;
        out.audioOffset = pd.audioOffset;
        out.warnings = warn;
        out.recovered = recovery;
        out.projectPath = recovery ? pd.recoveryOf : file;

        const size_t n = pd.models.size();
        std::vector<int> remap(n, -1);
        for (size_t i = 0; i < n; ++i) {
            auto& pm = pd.models[i];
            const float f0 = 0.05f + 0.9f * (float)i / (float)n;
            const float f1 = 0.05f + 0.9f * (float)(i + 1) / (float)n;
            if (progress) progress->SetStatus(Tr("모델 로드: ") + pm.name);
            
            StudioPackageModel m;
            const ModelRole role = pm.kind == ModelKind::Character ? ModelRole::Character
                                  : pm.kind == ModelKind::Prop    ? ModelRole::Prop
                                                                  : ModelRole::Stage;
            std::string merr;
            if (!LoadModel(pm.path, role, pm.name, progress, f0, f1, m, &merr)) {
                out.warnings.push_back(Tr("모델을 불러오지 못했습니다: ") + pm.name + " (" + PathToUtf8(pm.path) + ")");
                continue;
            }
            m.name = pm.name;
            m.libraryId = pm.libraryId;
            m.kind = pm.kind;
            m.visible = pm.visible;
            m.attach = pm.attach;
            m.place = pm.place;
            m.motion = std::move(pm.motion);
            m.motion.CanonicalizeNames(*m.pmx);
            if (m.motion.modelName.empty()) m.motion.modelName = m.pmx->name;
            
            remap[i] = (int)out.models.size();
            out.models.push_back(std::move(m));
        }

        for (auto& m : out.models) {
            if (m.kind == ModelKind::Prop) {
                int parent = m.attach.parent;
                m.attach.parent = (parent >= 0 && parent < (int)remap.size()) ? remap[parent] : -1;
            } else {
                m.attach = PropAttach{};
            }
        }

        if (out.editor.selectedModel >= 0 && out.editor.selectedModel < (int)remap.size()) {
            out.editor.selectedModel = remap[out.editor.selectedModel];
        } else {
            out.editor.selectedModel = -1;
        }

        LOG_INFO("studio: project %s: %d models, %d warnings", PathToUtf8(file).c_str(), (int)out.models.size(), (int)out.warnings.size());
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace mmdx::studio
