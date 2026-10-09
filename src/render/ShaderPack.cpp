#include "render/ShaderPack.h"

#include <Windows.h>

#include <json.hpp>

#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace mmdx {

namespace {

namespace fs = std::filesystem;

constexpr const char* kManifestName = "pack.json";

bool ParseClass(const std::string& s, PackClass& out) {
    static const std::pair<const char*, PackClass> kNames[] = {
        {"body", PackClass::Body}, {"skin", PackClass::Skin}, {"face", PackClass::Face},
        {"eye", PackClass::Eye},   {"hair", PackClass::Hair}, {"weapon", PackClass::Weapon},
    };
    for (const auto& [name, cls] : kNames)
        if (s == name) { out = cls; return true; }
    return false;
}

std::string Str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

float Num(const nlohmann::json& j, const char* key, float def) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : def;
}

// A plain JSON string (stored under "") or an object of language -> string.
LocalizedText Text(const nlohmann::json& j, const char* key) {
    LocalizedText t;
    auto it = j.find(key);
    if (it == j.end()) return t;
    if (it->is_string()) {
        t.byLang[""] = it->get<std::string>();
    } else if (it->is_object()) {
        for (const auto& [lang, v] : it->items())
            if (v.is_string()) t.byLang[lang] = v.get<std::string>();
    }
    return t;
}

bool HttpUrl(const std::string& s) { return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0; }

// ---- small file helpers (all filesystem calls take an error_code, never throw) --------------------------------------

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void FnvString(uint64_t& h, const std::string& s) {
    h = Fnv(h, s.data(), s.size());
    h = Fnv(h, "|", 1);
}

// (relative path, size, last_write_time) of every file in the folder: the pack's file stamp.
uint64_t FolderStamp(const fs::path& dir) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    std::vector<fs::path> files;
    while (!ec && it != end) {
        std::error_code e2;
        if (it->is_regular_file(e2) && !e2) files.push_back(it->path());
        it.increment(e2);
        ec = e2;
    }
    std::sort(files.begin(), files.end());
    uint64_t h = 1469598103934665603ull;
    for (const fs::path& f : files) {
        FnvString(h, PathToUtf8(f.lexically_relative(dir)));
        std::error_code e2;
        const uint64_t size = (uint64_t)fs::file_size(f, e2);
        h = Fnv(h, &size, sizeof(size));
        const auto t = fs::last_write_time(f, e2).time_since_epoch().count();
        h = Fnv(h, &t, sizeof(t));
    }
    return h;
}

// pack.json / *.hlsl / *.hlsli / *.png / *.jpg / preview.* of every pack folder, plus the folder list: cheap, stat only.
// Textures live in subfolders ("textures/x.png"), so this walks the pack recursively.
uint64_t PollStamp(const fs::path roots[2]) {
    uint64_t h = 1469598103934665603ull;
    std::error_code ec;
    for (int r = 0; r < 2; ++r) {
        FnvString(h, PathToUtf8(roots[r]));
        if (!fs::is_directory(roots[r], ec)) {
            FnvString(h, "missing");
            continue;
        }
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(roots[r], ec)) {
            std::error_code e2;
            if (e.is_directory(e2)) dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& d : dirs) {
            FnvString(h, PathToUtf8(d.filename()));
            const std::string rel = PathToUtf8(d);
            std::vector<fs::path> watched;
            std::error_code e2;
            fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, e2), end;
            while (!e2 && it != end) {
                std::error_code e3;
                if (it->is_regular_file(e3) && !e3) {
                    const std::string name = PathToUtf8(it->path().filename());
                    const std::string ext = ToLowerAscii(PathToUtf8(it->path().extension()));
                    if (name == kManifestName || ext == ".hlsl" || ext == ".hlsli" || name == "preview.png" ||
                        name == "preview.jpg" || ext == ".png" || ext == ".jpg" || ext == ".jpeg")
                        watched.push_back(it->path());
                }
                it.increment(e3);
                e2 = e3;
            }
            std::sort(watched.begin(), watched.end());
            for (const fs::path& f : watched) {
                std::error_code e3;
                FnvString(h, PathToUtf8(fs::relative(f, d, e3)));
                const auto t = fs::last_write_time(f, e3).time_since_epoch().count();
                h = Fnv(h, &t, sizeof(t));
            }
        }
    }
    return h;
}

// true when the path itself, or anything inside it, is a symlink or junction (a reparse point).
bool HasReparsePoint(const fs::path& dir) {
    std::error_code ec;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        const DWORD a = GetFileAttributesW(it->path().c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        it.increment(ec);
    }
    return false;
}

// pack.json, *.hlsl, *.hlsli, *.png, *.jpg, *.md, *.txt, LICENSE*
bool AllowedPackFile(const fs::path& f) {
    const std::string name = PathToUtf8(f.filename());
    if (name == kManifestName || name.rfind("LICENSE", 0) == 0) return true;
    const std::string ext = ToLowerAscii(PathToUtf8(f.extension()));
    return ext == ".hlsl" || ext == ".hlsli" || ext == ".png" || ext == ".jpg" || ext == ".md" || ext == ".txt";
}

// The pack folder's files with the registry's limits (200 files, 32 MB). Empty on a problem, with a Korean `error`.
std::vector<fs::path> PackFiles(const fs::path& root, std::string& error) {
    std::vector<fs::path> files;
    uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) {
            it.increment(e2);
            continue;
        }
        if (files.size() == 200) {
            error = Tr("팩 안에 파일이 너무 많습니다 (200개 초과)");
            return {};
        }
        if (!AllowedPackFile(it->path())) {
            error = std::string(Tr("허용되지 않은 파일이 있습니다: ")) + PathToUtf8(it->path().filename());
            return {};
        }
        total += (uint64_t)fs::file_size(it->path(), e2);
        if (e2) {
            error = Tr("팩 안의 파일을 읽을 수 없습니다");
            return {};
        }
        files.push_back(it->path());
        it.increment(e2);
        ec = e2;
    }
    if (total > 32ull << 20) {
        error = Tr("팩이 너무 큽니다 (32MB 초과)");
        return {};
    }
    return files;
}

// ---- manifest --------------------------------------------------------------------------------------------------------

std::string JoinProblems(const std::vector<std::string>& problems) {
    std::string joined;
    for (size_t i = 0; i < problems.size(); ++i) {
        if (i) joined += "; ";
        joined += problems[i];
    }
    return joined;
}

// ---- load a pack folder ----------------------------------------------------------------------------------------------

// Reads a pack folder: pack.json is required (folders without one are not packs). A bad manifest is still returned
// (out.status = InvalidManifest) so the UI can list it with the reason.
bool LoadPackFolder(const fs::path& dir, PackSource source, ShaderPack& out) {
    std::error_code ec;
    if (!fs::is_regular_file(dir / Utf8ToPath(kManifestName), ec)) return false;
    out = {};
    out.dir = dir;
    out.source = source;
    {
        std::ifstream in(dir / Utf8ToPath(kManifestName), std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && json.empty()) return false;
        std::string error;
        ParseShaderPackManifest(json, dir, out, error);
    }
    // online installs carry .mmdx_install.json
    std::ifstream ins(dir / L".mmdx_install.json", std::ios::binary);
    if (ins) {
        try {
            nlohmann::json m = nlohmann::json::parse(ins);
            if (m.is_object()) {
                const auto it = m.find("from");
                if (it != m.end() && it->is_string() && !it->get<std::string>().empty()) {
                    out.source = PackSource::Online;
                    out.installedFrom = it->get<std::string>();
                }
            }
        } catch (const std::exception&) {
            // a broken marker is ignored: the pack still installs as a user pack
        }
    }
    // PACK_HAS_EDGE: the pack opts in to the pack variant of the edge pass (PackEdge in surface.hlsl)
    std::ifstream surf(dir / L"surface.hlsl", std::ios::binary);
    if (surf) {
        std::string line;
        // only a "#define PACK_HAS_EDGE" line counts (a mention in a comment must not opt in)
        while (std::getline(surf, line)) {
            size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos || line[p] != '#') continue;
            p = line.find_first_not_of(" \t", p + 1);
            if (p != std::string::npos && line.compare(p, 6, "define") == 0) {
                p = line.find_first_not_of(" \t", p + 6);
                if (p != std::string::npos && line.compare(p, 13, "PACK_HAS_EDGE") == 0) {
                    out.hasEdge = true;
                    break;
                }
            }
        }
    }
    return true;
}

// A pack texture path: relative to the pack, no "..", no drive / root (the pack is copied and the
// file name ends up in an #include path's sibling folder).
bool ValidPackTexturePath(const std::string& file) {
    if (file.empty() || file.front() == '/' || file.front() == '\\') return false;
    if (file.find(':') != std::string::npos) return false;   // drive letter
    const fs::path p = Utf8ToPath(file);
    for (const auto& part : p)
        if (PathToUtf8(part) == "..") return false;
    return true;
}

} // namespace

// ---- LocalizedText / PackLanguage ------------------------------------------------------------------------------------

const std::string& LocalizedText::Get(const std::string& lang) const {
    static const std::string empty;
    if (!byLang.empty()) {
        for (const char* key : {lang.c_str(), "en", "", "ko"}) {
            const auto it = byLang.find(key);
            if (it != byLang.end()) return it->second;
        }
        return byLang.begin()->second;
    }
    return empty;
}

bool LocalizedText::Empty() const {
    for (const auto& [lang, text] : byLang)
        if (!text.empty()) return false;
    return true;
}

std::string PackLanguage() {
    switch (ActiveLanguage()) {
    case Language::Korean: return "ko";
    case Language::English: return "en";
    case Language::Japanese: return "ja";
    case Language::Chinese: return "zh";
    default: return "en";
    }
}

// ---- ShaderPack -------------------------------------------------------------------------------------------------------

PackClass ShaderPack::Classify(const std::string& name, const std::string& nameEn, const std::string& texture) const {
    for (const Rule& r : rules) {
        for (const std::string& m : r.match)
            if (name.find(m) != std::string::npos || nameEn.find(m) != std::string::npos) return r.cls;
        for (const std::string& t : r.texture)
            if (texture.find(t) != std::string::npos) return r.cls;
    }
    return PackClass::Body;
}

PackParamValues ShaderPack::Resolve(const std::map<std::string, float>& values) const {
    PackParamValues out{};
    for (size_t i = 0; i < params.size(); ++i) {
        const ShaderPackParam& p = params[i];
        auto it = values.find(p.key);
        out[i] = it == values.end() ? p.def : std::clamp(it->second, p.min, p.max);
    }
    return out;
}

// ---- manifest parsing -------------------------------------------------------------------------------------------------

bool ParseShaderPackManifest(const std::string& json, const std::filesystem::path& dir, ShaderPack& out,
                             std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    // reset what the manifest owns (dir / source / installedFrom stay the caller's)
    out.dir = dir;
    out.preview.clear();
    out.version.clear();
    out.apiVersion = 1;
    out.minAppVersion.clear();
    out.name = {};
    out.description = {};
    out.recommendedFor = {};
    out.authors.clear();
    out.license.clear();
    out.homepage.clear();
    out.repository.clear();
    out.tags.clear();
    out.rules.clear();
    out.params.clear();
    out.textures.clear();
    out.hasEdge = false;
    out.hasPtSurface = false;
    out.type = PackType::Surface;
    out.stage = PackEffectStage::Post;
    out.stateFloats = 0;
    out.status = PackStatus::Ready;
    out.statusMessage.clear();

    std::vector<std::string> problems;
    nlohmann::json j;
    bool object = false;
    try {
        j = nlohmann::json::parse(json);
        object = j.is_object();
        if (!object) problems.push_back("pack.json: not a JSON object");
    } catch (const std::exception& e) {
        problems.push_back(std::string("pack.json: ") + e.what());
    }
    if (!object) {
        out.id = PathToUtf8(dir.filename());   // at least the folder name, so the UI can list it
        out.status = PackStatus::InvalidManifest;
        out.statusMessage = JoinProblems(problems);
        error = out.statusMessage;
        return false;
    }

    // id: the manifest's, else the folder name; must be a valid pack id
    const std::string idInManifest = Str(j, "id");
    out.id = !idInManifest.empty() ? idInManifest : PathToUtf8(dir.filename());
    if (!ValidShaderPackId(out.id))
        problems.push_back("id '" + out.id + "' must be 1-64 characters of lower-case a-z, 0-9, _ or -");

    // version: required, digits and dots
    out.version = Str(j, "version");
    if (out.version.empty() || out.version.find_first_of("0123456789") == std::string::npos ||
        out.version.find_first_not_of("0123456789.") != std::string::npos)
        problems.push_back("version is required (digits and dots)");

    out.apiVersion = j.value("apiVersion", 1);
    if (auto it = j.find("apiVersion"); it != j.end() && !it->is_number())
        problems.push_back("apiVersion must be a number");
    out.minAppVersion = Str(j, "minAppVersion");
    if (auto it = j.find("format"); it != j.end() && !it->is_number())
        problems.push_back("format must be a number");

    // type: "surface" (default) or "effect"; an effect pack picks its injection point with "stage"
    const std::string type = ToLowerAscii(Str(j, "type"));
    if (type.empty() || type == "surface") {
        out.type = PackType::Surface;
    } else if (type == "effect") {
        out.type = PackType::Effect;
        const std::string stage = ToLowerAscii(Str(j, "stage"));
        if (stage.empty() || stage == "post")
            out.stage = PackEffectStage::Post;
        else if (stage == "pre-bloom")
            out.stage = PackEffectStage::PreBloom;
        else
            problems.push_back("stage '" + Str(j, "stage") + "' must be \"post\" or \"pre-bloom\"");
    } else {
        problems.push_back("type '" + Str(j, "type") + "' must be \"surface\" or \"effect\"");
    }

    out.name = Text(j, "name");
    if (out.name.Empty()) {
        problems.push_back("name is required");
        out.name.byLang[""] = out.id;   // a display fallback for the listing
    }
    out.description = Text(j, "description");
    out.recommendedFor = Text(j, "recommendedFor");

    // authors: [{name, role, url}]; the old "author": "string" is one author
    if (auto a = j.find("author"); a != j.end() && a->is_string() && !a->get<std::string>().empty())
        out.authors.push_back({a->get<std::string>(), {}, {}});
    if (auto a = j.find("authors"); a != j.end() && a->is_array()) {
        int index = 0;
        for (const auto& x : *a) {
            PackAuthor au;
            if (x.is_object()) {
                au.name = Str(x, "name");
                au.role = Str(x, "role");
                au.url = Str(x, "url");
                if (!HttpUrl(au.url)) au.url.clear();
            } else if (x.is_string()) {
                au.name = x.get<std::string>();
            }
            if (au.name.empty())
                problems.push_back("authors[" + std::to_string(index) + "] needs a name");
            else
                out.authors.push_back(std::move(au));
            ++index;
        }
    }

    out.license = Str(j, "license");
    const std::string homepage = Str(j, "homepage");
    if (HttpUrl(homepage)) out.homepage = homepage;   // non-http(s) URLs are dropped
    const std::string repository = Str(j, "repository");
    if (HttpUrl(repository)) out.repository = repository;

    if (auto t = j.find("tags"); t != j.end() && t->is_array()) {
        for (const auto& x : *t) {
            if (!x.is_string()) {
                problems.push_back("tags must be strings");
                continue;
            }
            if (out.tags.size() == 12) {
                problems.push_back("more than 12 tags");
                break;
            }
            std::string tag = ToLowerAscii(x.get<std::string>());
            if (tag.size() > 24) {
                problems.push_back("tag '" + tag + "' is longer than 24 characters");
                continue;
            }
            if (!tag.empty()) out.tags.push_back(std::move(tag));
        }
    }

    // classes: class body/skin/face/eye/hair with a match array; unknown classes are a problem
    if (auto it = j.find("classes"); it != j.end() && it->is_array()) {
        for (const auto& r : *it) {
            if (!r.is_object()) continue;
            ShaderPack::Rule rule;
            if (!ParseClass(Str(r, "class"), rule.cls)) {
                problems.push_back("unknown class '" + Str(r, "class") + "'");
                continue;
            }
            if (auto m = r.find("match"); m != r.end() && m->is_array())
                for (const auto& s : *m)
                    if (s.is_string() && !s.get<std::string>().empty()) rule.match.push_back(s.get<std::string>());
            if (auto m = r.find("texture"); m != r.end() && m->is_array())
                for (const auto& s : *m)
                    if (s.is_string() && !s.get<std::string>().empty()) rule.texture.push_back(s.get<std::string>());
            if (!rule.match.empty() || !rule.texture.empty()) out.rules.push_back(std::move(rule));
        }
    }

    // params: at most kPackMaxParams, unique non-empty keys, label string or localized object
    if (auto it = j.find("params"); it != j.end() && it->is_array()) {
        for (const auto& p : *it) {
            if (!p.is_object()) {
                problems.push_back("params entries must be objects");
                continue;
            }
            if (out.params.size() == kPackMaxParams) {
                LOG_WARN("shader pack %s: more than %u params, the rest are ignored", out.id.c_str(), kPackMaxParams);
                break;
            }
            ShaderPackParam sp;
            sp.key = Str(p, "key");
            if (sp.key.empty()) {
                problems.push_back("a param key is empty");
                continue;
            }
            sp.label = Text(p, "label");
            if (sp.label.Empty()) sp.label.byLang[""] = sp.key;
            sp.min = Num(p, "min", 0.0f);
            sp.max = Num(p, "max", 1.0f);
            if (sp.max < sp.min) std::swap(sp.min, sp.max);
            sp.def = std::clamp(Num(p, "default", sp.min), sp.min, sp.max);
            bool duplicate = false;
            for (const ShaderPackParam& q : out.params) duplicate |= q.key == sp.key;
            if (duplicate) {
                problems.push_back("duplicate param key '" + sp.key + "'");
                continue;
            }
            out.params.push_back(std::move(sp));
        }
    }

    // textures: at most kPackMaxTextures, optional extra textures the pack's shading samples
    if (auto it = j.find("textures"); it != j.end()) {
        if (!it->is_array())
            problems.push_back("textures must be an array");
        else
            for (const auto& x : *it) {
                if (!x.is_object()) {
                    problems.push_back("textures entries must be objects");
                    continue;
                }
                if (out.textures.size() == kPackMaxTextures) {
                    LOG_WARN("shader pack %s: more than %u textures, the rest are ignored", out.id.c_str(),
                             kPackMaxTextures);
                    break;
                }
                PackTexture t;
                t.file = Str(x, "file");
                if (t.file.empty()) {
                    problems.push_back("a texture entry has no file");
                    continue;
                }
                const std::string address = ToLowerAscii(Str(x, "address"));
                if (address.empty() || address == "wrap")
                    t.clamp = false;
                else if (address == "clamp")
                    t.clamp = true;
                else {
                    problems.push_back("texture '" + t.file + "': address must be wrap or clamp");
                    continue;
                }
                if (auto s = x.find("srgb"); s != x.end() && s->is_boolean()) t.srgb = s->get<bool>();
                const std::string ext = ToLowerAscii(PathToUtf8(fs::path(Utf8ToPath(t.file)).extension()));
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") {
                    problems.push_back("texture '" + t.file + "' must be a png / jpg / jpeg file");
                    continue;
                }
                if (!ValidPackTexturePath(t.file)) {
                    problems.push_back("texture '" + t.file + "' must be a path inside the pack (no '..', no drive)");
                    continue;
                }
                out.textures.push_back(std::move(t));
            }
    }

    // state: optional "state": { "floats": N } (1..16), effect packs only, apiVersion must be 4
    if (auto it = j.find("state"); it != j.end()) {
        if (!it->is_object()) {
            problems.push_back("state must be an object with \"floats\": N");
        } else {
            auto fit = it->find("floats");
            if (fit == it->end() || !fit->is_number_integer()) {
                problems.push_back("state.floats must be an integer between 1 and 16");
            } else {
                int64_t n = fit->get<int64_t>();
                if (n < 1 || n > 16) {
                    problems.push_back("state.floats must be between 1 and 16 (" + std::to_string(n) + ")");
                } else {
                    out.stateFloats = static_cast<uint32_t>(n);
                }
            }
        }
        if (out.apiVersion != 4) {
            problems.push_back("packs with 'state' must declare \"apiVersion\": 4");
        }
        if (out.type != PackType::Effect) {
            problems.push_back("state is only supported for effect packs");
        }
    }

    // optional preview
    std::error_code ec;
    const fs::path previewPng = dir / L"preview.png";
    const fs::path previewJpg = dir / L"preview.jpg";
    if (fs::is_regular_file(previewPng, ec))
        out.preview = previewPng;
    else if (fs::is_regular_file(previewJpg, ec))
        out.preview = previewJpg;

    // the pack's shader file must match its type (an effect pack implements PackEffect in effect.hlsl)
    if (out.type == PackType::Effect) {
        if (!fs::is_regular_file(dir / L"effect.hlsl", ec)) problems.push_back("effect packs need effect.hlsl");
    } else if (!fs::is_regular_file(dir / L"surface.hlsl", ec)) {
        problems.push_back("surface.hlsl is missing");
    } else {
        out.hasPtSurface = fs::is_regular_file(dir / L"pt_surface.hlsl", ec);
    }

    if (!problems.empty()) {
        out.status = PackStatus::InvalidManifest;
        out.statusMessage = JoinProblems(problems);
        error = out.statusMessage;
        return false;
    }
    if (out.apiVersion > kPackApiVersion) {
        out.status = PackStatus::Incompatible;
        out.statusMessage = "pack apiVersion " + std::to_string(out.apiVersion) +
                            " is newer than this build's " + std::to_string(kPackApiVersion);
        error = out.statusMessage;
        return false;
    }
    if (!out.minAppVersion.empty() && net::CompareVersions(out.minAppVersion, MMDX12_VERSION) > 0) {
        out.status = PackStatus::Incompatible;
        out.statusMessage =
            "requires MMDX12 " + out.minAppVersion + " or newer (this build is " + MMDX12_VERSION + ")";
        error = out.statusMessage;
        return false;
    }
    return true;
}

// ---- registry ---------------------------------------------------------------------------------------------------------

std::filesystem::path ShaderPackRegistry::BuiltInRoot() const { return ExecutableDir() / L"shaders" / L"packs"; }

std::filesystem::path ShaderPackRegistry::UserRoot() const { return ExecutableDir() / L"shader_packs"; }

void ShaderPackRegistry::Scan() {
    const fs::path roots[2] = {BuiltInRoot(), UserRoot()};
    std::vector<ShaderPack> packs;
    const auto findIn = [&packs](const std::string& id) -> bool {
        for (const ShaderPack& p : packs)
            if (p.id == id) return true;
        return false;
    };
    for (int r = 0; r < 2; ++r) {
        std::error_code ec;
        if (!fs::is_directory(roots[r], ec)) continue;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(roots[r], ec)) {
            std::error_code e2;
            if (e.is_directory(e2)) dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& dir : dirs) {
            ShaderPack pack;
            if (!LoadPackFolder(dir, r == 0 ? PackSource::BuiltIn : PackSource::User, pack)) continue;
            if (findIn(pack.id)) {
                pack.status = PackStatus::Duplicate;   // the later one, still listed
            } else {
                // a pack whose files did not change keeps its compile status + message across Scan()
                const uint64_t stamp = FolderStamp(dir);
                const auto it = statusById_.find(pack.id);
                if (it != statusById_.end() && it->second.first == stamp &&
                    (it->second.second == PackStatus::Compiled || it->second.second == PackStatus::CompileError)) {
                    pack.status = it->second.second;
                    const auto msg = messageById_.find(pack.id);
                    if (msg != messageById_.end()) pack.statusMessage = msg->second;
                }
                statusById_[pack.id] = {stamp, pack.status};
                messageById_[pack.id] = pack.statusMessage;
            }
            if (pack.status == PackStatus::InvalidManifest || pack.status == PackStatus::Incompatible)
                LOG_WARN("shader pack %s: %s", PathToUtf8(dir).c_str(), pack.statusMessage.c_str());
            packs.push_back(std::move(pack));
        }
    }
    packs_ = std::move(packs);
    stamp_ = PollStamp(roots);
    ++generation_;
    LOG_INFO("shader packs: %zu loaded (generation %u)", packs_.size(), generation_);
}

bool ShaderPackRegistry::PollChanges(double nowSeconds) {
    if (nowSeconds < nextPoll_) return false;
    nextPoll_ = nowSeconds + 0.5;   // cheap: stat only, at most twice a second
    const fs::path roots[2] = {BuiltInRoot(), UserRoot()};
    const uint64_t stamp = PollStamp(roots);
    if (stamp == stamp_) return false;
    Scan();
    return true;
}

const ShaderPack* ShaderPackRegistry::Find(const std::string& id) const {
    for (const ShaderPack& p : packs_)
        if (p.id == id) return &p;
    return nullptr;
}

void ShaderPackRegistry::ReportCompiled(const std::string& id) {
    for (ShaderPack& p : packs_) {
        if (p.id != id || p.status == PackStatus::Compiled) continue;
        p.status = PackStatus::Compiled;
        p.statusMessage.clear();
        const auto it = statusById_.find(id);
        if (it != statusById_.end()) it->second.second = PackStatus::Compiled;
        messageById_[id].clear();
    }
}

void ShaderPackRegistry::ReportError(const std::string& id, const std::string& message) {
    ++errorCount_;
    lastErrorPack_ = id;
    for (ShaderPack& p : packs_) {
        if (p.id != id) continue;
        p.status = PackStatus::CompileError;
        p.statusMessage = message;
        const auto it = statusById_.find(id);
        if (it != statusById_.end()) it->second.second = PackStatus::CompileError;
        messageById_[id] = message;
    }
}

// ---- install / uninstall ----------------------------------------------------------------------------------------------

bool ShaderPackRegistry::InstallFolder(const std::filesystem::path& src, const std::string& installedFrom,
                                       std::string& id, std::string& error) {
    std::error_code ec;
    // pack.json at the root, or one level below (zips usually wrap the pack folder)
    fs::path root = src;
    if (!fs::is_regular_file(src / Utf8ToPath(kManifestName), ec)) {
        std::vector<fs::path> candidates;
        for (const auto& e : fs::directory_iterator(src, ec)) {
            std::error_code e2;
            if (fs::is_regular_file(e.path() / Utf8ToPath(kManifestName), e2)) candidates.push_back(e.path());
        }
        if (candidates.empty()) {
            error = Tr("pack.json을 찾을 수 없습니다");
            return false;
        }
        if (candidates.size() > 1) {
            error = Tr("pack.json이 여러 개 있어 하나를 고를 수 없습니다");
            return false;
        }
        root = candidates[0];
    }
    if (HasReparsePoint(src) || HasReparsePoint(root)) {
        error = Tr("폴더에 바로가기(링크)가 있어 설치할 수 없습니다");
        return false;
    }

    // the manifest is validated before anything is copied
    ShaderPack parsed;
    {
        std::ifstream in(root / Utf8ToPath(kManifestName), std::ios::binary);
        if (!in) {
            error = Tr("pack.json을 읽을 수 없습니다");
            return false;
        }
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string parseError;
        if (!ParseShaderPackManifest(json, root, parsed, parseError)) {
            error = std::string(Tr("매니페스트에 문제가 있습니다: ")) + parseError;
            return false;
        }
    }
    for (const ShaderPack& p : packs_)
        if (p.id == parsed.id && p.source == PackSource::BuiltIn) {
            error = std::string(Tr("기본 제공 팩과 같은 id입니다: ")) + parsed.id;
            return false;
        }

    const std::vector<fs::path> files = PackFiles(root, error);
    if (files.empty()) return false;

    // copy into <id>.tmp, then rename over the old folder: never leave a half-copied pack
    fs::create_directories(UserRoot(), ec);
    const fs::path dest = UserRoot() / Utf8ToPath(parsed.id);
    const fs::path tmp = UserRoot() / Utf8ToPath(parsed.id + ".tmp");
    fs::remove_all(tmp, ec);
    for (const fs::path& f : files) {
        std::error_code e2;
        const fs::path target = tmp / f.lexically_relative(root);
        fs::create_directories(target.parent_path(), e2);
        fs::copy_file(f, target, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            fs::remove_all(tmp, ec);
            error = Tr("팩을 복사하지 못했습니다");
            return false;
        }
    }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        error = Tr("팩 폴더를 옮기지 못했습니다");
        return false;
    }
    if (!installedFrom.empty()) {
        SYSTEMTIME st;
        GetSystemTime(&st);
        char iso[40];
        std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);
        const nlohmann::json marker = {{"from", installedFrom}, {"version", parsed.version},
                                       {"installedAt", iso}};
        std::ofstream out(dest / L".mmdx_install.json", std::ios::binary);
        out << marker.dump(2);
    }
    id = parsed.id;
    Scan();
    return true;
}

bool ShaderPackRegistry::InstallZip(const std::filesystem::path& zip, const std::string& installedFrom,
                                    std::string& id, std::string& error) {
    std::error_code ec;
    // %TEMP%/mmdx12_pack_<random>, always deleted (the cleanup guard runs on every return)
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    char name[64];
    std::snprintf(name, sizeof(name), "mmdx12_pack_%08x",
                  (unsigned)((GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u)) & 0xffffffffu));
    const fs::path temp = fs::path(tempPath) / Utf8ToPath(name);
    struct Cleanup {
        const fs::path& path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } guard{temp};

    std::string extractError;
    if (!net::ExtractZip(zip, temp, extractError)) {
        error = std::string(Tr("압축을 풀지 못했습니다: ")) + extractError;
        return false;
    }
    // zip-slip: every extracted path must stay inside the temp dir (no symlinks / junctions either)
    if (HasReparsePoint(temp)) {
        error = Tr("압축 안에 바로가기(링크)가 있어 설치할 수 없습니다");
        return false;
    }
    const fs::path canon = fs::weakly_canonical(temp, ec);
    if (ec) {
        error = Tr("임시 폴더를 확인하지 못했습니다");
        return false;
    }
    fs::recursive_directory_iterator it(temp, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code e2;
        const fs::path c = fs::weakly_canonical(it->path(), e2);
        if (!e2) {
            auto pi = c.begin();
            auto bi = canon.begin();
            for (; bi != canon.end(); ++bi, ++pi)
                if (pi == c.end() || *pi != *bi) {
                    error = Tr("압축 안의 파일이 임시 폴더 밖으로 나가려 했습니다");
                    return false;
                }
        }
        it.increment(e2);
        ec = e2;
    }
    return InstallFolder(temp, installedFrom, id, error);
}

bool ShaderPackRegistry::Uninstall(const std::string& id, std::string& error) {
    const ShaderPack* p = Find(id);
    if (!p) {
        error = Tr("설치된 팩을 찾을 수 없습니다");
        return false;
    }
    if (p->source == PackSource::BuiltIn) {
        error = Tr("기본 제공 팩은 삭제할 수 없습니다");
        return false;
    }
    std::error_code ec;
    fs::remove_all(p->dir, ec);
    if (ec) {
        error = Tr("팩 폴더를 삭제하지 못했습니다");
        return false;
    }
    Scan();
    return true;
}

bool ShaderPackRegistry::CreateFromTemplate(const std::string& id, const std::string& name,
                                            const std::string& author, std::filesystem::path& dir,
                                            std::string& error, bool effect) {
    if (!ValidShaderPackId(id)) {
        error = Tr("id는 소문자 a-z, 0-9, _, -만 사용할 수 있습니다");
        return false;
    }
    if (Find(id)) {
        error = Tr("같은 id의 팩이 이미 있습니다");
        return false;
    }
    const fs::path tmpl = ExecutableDir() / L"shaders" / (effect ? L"pack_template_effect" : L"pack_template");
    std::error_code ec;
    if (!fs::is_directory(tmpl, ec)) {
        error = Tr("템플릿 폴더를 찾을 수 없습니다");
        return false;
    }
    fs::create_directories(UserRoot(), ec);
    const fs::path dest = UserRoot() / Utf8ToPath(id);
    fs::remove_all(dest, ec);   // an empty stray folder (packs with this id would have failed Find above)
    fs::copy(tmpl, dest, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(dest, ec);
        error = Tr("템플릿을 복사하지 못했습니다");
        return false;
    }
    // rewrite id, name and authors in the copied pack.json (keep the rest)
    const fs::path manifest = dest / Utf8ToPath(kManifestName);
    {
        std::ifstream in(manifest, std::ios::binary);
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(in);
        } catch (const std::exception&) {
            fs::remove_all(dest, ec);
            error = Tr("템플릿의 pack.json을 읽지 못했습니다");
            return false;
        }
        if (!j.is_object()) j = nlohmann::json::object();
        j["id"] = id;
        j["name"] = nlohmann::json::object({{"ko", name}, {"en", name}});
        j["authors"] = nlohmann::json::array({nlohmann::json::object({{"name", author}})});
        std::ofstream out(manifest, std::ios::binary);
        out << j.dump(2);
        if (!out) {
            fs::remove_all(dest, ec);
            error = Tr("pack.json을 쓰지 못했습니다");
            return false;
        }
    }
    dir = dest;
    Scan();
    return true;
}

ShaderPackRegistry& ShaderPacks() {
    static ShaderPackRegistry registry = [] {
        ShaderPackRegistry r;
        r.Scan();
        return r;
    }();
    return registry;
}

namespace {

// The missingTexBySet_ key for one (pack id, texture folder) set.
std::string SetKey(const std::string& id, const std::filesystem::path& folder) {
    return id + "\n" + PathToUtf8(folder);
}

} // namespace

void ShaderPackRegistry::SetTextureFolder(const std::string& id, const std::filesystem::path& dir) {
    if (dir.empty()) {
        if (textureFolders_.erase(id)) ++generation_;   // pack textures reload on the next draw
        return;
    }
    const auto it = textureFolders_.find(id);
    if (it != textureFolders_.end() && it->second == dir) return;
    textureFolders_[id] = dir;
    ++generation_;
}

std::filesystem::path ShaderPackRegistry::TextureFolder(const std::string& id) const {
    const auto it = textureFolders_.find(id);
    return it == textureFolders_.end() ? std::filesystem::path() : it->second;
}

void ShaderPackRegistry::ReportMissingTextures(const std::string& id, const std::filesystem::path& folder,
                                               uint32_t missing) {
    missingTexById_[id] = missing;   // pack-level: the manager screen (the pack folder's set)
    missingTexBySet_[SetKey(id, folder)] = missing;
}

uint32_t ShaderPackRegistry::MissingTextures(const std::string& id, const std::filesystem::path& folder) const {
    const auto it = missingTexBySet_.find(SetKey(id, folder));
    return it == missingTexBySet_.end() ? 0 : it->second;
}

bool ValidShaderPackId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    // pack ids end up in settings, project files and an #include path: keep them plain
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

} // namespace mmdx
