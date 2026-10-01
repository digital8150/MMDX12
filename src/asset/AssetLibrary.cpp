#include "asset/AssetLibrary.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <chrono>
#include <execution>
#include <map>
#include <optional>
#include <set>

namespace mmdx {

namespace {

struct FileWalk {
    std::vector<std::filesystem::path> pmxFiles;
    std::vector<std::filesystem::path> vmdFiles;
    std::vector<std::filesystem::path> audioFiles;
    int pmdCount = 0;
};

std::string LowerExt(const std::filesystem::path& p) {
    return ToLowerAscii(PathToUtf8(p.extension()));
}

std::string RelKey(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::string rel = PathToUtf8(p.lexically_relative(root));
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    return rel;
}

// WAV duration via RIFF chunk walk. 0 on any problem.
double WavDurationSec(const std::filesystem::path& p) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return 0;
    double result = 0;
    uint8_t header[12] = {};
    if (fread(header, 1, 12, f) == 12 && memcmp(header, "RIFF", 4) == 0 &&
        memcmp(header + 8, "WAVE", 4) == 0) {
        uint32_t byteRate = 0;
        uint32_t dataSize = 0;
        for (;;) {
            uint8_t chunkHeader[8] = {};
            if (fread(chunkHeader, 1, 8, f) != 8) break;
            uint32_t chunkSize = 0;
            memcpy(&chunkSize, chunkHeader + 4, 4);
            if (memcmp(chunkHeader, "fmt ", 4) == 0) {
                if (chunkSize >= 16) {
                    uint8_t fmt[16] = {};
                    if (fread(fmt, 1, 16, f) != 16) break;
                    memcpy(&byteRate, fmt + 8, 4);
                    if (chunkSize > 16 && _fseeki64(f, chunkSize - 16, SEEK_CUR) != 0) break;
                } else {
                    if (_fseeki64(f, chunkSize, SEEK_CUR) != 0) break;
                }
            } else if (memcmp(chunkHeader, "data", 4) == 0) {
                dataSize = chunkSize;
                // Do not seek over the data chunk; we are done with it.
                break;
            } else {
                if (_fseeki64(f, (static_cast<uint64_t>(chunkSize) + 1) & ~1ull, SEEK_CUR) != 0) break;
            }
        }
        if (byteRate > 0 && dataSize > 0) result = static_cast<double>(dataSize) / byteRate;
    }
    fclose(f);
    return result;
}

bool ContainsBoneName(const std::vector<std::string>& names, std::string_view n) {
    for (const auto& name : names) {
        if (name == n) return true;
    }
    return false;
}

bool IsHumanoid(const std::vector<std::string>& boneNames) {
    if (!ContainsBoneName(boneNames, "\xE9\xA0\xAD")) return false;           // 頭
    if (!ContainsBoneName(boneNames, "\xE5\xB7\xA6\xE8\x85\x95")) return false;  // 左腕
    static const char* kLeftLeg[] = {
        "\xE5\xB7\xA6\xE8\xB6\xB3",      // 左足
        "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x96",  // 左ひざ
        "\xE5\xB7\xA6\xE8\xB6\xB3""D",   // 左足D
    };
    bool any = false;
    for (const char* leg : kLeftLeg) {
        if (ContainsBoneName(boneNames, leg)) {
            any = true;
            break;
        }
    }
    return any;
}

bool IsStageHint(const std::string& rel) {
    static const char* kStageHints[] = {
        "stage", "stages",
        "\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xBC\xE3\x82\xB8",  // ステージ
        "\xE8\x88\x9E\xE5\x8F\xB0",                          // 舞台
        "\xE5\x9C\xBA\xE6\x99\xAF",                          // 场景
        "\xE5\xA0\xB4\xE6\x99\xAF",                          // 場景
    };
    // Directory components of rel (excluding the filename), lowercased.
    size_t pos = 0;
    while (pos < rel.size()) {
        size_t slash = rel.find('/', pos);
        std::string component = rel.substr(pos, slash == std::string::npos
                                                     ? std::string::npos
                                                     : slash - pos);
        std::string lower = ToLowerAscii(component);
        for (const char* hint : kStageHints) {
            if (lower == hint) return true;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

std::string Trimmed(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string FileStemUtf8(const std::filesystem::path& p) {
    return PathToUtf8(p.stem());
}

std::string FileNameUtf8(const std::filesystem::path& p) {
    return PathToUtf8(p.filename());
}

std::string DirNameUtf8(const std::filesystem::path& p) {
    return PathToUtf8(p.parent_path().filename());
}

} // namespace

LibraryScanResult ScanLibrary(const std::filesystem::path& root, ScanProgress* progress) {
    LibraryScanResult result;
    result.root = root;
    const auto timerStart = std::chrono::steady_clock::now();

    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        result.warnings.push_back("library folder not found: " + PathToUtf8(root));
        result.scanSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - timerStart).count();
        LOG_INFO("library scan: 0 characters, 0 stages, 0 songs, 1 warnings in %.2fs",
                 result.scanSeconds);
        return result;
    }

    // ---- 2. walk ----
    FileWalk walk;
    {
        std::error_code walkEc;
        std::filesystem::recursive_directory_iterator it(root,
                                                         std::filesystem::directory_options::skip_permission_denied,
                                                         walkEc);
        std::filesystem::recursive_directory_iterator end;
        if (walkEc) {
            result.warnings.push_back("cannot enumerate library folder: " + PathToUtf8(root));
        }
        while (!walkEc && it != end) {
            std::error_code entryEc;
            if (it->is_regular_file(entryEc)) {
                const std::string ext = LowerExt(it->path());
                if (ext == ".pmx") walk.pmxFiles.push_back(it->path());
                else if (ext == ".vmd") walk.vmdFiles.push_back(it->path());
                else if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg")
                    walk.audioFiles.push_back(it->path());
                else if (ext == ".pmd") ++walk.pmdCount;
            }
            std::error_code incEc;
            it.increment(incEc);
            if (incEc) {
                result.warnings.push_back("walk error at " +
                                          PathToUtf8(it->path()) + ": " + incEc.message());
                break;
            }
        }
    }
    if (walk.pmdCount > 0) {
        result.warnings.push_back("PMD is not supported yet (" +
                                  std::to_string(walk.pmdCount) + " files ignored)");
    }
    const size_t filesTotal = walk.pmxFiles.size() + walk.vmdFiles.size();
    if (progress) progress->filesTotal = static_cast<int>(filesTotal);

    // ---- 3. probe all pmx and vmd files in parallel ----
    struct PmxProbeResult {
        std::optional<PmxProbe> probe;
        std::string error;
    };
    struct VmdProbeResult {
        std::optional<VmdProbe> probe;
        std::string error;
    };
    std::vector<PmxProbeResult> pmxResults(walk.pmxFiles.size());
    std::vector<VmdProbeResult> vmdResults(walk.vmdFiles.size());
    std::vector<size_t> pmxIndices(walk.pmxFiles.size());
    std::vector<size_t> vmdIndices(walk.vmdFiles.size());
    for (size_t i = 0; i < pmxIndices.size(); ++i) pmxIndices[i] = i;
    for (size_t i = 0; i < vmdIndices.size(); ++i) vmdIndices[i] = i;

    auto probePmx = [&](size_t i) {
        PmxProbe probe;
        std::string err;
        if (ProbePmx(walk.pmxFiles[i], probe, &err)) pmxResults[i].probe = std::move(probe);
        else pmxResults[i].error = err;
        if (progress) ++progress->filesVisited;
    };
    auto probeVmd = [&](size_t i) {
        VmdProbe probe;
        std::string err;
        if (ProbeVmd(walk.vmdFiles[i], probe, &err)) vmdResults[i].probe = std::move(probe);
        else vmdResults[i].error = err;
        if (progress) ++progress->filesVisited;
    };
    std::for_each(std::execution::par, pmxIndices.begin(), pmxIndices.end(), probePmx);
    std::for_each(std::execution::par, vmdIndices.begin(), vmdIndices.end(), probeVmd);

    // Failed probes -> warning, in path order.
    {
        std::vector<std::string> failures;
        for (size_t i = 0; i < walk.pmxFiles.size(); ++i) {
            if (!pmxResults[i].probe) {
                failures.push_back("skipped " + RelKey(walk.pmxFiles[i], root) + ": " +
                                   pmxResults[i].error);
            }
        }
        for (size_t i = 0; i < walk.vmdFiles.size(); ++i) {
            if (!vmdResults[i].probe) {
                failures.push_back("skipped " + RelKey(walk.vmdFiles[i], root) + ": " +
                                   vmdResults[i].error);
            }
        }
        std::sort(failures.begin(), failures.end());
        for (auto& f : failures) result.warnings.push_back(std::move(f));
    }

    // ---- 5. PMX de-duplication ----
    struct PmxEntry {
        const std::filesystem::path* path = nullptr;
        const PmxProbe* probe = nullptr;
        std::string rel;
    };
    std::map<std::filesystem::path, std::vector<PmxEntry>> pmxGroups;  // keyed by parent dir
    for (size_t i = 0; i < walk.pmxFiles.size(); ++i) {
        if (!pmxResults[i].probe) continue;
        PmxEntry e;
        e.path = &walk.pmxFiles[i];
        e.probe = &*pmxResults[i].probe;
        e.rel = RelKey(*e.path, root);
        pmxGroups[e.path->parent_path()].push_back(e);
    }
    std::vector<PmxEntry> kept;
    for (auto& [dir, group] : pmxGroups) {
        (void)dir;
        // Mark duplicates within the group.
        std::vector<bool> dropped(group.size(), false);
        for (size_t i = 0; i < group.size(); ++i) {
            if (dropped[i]) continue;
            for (size_t j = i + 1; j < group.size(); ++j) {
                if (dropped[j]) continue;
                std::error_code ecA, ecB;
                const uintmax_t sizeA = std::filesystem::file_size(*group[i].path, ecA);
                const uintmax_t sizeB = std::filesystem::file_size(*group[j].path, ecB);
                if (!ecA && !ecB && sizeA == sizeB &&
                    group[i].probe->name == group[j].probe->name &&
                    group[i].probe->vertexCount == group[j].probe->vertexCount) {
                    // Keep the one with the smallest rel string.
                    if (group[i].rel <= group[j].rel) dropped[j] = true;
                    else {
                        dropped[i] = true;
                        break;
                    }
                }
            }
        }
        for (size_t i = 0; i < group.size(); ++i) {
            if (!dropped[i]) kept.push_back(group[i]);
        }
    }
    std::sort(kept.begin(), kept.end(), [](const PmxEntry& a, const PmxEntry& b) {
        return a.rel < b.rel;
    });

    // ---- 6. classify pmx entries ----
    struct ClassifiedPmx {
        PmxEntry entry;
        bool humanoid = false;
        bool stageHint = false;
    };
    std::vector<ClassifiedPmx> classified;
    classified.reserve(kept.size());
    for (const auto& e : kept) {
        ClassifiedPmx c;
        c.entry = e;
        c.humanoid = IsHumanoid(e.probe->boneNames);
        c.stageHint = IsStageHint(e.rel);
        classified.push_back(c);
    }
    // Map for accessory check: parent dir -> has humanoid pmx.
    std::set<std::filesystem::path> humanoidDirs;
    for (const auto& c : classified) {
        if (c.humanoid) humanoidDirs.insert(c.entry.path->parent_path());
    }

    std::vector<ClassifiedPmx> characterParts;
    std::vector<ClassifiedPmx> stageParts;
    for (auto& c : classified) {
        if (c.humanoid && !c.stageHint) {
            characterParts.push_back(c);
        } else if (!c.stageHint && [&] {
                       // Accessory: a character lives in the same folder or any ancestor below root
                       // (e.g. model/ps_len/Shader/Rim_Len.pmx next to model/ps_len/*.pmx).
                       for (auto d = c.entry.path->parent_path(); d != root && d.has_relative_path() && d != d.parent_path();
                            d = d.parent_path())
                           if (humanoidDirs.count(d)) return true;
                       return false;
                   }()) {
            LOG_INFO("accessory skipped: %s", c.entry.rel.c_str());
        } else {
            stageParts.push_back(c);
        }
    }

    // ---- 7. characters ----
    {
        struct CharacterTemp {
            std::string displayName;
            std::string folderName;
            std::string fileName;
            CharacterAsset asset;
        };
        std::vector<CharacterTemp> temps;
        for (const auto& c : characterParts) {
            CharacterTemp t;
            t.displayName = Trimmed(c.entry.probe->name);
            if (t.displayName.empty()) t.displayName = FileStemUtf8(*c.entry.path);
            t.folderName = DirNameUtf8(*c.entry.path);
            t.fileName = FileNameUtf8(*c.entry.path);
            t.asset.id = c.entry.rel;
            t.asset.displayName = t.displayName;
            t.asset.folderName = t.folderName;
            t.asset.pmxPath = *c.entry.path;
            t.asset.vertexCount = c.entry.probe->vertexCount;
            t.asset.boneCount = c.entry.probe->boneCount;
            t.asset.materialCount = c.entry.probe->materialCount;
            temps.push_back(std::move(t));
        }
        // Disambiguation pass: displayName shared by several -> append folderName.
        for (size_t i = 0; i < temps.size(); ++i) {
            size_t count = 0;
            for (size_t j = 0; j < temps.size(); ++j) {
                if (temps[j].displayName == temps[i].displayName) ++count;
            }
            if (count > 1) {
                temps[i].asset.displayName =
                    temps[i].displayName + " (" + temps[i].folderName + ")";
            } else {
                temps[i].asset.displayName = temps[i].displayName;
            }
        }
        // If still equal -> append [filename].
        for (size_t i = 0; i < temps.size(); ++i) {
            size_t count = 0;
            for (size_t j = 0; j < temps.size(); ++j) {
                if (temps[j].asset.displayName == temps[i].asset.displayName) ++count;
            }
            if (count > 1) {
                temps[i].asset.displayName =
                    temps[i].asset.displayName + " [" + temps[i].fileName + "]";
            }
        }
        for (auto& t : temps) result.characters.push_back(std::move(t.asset));
    }

    // ---- 8. stages ----
    {
        std::map<std::filesystem::path, std::vector<const ClassifiedPmx*>> stageGroups;
        for (const auto& c : stageParts) stageGroups[c.entry.path->parent_path()].push_back(&c);
        for (auto& [dir, group] : stageGroups) {
            StageAsset stage;
            stage.id = RelKey(dir, root);
            stage.displayName = PathToUtf8(dir.filename());
            std::vector<std::pair<std::string, std::filesystem::path>> sortedParts;
            uint32_t vertexSum = 0;
            for (const auto* c : group) {
                sortedParts.emplace_back(c->entry.rel, *c->entry.path);
                vertexSum += c->entry.probe->vertexCount;
            }
            std::sort(sortedParts.begin(), sortedParts.end());
            for (auto& [rel, p] : sortedParts) stage.pmxParts.push_back(p);
            stage.vertexCount = vertexSum;
            result.stages.push_back(std::move(stage));
        }
    }

    // ---- 9. songs ----
    {
        // Audio files by parent dir.
        std::map<std::filesystem::path, std::vector<const std::filesystem::path*>> audioByDir;
        for (const auto& a : walk.audioFiles) audioByDir[a.parent_path()].push_back(&a);

        struct VmdEntry {
            const std::filesystem::path* path = nullptr;
            const VmdProbe* probe = nullptr;
            std::string rel;
        };
        std::map<std::filesystem::path, std::vector<VmdEntry>> vmdGroups;
        for (size_t i = 0; i < walk.vmdFiles.size(); ++i) {
            if (!vmdResults[i].probe) continue;
            VmdEntry e;
            e.path = &walk.vmdFiles[i];
            e.probe = &*vmdResults[i].probe;
            e.rel = RelKey(*e.path, root);
            vmdGroups[e.path->parent_path()].push_back(e);
        }

        for (auto& [dir, group] : vmdGroups) {
            std::vector<VmdEntry> dances, facials, cameras;
            for (const auto& e : group) {
                const VmdProbe& p = *e.probe;
                if (p.cameraKeyCount > 0 && p.boneKeyCount == 0) cameras.push_back(e);
                else if (p.boneKeyCount > 0) dances.push_back(e);
                else if (p.morphKeyCount > 0) facials.push_back(e);
                // otherwise ignored.
            }
            if (dances.empty()) continue;  // not a song

            // ---- audio candidates ----
            std::vector<const std::filesystem::path*> candidates = audioByDir[dir];
            if (candidates.empty()) {
                auto parentIt = audioByDir.find(dir.parent_path());
                if (parentIt != audioByDir.end()) candidates = parentIt->second;
            }
            static const char* kExtOrder[] = {".wav", ".mp3", ".flac", ".ogg"};
            const std::filesystem::path* chosenAudio = nullptr;
            int chosenExtRank = 4;
            uintmax_t chosenSize = 0;
            for (const auto* a : candidates) {
                std::error_code ecSize;
                uintmax_t size = std::filesystem::file_size(*a, ecSize);
                if (ecSize) size = 0;
                const std::string ext = LowerExt(*a);
                int rank = 4;
                for (int i = 0; i < 4; ++i) {
                    if (ext == kExtOrder[i]) {
                        rank = i;
                        break;
                    }
                }
                const bool better = chosenAudio == nullptr || rank < chosenExtRank ||
                                    (rank == chosenExtRank && size > chosenSize);
                if (better) {
                    chosenAudio = a;
                    chosenExtRank = rank;
                    chosenSize = size;
                }
            }
            double audioSec = 0;
            if (chosenAudio && LowerExt(*chosenAudio) == ".wav") {
                audioSec = WavDurationSec(*chosenAudio);
            }

            // ---- primary dance ----
            const VmdEntry* primary = nullptr;
            if (audioSec > 0 && dances.size() >= 2) {
                double bestDiff = 0;
                for (const auto& e : dances) {
                    const double danceSec =
                        static_cast<double>(e.probe->maxFrame) / 30.0;
                    const double diff = danceSec > audioSec ? danceSec - audioSec
                                                            : audioSec - danceSec;
                    if (!primary || diff < bestDiff ||
                        (diff == bestDiff &&
                         e.probe->boneKeyCount > primary->probe->boneKeyCount)) {
                        primary = &e;
                        bestDiff = diff;
                    }
                }
            } else {
                for (const auto& e : dances) {
                    if (!primary || e.probe->boneKeyCount > primary->probe->boneKeyCount) {
                        primary = &e;
                    }
                }
            }

            // ---- other dances ----
            std::vector<std::pair<std::string, std::filesystem::path>> extra;
            for (const auto& e : dances) {
                if (&e == primary) continue;
                if (primary->probe->boneKeyCount > 0 &&
                    static_cast<double>(e.probe->boneKeyCount) <
                        0.1 * static_cast<double>(primary->probe->boneKeyCount)) {
                    extra.emplace_back(e.rel, *e.path);
                } else {
                    LOG_INFO("extra dance ignored: %s", e.rel.c_str());
                }
            }
            for (const auto& e : facials) extra.emplace_back(e.rel, *e.path);
            std::sort(extra.begin(), extra.end());

            // ---- camera ----
            const VmdEntry* chosenCam = nullptr;
            for (const auto& e : cameras) {
                if (!chosenCam || e.probe->cameraKeyCount > chosenCam->probe->cameraKeyCount) {
                    chosenCam = &e;
                }
            }
            if (!chosenCam) {
                // Camera vmds in direct child directories that contain no dance vmd.
                for (auto& [childDir, childGroup] : vmdGroups) {
                    if (childDir.parent_path() != dir) continue;
                    bool childHasDance = false;
                    for (const auto& e : childGroup) {
                        if (e.probe->boneKeyCount > 0) {
                            childHasDance = true;
                            break;
                        }
                    }
                    if (childHasDance) continue;
                    for (const auto& e : childGroup) {
                        if (e.probe->cameraKeyCount == 0) continue;
                        if (!chosenCam ||
                            e.probe->cameraKeyCount > chosenCam->probe->cameraKeyCount) {
                            chosenCam = &e;
                        }
                    }
                }
            }

            // ---- song ----
            SongAsset song;
            song.id = RelKey(dir, root);
            song.displayName = PathToUtf8(dir.filename());
            {
                const std::string lowerName = ToLowerAscii(song.displayName);
                static const char* kGenericNames[] = {
                    "src", "motion", "motions", "vmd", "data"};
                for (const char* generic : kGenericNames) {
                    if (lowerName == generic) {
                        song.displayName = PathToUtf8(dir.parent_path().filename());
                        break;
                    }
                }
            }
            song.danceVmd = *primary->path;
            for (auto& [rel, p] : extra) song.extraVmds.push_back(p);
            if (chosenCam) song.cameraVmd = *chosenCam->path;
            if (chosenAudio) song.audioPath = *chosenAudio;
            song.durationSec = static_cast<float>(primary->probe->maxFrame) / 30.0f;
            result.songs.push_back(std::move(song));
        }
    }

    // ---- 10. sort ----
    auto byLowerName = [](const auto& a, const auto& b) {
        return ToLowerAscii(a.displayName) < ToLowerAscii(b.displayName);
    };
    std::sort(result.characters.begin(), result.characters.end(), byLowerName);
    std::sort(result.stages.begin(), result.stages.end(), byLowerName);
    std::sort(result.songs.begin(), result.songs.end(), byLowerName);
    result.scanSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - timerStart).count();

    // ---- 11. summary ----
    LOG_INFO("library scan: %d characters, %d stages, %d songs, %d warnings in %.2fs",
             static_cast<int>(result.characters.size()),
             static_cast<int>(result.stages.size()),
             static_cast<int>(result.songs.size()),
             static_cast<int>(result.warnings.size()), result.scanSeconds);
    return result;
}

} // namespace mmdx
