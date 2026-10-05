#include "asset/AssetLibrary.h"
#include "asset/ImageLoader.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

std::string Rel(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::string rel = mmdx::PathToUtf8(p.lexically_relative(root));
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    return rel;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::filesystem::path libraryDir;
    bool full = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--full") full = true;
        else if (libraryDir.empty()) libraryDir = arg;
    }
    if (libraryDir.empty()) {
        fprintf(stderr, "usage: asset_probe <libraryDir> [--full]\n");
        return 2;
    }

    const mmdx::LibraryScanResult scan = mmdx::ScanLibrary(libraryDir, nullptr);

    printf("root: %s\n", mmdx::PathToUtf8(scan.root).c_str());
    printf("characters (%d):\n", static_cast<int>(scan.characters.size()));
    for (const auto& c : scan.characters) {
        printf("  %s | %s | %s | v=%u b=%u m=%u\n", c.displayName.c_str(), c.id.c_str(), c.format.c_str(),
               c.vertexCount, c.boneCount, c.materialCount);
    }
    printf("stages (%d):\n", static_cast<int>(scan.stages.size()));
    for (const auto& s : scan.stages) {
        printf("  %s | %s | %s | parts=%d v=%u\n", s.displayName.c_str(), s.id.c_str(), s.format.c_str(),
               static_cast<int>(s.parts.size()), s.vertexCount);
        for (const auto& part : s.parts) {
            printf("    - %s\n", Rel(part, scan.root).c_str());
        }
    }
    printf("songs (%d):\n", static_cast<int>(scan.songs.size()));
    for (const auto& s : scan.songs) {
        printf("  %s | %s | %.1fs | dance=%s | cam=%s | audio=%s | extra=%d\n",
               s.displayName.c_str(), s.id.c_str(), s.durationSec,
               s.danceVmd.empty() ? "-" : Rel(s.danceVmd, scan.root).c_str(),
               s.cameraVmd.empty() ? "-" : Rel(s.cameraVmd, scan.root).c_str(),
               s.audioPath.empty() ? "-" : Rel(s.audioPath, scan.root).c_str(),
               static_cast<int>(s.extraVmds.size()));
    }
    printf("warnings (%d):\n", static_cast<int>(scan.warnings.size()));
    for (const auto& w : scan.warnings) {
        printf("  %s\n", w.c_str());
    }
    printf("notes (%d):\n", static_cast<int>(scan.notes.size()));
    for (const auto& n : scan.notes) printf("  %s\n", n.c_str());
    printf("scan time: %.2fs\n", scan.scanSeconds);

    if (full) {
        printf("\n--- full load ---\n");
        std::vector<std::pair<std::filesystem::path, mmdx::ModelRole>> pmxPaths;
        for (const auto& c : scan.characters) pmxPaths.push_back({c.modelPath, mmdx::ModelRole::Character});
        for (const auto& s : scan.stages) {
            for (const auto& part : s.parts) pmxPaths.push_back({part, mmdx::ModelRole::Stage});
        }
        for (const auto& [p, role] : pmxPaths) {
            mmdx::PmxModel model;
            std::string error;
            const std::string rel = Rel(p, scan.root);
            if (!mmdx::LoadModelFile(p, role, model, &error)) {
                printf("LOAD %s: FAIL %s\n", rel.c_str(), error.c_str());
                continue;
            }
            printf("LOAD %s: OK v=%u f=%u mat=%d bones=%d morphs=%d rigid=%d joints=%d\n",
                   rel.c_str(), static_cast<uint32_t>(model.vertices.size()),
                   static_cast<uint32_t>(model.indices.size() / 3),
                   static_cast<int>(model.materials.size()),
                   static_cast<int>(model.bones.size()),
                   static_cast<int>(model.morphs.size()),
                   static_cast<int>(model.rigidBodies.size()),
                   static_cast<int>(model.joints.size()));

            // Texture indices referenced by any material (texture, sphere, non-shared toon).
            std::map<int32_t, std::string> referenced;
            for (const auto& m : model.materials) {
                auto add = [&referenced, &model](int32_t idx) {
                    if (idx >= 0 && idx < static_cast<int32_t>(model.textures.size())) {
                        referenced.emplace(idx, model.textures[static_cast<size_t>(idx)]);
                    }
                };
                add(m.textureIndex);
                add(m.sphereTextureIndex);
                if (!m.sharedToon) add(m.toonIndex);
            }
            int texOk = 0;
            std::vector<std::string> missing;
            for (const auto& [idx, raw] : referenced) {
                (void)raw;
                mmdx::ImageRGBA8 image;
                std::string texError;
                const bool embedded = (size_t)idx < model.embeddedTextures.size() && !model.embeddedTextures[idx].empty();
                const std::filesystem::path texPath = embedded ? std::filesystem::path{} : model.ResolveTexturePath(idx);
                if (embedded ? mmdx::LoadImageRGBA8FromMemory(model.embeddedTextures[idx].data(), model.embeddedTextures[idx].size(), image, &texError)
                             : (!texPath.empty() && mmdx::LoadImageRGBA8(texPath, image, &texError))) {
                    ++texOk;
                } else {
                    missing.push_back(raw);
                }
            }
            printf("  textures: %d/%d ok\n", texOk, static_cast<int>(referenced.size()));
            for (const auto& raw : missing) {
                printf("  missing: %s\n", raw.c_str());
            }
        }

        std::vector<std::filesystem::path> vmdPaths;
        for (const auto& s : scan.songs) {
            if (!s.danceVmd.empty()) vmdPaths.push_back(s.danceVmd);
            if (!s.cameraVmd.empty()) vmdPaths.push_back(s.cameraVmd);
            for (const auto& e : s.extraVmds) vmdPaths.push_back(e);
        }
        for (const auto& p : vmdPaths) {
            mmdx::VmdMotion motion;
            std::string error;
            const std::string rel = Rel(p, scan.root);
            if (!mmdx::LoadVmd(p, motion, &error)) {
                printf("VMD %s: FAIL %s\n", rel.c_str(), error.c_str());
                continue;
            }
            printf("VMD %s: bones=%d morphs=%d cams=%d ik=%d maxFrame=%u\n", rel.c_str(),
                   static_cast<int>(motion.boneKeys.size()),
                   static_cast<int>(motion.morphKeys.size()),
                   static_cast<int>(motion.cameraKeys.size()),
                   static_cast<int>(motion.ikKeys.size()), motion.maxFrame);
        }
    }

    const bool atLeastOneCharacter = !scan.characters.empty();
    const bool atLeastOneSong = !scan.songs.empty();
    return (atLeastOneCharacter && atLeastOneSong) ? 0 : 1;
}
