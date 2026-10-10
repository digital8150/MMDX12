#include "asset/AssetLibrary.h"
#include "asset/BinaryReader.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "core/AudioProbe.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <execution>
#include <fstream>
#include <map>
#include <optional>
#include <set>

namespace mmdx {

namespace {

enum class Kind { None, Character, Stage, Song, Ignore };

std::string LowerExt(const std::filesystem::path& p) {
    return ToLowerAscii(PathToUtf8(p.extension()));
}

std::string RelKey(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::string rel = PathToUtf8(p.lexically_relative(root));
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    if (rel == ".") rel.clear();
    return rel;
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
    for (const char* leg : kLeftLeg)
        if (ContainsBoneName(boneNames, leg)) return true;
    return false;
}

// Folder names that say what lives below them (compared lower-cased).
Kind KindOfFolderName(const std::string& component) {
    static const std::map<std::string, Kind> kHints = {
        {"characters", Kind::Character}, {"character", Kind::Character}, {"chara", Kind::Character},
        {"charas", Kind::Character}, {"model", Kind::Character}, {"models", Kind::Character},
        {"avatar", Kind::Character}, {"avatars", Kind::Character},
        {"キャラ", Kind::Character}, {"キャラクター", Kind::Character}, {"モデル", Kind::Character},
        {"角色", Kind::Character}, {"人物", Kind::Character}, {"模型", Kind::Character},
        {"캐릭터", Kind::Character}, {"모델", Kind::Character},
        {"stage", Kind::Stage}, {"stages", Kind::Stage}, {"ステージ", Kind::Stage}, {"舞台", Kind::Stage},
        {"场景", Kind::Stage}, {"場景", Kind::Stage}, {"스테이지", Kind::Stage}, {"무대", Kind::Stage},
        {"song", Kind::Song}, {"songs", Kind::Song}, {"motion", Kind::Song}, {"motions", Kind::Song},
        {"dance", Kind::Song}, {"dances", Kind::Song}, {"モーション", Kind::Song}, {"ダンス", Kind::Song},
        {"动作", Kind::Song}, {"舞蹈", Kind::Song}, {"모션", Kind::Song}, {"노래", Kind::Song}, {"댄스", Kind::Song},
    };
    auto it = kHints.find(ToLowerAscii(component));
    return it == kHints.end() ? Kind::None : it->second;
}

std::string Trimmed(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string FileStemUtf8(const std::filesystem::path& p) { return PathToUtf8(p.stem()); }
std::string FileNameUtf8(const std::filesystem::path& p) { return PathToUtf8(p.filename()); }
std::string DirNameUtf8(const std::filesystem::path& p) { return PathToUtf8(p.parent_path().filename()); }

bool IsUnder(const std::filesystem::path& p, const std::filesystem::path& dir) {
    auto pi = p.begin();
    for (auto di = dir.begin(); di != dir.end(); ++di, ++pi)
        if (pi == p.end() || *pi != *di) return false;
    return true;
}

// ---- name matching for songs ----

// Lower-cased name tokens without the words every file of a set shares.
std::vector<std::string> NameTokens(const std::string& name) {
    static const std::set<std::string> kStop = {
        "motion", "motions", "camera", "cam", "audio", "music", "song", "dance", "vmd", "wav", "mp3", "ogg", "flac",
        "for", "normal", "mmd", "main", "the", "ver", "face", "facial", "lip", "src", "data", "full", "short", "and",
        "モーション", "カメラ", "音源", "音楽", "表情", "ダンス"};
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 2 && !kStop.count(cur)) out.push_back(cur);
        cur.clear();
    };
    for (unsigned char c : ToLowerAscii(name)) {
        if (c >= 0x80 || std::isalnum(c)) cur.push_back((char)c);
        else flush();
    }
    flush();
    return out;
}

bool NamesMatch(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const auto& x : a)
        for (const auto& y : b)
            if (x == y || (x.size() >= 4 && y.find(x) != std::string::npos) || (y.size() >= 4 && x.find(y) != std::string::npos))
                return true;
    return false;
}

bool LooksLikeCameraName(const std::string& stem) {
    const std::string l = ToLowerAscii(stem);
    return l.find("cam") != std::string::npos || l.find("\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9") != std::string::npos;  // カメラ
}

// "worldismine_motion" -> "worldismine"; "Motion_Rin" -> "Rin".
std::string SongNameFromStem(const std::string& stem) {
    std::string out, cur;
    auto flush = [&] {
        if (!cur.empty() && !NameTokens(cur).empty()) out += (out.empty() ? "" : " ") + cur;
        cur.clear();
    };
    for (char c : stem) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

bool DurationsMatch(double a, double b, double absTol, double relTol) {
    if (a <= 0 || b <= 0) return false;
    return std::fabs(a - b) <= std::max(absTol, relTol * std::max(a, b));
}

// ---- sidecars ----

struct SidecarEntry {
    Kind kind = Kind::None;
    std::filesystem::path dir;
    std::string name;
    std::vector<std::filesystem::path> models;  // character model / stage parts
    std::filesystem::path dance, camera, audio;
    std::vector<std::filesystem::path> extra;
};

Kind KindFromString(const std::string& s) {
    const std::string l = ToLowerAscii(s);
    if (l == "character") return Kind::Character;
    if (l == "stage") return Kind::Stage;
    if (l == "song") return Kind::Song;
    if (l == "ignore" || l == "hidden" || l == "hide") return Kind::Ignore;
    return Kind::None;
}

void ParseSidecar(const std::filesystem::path& file, std::vector<SidecarEntry>& out, std::vector<std::string>& warnings,
                  const std::filesystem::path& root) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(file, bytes, nullptr)) return;
    nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false, true);
    if (j.is_discarded()) {
        warnings.push_back("unreadable " + RelKey(file, root) + " (JSON syntax)");
        return;
    }
    std::vector<nlohmann::json> entries;
    if (j.is_array()) entries.assign(j.begin(), j.end());
    else if (j.is_object() && j.contains("assets") && j["assets"].is_array()) entries.assign(j["assets"].begin(), j["assets"].end());
    else if (j.is_object()) entries.push_back(j);
    const std::filesystem::path dir = file.parent_path();
    auto pathOf = [&](const nlohmann::json& v) -> std::filesystem::path {
        return v.is_string() ? (dir / Utf8ToPath(v.get<std::string>())).lexically_normal() : std::filesystem::path{};
    };
    for (const auto& e : entries) {
        if (!e.is_object()) continue;
        SidecarEntry s;
        s.dir = dir;
        s.kind = KindFromString(e.value("type", std::string()));
        if (s.kind == Kind::None) {
            warnings.push_back(RelKey(file, root) + ": missing or unknown \"type\" (character, stage, song, ignore)");
            continue;
        }
        s.name = e.value("name", std::string());
        for (const char* key : {"model", "parts"}) {
            if (!e.contains(key)) continue;
            if (e[key].is_array())
                for (const auto& p : e[key]) s.models.push_back(pathOf(p));
            else s.models.push_back(pathOf(e[key]));
        }
        if (e.contains("dance")) s.dance = pathOf(e["dance"]);
        if (e.contains("camera")) s.camera = pathOf(e["camera"]);
        if (e.contains("audio")) s.audio = pathOf(e["audio"]);
        if (e.contains("extra") && e["extra"].is_array())
            for (const auto& p : e["extra"]) s.extra.push_back(pathOf(p));
        out.push_back(std::move(s));
    }
}

struct ModelEntry {
    std::filesystem::path path;
    std::string rel;
    ModelFormat format = ModelFormat::Unknown;
    bool probed = false;
    std::string error;
    // PMX
    std::optional<PmxProbe> pmx;
    // other formats
    std::optional<ModelProbe> other;
    bool humanoid = false;
    uint32_t vertexCount = 0, boneCount = 0, materialCount = 0;
    std::string name;
};

struct VmdEntry {
    const std::filesystem::path* path = nullptr;
    const VmdProbe* probe = nullptr;
    std::string rel;
    double sec = 0;
};

struct AudioEntry {
    std::filesystem::path path;
    double sec = 0;
};

bool IsGenericStem(const std::string& stem) {
    static const std::set<std::string> kGeneric = {"model", "scene", "untitled", "character", "avatar", "mesh", "export",
                                                   "stage", "main", "default", "body"};
    return kGeneric.count(ToLowerAscii(stem)) > 0;
}

} // namespace

LibraryScanResult ScanLibrary(const std::filesystem::path& rootIn, ScanProgress* progress, const LibraryOverrides* overrides) {
    const std::filesystem::path root = rootIn.lexically_normal();
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

    // ---- 1. walk ----
    std::vector<ModelEntry> models;
    std::vector<std::filesystem::path> vmdFiles, audioFiles, sidecarFiles;
    {
        std::error_code walkEc;
        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, walkEc);
        std::filesystem::recursive_directory_iterator end;
        if (walkEc) result.warnings.push_back("cannot enumerate library folder: " + PathToUtf8(root));
        while (!walkEc && it != end) {
            std::error_code entryEc;
            if (it->is_regular_file(entryEc)) {
                const std::filesystem::path& p = it->path();
                const std::string ext = LowerExt(p);
                const ModelFormat fmt = ModelFormatFromPath(p);
                if (fmt != ModelFormat::Unknown) {
                    ModelEntry m;
                    m.path = p;
                    m.rel = RelKey(p, root);
                    m.format = fmt;
                    models.push_back(std::move(m));
                } else if (ext == ".vmd") vmdFiles.push_back(p);
                else if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") audioFiles.push_back(p);
                else if (ToLowerAscii(FileNameUtf8(p)) == "mmdx.json") sidecarFiles.push_back(p);
            }
            std::error_code incEc;
            it.increment(incEc);
            if (incEc) {
                result.warnings.push_back("walk error: " + incEc.message());
                break;
            }
        }
    }

    // ---- 2. sidecars and overrides ----
    std::vector<SidecarEntry> sidecars;
    std::sort(sidecarFiles.begin(), sidecarFiles.end());
    for (const auto& f : sidecarFiles) ParseSidecar(f, sidecars, result.warnings, root);
    std::vector<std::filesystem::path> ignoredDirs;
    std::map<std::filesystem::path, Kind> kindDirs;                  // sidecar type without explicit files
    std::set<std::filesystem::path> claimed;                           // files used by explicit entries
    for (const auto& s : sidecars) {
        if (s.kind == Kind::Ignore && s.models.empty()) ignoredDirs.push_back(s.dir);
        else if (s.models.empty() && s.dance.empty()) kindDirs[s.dir] = s.kind;
    }
    auto overrideOf = [&](const std::string& rel) {
        if (!overrides) return AssetKind::Auto;
        auto it = overrides->byPath.find(rel);
        return it == overrides->byPath.end() ? AssetKind::Auto : it->second;
    };
    auto isIgnored = [&](const std::filesystem::path& p) {
        for (const auto& d : ignoredDirs)
            if (IsUnder(p, d)) return true;
        for (const auto& s : sidecars)
            if (s.kind == Kind::Ignore)
                for (const auto& m : s.models)
                    if (m == p) return true;
        return overrideOf(RelKey(p, root)) == AssetKind::Hidden;
    };
    // Type hint for a path: nearest sidecar folder type, else nearest hinting folder name.
    auto hintOf = [&](const std::filesystem::path& p) {
        for (auto d = p.parent_path(); IsUnder(d, root); d = d.parent_path()) {
            if (auto it = kindDirs.find(d); it != kindDirs.end()) return it->second;
            if (d == root) break;
            const Kind k = KindOfFolderName(PathToUtf8(d.filename()));
            if (k != Kind::None) return k;
            if (d == d.parent_path()) break;
        }
        return Kind::None;
    };
    models.erase(std::remove_if(models.begin(), models.end(), [&](const ModelEntry& m) { return isIgnored(m.path); }), models.end());
    vmdFiles.erase(std::remove_if(vmdFiles.begin(), vmdFiles.end(), isIgnored), vmdFiles.end());
    audioFiles.erase(std::remove_if(audioFiles.begin(), audioFiles.end(), isIgnored), audioFiles.end());

    // Folders that hold PMX files: other model formats there are usually conversions of the same
    // thing (FBX/OBJ exports shipped next to the PMX) and are skipped unless named explicitly.
    std::set<std::filesystem::path> pmxDirs;
    for (const auto& m : models)
        if (m.format == ModelFormat::Pmx) pmxDirs.insert(m.path.parent_path());
    std::set<std::filesystem::path> explicitModels;
    for (const auto& s : sidecars)
        for (const auto& m : s.models) explicitModels.insert(m);
    auto wanted = [&](const ModelEntry& m) {
        if (m.format == ModelFormat::Pmx || explicitModels.count(m.path)) return true;
        if (overrideOf(m.rel) != AssetKind::Auto) return true;
        if (pmxDirs.count(m.path.parent_path())) {
            result.notes.push_back("skipped " + m.rel + ": a PMX in the same folder is used instead");
            return false;
        }
        return true;
    };
    models.erase(std::remove_if(models.begin(), models.end(), [&](const ModelEntry& m) { return !wanted(m); }), models.end());

    if (progress) progress->filesTotal = (int)(models.size() + vmdFiles.size() + audioFiles.size());

    // ---- 3. probe models, vmds and audio in parallel ----
    const auto cancelled = [progress] { return progress && progress->cancel.load(); };
    std::for_each(std::execution::par, models.begin(), models.end(), [&](ModelEntry& m) {
        if (cancelled()) return;   // app exit: skip the remaining (slow) probes
        std::string err;
        if (IsMmdModelFormat(m.format)) {
            PmxProbe probe;
            if (m.format == ModelFormat::Pmx ? ProbePmx(m.path, probe, &err) : ProbePmd(m.path, probe, &err)) {
                m.humanoid = IsHumanoid(probe.boneNames);
                m.vertexCount = probe.vertexCount;
                m.boneCount = probe.boneCount;
                m.materialCount = probe.materialCount;
                m.name = Trimmed(probe.name);
                m.pmx = std::move(probe);
                m.probed = true;
            }
        } else {
            ModelProbe probe;
            if (ProbeModelFile(m.path, probe, &err)) {
                m.humanoid = probe.humanoid;
                m.vertexCount = probe.vertexCount;
                m.boneCount = probe.boneCount;
                m.materialCount = probe.materialCount;
                m.name = Trimmed(probe.name);
                m.other = std::move(probe);
                m.probed = true;
            }
        }
        m.error = err;
        if (progress) ++progress->filesVisited;
    });
    std::vector<std::optional<VmdProbe>> vmdProbes(vmdFiles.size());
    std::vector<std::string> vmdErrors(vmdFiles.size());
    {
        std::vector<size_t> idx(vmdFiles.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t i) {
            if (cancelled()) return;
            VmdProbe probe;
            if (ProbeVmd(vmdFiles[i], probe, &vmdErrors[i])) vmdProbes[i] = std::move(probe);
            if (progress) ++progress->filesVisited;
        });
    }
    std::vector<AudioEntry> audios(audioFiles.size());
    {
        std::vector<size_t> idx(audioFiles.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t i) {
            audios[i].path = audioFiles[i];
            if (cancelled()) return;
            audios[i].sec = AudioDurationSec(audioFiles[i]);
            if (progress) ++progress->filesVisited;
        });
    }

    {
        std::vector<std::string> failures;
        for (const auto& m : models) {
            if (!m.probed) failures.push_back("skipped " + m.rel + ": " + m.error);
            else if (m.other && !m.other->unsupported.empty()) failures.push_back("skipped " + m.rel + ": " + m.other->unsupported);
        }
        for (size_t i = 0; i < vmdFiles.size(); ++i)
            if (!vmdProbes[i]) failures.push_back("skipped " + RelKey(vmdFiles[i], root) + ": " + vmdErrors[i]);
        std::sort(failures.begin(), failures.end());
        for (auto& f : failures) result.warnings.push_back(std::move(f));
    }
    models.erase(std::remove_if(models.begin(), models.end(),
                                [](const ModelEntry& m) { return !m.probed || (m.other && !m.other->unsupported.empty()); }),
                 models.end());
    std::sort(models.begin(), models.end(), [](const ModelEntry& a, const ModelEntry& b) { return a.rel < b.rel; });

    // ---- 4. PMX de-duplication (same folder, same size + name + vertex count) ----
    {
        std::vector<bool> dropped(models.size(), false);
        for (size_t i = 0; i < models.size(); ++i) {
            if (dropped[i] || models[i].format != ModelFormat::Pmx) continue;
            for (size_t j = i + 1; j < models.size(); ++j) {
                if (dropped[j] || models[j].format != ModelFormat::Pmx) continue;
                if (models[i].path.parent_path() != models[j].path.parent_path()) continue;
                std::error_code ecA, ecB;
                const uintmax_t sizeA = std::filesystem::file_size(models[i].path, ecA);
                const uintmax_t sizeB = std::filesystem::file_size(models[j].path, ecB);
                if (!ecA && !ecB && sizeA == sizeB && models[i].pmx->name == models[j].pmx->name &&
                    models[i].vertexCount == models[j].vertexCount)
                    dropped[j] = true;  // sorted by rel: keep the smallest
            }
        }
        size_t w = 0;
        for (size_t i = 0; i < models.size(); ++i)
            if (!dropped[i]) models[w++] = std::move(models[i]);
        models.resize(w);
    }
    std::map<std::filesystem::path, const ModelEntry*> modelByPath;
    for (const auto& m : models) modelByPath[m.path] = &m;

    // ---- 5. classify models ----
    std::vector<const ModelEntry*> characterModels;
    std::map<std::filesystem::path, std::vector<const ModelEntry*>> pmxStageGroups;   // by folder
    std::vector<std::pair<std::string, std::vector<const ModelEntry*>>> namedStages;  // explicit / single-file
    std::set<const ModelEntry*> used;

    // 5a. explicit sidecar entries
    for (const auto& s : sidecars) {
        if (s.kind != Kind::Character && s.kind != Kind::Stage) continue;
        std::vector<const ModelEntry*> list;
        if (!s.models.empty()) {
            for (const auto& p : s.models) {
                auto it = modelByPath.find(p);
                if (it == modelByPath.end()) result.warnings.push_back(RelKey(s.dir, root) + "/mmdx.json: not a usable model: " + RelKey(p, root));
                else list.push_back(it->second);
            }
        } else if (s.kind == Kind::Stage) {
            for (const auto& m : models)
                if (IsUnder(m.path, s.dir) && !used.count(&m)) list.push_back(&m);
        } else {
            continue;  // character folder without a file: handled as a folder hint
        }
        if (s.kind == Kind::Character) {
            for (const auto* m : list)
                if (!used.count(m)) characterModels.push_back(m), used.insert(m);
        } else if (!list.empty()) {
            std::string name = !s.name.empty() ? s.name : PathToUtf8(s.dir.filename());
            namedStages.push_back({name, list});
            for (const auto* m : list) used.insert(m);
        }
    }

    // 5b. app overrides, then folder hints and content
    std::set<std::filesystem::path> humanoidDirs;
    for (const auto& m : models)
        if (m.humanoid && IsMmdModelFormat(m.format)) humanoidDirs.insert(m.path.parent_path());
    for (const auto& m : models) {
        if (used.count(&m)) continue;
        const AssetKind ov = overrideOf(m.rel);
        if (ov == AssetKind::Character) {
            characterModels.push_back(&m);
            continue;
        }
        if (ov == AssetKind::Stage) {
            namedStages.push_back({FileStemUtf8(m.path), {&m}});
            continue;
        }
        const Kind hint = hintOf(m.path);
        if (IsMmdModelFormat(m.format)) {
            if (m.humanoid && hint != Kind::Stage) {
                characterModels.push_back(&m);
                continue;
            }
            bool accessory = hint == Kind::Character;
            // Accessory: a character lives in the same folder or any ancestor below root
            // (e.g. model/ps_len/Shader/Rim_Len.pmx next to model/ps_len/*.pmx).
            if (hint != Kind::Stage)
                for (auto d = m.path.parent_path(); IsUnder(d, root) && d != root && d != d.parent_path(); d = d.parent_path())
                    if (humanoidDirs.count(d)) accessory = true;
            if (accessory) {
                LOG_INFO("accessory skipped: %s", m.rel.c_str());
                continue;
            }
            pmxStageGroups[m.path.parent_path()].push_back(&m);
            continue;
        }
        // glTF / VRM / FBX / OBJ / X
        const ModelProbe& p = *m.other;
        if (m.format == ModelFormat::X && hint != Kind::Stage) {
            // MMD accessories (.x) are props: a stage only when a stages folder says so
            bool nearCharacter = hint == Kind::Character;
            for (auto d = m.path.parent_path(); IsUnder(d, root) && d != root && d != d.parent_path(); d = d.parent_path())
                if (humanoidDirs.count(d)) nearCharacter = true;
            if (nearCharacter || p.extentMeters < 4.0f) {
                result.notes.push_back("skipped " + m.rel + ": accessory (.x), add it in the Studio as a prop");
                continue;
            }
        }
        if (hint == Kind::Stage) {
            namedStages.push_back({FileStemUtf8(m.path), {&m}});
        } else if (m.humanoid) {
            characterModels.push_back(&m);
        } else if (hint == Kind::Character) {
            result.notes.push_back("not used as a character: " + m.rel + " (" + p.humanoidNote + ")");
        } else if (p.skinned || (p.morphed && p.materialCount <= 2)) {
            result.notes.push_back("skipped " + m.rel + ": animated model without a humanoid skeleton (" + p.humanoidNote + ")");
        } else if (p.extentMeters >= 4.0f) {
            namedStages.push_back({FileStemUtf8(m.path), {&m}});
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f m", p.extentMeters);
            result.notes.push_back("skipped " + m.rel + ": small static model (" + buf + "), put it in a stages folder to use it as a stage");
        }
    }

    // ---- 6. characters ----
    {
        struct CharacterTemp {
            std::string displayName, folderName, fileName;
            CharacterAsset asset;
        };
        std::vector<CharacterTemp> temps;
        for (const ModelEntry* m : characterModels) {
            CharacterTemp t;
            t.displayName = m->name;
            if (t.displayName.empty()) t.displayName = FileStemUtf8(m->path);
            if (m->format != ModelFormat::Pmx && m->name.empty() && IsGenericStem(t.displayName)) t.displayName = DirNameUtf8(m->path);
            for (const auto& s : sidecars)
                if (s.kind == Kind::Character && !s.name.empty() && std::find(s.models.begin(), s.models.end(), m->path) != s.models.end())
                    t.displayName = s.name;
            t.folderName = DirNameUtf8(m->path);
            t.fileName = FileNameUtf8(m->path);
            t.asset.id = m->rel;
            t.asset.format = ModelFormatName(m->format);
            t.asset.modelPath = m->path;
            t.asset.vertexCount = m->vertexCount;
            t.asset.boneCount = m->boneCount;
            t.asset.materialCount = m->materialCount;
            temps.push_back(std::move(t));
        }
        // Disambiguation: displayName shared by several -> append folderName, then [filename].
        for (auto& t : temps) {
            const size_t count = std::count_if(temps.begin(), temps.end(), [&](const CharacterTemp& o) { return o.displayName == t.displayName; });
            t.asset.displayName = count > 1 ? t.displayName + " (" + t.folderName + ")" : t.displayName;
        }
        for (auto& t : temps) {
            const size_t count = std::count_if(temps.begin(), temps.end(), [&](const CharacterTemp& o) { return o.asset.displayName == t.asset.displayName; });
            if (count > 1) t.asset.displayName += " [" + t.fileName + "]";
        }
        for (auto& t : temps) result.characters.push_back(std::move(t.asset));
    }

    // ---- 7. stages ----
    {
        auto addStage = [&](std::string id, std::string name, const std::vector<const ModelEntry*>& parts) {
            StageAsset stage;
            stage.id = std::move(id);
            stage.displayName = std::move(name);
            std::vector<std::pair<std::string, std::filesystem::path>> sorted;
            std::set<std::string> formats;
            for (const auto* m : parts) {
                sorted.emplace_back(m->rel, m->path);
                stage.vertexCount += m->vertexCount;
                formats.insert(ModelFormatName(m->format));
            }
            std::sort(sorted.begin(), sorted.end());
            for (auto& [rel, p] : sorted) stage.parts.push_back(p);
            for (const auto& f : formats) stage.format += (stage.format.empty() ? "" : "+") + f;
            result.stages.push_back(std::move(stage));
        };
        for (auto& [dir, group] : pmxStageGroups) addStage(RelKey(dir, root), PathToUtf8(dir.filename()), group);
        for (auto& [name, parts] : namedStages) {
            std::string display = name;
            if (parts.size() == 1 && IsGenericStem(display)) display = DirNameUtf8(parts[0]->path);
            const std::string id = parts.size() == 1 ? parts[0]->rel : RelKey(parts[0]->path.parent_path(), root) + "#" + name;
            addStage(id, display, parts);
        }
    }

    // ---- 8. songs ----
    {
        std::vector<VmdEntry> vmds;
        for (size_t i = 0; i < vmdFiles.size(); ++i) {
            if (!vmdProbes[i]) continue;
            VmdEntry e;
            e.path = &vmdFiles[i];
            e.probe = &*vmdProbes[i];
            e.rel = RelKey(vmdFiles[i], root);
            e.sec = e.probe->maxFrame / 30.0;
            vmds.push_back(e);
        }
        std::map<std::filesystem::path, const VmdEntry*> vmdByPath;
        for (const auto& v : vmds) vmdByPath[*v.path] = &v;
        std::map<std::filesystem::path, const AudioEntry*> audioByPath;
        std::map<std::filesystem::path, std::vector<const AudioEntry*>> audioByDir;
        for (const auto& a : audios) {
            audioByPath[a.path] = &a;
            audioByDir[a.path.parent_path()].push_back(&a);
        }
        std::set<std::filesystem::path> claimedVmd, claimedAudio;

        auto genericDirName = [&](const std::filesystem::path& dir) {
            std::string name = PathToUtf8(dir.filename());
            static const char* kGenericNames[] = {"src", "motion", "motions", "vmd", "data"};
            for (const char* g : kGenericNames)
                if (ToLowerAscii(name) == g) return PathToUtf8(dir.parent_path().filename());
            return name;
        };

        // 8a. explicit songs
        for (const auto& s : sidecars) {
            if (s.kind != Kind::Song || s.dance.empty()) continue;
            auto d = vmdByPath.find(s.dance);
            if (d == vmdByPath.end()) {
                result.warnings.push_back(RelKey(s.dir, root) + "/mmdx.json: dance not found: " + RelKey(s.dance, root));
                continue;
            }
            SongAsset song;
            song.id = RelKey(s.dir, root) + "#" + FileNameUtf8(s.dance);
            song.displayName = !s.name.empty() ? s.name : genericDirName(s.dir);
            song.danceVmd = s.dance;
            song.durationSec = (float)d->second->sec;
            claimedVmd.insert(s.dance);
            if (!s.camera.empty()) {
                if (vmdByPath.count(s.camera)) song.cameraVmd = s.camera, claimedVmd.insert(s.camera);
                else result.warnings.push_back(RelKey(s.dir, root) + "/mmdx.json: camera not found: " + RelKey(s.camera, root));
            }
            if (!s.audio.empty()) {
                if (audioByPath.count(s.audio)) song.audioPath = s.audio, claimedAudio.insert(s.audio);
                else result.warnings.push_back(RelKey(s.dir, root) + "/mmdx.json: audio not found: " + RelKey(s.audio, root));
            }
            for (const auto& x : s.extra)
                if (vmdByPath.count(x)) song.extraVmds.push_back(x), claimedVmd.insert(x);
            result.songs.push_back(std::move(song));
        }

        // 8b. folders. A "song" sidecar without files pools its whole subtree into one folder.
        std::vector<std::filesystem::path> poolDirs;
        for (const auto& [dir, kind] : kindDirs)
            if (kind == Kind::Song) poolDirs.push_back(dir);
        auto groupDirOf = [&](const std::filesystem::path& p) {
            std::filesystem::path best;
            for (const auto& d : poolDirs)
                if (IsUnder(p, d) && d.native().size() > best.native().size()) best = d;
            return best.empty() ? p.parent_path() : best;
        };
        std::map<std::filesystem::path, std::vector<const VmdEntry*>> groups;
        for (const auto& v : vmds)
            if (!claimedVmd.count(*v.path)) groups[groupDirOf(*v.path)].push_back(&v);

        for (auto& [dir, group] : groups) {
            std::vector<const VmdEntry*> dances, layers, cameras;
            uint32_t maxBoneKeys = 0;
            for (const auto* e : group) maxBoneKeys = std::max(maxBoneKeys, e->probe->boneKeyCount);
            for (const auto* e : group) {
                const VmdProbe& p = *e->probe;
                if (p.cameraKeyCount > 0 && p.boneKeyCount == 0) cameras.push_back(e);
                else if (p.boneKeyCount > 0 && p.boneKeyCount >= 0.1 * maxBoneKeys) dances.push_back(e);
                else if (p.boneKeyCount > 0 || p.morphKeyCount > 0) layers.push_back(e);  // facial / lip / eye motions
            }
            if (dances.empty()) continue;  // not a song

            // Songs: dances of about the same length are alternatives of one song (duet parts,
            // a renamed copy); different lengths are different songs, but only when the folder
            // also holds several audio files or cameras to go with them. Otherwise it is one song
            // with a stray or misnamed extra dance.
            std::sort(dances.begin(), dances.end(), [](const VmdEntry* a, const VmdEntry* b) { return a->sec < b->sec; });
            std::vector<std::vector<const VmdEntry*>> clusters;
            for (const auto* d : dances) {
                if (!clusters.empty() && DurationsMatch(clusters.back().front()->sec, d->sec, 3.0, 0.03)) clusters.back().push_back(d);
                else clusters.push_back({d});
            }
            const size_t ownAudioCount = audioByDir[dir].size();
            if (clusters.size() > 1 && ownAudioCount < 2 && cameras.size() < 2) {
                std::vector<const VmdEntry*> merged;
                for (auto& c : clusters) merged.insert(merged.end(), c.begin(), c.end());
                clusters = {merged};
            }
            struct Draft {
                SongAsset song;
                const VmdEntry* dance = nullptr;
                std::vector<std::string> tokens;
            };
            std::vector<Draft> drafts;
            for (auto& c : clusters) {
                // Primary: the dance whose length matches the folder's only audio file; otherwise
                // the most bone keys, where a dance named like a camera only wins when nothing else is there.
                const double audioSec = clusters.size() == 1 && ownAudioCount == 1 ? audioByDir[dir][0]->sec : 0;
                const VmdEntry* primary = nullptr;
                double bestScore = -1e30;
                for (const auto* d : c) {
                    double score = d->probe->boneKeyCount * (LooksLikeCameraName(FileStemUtf8(*d->path)) ? 0.5 : 1.0);
                    if (audioSec > 0 && c.size() > 1) score = -std::fabs(d->sec - audioSec) + score * 1e-9;
                    if (score > bestScore) bestScore = score, primary = d;
                }
                for (const auto* d : c)
                    if (d != primary) LOG_INFO("extra dance ignored: %s", d->rel.c_str());
                Draft dr;
                dr.dance = primary;
                dr.song.danceVmd = *primary->path;
                dr.song.durationSec = (float)primary->sec;
                drafts.push_back(std::move(dr));
            }
            const bool single = drafts.size() == 1;
            // A folder that also holds explicit (mmdx.json) songs names its remaining song by file.
            const bool shared = std::any_of(claimedVmd.begin(), claimedVmd.end(),
                                            [&](const std::filesystem::path& c) { return c.parent_path() == dir; });
            for (size_t i = 0; i < drafts.size(); ++i) {
                Draft& d = drafts[i];
                if (single && !shared) {
                    d.song.id = RelKey(dir, root);
                    d.song.displayName = genericDirName(dir);
                } else {
                    d.song.id = single ? RelKey(dir, root) : RelKey(dir, root) + "#" + FileNameUtf8(*d.dance->path);
                    d.song.displayName = SongNameFromStem(FileStemUtf8(*d.dance->path));
                    if (d.song.displayName.empty()) d.song.displayName = genericDirName(dir) + " " + std::to_string(i + 1);
                }
                d.tokens = NameTokens(FileStemUtf8(*d.dance->path));
                for (auto& t : NameTokens(d.song.displayName)) d.tokens.push_back(t);
                if (single)
                    for (auto& t : NameTokens(genericDirName(dir))) d.tokens.push_back(t);
            }
            if (!single)
                result.notes.push_back(RelKey(dir, root) + ": " + std::to_string(drafts.size()) +
                                       " songs in one folder, matched by length and file names");

            // Layers (facial, lip sync): to the song of the closest length.
            for (const auto* l : layers) {
                Draft* best = nullptr;
                for (auto& d : drafts)
                    if (!best || std::fabs(d.dance->sec - l->sec) < std::fabs(best->dance->sec - l->sec)) best = &d;
                best->song.extraVmds.push_back(*l->path);
            }
            for (auto& d : drafts) std::sort(d.song.extraVmds.begin(), d.song.extraVmds.end());

            // Generic one-to-one matcher: cost = relative length difference, names halve it.
            // A pair is allowed when names match or lengths agree within the tolerance.
            auto assign = [&](std::vector<Draft*> targets, const std::vector<std::pair<std::filesystem::path, double>>& items,
                              double absTol, double relTol, auto&& apply) {
                struct Pair {
                    double cost;
                    size_t draft, item;
                };
                std::vector<Pair> pairs;
                for (size_t di = 0; di < targets.size(); ++di)
                    for (size_t ii = 0; ii < items.size(); ++ii) {
                        const double len = items[ii].second;
                        const bool names = NamesMatch(targets[di]->tokens, NameTokens(FileStemUtf8(items[ii].first)));
                        const bool lengths = DurationsMatch(targets[di]->dance->sec, len, absTol, relTol);
                        if (!names && !lengths) continue;
                        double cost = len > 0 ? std::fabs(targets[di]->dance->sec - len) / std::max(1.0, targets[di]->dance->sec) : 1.0;
                        if (names) cost *= 0.5;
                        pairs.push_back({cost, di, ii});
                    }
                std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
                std::vector<bool> draftDone(targets.size()), itemDone(items.size());
                for (const auto& p : pairs) {
                    if (draftDone[p.draft] || itemDone[p.item]) continue;
                    draftDone[p.draft] = itemDone[p.item] = true;
                    apply(*targets[p.draft], items[p.item].first);
                }
            };
            std::vector<Draft*> all;
            for (auto& d : drafts) all.push_back(&d);

            // Cameras: in the folder; a lone song also takes cameras from child folders without dances.
            {
                std::vector<std::pair<std::filesystem::path, double>> camItems;
                for (const auto* c : cameras) camItems.push_back({*c->path, c->sec});
                if (camItems.empty() && single) {
                    for (auto& [childDir, childGroup] : groups) {
                        if (childDir.parent_path() != dir) continue;
                        if (std::any_of(childGroup.begin(), childGroup.end(), [](const VmdEntry* e) { return e->probe->boneKeyCount > 0; }))
                            continue;
                        for (const auto* e : childGroup)
                            if (e->probe->cameraKeyCount > 0) camItems.push_back({*e->path, e->sec});
                    }
                }
                if (single) {
                    // One song: the camera with the most keys, whatever its length.
                    const VmdEntry* best = nullptr;
                    for (auto& [p, len] : camItems) {
                        const VmdEntry* e = vmdByPath[p];
                        if (!best || e->probe->cameraKeyCount > best->probe->cameraKeyCount) best = e;
                    }
                    if (best) drafts[0].song.cameraVmd = *best->path;
                } else {
                    assign(all, camItems, 10.0, 0.10, [](Draft& d, const std::filesystem::path& p) { d.song.cameraVmd = p; });
                }
            }

            // Audio: the folder's own files first; then the parent folder's loose files, which
            // must match by name or length (a stray wav must not end up under every song).
            {
                std::vector<std::pair<std::filesystem::path, double>> own;
                for (const auto* a : audioByDir[dir])
                    if (!claimedAudio.count(a->path)) own.push_back({a->path, a->sec});
                if (single && !own.empty()) {
                    // One song and its own audio: prefer a name or length match, then wav > mp3 > flac > ogg, then size.
                    static const char* kExtOrder[] = {".wav", ".mp3", ".flac", ".ogg"};
                    auto rank = [&](const std::filesystem::path& p) {
                        const std::string ext = LowerExt(p);
                        for (int i = 0; i < 4; ++i)
                            if (ext == kExtOrder[i]) return i;
                        return 4;
                    };
                    const std::filesystem::path* best = nullptr;
                    std::tuple<int, int, uintmax_t> bestKey{};
                    for (auto& [p, len] : own) {
                        const bool match = NamesMatch(drafts[0].tokens, NameTokens(FileStemUtf8(p))) ||
                                           DurationsMatch(drafts[0].dance->sec, len, 15.0, 0.15);
                        std::error_code ecSize;
                        uintmax_t size = std::filesystem::file_size(p, ecSize);
                        if (ecSize) size = 0;
                        const std::tuple<int, int, uintmax_t> key{match ? 0 : 1, rank(p), ~size};
                        if (!best || key < bestKey) best = &p, bestKey = key;
                    }
                    drafts[0].song.audioPath = *best;
                } else if (!own.empty()) {
                    assign(all, own, 15.0, 0.15, [](Draft& d, const std::filesystem::path& p) { d.song.audioPath = p; });
                }
                std::vector<std::pair<std::filesystem::path, double>> parent;
                if (dir != root)
                    for (const auto* a : audioByDir[dir.parent_path()])
                        if (!claimedAudio.count(a->path)) parent.push_back({a->path, a->sec});
                if (!parent.empty()) {
                    std::vector<Draft*> missing;
                    for (auto& d : drafts)
                        if (d.song.audioPath.empty()) missing.push_back(&d);
                    assign(missing, parent, 15.0, 0.15, [](Draft& d, const std::filesystem::path& p) { d.song.audioPath = p; });
                }
            }
            for (auto& d : drafts) {
                if (d.song.audioPath.empty()) result.notes.push_back("no audio found for " + d.song.id);
                result.songs.push_back(std::move(d.song));
            }
        }
    }

    // ---- 9. sort ----
    auto byLowerName = [](const auto& a, const auto& b) {
        return ToLowerAscii(a.displayName) < ToLowerAscii(b.displayName);
    };
    std::sort(result.characters.begin(), result.characters.end(), byLowerName);
    std::sort(result.stages.begin(), result.stages.end(), byLowerName);
    std::sort(result.songs.begin(), result.songs.end(), byLowerName);
    result.scanSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - timerStart).count();

    for (const auto& n : result.notes) LOG_INFO("library: %s", n.c_str());
    LOG_INFO("library scan: %d characters, %d stages, %d songs, %d warnings in %.2fs",
             static_cast<int>(result.characters.size()), static_cast<int>(result.stages.size()),
             static_cast<int>(result.songs.size()), static_cast<int>(result.warnings.size()), result.scanSeconds);
    return result;
}

bool CreateLibrarySkeleton(const std::filesystem::path& root, const std::filesystem::path& templateDir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(templateDir, ec)) return false;
    if (std::filesystem::exists(root, ec)) {
        if (!std::filesystem::is_directory(root, ec)) return false;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) || ToLowerAscii(FileNameUtf8(it->path())) != "readme.txt") return false;
    }
    bool created = false;
    for (std::filesystem::recursive_directory_iterator it(templateDir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path dst = root / it->path().lexically_relative(templateDir);
        std::error_code e2;
        if (it->is_directory(e2)) created |= std::filesystem::create_directories(dst, e2);
        else {
            std::filesystem::create_directories(dst.parent_path(), e2);
            created |= std::filesystem::copy_file(it->path(), dst, std::filesystem::copy_options::overwrite_existing, e2);
        }
    }
    if (created) LOG_INFO("library skeleton created in %s", PathToUtf8(root).c_str());
    return created;
}

} // namespace mmdx
