#include "core/I18n.h"
#include "app/SceneLoader.h"

#include <algorithm>
#include <execution>
#include <mutex>
#include <set>

#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "asset/ImageLoader.h"
#include "asset/ModelImport.h"
#include "asset/VmdMotion.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

// Decodes all textures referenced by the model's materials in parallel.
void DecodeModelTextures(LoadedModelCpu& m, LoadProgress* progress, float fracBegin, float fracEnd,
                         const char* label) {
    m.textures.resize(m.pmx->textures.size());

    // Referenced texture indices; only indices within range.
    std::set<int32_t> referenced;
    for (const PmxMaterial& mat : m.pmx->materials) {
        if (mat.textureIndex >= 0 && (size_t)mat.textureIndex < m.pmx->textures.size())
            referenced.insert(mat.textureIndex);
        if ((mat.sphereMode == PmxSphereMode::Multiply || mat.sphereMode == PmxSphereMode::Add) &&
            mat.sphereTextureIndex >= 0 && (size_t)mat.sphereTextureIndex < m.pmx->textures.size())
            referenced.insert(mat.sphereTextureIndex);
        if (!mat.sharedToon && mat.toonIndex >= 0 && (size_t)mat.toonIndex < m.pmx->textures.size())
            referenced.insert(mat.toonIndex);
    }

    if (progress) progress->SetStatus(std::string(Tr("텍스처 디코딩: ")) + label);

    std::atomic<int> done{0};
    const int total = (int)referenced.size();
    std::for_each(std::execution::par, referenced.begin(), referenced.end(),
                  [&](int32_t index) {
                      if ((size_t)index < m.pmx->embeddedTextures.size() && !m.pmx->embeddedTextures[index].empty()) {
                          const auto& bytes = m.pmx->embeddedTextures[index];
                          std::string err;
                          if (!LoadImageRGBA8FromMemory(bytes.data(), bytes.size(), m.textures[index], &err))
                              LOG_WARN("embedded texture decode failed: %s (%s): %s", m.pmx->textures[index].c_str(), label, err.c_str());
                          int d = ++done;
                          if (progress && total > 0)
                              progress->fraction.store(fracBegin + (fracEnd - fracBegin) * (float)d / (float)total,
                                                       std::memory_order_relaxed);
                          return;
                      }
                      const std::filesystem::path p = m.pmx->ResolveTexturePath(index);
                      if (p.empty()) {
                          LOG_WARN("texture not found: %s (%s)", m.pmx->textures[index].c_str(), label);
                      } else {
                          std::string err;
                          if (!LoadImageRGBA8(p, m.textures[index], &err))
                              LOG_WARN("texture decode failed: %s: %s", PathToUtf8(p).c_str(), err.c_str());
                      }
                      int d = ++done;
                      if (progress && total > 0)
                          progress->fraction.store(fracBegin + (fracEnd - fracBegin) * (float)d / (float)total,
                                                   std::memory_order_relaxed);
                  });
}

bool LoadScenePackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset& song,
                      ScenePackage& out, LoadProgress* progress, std::string* error) {
    try {
        out = ScenePackage{};

        // 1. Character.
        {
            std::string err;
            if (progress) {
                progress->fraction.store(0.0f, std::memory_order_relaxed);
                progress->SetStatus(Tr("캐릭터 로드: ") + character.displayName);
            }
            out.character.pmx = std::make_shared<PmxModel>();
            if (!LoadModelFile(character.modelPath, ModelRole::Character, *out.character.pmx, &err)) {
                if (error) *error = Tr("캐릭터를 불러오지 못했습니다: ") + err;
                return false;
            }
        }

        // 2. Character textures.
        DecodeModelTextures(out.character, progress, 0.05f, 0.45f, character.displayName.c_str());

        // 3. Stage parts.
        if (stage) {
            const size_t n = stage->parts.size();
            for (size_t k = 0; k < n; ++k) {
                const std::filesystem::path& partPath = stage->parts[k];
                std::string label = stage->displayName +
                                    (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
                if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);

                LoadedModelCpu part;
                part.pmx = std::make_shared<PmxModel>();
                std::string err;
                if (!LoadModelFile(partPath, ModelRole::Stage, *part.pmx, &err)) {
                    LOG_WARN("stage part load failed: %s: %s", PathToUtf8(partPath).c_str(), err.c_str());
                    continue;
                }
                float begin = 0.45f + 0.35f * (float)k / (float)std::max<size_t>(n, 1);
                float end = 0.45f + 0.35f * (float)(k + 1) / (float)std::max<size_t>(n, 1);
                DecodeModelTextures(part, progress, begin, end, label.c_str());
                out.stageParts.push_back(std::move(part));
            }
        }

        // 4. Motions.
        if (progress) progress->SetStatus(Tr("모션 로드"));
        VmdMotion dance;
        std::string err;
        if (!LoadVmd(song.danceVmd, dance, &err)) {
            if (error) *error = Tr("모션을 불러오지 못했습니다: ") + err;
            return false;
        }
        std::vector<std::unique_ptr<VmdMotion>> extraStorage;
        std::vector<const VmdMotion*> layers = {&dance};
        for (const std::filesystem::path& extra : song.extraVmds) {
            auto vmd = std::make_unique<VmdMotion>();
            std::string extraErr;
            if (!LoadVmd(extra, *vmd, &extraErr)) {
                LOG_WARN("extra vmd load skipped: %s: %s", PathToUtf8(extra).c_str(), extraErr.c_str());
                continue;
            }
            layers.push_back(vmd.get());
            extraStorage.push_back(std::move(vmd));
        }
        out.motion = BoundMotion::Bind(*out.character.pmx, layers);
        LOG_INFO("motion bound: %d bone tracks, %d morph tracks, end frame %.0f",
                 out.motion->BoundBoneCount(), out.motion->BoundMorphCount(), out.motion->EndFrame());

        // 5. Camera.
        if (!song.cameraVmd.empty()) {
            VmdMotion camVmd;
            std::string camErr;
            if (LoadVmd(song.cameraVmd, camVmd, &camErr)) {
                out.camera = CameraMotion::Create(camVmd);
                out.lightKeys = camVmd.lightKeys;
                out.shadowKeys = camVmd.shadowKeys;
                if (!out.camera) LOG_WARN("no camera keys in %s", PathToUtf8(song.cameraVmd).c_str());
            } else {
                LOG_WARN("camera vmd load failed: %s: %s", PathToUtf8(song.cameraVmd).c_str(), camErr.c_str());
            }
        }

        // 6. Footer data.
        out.audioPath = song.audioPath;
        float endFrame = out.motion->EndFrame();
        if (out.camera) endFrame = std::max(endFrame, out.camera->EndFrame());
        out.endFrame = endFrame;

        // 7. Done (GPU upload happens on the main thread).
        if (progress) {
            progress->fraction.store(1.0f, std::memory_order_relaxed);
            progress->SetStatus(Tr("GPU 업로드"));
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string(Tr("씬 로드 중 오류: ")) + e.what();
        return false;
    } catch (...) {
        if (error) *error = Tr("씬 로드 중 알 수 없는 오류");
        return false;
    }
}

bool LoadRenderBenchPackage(const std::vector<CharacterAsset>& characters, const std::vector<SongAsset>& songs,
                            ScenePackage& out, LoadProgress* progress, std::string* error) {
    try {
        if (characters.empty() || characters.size() != songs.size()) {
            if (error) *error = Tr("렌더 벤치마크 장면 구성이 올바르지 않습니다");
            return false;
        }
        out = ScenePackage{};

        const size_t n = characters.size();
        for (size_t i = 0; i < n; ++i) {
            const float fraction = (float)i / (float)n;

            // 1. Character.
            LoadedModelCpu m;
            if (progress) progress->SetStatus(Tr("캐릭터 로드: ") + characters[i].displayName);
            m.pmx = std::make_shared<PmxModel>();
            std::string err;
            if (!LoadModelFile(characters[i].modelPath, ModelRole::Character, *m.pmx, &err)) {
                if (error) *error = Tr("캐릭터를 불러오지 못했습니다: ") + err;
                return false;
            }

            // 2. Character textures.
            DecodeModelTextures(m, progress, fraction, (i + 0.8f) / (float)n, characters[i].displayName.c_str());

            // 3. Dance motion.
            VmdMotion dance;
            if (!LoadVmd(songs[i].danceVmd, dance, &err)) {
                if (error) *error = Tr("모션을 불러오지 못했습니다: ") + err;
                return false;
            }
            std::vector<std::unique_ptr<VmdMotion>> extraStorage;
            std::vector<const VmdMotion*> layers = {&dance};
            for (const std::filesystem::path& extra : songs[i].extraVmds) {
                auto vmd = std::make_unique<VmdMotion>();
                std::string extraErr;
                if (!LoadVmd(extra, *vmd, &extraErr)) {
                    LOG_WARN("extra vmd load skipped: %s: %s", PathToUtf8(extra).c_str(), extraErr.c_str());
                    continue;
                }
                layers.push_back(vmd.get());
                extraStorage.push_back(std::move(vmd));
            }
            auto motion = BoundMotion::Bind(*m.pmx, layers);

            if (i == 0) {
                out.character = std::move(m);
                out.motion = motion;
            } else {
                out.extraCharacters.push_back(std::move(m));
                out.extraMotions.push_back(motion);
            }
        }

        // No stage, camera or audio: endFrame stays 0.
        // Done (GPU upload happens on the main thread).
        if (progress) {
            progress->fraction.store(1.0f, std::memory_order_relaxed);
            progress->SetStatus(Tr("GPU 업로드"));
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string(Tr("씬 로드 중 오류: ")) + e.what();
        return false;
    } catch (...) {
        if (error) *error = Tr("씬 로드 중 알 수 없는 오류");
        return false;
    }
}

} // namespace mmdx
