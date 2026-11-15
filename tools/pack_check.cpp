// pack_check: validates a shader pack (folder or .zip) the way the app installs it, and with --compile compiles its
// surface through mmd.hlsl's PSPack with DXC (no D3D device needed). Prints every manifest field it read and every
// problem. Exit code 0 = ok, 1 = problems.
//
// Usage: pack_check <pack dir | pack.zip> [--compile]
#include "core/NetUtil.h"
#include "core/TextUtil.h"
#include "asset/ImageLoader.h"
#include "render/RenderPass.h"
#include "render/ShaderPack.h"
#include "render/LookPreset.h"

#include <Windows.h>
#include <json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using mmdx::LocalizedText;
using mmdx::PackAuthor;
using mmdx::ShaderPack;
using mmdx::ShaderPackParam;

constexpr const char* kManifestName = "pack.json";

// pack.json, *.hlsl, *.hlsli, *.png, *.jpg, *.md, *.txt, LICENSE*
bool AllowedPackFile(const fs::path& f) {
    const std::string name = mmdx::PathToUtf8(f.filename());
    if (name == kManifestName || name.rfind("LICENSE", 0) == 0) return true;
    const std::string ext = mmdx::ToLowerAscii(mmdx::PathToUtf8(f.extension()));
    return ext == ".hlsl" || ext == ".hlsli" || ext == ".png" || ext == ".jpg" || ext == ".md" || ext == ".txt";
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

// The pack folder's files with the install limits. Prints every problem; false on any.
bool CheckFiles(const fs::path& root, const ShaderPack& pack) {
    std::vector<fs::path> files;
    uint64_t total = 0;
    bool ok = true;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        files.push_back(it->path());
        if ((int)files.size() > 200) {
            printf("  [problem] more than 200 files\n");
            ok = false;
        }
        if (!AllowedPackFile(it->path())) {
            printf("  [problem] file not allowed by installs: %s\n", mmdx::PathToUtf8(it->path().filename()).c_str());
            ok = false;
        }
        const uint64_t size = (uint64_t)fs::file_size(it->path(), e2);
        if (e2) {
            printf("  [problem] cannot stat: %s\n", mmdx::PathToUtf8(it->path()).c_str());
            ok = false;
        }
        total += size;
    }
    const uint64_t maxTotal = (pack.type == mmdx::PackType::World) ? (64ull << 20) : (32ull << 20);
    if (total > maxTotal) {
        printf("  [problem] pack larger than %u MB (%.1f MB)\n", (uint32_t)(maxTotal >> 20), (double)total / (1024.0 * 1024.0));
        ok = false;
    }
    printf("  files: %zu (%.2f MB)\n", files.size(), (double)total / (1024.0 * 1024.0));
    return ok;
}

void PrintText(const char* label, const LocalizedText& t) {
    if (t.byLang.empty()) {
        printf("  %s: (none)\n", label);
        return;
    }
    for (const auto& [lang, text] : t.byLang)
        printf("  %s[%s]: %s\n", label, lang.c_str(), text.c_str());
}

constexpr uint32_t kMaxTextures = 16;
constexpr uint32_t kMaxTextureSize = 4096;

// Prints the textures the manifest declared; also validates them below.
void PrintTextures(const ShaderPack& pack) {
    if (pack.textures.empty()) {
        printf("  textures: (none)\n");
        return;
    }
    for (size_t i = 0; i < pack.textures.size(); ++i) {
        const mmdx::PackTexture& t = pack.textures[i];
        printf("  texture %zu: %s (%s, %s, origin %s)\n", i, t.file.c_str(), t.clamp ? "clamp" : "wrap",
               t.srgb ? "srgb" : "raw", t.origin == mmdx::PackTexture::Origin::Bundled ? "bundled" : "user");
    }
}

// Prints every field ParseShaderPackManifest read; false when it reported problems.
bool PrintManifest(const ShaderPack& pack, const std::string& error) {
    printf(" pack.json:\n");
    printf("  id: %s\n", pack.id.c_str());
    printf("  version: %s\n", pack.version.c_str());
    printf("  apiVersion: %d\n", pack.apiVersion);
    printf("  minAppVersion: %s\n", pack.minAppVersion.empty() ? "(none)" : pack.minAppVersion.c_str());
    PrintText("name", pack.name);
    PrintText("description", pack.description);
    PrintText("recommendedFor", pack.recommendedFor);
    if (pack.suggest.present)
        printf("  suggest: additionalUvsMin=%d toonMapMaterialsMin=%d\n", pack.suggest.additionalUvsMin,
               pack.suggest.toonMapMaterialsMin);
    else
        printf("  suggest: (none)\n");
    if (pack.authors.empty()) {
        printf("  authors: (none)\n");
    } else {
        for (const PackAuthor& a : pack.authors)
            printf("  author: %s (role: %s, url: %s)\n", a.name.c_str(),
                   a.role.empty() ? "-" : a.role.c_str(), a.url.empty() ? "-" : a.url.c_str());
    }
    printf("  license: %s\n", pack.license.empty() ? "(none)" : pack.license.c_str());
    printf("  homepage: %s\n", pack.homepage.empty() ? "(none)" : pack.homepage.c_str());
    printf("  repository: %s\n", pack.repository.empty() ? "(none)" : pack.repository.c_str());
    if (pack.tags.empty()) {
        printf("  tags: (none)\n");
    } else {
        printf("  tags:");
        for (const std::string& t : pack.tags) printf(" %s", t.c_str());
        printf("\n");
    }
    if (pack.rules.empty()) {
        printf("  classes: (none)\n");
    } else {
        static const char* kClasses[] = {"body", "skin", "face", "eye", "hair", "weapon"};
        for (const ShaderPack::Rule& r : pack.rules) {
            printf("  class %s:", kClasses[(size_t)r.cls]);
            for (const std::string& m : r.match) printf(" \"%s\"", m.c_str());
            for (const std::string& t : r.texture) printf(" texture:\"%s\"", t.c_str());
            const float key = pack.alphaKey[(size_t)r.cls];
            if (key > 0.0f) printf(" alphaKey:%.4g", key);
            printf("\n");
        }
    }
    // type: "surface" (default) or "effect" (+ its injection stage)
    if (pack.type == mmdx::PackType::Effect) {
        printf("  type: effect (stage: %s)\n", pack.stage == mmdx::PackEffectStage::PreBloom ? "pre-bloom" : "post");
        if (pack.stateFloats > 0)
            printf("  state: %u floats\n", pack.stateFloats);
        for (const mmdx::ShaderPack::Pass& pass : pack.passes)
            printf("  pass: %s (scale %.4g)\n", pass.entry.c_str(), pass.scale);
        if (pack.replacesDof) printf("  replaces: dof\n");
    } else if (pack.type == mmdx::PackType::Look) {
        printf("  type: look\n");
        if (pack.look) {
            const mmdx::LookData& look = *pack.look;
            printf("  look sets:");
            if (look.setsLighting) printf(" lighting");
            if (look.setsEffects) printf(" effects");
            if (look.setsColor) printf(" color");
            if (look.setsSurface) printf(" surface");
            printf("\n");
            if (look.hasSurface) {
                printf("  look surface: %s\n", look.surface.pack.empty() ? "(none)" : look.surface.pack.c_str());
                for (const auto& [mat, cls] : look.surface.materials)
                    printf("  look surface material %s: %d\n", mat.c_str(), cls);
            }
            for (const mmdx::EffectStackEntry& e : look.effects)
                printf("  look effect: %s (%s)\n", e.pack.c_str(), e.enabled ? "enabled" : "disabled");
            if (!look.colorLut.empty()) printf("  look lut: %s\n", look.colorLut.c_str());
            if (!look.lutFile.empty()) printf("  look lutFile: %s\n", look.lutFile.c_str());
            for (const mmdx::LookPackRef& ref : look.packs)
                printf("  look pack: %s%s%s\n", ref.id.c_str(), ref.minVersion.empty() ? "" : " min ",
                       ref.minVersion.empty() ? "" : ref.minVersion.c_str());
        }
    } else if (pack.type == mmdx::PackType::World) {
        printf("  type: world\n");
        if (pack.world) {
            printf("  world.step: %.4f s\n", pack.world->step);
            printf("  world.rewind: %.1f s\n", pack.world->rewind);
            for (size_t pi = 0; pi < pack.world->particles.size(); ++pi) {
                const auto& ps = pack.world->particles[pi];
                printf("  particle %zu: %s (count %u, %s, %s, shade %s, size [%.2f, %.2f])\n",
                       pi, ps.name.c_str(), ps.count, ps.blend == mmdx::PackWorldParticleSystem::Blend::Add ? "add" : "alpha",
                       ps.simulate.empty() ? ("spawn " + ps.spawn).c_str() : ("simulate " + ps.simulate).c_str(),
                       ps.shade.c_str(), ps.sizeMin, ps.sizeMax);
            }
            for (size_t fi = 0; fi < pack.world->fields.size(); ++fi) {
                const auto& fld = pack.world->fields[fi];
                printf("  field %zu: %s (size %u, extent %.1f, %s, %s, update %s)\n",
                       fi, fld.name.c_str(), fld.size, fld.extent,
                       fld.format == mmdx::PackWorldField::Format::R16F ? "r16f" : fld.format == mmdx::PackWorldField::Format::RG16F ? "rg16f" : "rgba16f",
                       fld.persistent ? "persistent" : "transient", fld.update.c_str());
            }
            for (size_t si = 0; si < pack.world->surfaces.size(); ++si) {
                const auto& srf = pack.world->surfaces[si];
                printf("  surface %zu: field %s (grid %u, height %s, shade %s)\n",
                       si, srf.field.c_str(), srf.grid, srf.height.c_str(), srf.shade.c_str());
            }
        }
    } else if (pack.type == mmdx::PackType::Screen) {
        printf("  type: screen (screen.hlsl -> PackScreen, docs/shader_screen_api.md)\n");
    } else {
        printf("  type: surface\n");
        printf("  pt_surface: %s\n", pack.hasPtSurface ? "yes" : "no");
    }
    PrintTextures(pack);
    if (pack.requiresCaps.empty()) {
        printf("  requires: (none)\n");
    } else {
        printf("  requires:");
        for (const std::string& r : pack.requiresCaps) printf(" %s", r.c_str());
        printf("\n");
    }
    if (pack.inputMask == 0) {
        printf("  inputs: (none)\n");
    } else {
        printf("  inputs:");
        for (uint8_t i = 0; i < (uint8_t)mmdx::PackInput::Count; ++i)
            if (pack.inputMask & (1u << i)) printf(" %s", mmdx::PackInputName((mmdx::PackInput)i));
        printf("\n");
    }
    if (pack.groups.empty()) {
        printf("  groups: (none)\n");
    } else {
        for (const mmdx::ShaderPackGroup& g : pack.groups) {
            const std::string label = g.label.Get(mmdx::PackLanguage());
            printf("  group %s: \"%s\"%s\n", g.id.c_str(), label.c_str(), g.collapsed ? " (collapsed)" : "");
        }
    }
    if (pack.params.empty()) {
        printf("  params: (none)\n");
    } else {
        for (const mmdx::ShaderPackParam& p : pack.params) {
            const std::string label = p.label.Get(mmdx::PackLanguage());
            static const char* kTypes[] = {"float", "int", "bool", "color", "vec3"};
            const char* typeStr = kTypes[(size_t)p.type];
            printf("  param %s (%s): \"%s\"", p.key.c_str(), typeStr, label.c_str());
            if (!p.group.empty()) printf(" group \"%s\"", p.group.c_str());
            if (p.type == mmdx::ShaderPackParamType::Float || p.type == mmdx::ShaderPackParamType::Int) {
                printf(" default %.3f min %.3f max %.3f", p.def, p.min, p.max);
                if (p.step > 0.0f) printf(" step %.3f", p.step);
            } else if (p.type == mmdx::ShaderPackParamType::Bool) {
                printf(" default %s", p.defBool ? "true" : "false");
            } else if (p.type == mmdx::ShaderPackParamType::Color) {
                if (p.alpha) {
                    printf(" default [%.3f, %.3f, %.3f, %.3f] alpha", p.defVec[0], p.defVec[1], p.defVec[2], p.defVec[3]);
                } else {
                    printf(" default [%.3f, %.3f, %.3f]", p.defVec[0], p.defVec[1], p.defVec[2]);
                }
            } else if (p.type == mmdx::ShaderPackParamType::Vec3) {
                printf(" default [%.3f, %.3f, %.3f] min %.3f max %.3f", p.defVec[0], p.defVec[1], p.defVec[2], p.min, p.max);
                if (p.step > 0.0f) printf(" step %.3f", p.step);
            }
            printf("\n");
        }
    }
    printf("  preview: %s\n", pack.preview.empty() ? "(none)"
                                                   : mmdx::PathToUtf8(pack.preview.filename()).c_str());
    if (!error.empty()) {
        printf("  [problem] %s\n", error.c_str());
        return false;
    }
    return true;
}

// The pack's texture files: format, size, dims, total bytes. Missing files are a warning (they may
// be user-supplied); bad paths / formats / dims are problems. False on any problem.
bool CheckTextures(const ShaderPack& pack, const fs::path& dir) {
    if (pack.textures.empty()) return true;
    bool ok = true;
    if (pack.textures.size() > kMaxTextures) {
        printf("  [problem] more than %u textures (%zu)\n", kMaxTextures, pack.textures.size());
        ok = false;
    }
    constexpr const char* kExts[] = {".png", ".jpg", ".jpeg"};
    uint64_t total = 0;
    for (size_t i = 0; i < pack.textures.size(); ++i) {
        const mmdx::PackTexture& t = pack.textures[i];
        const std::string ext = mmdx::ToLowerAscii(mmdx::PathToUtf8(fs::path(mmdx::Utf8ToPath(t.file)).extension()));
        bool extOk = false;
        for (const char* k : kExts) extOk |= ext == k;
        if (!extOk) {
            printf("  [problem] texture %zu (%s): not a png / jpg / jpeg file\n", i, t.file.c_str());
            ok = false;
        }
        const fs::path rel = mmdx::Utf8ToPath(t.file);
        if (t.file.find(':') != std::string::npos || t.file.front() == '/' || t.file.front() == '\\') {
            printf("  [problem] texture %zu (%s): absolute path\n", i, t.file.c_str());
            ok = false;
        }
        for (const auto& part : rel)
            if (mmdx::PathToUtf8(part) == "..") {
                printf("  [problem] texture %zu (%s): path escapes the pack folder\n", i, t.file.c_str());
                ok = false;
            }
        const fs::path path = dir / rel;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            if (t.origin == mmdx::PackTexture::Origin::Bundled) {
                printf("  [problem] texture %zu (%s): bundled file not found (broken pack)\n", i, t.file.c_str());
                ok = false;
            } else {
                printf("  [warn] texture %zu (%s): file not found (may be user-supplied)\n", i, t.file.c_str());
            }
            continue;
        }
        const uint64_t bytes = (uint64_t)fs::file_size(path, ec);
        if (!ec) total += bytes;
        mmdx::ImageRGBA8 img;
        std::string err;
        if (!mmdx::LoadImageRGBA8(path, img, &err)) {
            printf("  [problem] texture %zu (%s): cannot decode (%s)\n", i, t.file.c_str(), err.c_str());
            ok = false;
        } else if (img.Width() > kMaxTextureSize || img.Height() > kMaxTextureSize) {
            printf("  [problem] texture %zu (%s): %ux%u (max %u)\n", i, t.file.c_str(), img.Width(), img.Height(),
                   kMaxTextureSize);
            ok = false;
        } else {
            printf("  texture %zu: %ux%u (%.2f MB)\n", i, img.Width(), img.Height(), (double)bytes / (1024.0 * 1024.0));
        }
    }
    printf("  textures: %zu, %.2f MB total\n", pack.textures.size(), (double)total / (1024.0 * 1024.0));
    const uint64_t maxTotal = (pack.type == mmdx::PackType::World) ? (64ull << 20) : (32ull << 20);
    if (total > maxTotal) {
        printf("  [problem] textures larger than %u MB (%.1f MB)\n", (uint32_t)(maxTotal >> 20), (double)total / (1024.0 * 1024.0));
        ok = false;
    }
    return ok;
}

// PackSampleTex(N) / PackSampleTexLevel(N, ...) literal indices in surface.hlsl must be < the
// declared texture count (best effort: literal indices only).
void CheckTextureIndices(const ShaderPack& pack, const fs::path& dir, bool& ok) {
    std::error_code ec;
    const uint32_t count = (uint32_t)pack.textures.size();
    const wchar_t* shaderFile = pack.type == mmdx::PackType::Effect ? L"effect.hlsl"
                              : pack.type == mmdx::PackType::World  ? L"world.hlsl"
                              : pack.type == mmdx::PackType::Screen ? L"screen.hlsl"
                                                                    : L"surface.hlsl";
    if (fs::is_regular_file(dir / shaderFile, ec)) {
        FILE* f = _wfopen((dir / shaderFile).c_str(), L"rb");
        if (f) {
            std::string hlsl;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) hlsl.append(buf, n);
            fclose(f);
            for (const char* fn : {"PackSampleTex(", "PackSampleTexLevel(", "PackFxSampleTex(", "PackFxSampleTexLevel("}) {
                for (size_t pos = hlsl.find(fn); pos != std::string::npos; pos = hlsl.find(fn, pos + 1)) {
                    size_t i = pos + strlen(fn);
                    while (i < hlsl.size() && (hlsl[i] == ' ' || hlsl[i] == '\t')) ++i;
                    size_t digits = i;
                    while (digits < hlsl.size() && hlsl[digits] >= '0' && hlsl[digits] <= '9') ++digits;
                    if (digits == i) continue;   // not a literal index
                    while (digits < hlsl.size() && (hlsl[digits] == ' ' || hlsl[digits] == '\t')) ++digits;
                    if (digits >= hlsl.size() || (hlsl[digits] != ',' && hlsl[digits] != ')')) continue;
                    const unsigned long index = strtoul(hlsl.c_str() + i, nullptr, 10);
                    if (index >= count) {
                        printf("  [problem] %ls: %s%lu but the pack declares %u textures\n", shaderFile, fn, index, count);
                        ok = false;
                    }
                }
            }
        }
    }
    if (fs::is_regular_file(dir / L"pt_surface.hlsl", ec)) {
        FILE* f = _wfopen((dir / L"pt_surface.hlsl").c_str(), L"rb");
        if (f) {
            std::string hlsl;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) hlsl.append(buf, n);
            fclose(f);
            // PtPackOut is read as is: a field PackEvaluate never assigns is undefined (e.g. a garbage terminator)
            for (const char* field : {"albedo", "shadowTint", "shadowBias", "specular", "terminator", "flatFace"}) {
                const std::string key = std::string(".") + field;
                bool assigned = false;
                for (size_t pos = hlsl.find(key); pos != std::string::npos && !assigned; pos = hlsl.find(key, pos + 1)) {
                    size_t i = pos + key.size();
                    if (i < hlsl.size() && (isalnum((unsigned char)hlsl[i]) || hlsl[i] == '_')) continue;   // longer name
                    while (i < hlsl.size() && (hlsl[i] == ' ' || hlsl[i] == '\t')) ++i;
                    if (i + 1 < hlsl.size() && hlsl[i] == '=' && hlsl[i + 1] != '=') assigned = true;
                }
                if (!assigned) printf("  [warn] pt_surface.hlsl never assigns PtPackOut.%s (it would be undefined)\n", field);
            }
            // PT_PACK_HAS_LIGHTING: PackEvaluateLighting returns a PtPackLighting, whose fields must be assigned too
            // (only relevant to packs that opt in, so this is checked only when the file defines the macro)
            if (hlsl.find("PT_PACK_HAS_LIGHTING") != std::string::npos) {
                for (const char* field : {"flatIndirect", "sunShadow", "lit"}) {
                    const std::string key = std::string(".") + field;
                    bool assigned = false;
                    for (size_t pos = hlsl.find(key); pos != std::string::npos && !assigned; pos = hlsl.find(key, pos + 1)) {
                        size_t i = pos + key.size();
                        if (i < hlsl.size() && (isalnum((unsigned char)hlsl[i]) || hlsl[i] == '_')) continue;   // longer name
                        while (i < hlsl.size() && (hlsl[i] == ' ' || hlsl[i] == '\t')) ++i;
                        if (i + 1 < hlsl.size() && hlsl[i] == '=' && hlsl[i + 1] != '=') assigned = true;
                    }
                    if (!assigned) printf("  [warn] pt_surface.hlsl never assigns PtPackLighting.%s (it would be undefined)\n", field);
                }
            }
            for (const char* fn : {"PtPackSampleTex(", "PtPackSampleTexLevel("}) {
                for (size_t pos = hlsl.find(fn); pos != std::string::npos; pos = hlsl.find(fn, pos + 1)) {
                    size_t i = pos + strlen(fn);
                    while (i < hlsl.size() && (hlsl[i] == ' ' || hlsl[i] == '\t')) ++i;
                    size_t digits = i;
                    while (digits < hlsl.size() && hlsl[digits] >= '0' && hlsl[digits] <= '9') ++digits;
                    if (digits == i) continue;   // not a literal index
                    while (digits < hlsl.size() && (hlsl[digits] == ' ' || hlsl[digits] == '\t')) ++digits;
                    if (digits >= hlsl.size() || (hlsl[digits] != ',' && hlsl[digits] != ')')) continue;
                    const unsigned long index = strtoul(hlsl.c_str() + i, nullptr, 10);
                    if (index >= count) {
                        printf("  [problem] pt_surface.hlsl: %s%lu but the pack declares %u textures\n", fn, index, count);
                        ok = false;
                    }
                }
            }
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: pack_check <pack dir | pack.zip> [--compile]\n");
        return 1;
    }
    const fs::path input = argv[1];
    bool compile = false;
    for (int i = 2; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--compile") compile = true;
        else printf("unknown argument: %s\n", mmdx::WideToUtf8(argv[i]).c_str());

    bool ok = true;

    // A zip is extracted into a temp dir first (zip-slip checked, like InstallZip); always cleaned up.
    fs::path dir = input;
    std::string extractError;
    std::error_code dirEc;
    fs::path temp;
    struct Cleanup {
        fs::path& path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    };
    Cleanup guard{temp};   // an empty path removes nothing
    if (!fs::is_directory(input, dirEc)) {
        wchar_t tempPath[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempPath);
        char name[64];
        std::snprintf(name, sizeof(name), "mmdx12_packcheck_%08x",
                      (unsigned)((GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u)) & 0xffffffffu));
        temp = fs::path(tempPath) / mmdx::Utf8ToPath(name);
        if (!mmdx::net::ExtractZip(input, temp, extractError)) {
            printf("[problem] cannot extract %s: %s\n", mmdx::PathToUtf8(input).c_str(), extractError.c_str());
            return 1;
        }
        dir = temp;
        // every extracted path must stay inside the temp dir (ExtractZip uses tar.exe)
        std::error_code ec;
        const fs::path canon = fs::weakly_canonical(temp, ec);
        if (!ec) {
            for (fs::recursive_directory_iterator it(temp, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                const fs::path c = fs::weakly_canonical(it->path(), e2);
                if (e2) continue;
                auto pi = c.begin();
                auto bi = canon.begin();
                for (; bi != canon.end(); ++bi, ++pi)
                    if (pi == c.end() || *pi != *bi) {
                        printf("[problem] zip-slip: %s escapes the temp dir\n", mmdx::PathToUtf8(it->path()).c_str());
                        return 1;
                    }
            }
        }
        // pack.json at the root or exactly one level below (as zips usually wrap it)
        std::error_code ec2;
        if (!fs::is_regular_file(dir / kManifestName, ec2)) {
            fs::path found;
            int count = 0;
            for (const auto& e : fs::directory_iterator(dir, ec2))
                if (fs::is_regular_file(e.path() / kManifestName, ec2)) {
                    ++count;
                    found = e.path();
                }
            if (count == 1) dir = found;
        }
    }

    printf("pack: %s\n", mmdx::PathToUtf8(dir).c_str());
    if (HasReparsePoint(dir)) {
        printf("  [problem] folder contains a symlink / junction\n");
        ok = false;
    }

    // the manifest
    std::error_code ec;
    if (!fs::is_regular_file(dir / kManifestName, ec)) {
        printf("[problem] no pack.json\n");
        return 1;
    }
    ShaderPack pack;
    std::string error;
    {
        FILE* f = _wfopen((dir / kManifestName).c_str(), L"rb");
        std::string json;
        if (f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
            fclose(f);
        }
        mmdx::ParseShaderPackManifest(json, dir, pack, error);
        // "suggest": unknown keys are ignored by the app; warn so typos show up (type errors are manifest problems)
        try {
            const nlohmann::json j = nlohmann::json::parse(json);
            if (auto it = j.find("suggest"); it != j.end() && it->is_object())
                for (auto kv = it->begin(); kv != it->end(); ++kv)
                    if (kv.key() != "additionalUvsMin" && kv.key() != "toonMapMaterialsMin")
                        printf("  [warn] suggest: unknown key '%s' (ignored)\n", kv.key().c_str());
        } catch (...) {}
    }
    ok = PrintManifest(pack, error) && ok;
    ok = CheckFiles(dir, pack) && ok;
    ok = CheckTextures(pack, dir) && ok;
    CheckTextureIndices(pack, dir, ok);

    if (compile) {
        const bool effect = pack.type == mmdx::PackType::Effect;
        const wchar_t* packFile = effect ? L"effect.hlsl" : L"surface.hlsl";
        if (pack.type == mmdx::PackType::Look) {
            printf("compile: skipped (look packs have no shader)\n");
        } else if (pack.type == mmdx::PackType::World) {
            const fs::path worldHlsl = mmdx::ExecutableDir() / L"shaders" / L"world.hlsl";
            if (!fs::is_regular_file(worldHlsl, ec)) {
                printf("[problem] %s not found\n", mmdx::PathToUtf8(worldHlsl).c_str());
                return 1;
            }
            fs::path rel = fs::relative(pack.dir / L"world.hlsl", worldHlsl.parent_path(), ec);
            if (ec || rel.empty()) rel = pack.dir / L"world.hlsl";
            std::string inc = mmdx::PathToUtf8(rel);
            std::replace(inc.begin(), inc.end(), '\\', '/');
            const std::string incDefine = "\"" + inc + "\"";

            uint32_t clampMask = 0, srgbMask = 0;
            const uint32_t texCount = (uint32_t)std::min<size_t>(pack.textures.size(), 32u);
            for (uint32_t i = 0; i < texCount; ++i) {
                if (pack.textures[i].clamp) clampMask |= 1u << i;
                if (pack.textures[i].srgb) srgbMask |= 1u << i;
            }
            std::vector<std::pair<std::string, std::string>> baseDefines = {
                {"MMDX_PACK", incDefine},
                {"PACK_TEX_COUNT", std::to_string(texCount)},
                {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"},
                {"PACK_TEX_SRGB_MASK", std::to_string(srgbMask) + "u"},
                {"PACK_CAP_PARAMS_EXT", "1"}
            };
            // The world capabilities (the app's world host defines them the same way)
            if (pack.HasCap("world.camera")) baseDefines.push_back({"PACK_CAP_WORLD_CAMERA", "1"});
            if (pack.HasCap("world.anchor")) baseDefines.push_back({"PACK_CAP_WORLD_ANCHOR", "1"});
            if (pack.HasCap("world.ground")) baseDefines.push_back({"PACK_CAP_WORLD_GROUND", "1"});
            if (pack.HasCap("world.overlay")) baseDefines.push_back({"PACK_CAP_WORLD_OVERLAY", "1"});
            if (pack.HasCap("world.body")) baseDefines.push_back({"PACK_CAP_WORLD_BODY", "1"});
            if (pack.HasCap("world.lights")) baseDefines.push_back({"PACK_CAP_WORLD_LIGHTS", "1"});
            if (pack.HasCap("inputs.audio")) baseDefines.push_back({"PACK_CAP_INPUTS_AUDIO", "1"});
            if (pack.world) {
                for (const auto& ps : pack.world->particles) {
                    if (!ps.simulate.empty()) {
                        auto defs = baseDefines;
                        defs.push_back({"WORLD_SIMULATE", "1"});
                        defs.push_back({"WORLD_SIMULATE_FN", ps.simulate});
                        printf("compile: particle %s simulate %s (cs_6_0)\n", ps.name.c_str(), ps.simulate.c_str());
                        std::string errs;
                        const mmdx::ComPtr blob = mmdx::CompileShaderDxc(worldHlsl, "CSWorldSimulate", "cs_6_0", defs, &errs);
                        if (!errs.empty()) printf("%s\n", errs.c_str());
                        if (!blob) { printf("[problem] simulate compile failed\n"); ok = false; }
                        else { printf("compile ok (%zu bytes)\n", blob->GetBufferSize()); }
                    }
                    if (!ps.spawn.empty()) {
                        auto defs = baseDefines;
                        defs.push_back({"WORLD_SPAWN", "1"});
                        defs.push_back({"WORLD_SPAWN_FN", ps.spawn});
                        printf("compile: particle %s spawn %s (cs_6_0)\n", ps.name.c_str(), ps.spawn.c_str());
                        std::string errs;
                        const mmdx::ComPtr blob = mmdx::CompileShaderDxc(worldHlsl, "CSWorldSpawn", "cs_6_0", defs, &errs);
                        if (!errs.empty()) printf("%s\n", errs.c_str());
                        if (!blob) { printf("[problem] spawn compile failed\n"); ok = false; }
                        else { printf("compile ok (%zu bytes)\n", blob->GetBufferSize()); }
                    }
                    if (!ps.shade.empty()) {
                        auto defs = baseDefines;
                        defs.push_back({"WORLD_PARTICLE", "1"});
                        defs.push_back({"WORLD_PARTICLE_SHADE_FN", ps.shade});
                        if (!ps.spawn.empty()) {
                            defs.push_back({"WORLD_STATELESS", "1"});
                            defs.push_back({"WORLD_SPAWN_FN", ps.spawn});
                        }
                        printf("compile: particle %s shade %s (vs_6_0 / ps_6_0)\n", ps.name.c_str(), ps.shade.c_str());
                        std::string errs;
                        const mmdx::ComPtr vsBlob = mmdx::CompileShaderDxc(worldHlsl, "VSWorldParticle", "vs_6_0", defs, &errs);
                        const mmdx::ComPtr psBlob = mmdx::CompileShaderDxc(worldHlsl, "PSWorldParticle", "ps_6_0", defs, &errs);
                        if (!errs.empty()) printf("%s\n", errs.c_str());
                        if (!vsBlob || !psBlob) { printf("[problem] particle shade compile failed\n"); ok = false; }
                        else { printf("compile ok (vs: %zu, ps: %zu bytes)\n", vsBlob->GetBufferSize(), psBlob->GetBufferSize()); }
                    }
                }
                for (const auto& fld : pack.world->fields) {
                    if (!fld.update.empty()) {
                        auto defs = baseDefines;
                        defs.push_back({"WORLD_FIELD_UPDATE", "1"});
                        defs.push_back({"WORLD_FIELD_UPDATE_FN", fld.update});
                        printf("compile: field %s update %s (cs_6_0)\n", fld.name.c_str(), fld.update.c_str());
                        std::string errs;
                        const mmdx::ComPtr blob = mmdx::CompileShaderDxc(worldHlsl, "CSWorldFieldUpdate", "cs_6_0", defs, &errs);
                        if (!errs.empty()) printf("%s\n", errs.c_str());
                        if (!blob) { printf("[problem] field update compile failed\n"); ok = false; }
                        else { printf("compile ok (%zu bytes)\n", blob->GetBufferSize()); }
                    }
                }
                for (const auto& srf : pack.world->surfaces) {
                    auto defs = baseDefines;
                    defs.push_back({"WORLD_SURFACE", "1"});
                    defs.push_back({"WORLD_SURFACE_HEIGHT_FN", srf.height});
                    defs.push_back({"WORLD_SURFACE_SHADE_FN", srf.shade});
                    if (srf.blend == mmdx::PackWorldSurface::Blend::Overlay)
                        defs.push_back({"WORLD_SURFACE_OVERLAY", "1"});
                    printf("compile: surface height %s / shade %s (vs_6_0 / ps_6_0)\n", srf.height.c_str(), srf.shade.c_str());
                    std::string errs;
                    const mmdx::ComPtr vsBlob = mmdx::CompileShaderDxc(worldHlsl, "VSWorldSurface", "vs_6_0", defs, &errs);
                    const mmdx::ComPtr psBlob = mmdx::CompileShaderDxc(worldHlsl, "PSWorldSurface", "ps_6_0", defs, &errs);
                    if (!errs.empty()) printf("%s\n", errs.c_str());
                    if (!vsBlob || !psBlob) { printf("[problem] surface compile failed\n"); ok = false; }
                    else { printf("compile ok (vs: %zu, ps: %zu bytes)\n", vsBlob->GetBufferSize(), psBlob->GetBufferSize()); }
                }
            }
        } else if (pack.type == mmdx::PackType::Screen) {
            // S6: a stage screen pack (screen.hlsl -> PackScreen) compiles through stage_screen.hlsl, like the app does
            const fs::path hostHlsl = mmdx::ExecutableDir() / L"shaders" / L"stage_screen.hlsl";
            if (!fs::is_regular_file(hostHlsl, ec)) {
                printf("[problem] %s not found\n", mmdx::PathToUtf8(hostHlsl).c_str());
                return 1;
            }
            fs::path rel = fs::relative(pack.dir / L"screen.hlsl", hostHlsl.parent_path(), ec);
            if (ec || rel.empty()) rel = pack.dir / L"screen.hlsl";
            std::string inc = mmdx::PathToUtf8(rel);
            std::replace(inc.begin(), inc.end(), '\\', '/');
            uint32_t clampMask = 0;
            const uint32_t texCount = (uint32_t)std::min<size_t>(pack.textures.size(), kMaxTextures);
            for (uint32_t i = 0; i < texCount; ++i)
                if (pack.textures[i].clamp) clampMask |= 1u << i;
            std::vector<std::pair<std::string, std::string>> defines = {
                {"MMDX_PACK", "\"" + inc + "\""},
                {"PACK_TEX_COUNT", std::to_string(texCount)},
                {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"}};
            if (pack.HasCap("inputs.audio")) defines.push_back({"PACK_CAP_INPUTS_AUDIO", "1"});
            printf("compile: PSPackScreen of %s (ps_6_0, MMDX_PACK = %s, %u textures)\n",
                   mmdx::PathToUtf8(hostHlsl.filename()).c_str(), inc.c_str(), texCount);
            std::string errors;
            const mmdx::ComPtr vsBlob = mmdx::CompileShaderDxc(hostHlsl, "VSFullscreen", "vs_6_0", defines, &errors);
            const mmdx::ComPtr psBlob = mmdx::CompileShaderDxc(hostHlsl, "PSPackScreen", "ps_6_0", defines, &errors);
            if (!errors.empty()) printf("%s\n", errors.c_str());
            if (!vsBlob || !psBlob) {
                printf("[problem] screen shader compile failed\n");
                ok = false;
            } else {
                printf("compile ok (vs: %zu, ps: %zu bytes)\n", vsBlob->GetBufferSize(), psBlob->GetBufferSize());
            }
        } else {
        const fs::path mmdHlsl = mmdx::ExecutableDir() / L"shaders" / (effect ? L"effect.hlsl" : L"mmd.hlsl");
        if (!fs::is_regular_file(mmdHlsl, ec)) {
            printf("[problem] %s not found\n", mmdx::PathToUtf8(mmdHlsl).c_str());
            return 1;
        }
        // the surface as a quoted include path relative to the shaders dir, as the app compiles it
        fs::path rel = fs::relative(pack.dir / packFile, mmdHlsl.parent_path(), ec);
        if (ec || rel.empty()) rel = pack.dir / packFile;
        std::string inc = mmdx::PathToUtf8(rel);
        std::replace(inc.begin(), inc.end(), '\\', '/');
        const std::string incDefine = "\"" + inc + "\"";
        // the pack texture state, as the app compiles it (pack_api.hlsli PACK_TEX_*)
        uint32_t clampMask = 0, srgbMask = 0;
        const uint32_t texCount = (uint32_t)std::min<size_t>(pack.textures.size(), kMaxTextures);
        for (uint32_t i = 0; i < texCount; ++i) {
            if (pack.textures[i].clamp) clampMask |= 1u << i;
            if (pack.textures[i].srgb) srgbMask |= 1u << i;
        }
        std::vector<std::pair<std::string, std::string>> defines = {
            {"MMDX_PACK", incDefine},
            {"PACK_TEX_COUNT", std::to_string(texCount)},
            {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"},
            {"PACK_TEX_SRGB_MASK", std::to_string(srgbMask) + "u"}};
        if (effect && pack.stateFloats > 0) {
            defines.push_back({"PACK_STATE", "1"});
            defines.push_back({"PACK_STATE_FLOATS", std::to_string(pack.stateFloats)});
        }
        if (effect && !pack.passes.empty())
            defines.push_back({"PACK_PASS_COUNT", std::to_string(pack.passes.size())});
        // capability macros, as the app's pack hosts define them
        if (pack.HasCap("inputs.scene")) defines.push_back({"PACK_CAP_INPUTS_SCENE", "1"});
        if (pack.HasCap("inputs.audio")) defines.push_back({"PACK_CAP_INPUTS_AUDIO", "1"});
        if (pack.HasCap("surface.output")) defines.push_back({"PACK_CAP_SURFACE_OUTPUT", "1"});
        if (pack.HasCap("inputs.id")) defines.push_back({"PACK_CAP_INPUTS_ID", "1"});
        if (pack.HasCap("passes.history")) defines.push_back({"PACK_CAP_PASSES_HISTORY", "1"});
        if (pack.HasCap("passes.mips")) defines.push_back({"PACK_CAP_PASSES_MIPS", "1"});
        if (pack.HasCap("params.ext")) defines.push_back({"PACK_CAP_PARAMS_EXT", "1"});
        printf("compile: %s of %s (ps_6_0, MMDX_PACK = %s, %u textures)\n",
               effect ? "PSEffect" : "PSPack", mmdx::PathToUtf8(mmdHlsl.filename()).c_str(), incDefine.c_str(), texCount);
        std::string errors;
        const mmdx::ComPtr blob = mmdx::CompileShaderDxc(mmdHlsl, effect ? "PSEffect" : "PSPack", "ps_6_0", defines, &errors);
        if (!errors.empty()) printf("%s\n", errors.c_str());
        if (!blob) {
            printf("[problem] shader compile failed\n");
            ok = false;
        } else {
            printf("compile ok (%zu bytes)\n", blob->GetBufferSize());
        }

        if (effect && pack.stateFloats > 0) {
            printf("compile: PSEffectState of %s (ps_6_0, MMDX_PACK = %s, %u textures)\n",
                   mmdx::PathToUtf8(mmdHlsl.filename()).c_str(), incDefine.c_str(), texCount);
            std::string stateErrors;
            const mmdx::ComPtr stateBlob = mmdx::CompileShaderDxc(mmdHlsl, "PSEffectState", "ps_6_0", defines, &stateErrors);
            if (!stateErrors.empty()) printf("%s\n", stateErrors.c_str());
            if (!stateBlob) {
                printf("[problem] PSEffectState compile failed\n");
                ok = false;
            } else {
                printf("compile ok (%zu bytes)\n", stateBlob->GetBufferSize());
            }
        }

        if (effect) {
            for (const mmdx::ShaderPack::Pass& pass : pack.passes) {
                auto passDefines = defines;
                passDefines.push_back({"PACK_PASS_ENTRY", pass.entry});
                printf("compile: pass %s (PSPackPass, scale %.4g)\n", pass.entry.c_str(), pass.scale);
                std::string passErrors;
                const mmdx::ComPtr passBlob = mmdx::CompileShaderDxc(mmdHlsl, "PSPackPass", "ps_6_0", passDefines, &passErrors);
                if (!passErrors.empty()) printf("%s\n", passErrors.c_str());
                if (!passBlob) {
                    printf("[problem] pass %s compile failed\n", pass.entry.c_str());
                    ok = false;
                } else {
                    printf("compile ok (%zu bytes)\n", passBlob->GetBufferSize());
                }
            }
        }

        if (pack.hasPtSurface) {
            const fs::path giHlsl = mmdx::ExecutableDir() / L"shaders" / L"offline_gi.hlsl";
            if (!fs::is_regular_file(giHlsl, ec)) {
                printf("[problem] %s not found\n", mmdx::PathToUtf8(giHlsl).c_str());
                ok = false;
            } else {
                fs::path ptRel = fs::relative(pack.dir / L"pt_surface.hlsl", giHlsl.parent_path(), ec);
                if (ec || ptRel.empty()) ptRel = pack.dir / L"pt_surface.hlsl";
                std::string ptInc = mmdx::PathToUtf8(ptRel);
                std::replace(ptInc.begin(), ptInc.end(), '\\', '/');
                const std::string ptIncDefine = "\"" + ptInc + "\"";
                std::vector<std::pair<std::string, std::string>> ptDefines = {{"MMDX_PT_PACK", ptIncDefine}};
                if (pack.HasCap("inputs.scene")) ptDefines.push_back({"PACK_CAP_INPUTS_SCENE", "1"});
                if (pack.HasCap("inputs.audio")) ptDefines.push_back({"PACK_CAP_INPUTS_AUDIO", "1"});
                if (pack.HasCap("surface.output")) ptDefines.push_back({"PACK_CAP_SURFACE_OUTPUT", "1"});
                if (pack.HasCap("inputs.id")) ptDefines.push_back({"PACK_CAP_INPUTS_ID", "1"});
                if (pack.HasCap("passes.history")) ptDefines.push_back({"PACK_CAP_PASSES_HISTORY", "1"});
                if (pack.HasCap("passes.mips")) ptDefines.push_back({"PACK_CAP_PASSES_MIPS", "1"});
                if (pack.HasCap("params.ext")) ptDefines.push_back({"PACK_CAP_PARAMS_EXT", "1"});
                printf("compile: CSRender of %s (cs_6_5, MMDX_PT_PACK = %s)\n",
                       mmdx::PathToUtf8(giHlsl.filename()).c_str(), ptIncDefine.c_str());
                std::string ptErrors;
                const mmdx::ComPtr ptBlob = mmdx::CompileShaderDxc(giHlsl, "CSRender", "cs_6_5", ptDefines, &ptErrors);
                if (!ptErrors.empty()) printf("%s\n", ptErrors.c_str());
                if (!ptBlob) {
                    printf("[problem] pt_surface shader compile failed\n");
                    ok = false;
                } else {
                    printf("compile ok (%zu bytes)\n", ptBlob->GetBufferSize());
                }

                const fs::path ptHlsl = mmdx::ExecutableDir() / L"shaders" / L"pathtrace.hlsl";
                if (!fs::is_regular_file(ptHlsl, ec)) {
                    printf("[problem] %s not found\n", mmdx::PathToUtf8(ptHlsl).c_str());
                    ok = false;
                } else {
                    printf("compile: CSPathTrace of %s (cs_6_5, MMDX_PT_PACK = %s)\n",
                           mmdx::PathToUtf8(ptHlsl.filename()).c_str(), ptIncDefine.c_str());
                    std::string ptTraceErrors;
                    const mmdx::ComPtr ptTraceBlob =
                        mmdx::CompileShaderDxc(ptHlsl, "CSPathTrace", "cs_6_5", ptDefines, &ptTraceErrors);
                    if (!ptTraceErrors.empty()) printf("%s\n", ptTraceErrors.c_str());
                    if (!ptTraceBlob) {
                        printf("[problem] CSPathTrace shader compile failed\n");
                        ok = false;
                    } else {
                        printf("compile ok (%zu bytes)\n", ptTraceBlob->GetBufferSize());
                    }
                }
            }
        }
        }
    }

    printf(ok ? "OK\n" : "PROBLEMS\n");
    return ok ? 0 : 1;
}
