#pragma once
// Shader packs: opt-in, per-model replacements of the scene pass's material shading, made by anyone.
// A pack is a folder with pack.json (manifest: metadata, material class rules, parameters), surface.hlsl (PackShade, the
// contract in shaders/pack_api.hlsli, versioned by kPackApiVersion) and an optional preview.png.
//   built-in   <exe>/shaders/packs/<id>      shipped with the app, read-only
//   installed  <exe>/shader_packs/<id>       user / downloaded packs (online installs carry .mmdx_install.json)
// The renderer compiles a pack's PSOs on first use (ScenePass); a model opts in with GpuModel::SetShaderPack.
// The registry is used from the main thread only.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

// The PackShade contract version (pack_api.hlsli PACK_API_VERSION). A pack declares the version it was written for;
// packs with a higher version are listed as incompatible and never compiled.
inline constexpr int kPackApiVersion = 1;
inline constexpr uint32_t kPackMaxParams = 16;
using PackParamValues = std::array<float, kPackMaxParams>;

// Material classes, as the PACK_* ids in pack_api.hlsli.
enum class PackClass : uint32_t { Body = 0, Skin = 1, Face = 2, Eye = 3, Hair = 4 };

// A model's shader pack choice: pack id (empty = the default shading) and the parameter values the user changed
// (key -> value; the rest use the pack's defaults). Saved per character (settings) and per studio model (project).
struct ShaderChoice {
    std::string pack;
    std::map<std::string, float> params;
    bool operator==(const ShaderChoice&) const = default;
};

// A manifest string: either a plain JSON string (stored under "") or an object {"ko": .., "en": .., "ja": .., "zh": ..}.
struct LocalizedText {
    std::map<std::string, std::string> byLang;
    // `lang` = "ko" / "en" / "ja" / "zh"; falls back to "en", then "", then "ko", then any non-empty entry.
    const std::string& Get(const std::string& lang) const;
    bool Empty() const;
};
// The UI language as a manifest key ("ko" / "en" / "ja" / "zh"), from core/I18n.h's ActiveLanguage().
std::string PackLanguage();

struct PackAuthor {
    std::string name;   // required
    std::string role;   // free text ("셰이더", "translation", ...), may be empty
    std::string url;    // homepage / profile, may be empty (http/https only, anything else is dropped)
};

struct ShaderPackParam {
    std::string key;
    LocalizedText label;          // "label": string or localized object; empty -> key
    float def = 0.0f, min = 0.0f, max = 1.0f;
};

enum class PackSource : uint8_t { BuiltIn, User, Online };
enum class PackStatus : uint8_t {
    Ready,            // manifest fine; not compiled yet or compiled fine
    Compiled,         // its PSOs were built in this run
    CompileError,     // statusMessage = the compiler output (ScenePass)
    Incompatible,     // apiVersion > kPackApiVersion, or minAppVersion newer than this build
    InvalidManifest,  // listed so authors see why; statusMessage says what is wrong; never selectable
    Duplicate,        // same id as an earlier pack (built-in wins, then folder order); never selectable
};

struct ShaderPack {
    // identity
    std::string id;               // [a-z0-9_-]{1,64}; also the folder name for installed packs
    std::string version;          // "1.2.0" (dotted numbers; compared with Updater's CompareVersions rules)
    int apiVersion = 1;           // "apiVersion", default 1
    std::string minAppVersion;    // optional
    // presentation
    LocalizedText name, description;
    std::vector<PackAuthor> authors;
    std::string license;          // SPDX-like free text ("MIT", "CC-BY-4.0", ...)
    std::string homepage, repository;   // http/https only
    std::vector<std::string> tags;      // lower-case words ("toon", "anime", "hoyoverse", ...)
    LocalizedText recommendedFor;       // which models it is made for (free text)
    std::filesystem::path preview;      // dir/preview.png (or .jpg) when present, else empty
    // files
    std::filesystem::path dir;          // holds pack.json and surface.hlsl
    PackSource source = PackSource::User;
    std::string installedFrom;          // online installs: the index URL they came from (.mmdx_install.json)
    // shading
    struct Rule {
        PackClass cls = PackClass::Body;
        std::vector<std::string> match;   // UTF-8 substrings of the PMX material name (or its English name)
    };
    std::vector<Rule> rules;              // first match wins; no match: Body
    std::vector<ShaderPackParam> params;  // at most kPackMaxParams
    // state
    PackStatus status = PackStatus::Ready;
    std::string statusMessage;            // English, for the log / the pack details (compiler output may be long)

    bool Selectable() const { return status == PackStatus::Ready || status == PackStatus::Compiled; }
    PackClass Classify(const std::string& name, const std::string& nameEn) const;
    // Parameter values in manifest order: `values` (key -> value) over the defaults, clamped to the ranges.
    PackParamValues Resolve(const std::map<std::string, float>& values) const;
};

// Parses a manifest. `dir` is the pack folder (for preview / surface checks). Never throws. On failure returns false
// with `error` set, and `out` still holds whatever could be read (at least id from the folder name) so the UI can list it.
bool ParseShaderPackManifest(const std::string& json, const std::filesystem::path& dir, ShaderPack& out, std::string& error);

class ShaderPackRegistry {
public:
    std::filesystem::path BuiltInRoot() const;   // <exe>/shaders/packs
    std::filesystem::path UserRoot() const;      // <exe>/shader_packs (created on demand)

    // (Re)reads every pack folder (built-in first, then installed, each sorted by folder name). Keeps the compile
    // status of packs whose files did not change. Bumps Generation(), so the scene pass drops its compiled PSOs and
    // models re-resolve their packs.
    void Scan();
    // Hot reload for authors: call every frame; checks the pack folders' file times at most twice a second and calls
    // Scan() when a pack.json / *.hlsl / *.hlsli / preview changed, a folder appeared or disappeared. True if it rescanned.
    bool PollChanges(double nowSeconds);

    const std::vector<ShaderPack>& Packs() const { return packs_; }
    const ShaderPack* Find(const std::string& id) const;      // any status
    uint32_t Generation() const { return generation_; }

    // ScenePass reports compile results; the status survives Scan() while the pack's files are unchanged.
    void ReportCompiled(const std::string& id);
    void ReportError(const std::string& id, const std::string& message);
    // Grows with every ReportError; the UI shows a toast when it changes.
    uint32_t ErrorCount() const { return errorCount_; }
    const std::string& LastErrorPack() const { return lastErrorPack_; }

    // Installs a pack folder (a dir with pack.json at its root, or one level below, as zips usually wrap it) into
    // UserRoot()/<id>, replacing an installed pack with the same id (never a built-in one). Validates the manifest and
    // the files first: only pack.json, *.hlsl, *.hlsli, *.png, *.jpg, *.md, *.txt, LICENSE*; no links; total <= 32 MB.
    // `installedFrom` non-empty writes .mmdx_install.json. Rescans. Returns false with a user-facing (Korean) error.
    bool InstallFolder(const std::filesystem::path& src, const std::string& installedFrom, std::string& id,
                       std::string& error);
    // Extracts a .zip into a temp folder (Windows tar.exe, as the updater) and installs it with InstallFolder.
    bool InstallZip(const std::filesystem::path& zip, const std::string& installedFrom, std::string& id, std::string& error);
    // Deletes an installed (User / Online) pack's folder. Built-in packs cannot be removed. Rescans.
    bool Uninstall(const std::string& id, std::string& error);
    // New pack for authors: copies <exe>/shaders/pack_template into UserRoot()/<id> and sets its id / name / author
    // in pack.json. `id` must be valid and unused. Rescans. Returns the new folder.
    bool CreateFromTemplate(const std::string& id, const std::string& name, const std::string& author,
                            std::filesystem::path& dir, std::string& error);

private:
    std::vector<ShaderPack> packs_;
    uint32_t generation_ = 0;
    uint32_t errorCount_ = 0;
    std::string lastErrorPack_;
    // PollChanges / status carry-over state (implementation detail)
    double nextPoll_ = 0;
    uint64_t stamp_ = 0;
    std::map<std::string, std::pair<uint64_t, PackStatus>> statusById_;   // files stamp -> status
    std::map<std::string, std::string> messageById_;
};

// The process-wide registry (scanned on first use).
ShaderPackRegistry& ShaderPacks();

// Valid pack id: [a-z0-9_-]{1,64}.
bool ValidShaderPackId(const std::string& id);

} // namespace mmdx
