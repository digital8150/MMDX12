#pragma once
// Shader packs: opt-in, per-model replacements of the scene pass's material shading ("type": "surface", default) or
// whole-screen post effects ("type": "effect", one entry of the user's effect stack), made by anyone.
// A pack is a folder with pack.json (manifest: metadata, material class rules / injection stage, parameters),
// surface.hlsl (PackShade) or effect.hlsl (PackEffect, the contracts in pack_api.hlsli / effect_api.hlsli, versioned
// by kPackApiVersion) and an optional preview.png.
//   built-in   <exe>/shaders/packs/<id>      shipped with the app, read-only
//   installed  <exe>/shader_packs/<id>       user / downloaded packs (online installs carry .mmdx_install.json)
// The renderer compiles a surface pack's PSOs on first use (ScenePass) and an effect pack's PSO on first frame
// (PackEffectPass); a model opts in with GpuModel::SetShaderPack, the effect stack is RenderSettings::packEffects.
// The registry is used from the main thread only.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

// The PackShade / PackEffect contract version (pack_api.hlsli / effect_api.hlsli PACK_API_VERSION). A pack declares
// the version it was written for; packs with a higher version are listed as incompatible and never compiled.
inline constexpr int kPackApiVersion = 3;
inline constexpr uint32_t kPackMaxParams = 16;
inline constexpr uint32_t kPackMaxTextures = 16;   // pack.json "textures" entries (one SRV each)
inline constexpr uint32_t kPackMaxTextureSize = 4096;   // per-texture width / height cap
using PackParamValues = std::array<float, kPackMaxParams>;

// What a pack does: replace the character material shading (surface.hlsl, default; every v1/v2 pack) or a whole-screen
// effect (effect.hlsl, PackEffect in effect_api.hlsli, one entry of the user's effect stack).
enum class PackType : uint8_t { Surface = 0, Effect };
// Where an effect pack injects ("stage"): "pre-bloom" (linear HDR before BloomPass) or "post" (default; after
// PostPass / tonemap, display-referred sRGB into the LDR target).
enum class PackEffectStage : uint8_t { Post = 0, PreBloom };

// One entry of the user's effect stack: an effect pack, whether it runs, its parameter values (key -> value, the
// rest use the pack defaults) and an optional texture folder (empty = the pack-level folder). Carried by
// RenderSettings::packEffects, persisted as AppSettings::effectStack (ini) and edited in the shader screens.
struct EffectStackEntry {
    std::string pack;
    bool enabled = true;
    std::map<std::string, float> params;
    std::string textureFolder;
    bool operator==(const EffectStackEntry&) const = default;
};

// Material classes, as the PACK_* ids in pack_api.hlsli.
enum class PackClass : uint32_t { Body = 0, Skin = 1, Face = 2, Eye = 3, Hair = 4, Weapon = 5 };

// A model's shader pack choice: pack id (empty = the default shading), the parameter values the user changed
// (key -> value; the rest use the pack's defaults) and an optional per-character texture folder (UTF-8, empty =
// the pack-level folder). Saved per character (settings) and per studio model (project).
struct ShaderMemo {   // what was set for a pack that is not the current one
    std::map<std::string, float> params;
    std::string textureFolder;
    bool operator==(const ShaderMemo&) const = default;
};
// `remembered` holds the parameters and texture folder of the packs this model used before, so going back to one
// (after trying another pack or the default shading) restores them instead of starting from scratch.
struct ShaderChoice {
    std::string pack;
    std::map<std::string, float> params;
    std::string textureFolder;
    std::map<std::string, ShaderMemo> remembered;   // pack id -> its last params / texture folder (not the current pack)
    // Selects `to` ("" = the default shading): the current pack's settings are remembered, `to`'s come back.
    void SwitchPack(const std::string& to) {
        if (to == pack) return;
        if (!pack.empty()) {
            if (params.empty() && textureFolder.empty()) remembered.erase(pack);
            else remembered[pack] = ShaderMemo{params, textureFolder};
        }
        pack = to;
        params.clear();
        textureFolder.clear();
        if (const auto it = remembered.find(to); !to.empty() && it != remembered.end()) {
            params = it->second.params;
            textureFolder = it->second.textureFolder;
            remembered.erase(it);
        }
    }
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

// A pack.json "textures" entry: an extra texture the pack's shading samples (light maps, ramps,
// face SDFs, matcaps, LUTs). `file` is relative to the pack folder (png/jpg/jpeg). Address mode and
// the sampled space: sRGB textures (default) get an _SRGB SRV and sample as LINEAR values, others
// are UNORM (raw values; data maps).
struct PackTexture {
    std::string file;    // required, relative to the pack, no "..", no drive / root
    bool clamp = false;  // "address": "clamp" (else "wrap")
    bool srgb = true;    // "srgb"
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
    // "type": "surface" (default; material shading) or "effect" (PackEffect; pack.json "stage" = pre-bloom / post)
    PackType type = PackType::Surface;
    PackEffectStage stage = PackEffectStage::Post;   // effect packs only
    // shading
    struct Rule {
        PackClass cls = PackClass::Body;
        std::vector<std::string> match;     // UTF-8 substrings of the PMX material name (or its English name)
        std::vector<std::string> texture;   // "texture": UTF-8 substrings of the material's diffuse texture path
    };
    std::vector<Rule> rules;              // first match wins; no match: Body
    std::vector<ShaderPackParam> params;  // at most kPackMaxParams
    std::vector<PackTexture> textures;    // at most kPackMaxTextures; optional extra textures (PackSampleTex)
    bool hasEdge = false;                 // surface.hlsl sets PACK_HAS_EDGE: the pack draws the outlines too (PackEdge)
    bool hasPtSurface = false;            // pt_surface.hlsl exists next to surface.hlsl (offline GI stills)
    // state
    PackStatus status = PackStatus::Ready;
    std::string statusMessage;            // English, for the log / the pack details (compiler output may be long)

    bool Selectable() const { return status == PackStatus::Ready || status == PackStatus::Compiled; }
    // `texture` = the material's diffuse texture path as stored in the PMX (empty without one).
    PackClass Classify(const std::string& name, const std::string& nameEn, const std::string& texture) const;
    // Parameter values in manifest order: `values` (key -> value) over the defaults, clamped to the ranges.
    PackParamValues Resolve(const std::map<std::string, float>& values) const;
};

// Parses a manifest. `dir` is the pack folder (for preview / surface checks). Never throws. On failure returns false
// with `error` set, and `out` still holds whatever could be read (at least id from the folder name) so the UI can list it.
bool ParseShaderPackManifest(const std::string& json, const std::filesystem::path& dir, ShaderPack& out, std::string& error);

// pack.json "textures" lookup: the user texture folder first (same relative path, then the same file
// name, then a file whose name ends with "_" + the declared name, case-insensitive, shortest wins),
// then the pack folder (exact path). False = missing (white is used). Used from the scene pass and
// the effect pass (Passes.cpp, shared helper).
bool ResolvePackTextureFile(const ShaderPack& pack, const std::string& file, const std::filesystem::path& userFolder,
                            std::filesystem::path& out);

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
    // New pack for authors: copies <exe>/shaders/pack_template (effect: pack_template_effect) into UserRoot()/<id> and sets its id / name / author
    // in pack.json. `id` must be valid and unused. Rescans. Returns the new folder.
    bool CreateFromTemplate(const std::string& id, const std::string& name, const std::string& author,
                            std::filesystem::path& dir, std::string& error, bool effect = false);

    // Per-pack user texture folder: game textures that can't be redistributed live outside the pack. Set from the
    // shader manager screen and saved in the ini (AppSettings::packTextureFolders); empty `dir` = clear. Changing it
    // bumps Generation(), so the pack's textures reload on the next draw. Used from the renderer.
    void SetTextureFolder(const std::string& id, const std::filesystem::path& dir);
    std::filesystem::path TextureFolder(const std::string& id) const;
    // Missing-texture count per (pack id, texture folder) set: ScenePass reports how many of the pack's textures
    // were missing on the last load of that set (white is used); the manager keeps showing the pack-level folder's
    // count. Survives Scan().
    void ReportMissingTextures(const std::string& id, const std::filesystem::path& folder, uint32_t missing);
    uint32_t MissingTextures(const std::string& id, const std::filesystem::path& folder) const;

private:
    std::vector<ShaderPack> packs_;
    uint32_t generation_ = 0;
    uint32_t errorCount_ = 0;
    std::string lastErrorPack_;
    std::map<std::string, std::filesystem::path> textureFolders_;   // pack id -> user texture folder
    std::map<std::string, uint32_t> missingTexById_;                // pack id -> missing texture count (last load)
    std::map<std::string, uint32_t> missingTexBySet_;               // pack id + "\n" + folder -> count per set
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
