#include "asset/AssetLibrary.h"
#include "asset/BinaryReader.h"
#include "asset/MmeEffect.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/StageManifest.h"
#include "asset/VideoReader.h"
#include "asset/VmdMotion.h"
#include "core/AudioProbe.h"
#include "core/Log.h"
#include "core/StorageInfo.h"
#include "core/TextUtil.h"

#include <json.hpp>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <execution>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace mmdx {

namespace {

// Seek-penalty drives (HDDs): one thread reads the files front to back in the given order, so the disk streams
// them instead of seeking between the parallel probes; each probe waits until its file has been read once (it is
// then in the OS file cache) and parses in parallel as before. Files flagged `whole == false` are not read ahead.
class SequentialPrefetch {
public:
    SequentialPrefetch(std::vector<std::filesystem::path> files, std::vector<bool> whole, std::function<bool()> cancelled)
        : files_(std::move(files)), whole_(std::move(whole)), cancelled_(std::move(cancelled)),
          thread_([this] { Run(); }) {}
    ~SequentialPrefetch() { thread_.join(); }
    SequentialPrefetch(const SequentialPrefetch&) = delete;
    SequentialPrefetch& operator=(const SequentialPrefetch&) = delete;

    void WaitFor(size_t i) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return done_ > i; });
    }

private:
    void Run() {
        std::vector<char> buffer(1 << 20);
        for (size_t i = 0; i < files_.size(); ++i) {
            if (cancelled_()) break;
            if (whole_[i]) {
                const HANDLE h = CreateFileW(files_[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD got = 0;
                    while (!cancelled_() && ReadFile(h, buffer.data(), (DWORD)buffer.size(), &got, nullptr) && got) {}
                    CloseHandle(h);
                }
            }
            std::lock_guard lock(mutex_);
            done_ = i + 1;
            cv_.notify_all();
        }
        std::lock_guard lock(mutex_);
        done_ = files_.size();  // cancelled: release every waiter (they check cancelled() themselves)
        cv_.notify_all();
    }

    std::vector<std::filesystem::path> files_;
    std::vector<bool> whole_;
    std::function<bool()> cancelled_;
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t done_ = 0;
    std::thread thread_;  // last: starts after the members above exist
};

enum class Kind { None, Character, Stage, Song, Ignore };

// Version of the separate `.fx` probe cache (cache/fx_probes.json); independent of kProbeCacheVersion / probes.json.
inline constexpr uint32_t kFxCacheVersion = 1;

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

// Humanoid core bones: a VMD keying one of these is a performer motion, never a stage motion.
inline constexpr const char* kCoreBones[] = {
    "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC",  // センター
    "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB",              // 上半身
    "\xE4\xB8\x8B\xE5\x8D\x8A\xE8\xBA\xAB"};             // 下半身

// Folder names / file stems that say "prop" (lower-cased prefix match).
bool NameIsProp(const std::string& raw) {
    const std::string l = ToLowerAscii(raw);
    static const char* kProp[] = {
        "prop", "props", "accessory", "accessories",
        "\xE5\xB0\x8F\xE7\x89\xA9",                                       // 小物
        "\xE5\xB0\x8F\xE9\x81\x93\xE5\x85\xB7",                          // 小道具
        "\xE3\x82\xA2\xE3\x82\xAF\xE3\x82\xBB\xE3\x82\xB5\xE3\x83\xAA",  // アクセサリ
        "\xEC\x86\x8C\xED\x92\x88"};                                      // 소품
    for (const char* p : kProp)
        if (l.rfind(p, 0) == 0) return true;
    return false;
}

// Folder names / file stems that say "stage" (lower-cased substring).
bool NameIsStage(const std::string& raw) {
    const std::string l = ToLowerAscii(raw);
    static const char* kStage[] = {
        "stage",
        "\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xBC\xE3\x82\xB8",  // ステージ
        "\xE8\x88\x9E\xE5\x8F\xB0",                          // 舞台
        "\xEC\x8A\xA4\xED\x85\x8C\xEC\x9D\xB4\xEC\xA7\x80",  // 스테이지
        "\xEB\xAC\xB4\xEB\x8C\x80",                          // 무대
        "\xE5\x9C\xBA\xE6\x99\xAF",                          // 场景
        "\xE5\xA0\xB4\xE6\x99\xAF"};                         // 場景
    for (const char* s : kStage)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

bool PartsPropNamed(const std::vector<std::filesystem::path>& parts) {
    if (parts.empty()) return false;
    return NameIsProp(PathToUtf8(parts[0].parent_path().filename())) || NameIsProp(PathToUtf8(parts[0].stem()));
}

bool PartsStageNamed(const std::vector<std::filesystem::path>& parts) {
    if (parts.empty()) return false;
    return NameIsStage(PathToUtf8(parts[0].parent_path().filename())) || NameIsStage(PathToUtf8(parts[0].stem()));
}

// M8a: does a keyed VMD bone name (truncated to 15 bytes) name one of the model's bones? Names shorter than 15
// bytes must match exactly; a 15-byte name may be a truncated model bone name (prefix match).
bool StageBoneMatches(const std::string& keyed, const std::set<std::string>& modelBones) {
    if (modelBones.count(keyed)) return true;
    if (keyed.size() < 15) return false;
    for (const auto& b : modelBones)
        if (b.size() > keyed.size() && b.compare(0, keyed.size(), keyed) == 0) return true;
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

// Camera choice among several in one folder: plain MMD cameras over ones made for a plugin ("Camera for FOVMOD",
// MME / MMM variants), which need that plugin to look right; "normal" in the name wins.
int CameraNamePreference(const std::string& stem) {
    const std::string l = ToLowerAscii(stem);
    int score = 0;
    if (l.find("normal") != std::string::npos) score += 2;
    for (const char* plugin : {"fovmod", "fov mod", "fov_mod", "mme", "mmm", "for mod", "_mod", " mod"})
        if (l.find(plugin) != std::string::npos) { score -= 3; break; }
    return score;
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

std::string DerivePartLabel(const std::string& stem, const std::string& songOrDir) {
    std::string s = stem;
    size_t dash = s.find(" - ");
    if (dash != std::string::npos) {
        s = s.substr(dash + 3);
    } else {
        static const char* kPrefixes[] = {"motion_", "motion ", "dance_", "dance_"};
        for (const char* pfx : kPrefixes) {
            size_t len = strlen(pfx);
            if (s.size() > len && ToLowerAscii(s.substr(0, len)) == pfx) {
                s = s.substr(len);
                break;
            }
        }
    }
    static const char* kSuffixes[] = {"_motion", " motion", "_dance", " dance"};
    for (const char* sfx : kSuffixes) {
        size_t len = strlen(sfx);
        if (s.size() > len && ToLowerAscii(s.substr(s.size() - len)) == sfx) {
            s = s.substr(0, s.size() - len);
            break;
        }
    }
    size_t b = s.find_first_not_of(" \t\r\n_-.");
    if (b != std::string::npos) {
        size_t e = s.find_last_not_of(" \t\r\n_-.");
        s = s.substr(b, e - b + 1);
    } else {
        s.clear();
    }
    return s.empty() ? stem : s;
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
    double fps = 30.0;   // motion time base: 60 when the file name says so, else 30
};

struct AudioEntry {
    std::filesystem::path path;
    double sec = 0;
};

// M8b: a video file found under a song folder (mp4, mov, wmv, avi, mkv), with its duration from the reader's open.
struct VideoEntry {
    std::filesystem::path path;
    double sec = 0;
};

bool IsGenericStem(const std::string& stem) {
    static const std::set<std::string> kGeneric = {"model", "scene", "untitled", "character", "avatar", "mesh", "export",
                                                   "stage", "main", "default", "body"};
    return kGeneric.count(ToLowerAscii(stem)) > 0;
}

void WriteJsonAtomically(const std::filesystem::path& target, const nlohmann::json& j) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path tmp = target;
    tmp += L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        const std::string text = j.dump(2);
        out.write(text.data(), text.size());
        if (!out) return;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

nlohmann::json ReadJsonFile(const std::filesystem::path& file) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(file, bytes, nullptr)) return {};
    nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) return {};
    return j;
}

} // namespace

bool SaveLibraryScanResult(const LibraryScanResult& result) {
    const std::filesystem::path cacheDir = ExecutableDir() / L"cache";
    const std::filesystem::path resultFile = cacheDir / L"library_result.json";
    nlohmann::json j;
    j["version"] = kLibraryScanResultVersion;
    j["root"] = PathToUtf8(result.root);
    j["scanSeconds"] = result.scanSeconds;
    j["warnings"] = result.warnings;
    j["notes"] = result.notes;

    nlohmann::json chars = nlohmann::json::array();
    for (const auto& c : result.characters) {
        nlohmann::json jc;
        jc["id"] = c.id;
        jc["displayName"] = c.displayName;
        jc["folderName"] = c.folderName;
        jc["format"] = c.format;
        jc["modelPath"] = PathToUtf8(c.modelPath);
        jc["vertexCount"] = c.vertexCount;
        jc["boneCount"] = c.boneCount;
        jc["materialCount"] = c.materialCount;
        chars.push_back(std::move(jc));
    }
    j["characters"] = std::move(chars);

    nlohmann::json stages = nlohmann::json::array();
    for (const auto& s : result.stages) {
        nlohmann::json js;
        js["id"] = s.id;
        js["displayName"] = s.displayName;
        js["format"] = s.format;
        nlohmann::json parts = nlohmann::json::array();
        for (const auto& p : s.parts) parts.push_back(PathToUtf8(p));
        js["parts"] = std::move(parts);
        js["vertexCount"] = s.vertexCount;
        js["manifestPath"] = PathToUtf8(s.manifestPath);
        stages.push_back(std::move(js));
    }
    j["stages"] = std::move(stages);

    nlohmann::json songs = nlohmann::json::array();
    for (const auto& s : result.songs) {
        nlohmann::json js;
        js["id"] = s.id;
        js["displayName"] = s.displayName;
        js["danceVmd"] = PathToUtf8(s.danceVmd);
        nlohmann::json extra = nlohmann::json::array();
        for (const auto& e : s.extraVmds) extra.push_back(PathToUtf8(e));
        js["extraVmds"] = std::move(extra);
        js["cameraVmd"] = PathToUtf8(s.cameraVmd);
        js["audioPath"] = PathToUtf8(s.audioPath);
        js["durationSec"] = s.durationSec;
        js["motionFps"] = s.motionFps;
        nlohmann::json jparts = nlohmann::json::array();
        for (const auto& p : s.parts) {
            nlohmann::json jp;
            jp["label"] = p.label;
            jp["danceVmd"] = PathToUtf8(p.danceVmd);
            nlohmann::json pextra = nlohmann::json::array();
            for (const auto& e : p.extraVmds) pextra.push_back(PathToUtf8(e));
            jp["extraVmds"] = std::move(pextra);
            jparts.push_back(std::move(jp));
        }
        js["parts"] = std::move(jparts);
        js["centerPart"] = s.centerPart;
        nlohmann::json jcams = nlohmann::json::array();
        for (const auto& c : s.cameraVmds) jcams.push_back(PathToUtf8(c));
        js["cameraVmds"] = std::move(jcams);
        js["bundledStageId"] = s.bundledStageId;
        js["stageVmd"] = PathToUtf8(s.stageVmd);
        nlohmann::json jstageVmds = nlohmann::json::array();
        for (const auto& v : s.stageVmds) jstageVmds.push_back(PathToUtf8(v));
        js["stageVmds"] = std::move(jstageVmds);
        nlohmann::json jaudio = nlohmann::json::array();
        for (const auto& a : s.audioPaths) jaudio.push_back(PathToUtf8(a));
        js["audioPaths"] = std::move(jaudio);
        js["overlayVideo"] = PathToUtf8(s.overlayVideo);
        js["backgroundVideo"] = PathToUtf8(s.backgroundVideo);
        songs.push_back(std::move(js));
    }
    j["songs"] = std::move(songs);

    nlohmann::json effects = nlohmann::json::array();
    for (const auto& e : result.effects) {
        nlohmann::json je;
        je["id"] = e.id;
        je["path"] = PathToUtf8(e.path);
        je["displayName"] = e.displayName;
        je["kind"] = e.kind;
        je["readmeImage"] = PathToUtf8(e.readmeImage);
        effects.push_back(std::move(je));
    }
    j["effects"] = std::move(effects);

    WriteJsonAtomically(resultFile, j);
    std::error_code ec;
    const bool ok = std::filesystem::exists(resultFile, ec);
    if (!ok) LOG_WARN("library: scan cache write failed: %s", PathToUtf8(resultFile).c_str());
    return ok;
}

bool LoadLibraryScanResult(const std::filesystem::path& root, LibraryScanResult& out) {
    const std::filesystem::path cacheDir = ExecutableDir() / L"cache";
    const std::filesystem::path resultFile = cacheDir / L"library_result.json";
    nlohmann::json j = ReadJsonFile(resultFile);
    if (!j.is_object()) return false;
    if (j.value("version", 0u) != kLibraryScanResultVersion) return false;
    const std::string rootStr = j.value("root", "");
    if (rootStr.empty()) return false;
    if (Utf8ToPath(rootStr).lexically_normal() != root.lexically_normal()) return false;

    out = LibraryScanResult{};
    out.root = root.lexically_normal();
    out.scanSeconds = j.value("scanSeconds", 0.0);
    if (j.contains("warnings") && j["warnings"].is_array()) {
        for (const auto& w : j["warnings"]) if (w.is_string()) out.warnings.push_back(w.get<std::string>());
    }
    if (j.contains("notes") && j["notes"].is_array()) {
        for (const auto& n : j["notes"]) if (n.is_string()) out.notes.push_back(n.get<std::string>());
    }
    if (j.contains("characters") && j["characters"].is_array()) {
        for (const auto& jc : j["characters"]) {
            if (!jc.is_object()) continue;
            CharacterAsset c;
            c.id = jc.value("id", "");
            c.displayName = jc.value("displayName", "");
            c.folderName = jc.value("folderName", "");
            c.format = jc.value("format", "");
            c.modelPath = Utf8ToPath(jc.value("modelPath", ""));
            c.vertexCount = jc.value("vertexCount", 0u);
            c.boneCount = jc.value("boneCount", 0u);
            c.materialCount = jc.value("materialCount", 0u);
            out.characters.push_back(std::move(c));
        }
    }
    if (j.contains("stages") && j["stages"].is_array()) {
        for (const auto& js : j["stages"]) {
            if (!js.is_object()) continue;
            StageAsset s;
            s.id = js.value("id", "");
            s.displayName = js.value("displayName", "");
            s.format = js.value("format", "");
            if (js.contains("parts") && js["parts"].is_array()) {
                for (const auto& p : js["parts"])
                    if (p.is_string()) s.parts.push_back(Utf8ToPath(p.get<std::string>()));
            }
            s.vertexCount = js.value("vertexCount", 0u);
            s.manifestPath = Utf8ToPath(js.value("manifestPath", ""));
            out.stages.push_back(std::move(s));
        }
    }
    if (j.contains("songs") && j["songs"].is_array()) {
        for (const auto& js : j["songs"]) {
            if (!js.is_object()) continue;
            SongAsset s;
            s.id = js.value("id", "");
            s.displayName = js.value("displayName", "");
            s.danceVmd = Utf8ToPath(js.value("danceVmd", ""));
            if (js.contains("extraVmds") && js["extraVmds"].is_array()) {
                for (const auto& e : js["extraVmds"])
                    if (e.is_string()) s.extraVmds.push_back(Utf8ToPath(e.get<std::string>()));
            }
            s.cameraVmd = Utf8ToPath(js.value("cameraVmd", ""));
            s.audioPath = Utf8ToPath(js.value("audioPath", ""));
            s.durationSec = js.value("durationSec", 0.0f);
            s.motionFps = js.value("motionFps", 30.0f);
            if (js.contains("parts") && js["parts"].is_array()) {
                for (const auto& jp : js["parts"]) {
                    if (!jp.is_object()) continue;
                    SongPart p;
                    p.label = jp.value("label", "");
                    p.danceVmd = Utf8ToPath(jp.value("danceVmd", ""));
                    if (jp.contains("extraVmds") && jp["extraVmds"].is_array()) {
                        for (const auto& e : jp["extraVmds"])
                            if (e.is_string()) p.extraVmds.push_back(Utf8ToPath(e.get<std::string>()));
                    }
                    s.parts.push_back(std::move(p));
                }
            }
            s.centerPart = js.value("centerPart", -1);
            if (js.contains("cameraVmds") && js["cameraVmds"].is_array())
                for (const auto& c : js["cameraVmds"])
                    if (c.is_string()) s.cameraVmds.push_back(Utf8ToPath(c.get<std::string>()));
            s.bundledStageId = js.value("bundledStageId", "");
            s.stageVmd = Utf8ToPath(js.value("stageVmd", ""));
            if (js.contains("stageVmds") && js["stageVmds"].is_array())
                for (const auto& v : js["stageVmds"])
                    if (v.is_string()) s.stageVmds.push_back(Utf8ToPath(v.get<std::string>()));
            if (js.contains("audioPaths") && js["audioPaths"].is_array())
                for (const auto& a : js["audioPaths"])
                    if (a.is_string()) s.audioPaths.push_back(Utf8ToPath(a.get<std::string>()));
            s.overlayVideo = Utf8ToPath(js.value("overlayVideo", ""));
            s.backgroundVideo = Utf8ToPath(js.value("backgroundVideo", ""));
            out.songs.push_back(std::move(s));
        }
    }
    if (j.contains("effects") && j["effects"].is_array()) {
        for (const auto& je : j["effects"]) {
            if (!je.is_object()) continue;
            EffectAsset e;
            e.id = je.value("id", "");
            e.path = Utf8ToPath(je.value("path", ""));
            e.displayName = je.value("displayName", "");
            e.kind = je.value("kind", "");
            e.readmeImage = Utf8ToPath(je.value("readmeImage", ""));
            out.effects.push_back(std::move(e));
        }
    }
    return true;
}

bool LibraryScanResultsEqual(const LibraryScanResult& a, const LibraryScanResult& b) {
    if (a.characters.size() != b.characters.size() ||
        a.stages.size() != b.stages.size() ||
        a.songs.size() != b.songs.size() ||
        a.effects.size() != b.effects.size()) {
        return false;
    }
    for (size_t i = 0; i < a.effects.size(); ++i) {
        if (a.effects[i].id != b.effects[i].id || a.effects[i].kind != b.effects[i].kind ||
            a.effects[i].path != b.effects[i].path)
            return false;
    }
    for (size_t i = 0; i < a.characters.size(); ++i) {
        if (a.characters[i].id != b.characters[i].id ||
            a.characters[i].displayName != b.characters[i].displayName ||
            a.characters[i].modelPath != b.characters[i].modelPath ||
            a.characters[i].vertexCount != b.characters[i].vertexCount ||
            a.characters[i].boneCount != b.characters[i].boneCount ||
            a.characters[i].materialCount != b.characters[i].materialCount)
            return false;
    }
    for (size_t i = 0; i < a.stages.size(); ++i) {
        if (a.stages[i].id != b.stages[i].id ||
            a.stages[i].displayName != b.stages[i].displayName ||
            a.stages[i].parts != b.stages[i].parts ||
            a.stages[i].manifestPath != b.stages[i].manifestPath ||
            a.stages[i].vertexCount != b.stages[i].vertexCount)
            return false;
    }
    for (size_t i = 0; i < a.songs.size(); ++i) {
        if (a.songs[i].id != b.songs[i].id ||
            a.songs[i].displayName != b.songs[i].displayName ||
            a.songs[i].danceVmd != b.songs[i].danceVmd ||
            a.songs[i].cameraVmd != b.songs[i].cameraVmd ||
            a.songs[i].audioPath != b.songs[i].audioPath ||
            a.songs[i].extraVmds != b.songs[i].extraVmds ||
            a.songs[i].bundledStageId != b.songs[i].bundledStageId ||
            a.songs[i].stageVmd != b.songs[i].stageVmd ||
            a.songs[i].stageVmds != b.songs[i].stageVmds ||
            a.songs[i].audioPaths != b.songs[i].audioPaths ||
            a.songs[i].overlayVideo != b.songs[i].overlayVideo ||
            a.songs[i].backgroundVideo != b.songs[i].backgroundVideo ||
            std::fabs(a.songs[i].durationSec - b.songs[i].durationSec) > 0.01f)
            return false;
        if (a.songs[i].parts.size() != b.songs[i].parts.size() || a.songs[i].centerPart != b.songs[i].centerPart ||
            a.songs[i].cameraVmds != b.songs[i].cameraVmds)
            return false;
        for (size_t j = 0; j < a.songs[i].parts.size(); ++j) {
            if (a.songs[i].parts[j].label != b.songs[i].parts[j].label ||
                a.songs[i].parts[j].danceVmd != b.songs[i].parts[j].danceVmd ||
                a.songs[i].parts[j].extraVmds != b.songs[i].parts[j].extraVmds)
                return false;
        }
    }
    return true;
}

LibraryScanResult ScanLibrary(const std::filesystem::path& rootIn, ScanProgress* progress, const LibraryOverrides* overrides) {
    const std::filesystem::path root = rootIn.lexically_normal();
    LibraryScanResult result;
    result.root = root;
    const auto timerStart = std::chrono::steady_clock::now();
    double walkSec = 0, modelSec = 0, vmdSec = 0, audioSec = 0, classifySec = 0, fxSec = 0;

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
    std::vector<std::filesystem::path> vmdFiles, audioFiles, sidecarFiles, videoFiles, fxFiles, stageFiles;
    // Size and mtime of each kept file, cached from the walk's directory_entry (MSVC fills these from
    // FindNextFileW). Keyed by the lexically-normal path; a missing key falls back to path-based calls.
    struct FileStat {
        uintmax_t size = 0;
        int64_t mtime = 0;
        bool ok = false;  // false: the size lookup failed (size is then meaningless)
    };
    std::map<std::filesystem::path, FileStat> walkStats;
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
                const bool keep = fmt != ModelFormat::Unknown || (ext == ".vmd" || ext == ".mvd") || ext == ".fx" ||
                                  ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" ||
                                  ext == ".mp4" || ext == ".mov" || ext == ".wmv" || ext == ".avi" || ext == ".mkv";
                if (keep) {
                    // A symlink / reparse point (or a failed cached read) falls back to path-based calls.
                    std::error_code statEc;
                    const bool symlink = it->is_symlink(statEc);
                    if (!statEc && !symlink) {
                        const uintmax_t sz = it->file_size(statEc);
                        if (!statEc) {
                            const auto lwt = it->last_write_time(statEc);
                            if (!statEc)
                                walkStats[p.lexically_normal()] =
                                    FileStat{sz, static_cast<int64_t>(lwt.time_since_epoch().count()), true};
                        }
                    }
                }
                if (fmt != ModelFormat::Unknown) {
                    ModelEntry m;
                    m.path = p;
                    m.rel = RelKey(p, root);
                    m.format = fmt;
                    models.push_back(std::move(m));
                } else if ((ext == ".vmd" || ext == ".mvd")) vmdFiles.push_back(p);
                else if (ext == ".fx") fxFiles.push_back(p.lexically_normal());
                else if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") audioFiles.push_back(p);
                else if (ext == ".mp4" || ext == ".mov" || ext == ".wmv" || ext == ".avi" || ext == ".mkv")
                    videoFiles.push_back(p);
                else if (ToLowerAscii(FileNameUtf8(p)) == "mmdx.json") sidecarFiles.push_back(p);
                else if (ToLowerAscii(FileNameUtf8(p)) == kStageManifestName) stageFiles.push_back(p.lexically_normal());
            }
            std::error_code incEc;
            it.increment(incEc);
            if (incEc) {
                result.warnings.push_back("walk error: " + incEc.message());
                break;
            }
        }
    }

    // Size + mtime for a walked file: from the walk's directory_entry when available, else the
    // path-based calls (files outside the walk, or the symlink / error fallback). Numbers match the
    // old path-based calls exactly.
    const auto fileStatOf = [&](const std::filesystem::path& p) -> FileStat {
        auto it = walkStats.find(p.lexically_normal());
        if (it != walkStats.end()) return it->second;
        std::error_code ecSz, ecFs;
        const uintmax_t sz = std::filesystem::file_size(p, ecSz);
        const auto lwt = std::filesystem::last_write_time(p, ecFs);
        return FileStat{sz, ecFs ? 0 : static_cast<int64_t>(lwt.time_since_epoch().count()), !ecSz};
    };

    // Sequential I/O on drives that incur a seek penalty (spinning disks); parallel elsewhere.
    const bool seekPenalty = PathIncursSeekPenalty(root);
    const char* ioEnv = std::getenv("MMDX12_SCAN_IO");
    const bool sequentialIo = (ioEnv && std::string(ioEnv) == "seq") ? true
                            : (ioEnv && std::string(ioEnv) == "par") ? false
                            : seekPenalty;
    LOG_INFO("library scan: storage %s -> %s", seekPenalty ? "seek-penalty" : "no seek-penalty/unknown",
             sequentialIo ? "path-ordered read-ahead + parallel probes" : "parallel probes");

    // Runs a probe-index list in parallel. On seek-penalty drives the list is sorted by path (directory locality)
    // and a SequentialPrefetch reads the files ahead in that order; `key` maps an index to its file path,
    // `readsWhole` says whether the probe reads the whole file (else it is not read ahead).
    const auto runProbes = [&](std::vector<size_t>& indices, auto key, auto readsWhole, auto&& body) {
        if (!sequentialIo) {
            std::for_each(std::execution::par, indices.begin(), indices.end(), body);
            return;
        }
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
        std::vector<std::filesystem::path> files;
        std::vector<bool> whole;
        for (size_t i : indices) files.push_back(key(i)), whole.push_back(readsWhole(i));
        SequentialPrefetch prefetch(std::move(files), std::move(whole), [progress] { return progress && progress->cancel.load(); });
        std::vector<size_t> order(indices.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t k) {
            prefetch.WaitFor(k);
            body(indices[k]);
        });
    };
    const auto always = [](size_t) { return true; };

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
    videoFiles.erase(std::remove_if(videoFiles.begin(), videoFiles.end(), isIgnored), videoFiles.end());
    fxFiles.erase(std::remove_if(fxFiles.begin(), fxFiles.end(), isIgnored), fxFiles.end());
    // R1: lower-cased "folder/stem" of every .fx, so a same-stem .x can be spotted without more filesystem calls.
    std::set<std::string> fxStems;
    for (const auto& p : fxFiles) fxStems.insert(ToLowerAscii(PathToUtf8(p.parent_path() / p.stem())));

    // MMDX stages (docs/stage_format.md): a folder holding a stage.json that parses ("format": "mmdx-stage") is ONE
    // stage by content, and its model files are not also characters / props / stages. A stage.json that does not
    // parse is a note and the folder is scanned as before. Semantic problems (unknown binding targets, missing model
    // files) do not hide a stage: tools/stage_check reports them.
    struct MmdxStage {
        std::filesystem::path dir;
        StageManifest manifest;
        std::set<std::filesystem::path> partPaths;   // absolute, lexically normal
    };
    std::vector<MmdxStage> mmdxStages;
    std::sort(stageFiles.begin(), stageFiles.end());
    for (const auto& file : stageFiles) {
        MmdxStage stage;
        std::string error;
        if (!LoadStageManifest(file, stage.manifest, error) || stage.manifest.models.empty()) {
            result.notes.push_back("skipped " + RelKey(file, root) + ": not an MMDX stage (" +
                                   (error.empty() ? std::string("no model parts") : error) + ")");
            continue;
        }
        stage.dir = file.parent_path().lexically_normal();
        for (const auto& part : stage.manifest.models)
            stage.partPaths.insert((stage.dir / Utf8ToPath(part.file)).lexically_normal());
        mmdxStages.push_back(std::move(stage));
    }
    // The nearest MMDX stage folder above a file (nested stage folders: the innermost one owns its files).
    const auto mmdxStageOf = [&](const std::filesystem::path& p) -> const MmdxStage* {
        const MmdxStage* best = nullptr;
        for (const auto& s : mmdxStages)
            if (IsUnder(p, s.dir) && (!best || s.dir.native().size() > best->dir.native().size())) best = &s;
        return best;
    };
    auto isMmdxPart = [&](const std::filesystem::path& p) {
        const MmdxStage* s = mmdxStageOf(p);
        return s && s->partPaths.count(p.lexically_normal()) > 0;
    };


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
    const auto walkEnd = std::chrono::steady_clock::now();
    walkSec = std::chrono::duration<double>(walkEnd - timerStart).count();

    // ---- 3. probe models, vmds and audio in parallel (with persistent probe cache) ----
    const auto cancelled = [progress] { return progress && progress->cancel.load(); };
    const std::filesystem::path cacheDir = ExecutableDir() / L"cache";
    const std::filesystem::path probeCacheFile = cacheDir / L"probes.json";
    nlohmann::json cachedProbesJson = ReadJsonFile(probeCacheFile);
    bool cacheValid = false;
    std::map<std::string, nlohmann::json> probeCacheMap;
    if (cachedProbesJson.is_object() &&
        cachedProbesJson.value("version", 0u) == kProbeCacheVersion &&
        cachedProbesJson.contains("files") && cachedProbesJson["files"].is_object()) {
        cacheValid = true;
        for (auto& [k, v] : cachedProbesJson["files"].items()) {
            if (v.is_object()) probeCacheMap[k] = std::move(v);
        }
    }

    std::vector<size_t> modelsToProbe;
    for (size_t i = 0; i < models.size(); ++i) {
        ModelEntry& m = models[i];
        const FileStat st = fileStatOf(m.path);
        const uintmax_t sz = st.size;
        const int64_t mt = st.mtime;
        bool hit = false;
        if (cacheValid) {
            auto it = probeCacheMap.find(m.rel);
            if (it != probeCacheMap.end()) {
                const auto& ent = it->second;
                if (ent.value("size", uint64_t(0)) == sz && ent.value("mtime", int64_t(0)) == mt) {
                    m.probed = ent.value("probed", false);
                    m.error = ent.value("error", "");
                    m.humanoid = ent.value("humanoid", false);
                    m.vertexCount = ent.value("vertexCount", 0u);
                    m.boneCount = ent.value("boneCount", 0u);
                    m.materialCount = ent.value("materialCount", 0u);
                    m.name = ent.value("name", "");
                    if (ent.contains("pmx") && ent["pmx"].is_object()) {
                        PmxProbe p;
                        const auto& jp = ent["pmx"];
                        p.name = jp.value("name", "");
                        p.nameEn = jp.value("nameEn", "");
                        p.comment = jp.value("comment", "");
                        p.vertexCount = jp.value("vertexCount", 0u);
                        p.faceCount = jp.value("faceCount", 0u);
                        p.textureCount = jp.value("textureCount", 0u);
                        p.materialCount = jp.value("materialCount", 0u);
                        p.boneCount = jp.value("boneCount", 0u);
                        if (jp.contains("boneNames") && jp["boneNames"].is_array()) {
                            for (const auto& bn : jp["boneNames"])
                                if (bn.is_string()) p.boneNames.push_back(bn.get<std::string>());
                        }
                        m.pmx = std::move(p);
                    }
                    if (ent.contains("other") && ent["other"].is_object()) {
                        ModelProbe p;
                        const auto& jo = ent["other"];
                        p.name = jo.value("name", "");
                        p.vertexCount = jo.value("vertexCount", 0u);
                        p.materialCount = jo.value("materialCount", 0u);
                        p.boneCount = jo.value("boneCount", 0u);
                        p.skinned = jo.value("skinned", false);
                        p.morphed = jo.value("morphed", false);
                        p.humanoid = jo.value("humanoid", false);
                        p.humanoidNote = jo.value("humanoidNote", "");
                        p.heightMeters = jo.value("heightMeters", 0.0f);
                        p.extentMeters = jo.value("extentMeters", 0.0f);
                        p.unsupported = jo.value("unsupported", "");
                        m.other = std::move(p);
                    }
                    hit = true;
                }
            }
        }
        if (!hit) modelsToProbe.push_back(i);
        else if (progress) ++progress->filesVisited;
    }

    runProbes(modelsToProbe, [&](size_t idx) { return models[idx].path; }, always, [&](size_t idx) {
        if (cancelled()) return;
        ModelEntry& m = models[idx];
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
    const auto modelEnd = std::chrono::steady_clock::now();
    modelSec = std::chrono::duration<double>(modelEnd - walkEnd).count();

    std::vector<std::optional<VmdProbe>> vmdProbes(vmdFiles.size());
    std::vector<std::string> vmdErrors(vmdFiles.size());
    std::vector<size_t> vmdsToProbe;
    for (size_t i = 0; i < vmdFiles.size(); ++i) {
        const std::string rel = RelKey(vmdFiles[i], root);
        const FileStat st = fileStatOf(vmdFiles[i]);
        const uintmax_t sz = st.size;
        const int64_t mt = st.mtime;
        bool hit = false;
        if (cacheValid) {
            auto it = probeCacheMap.find(rel);
            if (it != probeCacheMap.end()) {
                const auto& ent = it->second;
                if (ent.value("size", uint64_t(0)) == sz && ent.value("mtime", int64_t(0)) == mt) {
                    bool ok = ent.value("ok", false);
                    vmdErrors[i] = ent.value("error", "");
                    if (ok && ent.contains("probe") && ent["probe"].is_object()) {
                        VmdProbe p;
                        const auto& jp = ent["probe"];
                        p.modelName = jp.value("modelName", "");
                        p.boneKeyCount = jp.value("boneKeyCount", 0u);
                        p.morphKeyCount = jp.value("morphKeyCount", 0u);
                        p.cameraKeyCount = jp.value("cameraKeyCount", 0u);
                        p.maxFrame = jp.value("maxFrame", 0u);
                        p.stageStep = jp.value("stageStep", 0u);
                        if (jp.contains("boneNames") && jp["boneNames"].is_array())
                            for (const auto& bn : jp["boneNames"])
                                if (bn.is_string()) p.boneNames.push_back(bn.get<std::string>());
                        if (jp.contains("stageX") && jp["stageX"].is_array())
                            for (const auto& v : jp["stageX"]) p.stageX.push_back(v.get<float>());
                        vmdProbes[i] = std::move(p);
                    }
                    hit = true;
                }
            }
        }
        if (!hit) vmdsToProbe.push_back(i);
        else if (progress) ++progress->filesVisited;
    }
    runProbes(vmdsToProbe, [&](size_t i) { return vmdFiles[i]; }, always, [&](size_t i) {
        if (cancelled()) return;
        VmdProbe probe;
        if (ProbeVmd(vmdFiles[i], probe, &vmdErrors[i])) vmdProbes[i] = std::move(probe);
        if (progress) ++progress->filesVisited;
    });
    const auto vmdEnd = std::chrono::steady_clock::now();
    vmdSec = std::chrono::duration<double>(vmdEnd - modelEnd).count();

    std::vector<AudioEntry> audios(audioFiles.size());
    std::vector<size_t> audiosToProbe;
    for (size_t i = 0; i < audioFiles.size(); ++i) {
        audios[i].path = audioFiles[i];
        const std::string rel = RelKey(audioFiles[i], root);
        const FileStat st = fileStatOf(audioFiles[i]);
        const uintmax_t sz = st.size;
        const int64_t mt = st.mtime;
        bool hit = false;
        if (cacheValid) {
            auto it = probeCacheMap.find(rel);
            if (it != probeCacheMap.end()) {
                const auto& ent = it->second;
                if (ent.value("size", uint64_t(0)) == sz && ent.value("mtime", int64_t(0)) == mt) {
                    audios[i].sec = ent.value("sec", 0.0);
                    hit = true;
                }
            }
        }
        if (!hit) audiosToProbe.push_back(i);
        else if (progress) ++progress->filesVisited;
    }
    // Only MP3 lengths scan the whole file; WAV / FLAC / OGG read their headers.
    runProbes(audiosToProbe, [&](size_t i) { return audioFiles[i]; }, [&](size_t i) { return LowerExt(audioFiles[i]) == ".mp3"; },
              [&](size_t i) {
        if (cancelled()) return;
        audios[i].sec = AudioDurationSec(audioFiles[i]);
        if (progress) ++progress->filesVisited;
    });
    const auto audioEnd = std::chrono::steady_clock::now();
    audioSec = std::chrono::duration<double>(audioEnd - vmdEnd).count();

    // M8b: video durations (header only, from the new reader's open). Cached like the audio.
    std::vector<VideoEntry> videos(videoFiles.size());
    for (size_t i = 0; i < videoFiles.size(); ++i) {
        videos[i].path = videoFiles[i];
        const std::string rel = RelKey(videoFiles[i], root);
        const FileStat st = fileStatOf(videoFiles[i]);
        const uintmax_t sz = st.size;
        const int64_t mt = st.mtime;
        bool hit = false;
        if (cacheValid) {
            auto it = probeCacheMap.find(rel);
            if (it != probeCacheMap.end() && it->second.value("size", uint64_t(0)) == sz &&
                it->second.value("mtime", int64_t(0)) == mt) {
                videos[i].sec = it->second.value("sec", 0.0);
                hit = true;
            }
        }
        if (!hit) {
            VideoReader reader;
            std::string err;
            if (reader.Open(videoFiles[i], &err)) videos[i].sec = reader.Duration();
            else LOG_WARN("library: video probe failed: %s: %s", rel.c_str(), err.c_str());
        }
    }

    // Save updated probe cache atomically (dropping vanished entries) if scan not cancelled
    if (!cancelled()) {
        nlohmann::json newFiles = nlohmann::json::object();
        for (const auto& m : models) {
            const FileStat st = fileStatOf(m.path);
            uintmax_t sz = st.size;
            int64_t mt = st.mtime;
            nlohmann::json ent;
            ent["size"] = sz;
            ent["mtime"] = mt;
            ent["probed"] = m.probed;
            ent["error"] = m.error;
            ent["humanoid"] = m.humanoid;
            ent["vertexCount"] = m.vertexCount;
            ent["boneCount"] = m.boneCount;
            ent["materialCount"] = m.materialCount;
            ent["name"] = m.name;
            if (m.pmx) {
                nlohmann::json jp;
                jp["name"] = m.pmx->name;
                jp["nameEn"] = m.pmx->nameEn;
                jp["comment"] = m.pmx->comment;
                jp["vertexCount"] = m.pmx->vertexCount;
                jp["faceCount"] = m.pmx->faceCount;
                jp["textureCount"] = m.pmx->textureCount;
                jp["materialCount"] = m.pmx->materialCount;
                jp["boneCount"] = m.pmx->boneCount;
                jp["boneNames"] = m.pmx->boneNames;
                ent["pmx"] = std::move(jp);
            }
            if (m.other) {
                nlohmann::json jo;
                jo["name"] = m.other->name;
                jo["vertexCount"] = m.other->vertexCount;
                jo["materialCount"] = m.other->materialCount;
                jo["boneCount"] = m.other->boneCount;
                jo["skinned"] = m.other->skinned;
                jo["morphed"] = m.other->morphed;
                jo["humanoid"] = m.other->humanoid;
                jo["humanoidNote"] = m.other->humanoidNote;
                jo["heightMeters"] = m.other->heightMeters;
                jo["extentMeters"] = m.other->extentMeters;
                jo["unsupported"] = m.other->unsupported;
                ent["other"] = std::move(jo);
            }
            newFiles[m.rel] = std::move(ent);
        }
        for (size_t i = 0; i < vmdFiles.size(); ++i) {
            const std::string rel = RelKey(vmdFiles[i], root);
            const FileStat st = fileStatOf(vmdFiles[i]);
            uintmax_t sz = st.size;
            int64_t mt = st.mtime;
            nlohmann::json ent;
            ent["size"] = sz;
            ent["mtime"] = mt;
            ent["ok"] = vmdProbes[i].has_value();
            ent["error"] = vmdErrors[i];
            if (vmdProbes[i]) {
                nlohmann::json jp;
                jp["modelName"] = vmdProbes[i]->modelName;
                jp["boneKeyCount"] = vmdProbes[i]->boneKeyCount;
                jp["morphKeyCount"] = vmdProbes[i]->morphKeyCount;
                jp["cameraKeyCount"] = vmdProbes[i]->cameraKeyCount;
                jp["maxFrame"] = vmdProbes[i]->maxFrame;
                if (!vmdProbes[i]->boneNames.empty()) {
                    nlohmann::json bn = nlohmann::json::array();
                    for (const auto& n : vmdProbes[i]->boneNames) bn.push_back(n);
                    jp["boneNames"] = std::move(bn);
                }
                if (!vmdProbes[i]->stageX.empty()) {
                    jp["stageStep"] = vmdProbes[i]->stageStep;
                    nlohmann::json xs = nlohmann::json::array();
                    for (float v : vmdProbes[i]->stageX) xs.push_back(std::round(v * 100.0f) / 100.0f);
                    jp["stageX"] = std::move(xs);
                }
                ent["probe"] = std::move(jp);
            }
            newFiles[rel] = std::move(ent);
        }
        for (size_t i = 0; i < audioFiles.size(); ++i) {
            const std::string rel = RelKey(audioFiles[i], root);
            const FileStat st = fileStatOf(audioFiles[i]);
            uintmax_t sz = st.size;
            int64_t mt = st.mtime;
            nlohmann::json ent;
            ent["size"] = sz;
            ent["mtime"] = mt;
            ent["sec"] = audios[i].sec;
            newFiles[rel] = std::move(ent);
        }
        for (size_t i = 0; i < videoFiles.size(); ++i) {
            const std::string rel = RelKey(videoFiles[i], root);
            const FileStat st = fileStatOf(videoFiles[i]);
            uintmax_t sz = st.size;
            int64_t mt = st.mtime;
            nlohmann::json ent;
            ent["size"] = sz;
            ent["mtime"] = mt;
            ent["sec"] = videos[i].sec;
            newFiles[rel] = std::move(ent);
        }
        nlohmann::json rootJ;
        rootJ["version"] = kProbeCacheVersion;
        rootJ["files"] = std::move(newFiles);
        WriteJsonAtomically(probeCacheFile, rootJ);
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
                const FileStat stA = fileStatOf(models[i].path), stB = fileStatOf(models[j].path);
                if (stA.ok && stB.ok && stA.size == stB.size && models[i].pmx->name == models[j].pmx->name &&
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
    // explicit / single-file stage. `forced` = explicit mmdx.json sidecar (never auto-filtered).
    struct NamedStage {
        std::string name;
        std::vector<const ModelEntry*> parts;
        bool forced = false;
    };
    std::vector<NamedStage> namedStages;
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
            namedStages.push_back({name, list, true});
            for (const auto* m : list) used.insert(m);
        }
    }

    // 5b. app overrides, then folder hints and content
    std::set<std::filesystem::path> humanoidDirs;
    for (const auto& m : models)
        if (m.humanoid && IsMmdModelFormat(m.format)) humanoidDirs.insert(m.path.parent_path());
    for (const auto& m : models) {
        if (used.count(&m)) continue;
        // An MMDX stage's model files belong to that stage, never to the character / stage lists (docs/stage_format.md).
        if (isMmdxPart(m.path)) continue;
        const AssetKind ov = overrideOf(m.rel);
        if (ov == AssetKind::Character) {
            characterModels.push_back(&m);
            continue;
        }
        if (ov == AssetKind::Stage) {
            namedStages.push_back({FileStemUtf8(m.path), {&m}, true});
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
        // R1: an MME effect object (.x with a same-stem .fx, or anywhere under an effects/ folder) is never a stage.
        if (m.format == ModelFormat::X) {
            const std::string loRel = ToLowerAscii(m.rel);
            const bool underEffects = loRel.rfind("effects/", 0) == 0 || loRel.find("/effects/") != std::string::npos;
            const bool sameStemFx = fxStems.count(ToLowerAscii(PathToUtf8(m.path.parent_path() / m.path.stem()))) > 0;
            if (underEffects || sameStemFx) {
                result.notes.push_back("skipped " + m.rel + ": MME effect object (.x + .fx)");
                continue;
            }
        }
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
        // R2/R3: props and dummy models are not stages; an explicit sidecar / app override is never filtered.
        auto rejectAuto = [&](const std::string& id, uint32_t vertexCount, const std::vector<std::filesystem::path>& parts) {
            if (vertexCount < 100) {
                result.notes.push_back("skipped " + id + ": too small for a stage (" + std::to_string(vertexCount) + " vertices)");
                return true;
            }
            if (PartsPropNamed(parts)) {
                result.notes.push_back("skipped " + id + ": prop");
                return true;
            }
            return false;
        };
        // MMDX stages: one asset per valid stage.json folder, its declared parts (all files that loaded) sorted.
        for (const auto& st : mmdxStages) {
            StageAsset stage;
            stage.id = RelKey(st.dir, root);
            stage.format = "MMDX";
            stage.manifestPath = st.dir / Utf8ToPath(kStageManifestName);
            const std::string lang = StageLanguage();
            stage.displayName = st.manifest.DisplayName(lang);
            if (stage.displayName.empty()) stage.displayName = PathToUtf8(st.dir.filename());
            std::vector<std::pair<std::string, std::filesystem::path>> sorted;
            uint32_t missing = 0;
            for (const auto& part : st.manifest.models) {
                const std::filesystem::path p = (st.dir / Utf8ToPath(part.file)).lexically_normal();
                const auto it = modelByPath.find(p);
                if (it == modelByPath.end()) {
                    ++missing;
                    continue;
                }
                sorted.emplace_back(it->second->rel, it->second->path);
                stage.vertexCount += it->second->vertexCount;
            }
            std::sort(sorted.begin(), sorted.end());
            for (auto& [rel, p] : sorted) stage.parts.push_back(p);
            if (stage.parts.empty()) {
                // nothing of it loaded: the folder is scanned as before, like an unparsable manifest
                result.notes.push_back("skipped " + stage.id + ": MMDX stage without usable model parts");
                continue;
            }
            if (missing)
                result.notes.push_back(stage.id + ": MMDX stage, " + std::to_string(missing) +
                                       " declared part(s) not loaded (see stage_check)");
            result.stages.push_back(std::move(stage));
        }
        for (auto& [dir, group] : pmxStageGroups) {
            const std::string id = RelKey(dir, root);
            uint32_t totalVerts = 0;
            std::vector<std::filesystem::path> partPaths;
            for (const auto* m : group) totalVerts += m->vertexCount, partPaths.push_back(m->path);
            if (rejectAuto(id, totalVerts, partPaths)) continue;
            addStage(id, PathToUtf8(dir.filename()), group);
        }
        for (auto& [name, parts, forced] : namedStages) {
            std::string display = name;
            if (parts.size() == 1 && IsGenericStem(display)) display = DirNameUtf8(parts[0]->path);
            const std::string id = parts.size() == 1 ? parts[0]->rel : RelKey(parts[0]->path.parent_path(), root) + "#" + name;
            if (!forced) {
                uint32_t totalVerts = 0;
                std::vector<std::filesystem::path> partPaths;
                for (const auto* m : parts) totalVerts += m->vertexCount, partPaths.push_back(m->path);
                if (rejectAuto(id, totalVerts, partPaths)) continue;
            }
            addStage(id, display, parts);
        }
    }

    // ---- 7b. a song folder's sub-folder props are not stages (R4) ----
    {
        // A song folder = a directory that directly contains a probed VMD keying a humanoid core bone.
        // The scan root is never a song folder here: every stage lies under it, so R4 would drop them all.
        std::set<std::filesystem::path> songFolders;
        for (size_t i = 0; i < vmdFiles.size(); ++i) {
            if (!vmdProbes[i]) continue;
            if (vmdFiles[i].parent_path() == root) continue;
            for (const char* c : kCoreBones)
                if (ContainsBoneName(vmdProbes[i]->boneNames, c)) {
                    songFolders.insert(vmdFiles[i].parent_path());
                    break;
                }
        }
        // Ids of stages that came from a sidecar or app override: never dropped here.
        std::set<std::string> forcedStageIds;
        for (const auto& ns : namedStages) {
            if (!ns.forced) continue;
            forcedStageIds.insert(ns.parts.size() == 1 ? ns.parts[0]->rel
                                                       : RelKey(ns.parts[0]->path.parent_path(), root) + "#" + ns.name);
        }
        std::set<std::string> dropIds;
        std::map<std::string, std::string> dropDirRel;
        for (const auto& songDir : songFolders) {
            std::vector<size_t> idx;
            for (size_t i = 0; i < result.stages.size(); ++i) {
                const auto& st = result.stages[i];
                if (st.parts.empty() || forcedStageIds.count(st.id)) continue;
                bool under = true;
                for (const auto& p : st.parts)
                    if (!IsUnder(p, songDir) || p.parent_path() == songDir) { under = false; break; }
                if (under) idx.push_back(i);
            }
            if (idx.size() < 2) continue;
            const bool anyNamed = std::any_of(idx.begin(), idx.end(), [&](size_t i) { return PartsStageNamed(result.stages[i].parts); });
            if (anyNamed) {
                for (size_t i : idx)
                    if (!PartsStageNamed(result.stages[i].parts))
                        dropIds.insert(result.stages[i].id), dropDirRel[result.stages[i].id] = RelKey(songDir, root);
            } else {
                size_t keep = idx[0];
                for (size_t i : idx)
                    if (result.stages[i].vertexCount > result.stages[keep].vertexCount) keep = i;
                for (size_t i : idx)
                    if (i != keep)
                        dropIds.insert(result.stages[i].id), dropDirRel[result.stages[i].id] = RelKey(songDir, root);
            }
        }
        if (!dropIds.empty()) {
            for (auto& st : result.stages)
                if (dropIds.count(st.id))
                    result.notes.push_back("skipped " + st.id + ": prop of song folder " + dropDirRel[st.id]);
            std::vector<StageAsset> kept;
            kept.reserve(result.stages.size());
            for (auto& st : result.stages)
                if (!dropIds.count(st.id)) kept.push_back(std::move(st));
            result.stages = std::move(kept);
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
            // VMD has no fps field: a file named "...60fps..." is authored at 60 frames/s (a key on
            // nearly every frame), everything else at the 30 frames/s MMD default.
            e.fps = ToLowerAscii(FileStemUtf8(vmdFiles[i])).find("60fps") != std::string::npos ? 60.0 : 30.0;
            e.sec = e.probe->maxFrame / e.fps;
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
        std::map<std::filesystem::path, std::vector<const VideoEntry*>> videoByDir;
        for (const auto& v : videos) videoByDir[v.path.parent_path()].push_back(&v);
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
            {
                const auto cam = song.cameraVmd.empty() ? vmdByPath.end() : vmdByPath.find(song.cameraVmd);
                const bool fps60 = d->second->fps == 60.0 || (cam != vmdByPath.end() && cam->second->fps == 60.0);
                song.motionFps = fps60 ? 60.0f : 30.0f;
                if (fps60)
                    LOG_INFO("library: %s uses a 60 fps motion time base (60fps file name)", song.id.c_str());
            }
            if (!s.audio.empty()) {
                if (audioByPath.count(s.audio)) song.audioPath = s.audio, claimedAudio.insert(s.audio);
                else result.warnings.push_back(RelKey(s.dir, root) + "/mmdx.json: audio not found: " + RelKey(s.audio, root));
            }
            if (!song.audioPath.empty()) song.audioPaths.push_back(song.audioPath);
            for (const auto& x : s.extra)
                if (vmdByPath.count(x)) song.extraVmds.push_back(x), claimedVmd.insert(x);
            song.parts.push_back({song.displayName, song.danceVmd, song.extraVmds});
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

        // M8a: a song's bundled stage = a stage asset whose parts all lie in a sub-folder of the song folder
        // (any depth). The song folder itself is not a bundled stage: a stray non-humanoid PMX directly in the
        // folder (e.g. a camera model) must not become the song's default stage.
        // R5: a stage-named candidate wins; ties / none decided by the largest vertexCount.
        auto bundledStageOf = [&](const std::filesystem::path& songDir) -> const StageAsset* {
            // stray VMDs at the library root are not a song package: every stage would be "bundled"
            if (songDir == root) return nullptr;
            const StageAsset* best = nullptr;
            for (const auto& st : result.stages) {
                if (st.parts.empty()) continue;
                bool under = true;
                for (const auto& p : st.parts)
                    if (!IsUnder(p, songDir) || p.parent_path() == songDir) { under = false; break; }
                if (!under) continue;
                if (!best) { best = &st; continue; }
                const bool stNamed = PartsStageNamed(st.parts);
                const bool bestNamed = PartsStageNamed(best->parts);
                if (stNamed != bestNamed) {
                    if (stNamed) best = &st;
                } else if (st.vertexCount > best->vertexCount) {
                    best = &st;
                }
            }
            return best;
        };
        auto stageBoneNamesOf = [&](const StageAsset& st) {
            std::set<std::string> names;
            for (const auto& p : st.parts) {
                auto it = modelByPath.find(p);
                if (it != modelByPath.end() && it->second->pmx)
                    for (const auto& b : it->second->pmx->boneNames) names.insert(b);
            }
            return names;
        };
        // M8a: stage-motion score = matched keyed bones / keyed bones (>= 0.5 wins). A motion that keys a
        // humanoid core bone (センター / 上半身 / 下半身) is a performer, never a stage motion.
        // VMD bone names are truncated to 15 bytes, so a model bone that begins with the keyed name also matches.
        auto stageMotionScore = [](const VmdEntry& e, const std::set<std::string>& stageBones) {
            if (e.probe->boneNames.empty()) return -1.0;
            for (const char* c : kCoreBones)
                if (ContainsBoneName(e.probe->boneNames, c)) return -1.0;
            size_t matched = 0;
            for (const auto& n : e.probe->boneNames)
                if (StageBoneMatches(n, stageBones)) ++matched;
            return (double)matched / (double)e.probe->boneNames.size();
        };

        for (auto& [dir, group] : groups) {
            const StageAsset* bundledStage = bundledStageOf(dir);
            const std::set<std::string> stageBones = bundledStage ? stageBoneNamesOf(*bundledStage)
                                                                  : std::set<std::string>{};
            std::vector<const VmdEntry*> dances, layers, cameras, stageMotions;
            uint32_t maxBoneKeys = 0;
            for (const auto* e : group) maxBoneKeys = std::max(maxBoneKeys, e->probe->boneKeyCount);
            for (const auto* e : group) {
                const VmdProbe& p = *e->probe;
                if (p.cameraKeyCount > 0 && p.boneKeyCount == 0) cameras.push_back(e);
                else if (!stageBones.empty() && stageMotionScore(*e, stageBones) >= 0.5) stageMotions.push_back(e);
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
                // Sort dances in cluster case-insensitively by filename so part order is stable
                std::sort(c.begin(), c.end(), [](const VmdEntry* a, const VmdEntry* b) {
                    return ToLowerAscii(FileNameUtf8(*a->path)) < ToLowerAscii(FileNameUtf8(*b->path));
                });

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

                Draft dr;
                dr.dance = primary;
                dr.song.danceVmd = *primary->path;
                dr.song.durationSec = (float)primary->sec;

                // Group byte-identical copies (same size and same probe counts/maxFrame)
                struct PartGroup {
                    const VmdEntry* kept = nullptr;
                    std::vector<std::string> names;
                };
                std::vector<PartGroup> partGroups;
                for (const auto* d : c) {
                    std::string name = DerivePartLabel(FileStemUtf8(*d->path), genericDirName(dir));
                    const FileStat stA = fileStatOf(*d->path);
                    bool matched = false;
                    for (auto& pg : partGroups) {
                        const FileStat stB = fileStatOf(*pg.kept->path);
                        if (stA.ok && stB.ok && stA.size == stB.size &&
                            d->probe->boneKeyCount == pg.kept->probe->boneKeyCount &&
                            d->probe->morphKeyCount == pg.kept->probe->morphKeyCount &&
                            d->probe->cameraKeyCount == pg.kept->probe->cameraKeyCount &&
                            d->probe->maxFrame == pg.kept->probe->maxFrame) {
                            matched = true;
                            if (d == primary) pg.kept = d;
                            if (std::none_of(pg.names.begin(), pg.names.end(), [&](const std::string& n) {
                                return ToLowerAscii(n) == ToLowerAscii(name);
                            })) {
                                pg.names.push_back(name);
                            }
                            break;
                        }
                    }
                    if (!matched) {
                        partGroups.push_back({d, {name}});
                    }
                }

                for (const auto& pg : partGroups) {
                    std::string label = pg.names[0];
                    for (size_t ni = 1; ni < pg.names.size(); ++ni) {
                        label += " / " + pg.names[ni];
                    }
                    SongPart part;
                    part.label = label;
                    part.danceVmd = *pg.kept->path;
                    dr.song.parts.push_back(std::move(part));
                }

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
            // Layers whose name matches a part's label go to that part, the rest to the primary as today.
            for (const auto* l : layers) {
                Draft* best = nullptr;
                for (auto& d : drafts)
                    if (!best || std::fabs(d.dance->sec - l->sec) < std::fabs(best->dance->sec - l->sec)) best = &d;
                if (!best) continue;

                const auto lTokens = NameTokens(FileStemUtf8(*l->path));
                SongPart* targetPart = nullptr;
                for (auto& p : best->song.parts) {
                    if (NamesMatch(NameTokens(p.label), lTokens)) {
                        targetPart = &p;
                        break;
                    }
                }
                if (!targetPart) {
                    for (auto& p : best->song.parts) {
                        if (p.danceVmd == best->song.danceVmd) {
                            targetPart = &p;
                            break;
                        }
                    }
                }
                if (targetPart) {
                    targetPart->extraVmds.push_back(*l->path);
                } else {
                    best->song.extraVmds.push_back(*l->path);
                }
            }
            for (auto& d : drafts) {
                for (auto& p : d.song.parts) {
                    std::sort(p.extraVmds.begin(), p.extraVmds.end());
                    if (p.danceVmd == d.song.danceVmd) {
                        d.song.extraVmds = p.extraVmds;
                    }
                }
                std::sort(d.song.extraVmds.begin(), d.song.extraVmds.end());
            }

            // Unit songs: order the parts as they stand on stage (left to right seen from the front, MMD +X = viewer's
            // right) and find the center = the part closest to the group's middle in most samples. Positions come
            // from the dances' sampled センター + 全ての親 X (VmdProbe::stageX).
            for (auto& d : drafts) {
                std::vector<SongPart>& parts = d.song.parts;
                if (parts.size() < 2) continue;
                std::vector<const VmdProbe*> probes;
                for (const auto& p : parts) {
                    auto it = vmdByPath.find(p.danceVmd);
                    probes.push_back(it != vmdByPath.end() ? it->second->probe : nullptr);
                }
                size_t samples = SIZE_MAX;
                bool usable = true;
                for (const VmdProbe* pr : probes) {
                    if (!pr || pr->stageX.empty() || pr->stageStep != probes[0]->stageStep) { usable = false; break; }
                    samples = std::min(samples, pr->stageX.size());
                }
                if (!usable || samples == 0) continue;
                std::vector<double> rankSum(parts.size(), 0.0);
                std::vector<int> middleCount(parts.size(), 0);
                std::vector<size_t> order(parts.size());
                for (size_t k = 0; k < samples; ++k) {
                    double mean = 0.0;
                    for (const VmdProbe* pr : probes) mean += pr->stageX[k];
                    mean /= (double)probes.size();
                    size_t mid = 0;
                    for (size_t i = 0; i < probes.size(); ++i)
                        if (std::fabs(probes[i]->stageX[k] - mean) < std::fabs(probes[mid]->stageX[k] - mean)) mid = i;
                    ++middleCount[mid];
                    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return probes[a]->stageX[k] < probes[b]->stageX[k]; });
                    for (size_t r = 0; r < order.size(); ++r) rankSum[order[r]] += (double)r;
                }
                const size_t center = (size_t)(std::max_element(middleCount.begin(), middleCount.end()) - middleCount.begin());
                for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rankSum[a] < rankSum[b]; });
                std::vector<SongPart> sorted;
                for (size_t i : order) {
                    if (i == center) d.song.centerPart = (int)sorted.size();
                    sorted.push_back(std::move(parts[i]));
                }
                parts = std::move(sorted);
            }

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

            // M8a: the song-bundled stage and its stage motion. The stage id goes on every draft of the folder;
            // the stage motion VMD (best score) is assigned to the song of the closest dance length.
            if (bundledStage) {
                for (auto& d : drafts) d.song.bundledStageId = bundledStage->id;
                if (!stageMotions.empty()) {
                    const VmdEntry* best = nullptr;
                    double bestScore = -1.0;
                    for (const auto* e : stageMotions) {
                        const double s = stageMotionScore(*e, stageBones);
                        if (s > bestScore) bestScore = s, best = e;
                    }
                    if (best) {
                        Draft* target = nullptr;
                        for (auto& d : drafts)
                            if (!target || std::fabs(d.dance->sec - best->sec) < std::fabs(target->dance->sec - best->sec))
                                target = &d;
                        if (target) {
                            target->song.stageVmd = *best->path;
                            for (const auto* e : stageMotions) {
                                target->song.stageVmds.push_back(*e->path);
                                result.notes.push_back(RelKey(dir, root) + ": stage motion " + RelKey(*e->path, root) +
                                                       " for stage " + bundledStage->id +
                                                       " (score " + std::to_string(stageMotionScore(*e, stageBones)) + ")");
                            }
                        }
                    }
                }
            }

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
                    // One song: a plain MMD camera before plugin cameras, then the most keys, whatever its length.
                    // Every candidate stays listed (cameraVmds) so the lobby can offer the others.
                    std::vector<const VmdEntry*> cands;
                    for (auto& [p, len] : camItems) cands.push_back(vmdByPath[p]);
                    std::stable_sort(cands.begin(), cands.end(), [](const VmdEntry* a, const VmdEntry* b) {
                        const int pa = CameraNamePreference(FileStemUtf8(*a->path)), pb = CameraNamePreference(FileStemUtf8(*b->path));
                        if (pa != pb) return pa > pb;
                        return a->probe->cameraKeyCount > b->probe->cameraKeyCount;
                    });
                    if (!cands.empty()) drafts[0].song.cameraVmd = *cands[0]->path;
                    for (const auto* c : cands) drafts[0].song.cameraVmds.push_back(*c->path);
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
                        const FileStat st = fileStatOf(p);
                        const uintmax_t size = st.ok ? st.size : 0;
                        const std::tuple<int, int, uintmax_t> key{match ? 0 : 1, rank(p), ~size};
                        if (!best || key < bestKey) best = &p, bestKey = key;
                    }
                    drafts[0].song.audioPath = *best;
                    // M8a: audio versions of one song = own files whose length matches the default within 0.5 s.
                    const double defSec = [&] {
                        for (const auto* a : audioByDir[dir])
                            if (a->path == *best) return a->sec;
                        return 0.0;
                    }();
                    for (const auto* a : audioByDir[dir]) {
                        if (claimedAudio.count(a->path)) continue;
                        if (a->path == *best || (defSec > 0 && std::fabs(a->sec - defSec) <= 0.5))
                            drafts[0].song.audioPaths.push_back(a->path);
                    }
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

            // M8b: the song's videos (docs/mme/mme_design.md §5b). A video whose duration is within +-3 % of the
            // song (or >= 90 % of it) is the overlay; the longest of the rest is the background. One song of the
            // folder (the one whose dance length is closest to the overlay) takes the folder's videos.
            if (!videoByDir[dir].empty()) {
                const SongAsset& song = drafts[0].song;
                const double songSec = drafts[0].dance->sec;
                const VideoEntry* overlay = nullptr;
                const VideoEntry* background = nullptr;
                double overlayDelta = 0, backgroundSec = 0;
                for (const auto* v : videoByDir[dir]) {
                    if (v->sec <= 0.0) continue;
                    const bool overlayLike = DurationsMatch(songSec, v->sec, 0.0, 0.03) || v->sec >= 0.90 * songSec;
                    if (overlayLike) {
                        const double d = std::fabs(v->sec - songSec);
                        if (!overlay || d < overlayDelta) overlay = v, overlayDelta = d;
                    } else if (!background || v->sec > backgroundSec) {
                        background = v, backgroundSec = v->sec;
                    }
                }
                // the draft (of a multi-song folder) whose dance length is closest to the background video length
                auto closestDraft = [&](double sec) -> Draft* {
                    Draft* best = nullptr;
                    for (auto& d : drafts)
                        if (!best || std::fabs(d.dance->sec - sec) < std::fabs(best->dance->sec - sec)) best = &d;
                    return best;
                };
                if (overlay) {
                    Draft* target = closestDraft(overlay->sec);
                    target->song.overlayVideo = overlay->path;
                    result.notes.push_back(RelKey(dir, root) + ": overlay video " + RelKey(overlay->path, root) +
                                           " (" + std::to_string(overlay->sec) + " s, song " +
                                           std::to_string(songSec) + " s)");
                }
                if (background) {
                    Draft* target = closestDraft(background->sec);
                    target->song.backgroundVideo = background->path;
                    result.notes.push_back(RelKey(dir, root) + ": background video " + RelKey(background->path, root) +
                                           " (" + std::to_string(background->sec) + " s)");
                }
            }
            for (auto& d : drafts) {
                if (d.song.audioPath.empty()) result.notes.push_back("no audio found for " + d.song.id);
                // M8a: the chosen audio is always the first (default) version; keep at least one entry.
                if (!d.song.audioPath.empty()) {
                    std::vector<std::filesystem::path> ordered;
                    ordered.push_back(d.song.audioPath);
                    for (const auto& a : d.song.audioPaths)
                        if (a != d.song.audioPath) ordered.push_back(a);
                    d.song.audioPaths = std::move(ordered);
                }
                const auto cam = d.song.cameraVmd.empty() ? vmdByPath.end() : vmdByPath.find(d.song.cameraVmd);
                const bool fps60 = d.dance->fps == 60.0 || (cam != vmdByPath.end() && cam->second->fps == 60.0);
                d.song.motionFps = fps60 ? 60.0f : 30.0f;
                if (fps60)
                    LOG_INFO("library: %s uses a 60 fps motion time base (60fps file name)", d.song.id.c_str());
                result.songs.push_back(std::move(d.song));
            }
        }
    }

    // ---- 8b. effects/ (M6b) ----
    {
        const auto fxTimerStart = std::chrono::steady_clock::now();
        const std::string want = "effects";
        // One effect entry per `.fx` under an `effects/` folder at any depth. Entry points only: a `.fx` any sibling
        // `.fx` of the same folder #includes is a helper, not an entry point (the parser already resolved the
        // include chain). `.fxsub` has a different extension and is never listed.
        std::vector<std::filesystem::path> underEffects;
        for (const auto& p : fxFiles) {
            const std::string lo = ToLowerAscii(RelKey(p, root));
            const bool under = lo.rfind(want + "/", 0) == 0 || lo.find("/" + want + "/") != std::string::npos;
            if (!under) continue;
            if (LowerExt(p) != ".fx") continue;
            underEffects.push_back(p);
        }

        // Parse every candidate once (parallel, with a persistent size+mtime cache): reuse the include chain
        // (helpers) and the kind classification. An include changing can change the kind, so the recorded
        // includes are revalidated too.
        struct FxCandidate {
            std::string kind;
            std::vector<std::filesystem::path> includes;
        };
        const std::filesystem::path fxCacheFile = cacheDir / L"fx_probes.json";
        nlohmann::json cachedFxJson = ReadJsonFile(fxCacheFile);
        bool fxCacheValid = false;
        std::map<std::string, nlohmann::json> fxCacheMap;
        if (cachedFxJson.is_object() && cachedFxJson.value("version", 0u) == kFxCacheVersion &&
            cachedFxJson.contains("files") && cachedFxJson["files"].is_object()) {
            fxCacheValid = true;
            for (auto& [k, v] : cachedFxJson["files"].items()) {
                if (v.is_object()) fxCacheMap[k] = std::move(v);
            }
        }

        std::vector<FxCandidate> candidates(underEffects.size());
        std::vector<size_t> fxToParse;
        int fxCached = 0;
        for (size_t i = 0; i < underEffects.size(); ++i) {
            const std::filesystem::path& p = underEffects[i];
            const std::string rel = RelKey(p, root);
            const FileStat st = fileStatOf(p);
            const uintmax_t sz = st.size;
            const int64_t mt = st.mtime;
            bool hit = false;
            if (fxCacheValid) {
                auto it = fxCacheMap.find(rel);
                if (it != fxCacheMap.end()) {
                    const auto& ent = it->second;
                    if (ent.value("size", uint64_t(0)) == sz && ent.value("mtime", int64_t(0)) == mt &&
                        ent.contains("kind") && ent["kind"].is_string() &&
                        ent.contains("includes") && ent["includes"].is_array()) {
                        bool incOk = true;
                        std::vector<std::filesystem::path> incs;
                        for (const auto& inc : ent["includes"]) {
                            if (!inc.is_object()) { incOk = false; break; }
                            const std::filesystem::path ip = Utf8ToPath(inc.value("path", ""));
                            std::error_code iec;
                            const uintmax_t isz = std::filesystem::file_size(ip, iec);
                            const auto ilwt = std::filesystem::last_write_time(ip, iec);
                            const int64_t imt = iec ? 0 : ilwt.time_since_epoch().count();
                            if (isz != inc.value("size", uint64_t(0)) || imt != inc.value("mtime", int64_t(0))) {
                                incOk = false;
                                break;
                            }
                            incs.push_back(ip);
                        }
                        if (incOk) {
                            candidates[i].kind = ent["kind"].get<std::string>();
                            candidates[i].includes = std::move(incs);
                            hit = true;
                        }
                    }
                }
            }
            if (!hit) fxToParse.push_back(i);
            else ++fxCached;
        }

        runProbes(fxToParse, [&](size_t i) { return underEffects[i]; }, always, [&](size_t i) {
            if (cancelled()) return;
            MmeEffectDesc d = LoadMmeEffect(underEffects[i], MmeLoadOptions{});
            candidates[i].kind = d.kind();
            candidates[i].includes = std::move(d.includes);
        });

        std::set<std::filesystem::path> includedBy;
        for (const auto& c : candidates)
            for (const auto& inc : c.includes) includedBy.insert(inc.lexically_normal());
        for (size_t i = 0; i < underEffects.size(); ++i) {
            const std::filesystem::path& p = underEffects[i];
            if (includedBy.count(p.lexically_normal())) continue;   // a helper of another effect in this folder
            EffectAsset e;
            e.id = RelKey(p, root);
            e.path = p;
            e.displayName = PathToUtf8(p.stem());
            e.kind = candidates[i].kind;
            // a readme image next to the effect
            for (const char* name : {"readme.png", "readme.jpg", "readme.jpeg", "preview.png", "preview.jpg"}) {
                const std::filesystem::path cand = p.parent_path() / Utf8ToPath(name);
                std::error_code ec2;
                if (std::filesystem::exists(cand, ec2)) { e.readmeImage = cand; break; }
            }
            result.effects.push_back(std::move(e));
        }
        fxSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - fxTimerStart).count();
        LOG_INFO("library scan: fx %d files, %d cached, %d parsed in %.3fs", static_cast<int>(underEffects.size()),
                 fxCached, static_cast<int>(fxToParse.size()), fxSec);

        // Save the fx probe cache (dropping vanished entries / stale includes) if the scan was not cancelled.
        if (!cancelled()) {
            nlohmann::json fxFilesJson = nlohmann::json::object();
            for (size_t i = 0; i < underEffects.size(); ++i) {
                const std::filesystem::path& p = underEffects[i];
                const std::string rel = RelKey(p, root);
                const FileStat st = fileStatOf(p);
                uintmax_t sz = st.size;
                int64_t mt = st.mtime;
                nlohmann::json ent;
                ent["size"] = sz;
                ent["mtime"] = mt;
                ent["kind"] = candidates[i].kind;
                nlohmann::json incs = nlohmann::json::array();
                for (const auto& inc : candidates[i].includes) {
                    std::error_code iec;
                    uintmax_t isz = std::filesystem::file_size(inc, iec);
                    int64_t imt = iec ? 0 : std::filesystem::last_write_time(inc, iec).time_since_epoch().count();
                    nlohmann::json ji;
                    ji["path"] = PathToUtf8(inc);
                    ji["size"] = isz;
                    ji["mtime"] = imt;
                    incs.push_back(std::move(ji));
                }
                ent["includes"] = std::move(incs);
                fxFilesJson[rel] = std::move(ent);
            }
            nlohmann::json fxRoot;
            fxRoot["version"] = kFxCacheVersion;
            fxRoot["files"] = std::move(fxFilesJson);
            WriteJsonAtomically(fxCacheFile, fxRoot);
        }
    }

    // ---- 9. sort ----
    auto byLowerName = [](const auto& a, const auto& b) {
        return ToLowerAscii(a.displayName) < ToLowerAscii(b.displayName);
    };
    std::sort(result.characters.begin(), result.characters.end(), byLowerName);
    // Installed MMDX stages (`<exe>/stages`, prefix "stages:") join the stage list: their ids never collide.
    {
        const std::vector<StageAsset> installed = ScanInstalledStages(ExecutableDir() / L"stages");
        result.stages.insert(result.stages.end(), installed.begin(), installed.end());
    }
    std::sort(result.stages.begin(), result.stages.end(), byLowerName);
    std::sort(result.songs.begin(), result.songs.end(), byLowerName);
    std::sort(result.effects.begin(), result.effects.end(), byLowerName);
    result.scanSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - timerStart).count();
    classifySec = std::chrono::duration<double>(std::chrono::steady_clock::now() - audioEnd).count();

    for (const auto& n : result.notes) LOG_INFO("library: %s", n.c_str());
    LOG_INFO("library scan phases: walk=%.3fs, model_probes=%.3fs, vmd_probes=%.3fs, audio_durations=%.3fs, classification=%.3fs, fx=%.3fs (total=%.3fs)",
             walkSec, modelSec, vmdSec, audioSec, classifySec, fxSec, result.scanSeconds);
    LOG_INFO("library scan: %d characters, %d stages, %d songs, %d effects, %d warnings in %.2fs",
             static_cast<int>(result.characters.size()), static_cast<int>(result.stages.size()),
             static_cast<int>(result.songs.size()), static_cast<int>(result.effects.size()),
             static_cast<int>(result.warnings.size()), result.scanSeconds);
    if (!cancelled()) {
        SaveLibraryScanResult(result);
    }
    return result;
}

// MMDX stages installed next to the executable (docs/stage_ecosystem.md §1): <root>/<folder>/stage.json.
std::vector<StageAsset> ScanInstalledStages(const std::filesystem::path& root) {
    std::vector<StageAsset> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return out;
    std::vector<std::filesystem::path> dirs;
    for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        dirs.push_back(it->path().lexically_normal());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const std::filesystem::path& dir : dirs) {
        StageManifest manifest;
        std::string error;
        if (!LoadStageManifest(dir / Utf8ToPath(kStageManifestName), manifest, error) || manifest.models.empty()) continue;
        StageAsset stage;
        stage.id = std::string(kInstalledStageIdPrefix) + PathToUtf8(dir.filename());
        stage.format = "MMDX";
        stage.manifestPath = dir / Utf8ToPath(kStageManifestName);
        stage.displayName = manifest.DisplayName(StageLanguage());
        if (stage.displayName.empty()) stage.displayName = PathToUtf8(dir.filename());
        for (const StagePart& part : manifest.models) {
            const std::filesystem::path p = (dir / Utf8ToPath(part.file)).lexically_normal();
            std::error_code partEc;
            if (!std::filesystem::is_regular_file(p, partEc)) continue;
            PmxProbe probe;
            std::string probeErr;
            if (IsMmdModelFormat(ModelFormatFromPath(p))) {
                if (ModelFormatFromPath(p) == ModelFormat::Pmx ? ProbePmx(p, probe, &probeErr)
                                                               : ProbePmd(p, probe, &probeErr))
                    stage.vertexCount += probe.vertexCount;
            } else {
                ModelProbe mp;
                if (ProbeModelFile(p, mp, &probeErr)) stage.vertexCount += mp.vertexCount;
            }
            stage.parts.push_back(p);
        }
        std::sort(stage.parts.begin(), stage.parts.end());
        if (stage.parts.empty()) continue;
        out.push_back(std::move(stage));
    }
    return out;
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
