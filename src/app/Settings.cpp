#include "app/Settings.h"

#include "imgui.h"

#include "core/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace mmdx {

ShaderChoice AppSettings::CharacterShader(const std::string& id) const {
    auto it = characterShaders.find(id);
    return it == characterShaders.end() ? ShaderChoice{} : it->second;
}

void AppSettings::SetCharacterShader(const std::string& id, const ShaderChoice& choice) {
    if (choice.pack.empty() && choice.remembered.empty() && choice.materials.empty())
        characterShaders.erase(id);   // nothing left to keep
    else characterShaders[id] = choice;
}

// M6a: MME choice mode names ("none" | "auto" | "file").
static const char* MmeModeName(MmeChoice::Mode m) {
    switch (m) {
        case MmeChoice::Auto: return "auto";
        case MmeChoice::File: return "file";
        default: return "none";
    }
}

static bool ParseMmeMode(const std::string& v, MmeChoice::Mode& out) {
    const std::string lo = [&] {
        std::string s = v;
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }();
    if (lo == "none") { out = MmeChoice::None; return true; }
    if (lo == "auto") { out = MmeChoice::Auto; return true; }
    if (lo == "file") { out = MmeChoice::File; return true; }
    return false;
}

MmeChoice AppSettings::CharacterMme(const std::string& id) const {
    auto it = characterMmes.find(id);
    return it == characterMmes.end() ? MmeChoice{} : it->second;   // characters default to None
}

void AppSettings::SetCharacterMme(const std::string& id, const MmeChoice& choice) {
    if (choice.mode == MmeChoice::None) characterMmes.erase(id);
    else characterMmes[id] = choice;
}

MmeChoice AppSettings::StageMme(const std::string& id) const {
    auto it = stageMmes.find(id);
    if (it != stageMmes.end()) return it->second;
    return MmeChoice{MmeChoice::Auto, {}};   // stages default to Auto
}

void AppSettings::SetStageMme(const std::string& id, const MmeChoice& choice) {
    static const MmeChoice kDefault{MmeChoice::Auto, {}};
    if (choice == kDefault) stageMmes.erase(id);
    else stageMmes[id] = choice;
}

MaterialChoice AppSettings::CharacterMaterial(const std::string& id) const {
    auto it = characterMaterials.find(id);
    return it == characterMaterials.end() ? MaterialChoice{} : it->second;
}

void AppSettings::SetCharacterMaterial(const std::string& id, const MaterialChoice& choice) {
    if (!choice.ray) characterMaterials.erase(id);   // Default removes the entry
    else characterMaterials[id] = choice;
}

namespace {

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

bool ParseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "true") { out = true; return true; }
    if (v == "0" || v == "false") { out = false; return true; }
    return false;
}

bool ParseInt(const std::string& v, int& out) {
    int value = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), value);
    if (ec != std::errc() || p != v.data() + v.size()) {
        char* end = nullptr;
        long l = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0') return false;
        value = (int)l;
    }
    out = value;
    return true;
}

bool ParseFloat(const std::string& v, float& out) {
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) return false;
    out = f;
    return true;
}

// Built-in editor / viewport key bindings (studio bone / camera tools). The action names are stable ids; users may
// rebind them in the settings popup (saved as "key<Action>=<ImGuiKey int>" ini lines). The viewport camera fly
// keys (W/A/S/D/E/Q/Space) stay hardcoded.
struct DefaultKey { const char* action; int key; };
const DefaultKey kDefaultKeys[] = {
    {"toolMove",      ImGuiKey_W},
    {"toolRotate",    ImGuiKey_E},
    {"toolAutoIk",    ImGuiKey_K},
    {"toggleBones",   ImGuiKey_C},
    {"registerPose",  ImGuiKey_I},
    {"togglePlay",    ImGuiKey_Space},
};

int FindDefaultKey(const char* action) {
    for (const DefaultKey& dk : kDefaultKeys)
        if (std::strcmp(dk.action, action) == 0) return dk.key;
    return -1;
}

} // namespace

int AppSettings::KeyFor(const char* action) const {
    auto it = keys.find(action);
    if (it != keys.end()) return it->second;
    return FindDefaultKey(action);
}

const VideoProbe* AppSettings::FindVideoProbe(uint64_t key) const {
    for (const VideoProbe& p : videoProbes)
        if (p.key == key) return &p;
    return nullptr;
}

void AppSettings::SetVideoProbe(uint64_t key, double secondsPerFrame) {
    videoProbes.erase(std::remove_if(videoProbes.begin(), videoProbes.end(),
                                     [&](const VideoProbe& p) { return p.key == key; }),
                      videoProbes.end());
    videoProbes.push_back({key, secondsPerFrame});
    constexpr size_t kMaxProbes = 24;  // newest kept
    if (videoProbes.size() > kMaxProbes) videoProbes.erase(videoProbes.begin(), videoProbes.end() - kMaxProbes);
}

bool AppSettings::Load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));

        bool b;
        int i;
        float f;
        if (key == "libraryPath") libraryPath = value;
        else if (key == "nickname") nickname = value;
        else if (key == "theme" && ParseInt(value, i)) theme = std::clamp(i, 0, 2);
        else if (key == "language" && ParseInt(value, i)) language = std::clamp(i, 0, 4);
        else if (key == "vsync" && ParseBool(value, b)) vsync = b;
        else if (key == "msaa" && ParseInt(value, i)) msaa = i;
        else if (key == "renderScale" && ParseFloat(value, f)) renderScale = f;
        else if (key == "volume" && ParseFloat(value, f)) volume = f;
        else if (key == "drawEdges" && ParseBool(value, b)) drawEdges = b;
        else if (key == "motionLighting" && ParseBool(value, b)) motionLighting = b;
        else if (key == "physics" && ParseBool(value, b)) physics = b;
        else if (key == "mcp" && ParseBool(value, b)) mcpEnabled = b;
        else if (key == "graphicsPreset" && ParseInt(value, i)) graphicsPreset = i;
        else if (key == "shadows" && ParseBool(value, b)) shadows = b;
        else if (key == "ssao" && ParseBool(value, b)) ssao = b;
        else if (key == "ssr" && ParseBool(value, b)) ssr = b;
        else if (key == "bloom" && ParseBool(value, b)) bloom = b;
        else if (key == "autoLuminous" && ParseBool(value, b)) autoLuminous = b;
        else if (key == "screenLights" && ParseBool(value, b)) screenLights = b;
        else if (key == "mmeEnabled" && ParseBool(value, b)) mmeEnabled = b;
        else if (key == "rayMmdEnabled" && ParseBool(value, b)) rayMmdEnabled = b;
        else if (key == "mmeWorkflow") {
            const std::string v = ToLowerAscii(value);
            if (v == "manual") mmeWorkflow = 0;
            else if (v == "auto") mmeWorkflow = 1;
        }
        else if (key == "taa" && ParseBool(value, b)) taa = b;
        else if (key == "shadowMapSize" && ParseInt(value, i)) shadowMapSize = i;
        else if (key == "lighting" && ParseInt(value, i)) lighting = i;
        else if (key == "exposure" && ParseFloat(value, f)) exposure = f;
        else if (key == "renderPath" && ParseInt(value, i)) renderPath = i;
        else if (key == "studioRenderer" && ParseInt(value, i)) studioRenderer = i;
        else if (key == "studioRibbonTab" && ParseInt(value, i)) studioRibbonTab = i;
        else if (key == "studioRibbonCollapsed" && ParseBool(value, b)) studioRibbonCollapsed = b;
        else if (key == "upscaler" && ParseInt(value, i)) upscaler = i;
        else if (key == "upscalerQuality" && ParseInt(value, i)) upscalerQuality = i;
        else if (key == "dlssFrameGen" && ParseInt(value, i)) dlssFrameGen = i;
        else if (key == "dlssModel") {
            const std::string v = ToLowerAscii(value);
            if (v == "auto") dlssModel = 0;
            else if (v == "cnn") dlssModel = 1;
            else if (v == "transformer") dlssModel = 2;
            else if (v == "transformer2") dlssModel = 3;
        }
        else if (key == "dlssRayReconstruction" && ParseBool(value, b)) dlssRayReconstruction = b;
        else if (key == "rtaoRays" && ParseInt(value, i)) rtaoRays = i;
        else if (key == "ptSamples" && ParseInt(value, i)) ptSamples = i;
        else if (key == "ptBounces" && ParseInt(value, i)) ptBounces = i;
        else if (key == "dof" && ParseBool(value, b)) dof = b;
        else if (key == "dofAperture" && ParseFloat(value, f)) dofAperture = f;
        else if (key == "volumetric" && ParseBool(value, b)) volumetric = b;
        else if (key == "volumetricDensity" && ParseFloat(value, f)) volumetricDensity = f;
        else if (key == "volumetricLightGain" && ParseFloat(value, f)) volumetricLightGain = f;
        else if (key == "volumetricAmbientGain" && ParseFloat(value, f)) volumetricAmbientGain = f;
        else if (key == "volumetricRange" && ParseFloat(value, f)) volumetricRange = f;
        // S9d: 0 = froxel (default), 1 = march; volumetricMultiScatter=1|0 (froxel only)
        else if (key == "volumetricMode" && ParseInt(value, i)) volumetricMode = i;
        else if (key == "volumetricMultiScatter" && ParseBool(value, b)) volumetricMultiScatter = b;
        else if (key == "volumetricFull" && ParseBool(value, b)) volumetricFull = b;
        else if (key == "bloomConvolution" && ParseBool(value, b)) bloomConvolution = b;
        else if (key == "bloomIntensity" && ParseFloat(value, f)) bloomIntensity = std::clamp(f, 0.0f, 1.5f);
        else if (key == "bloomThreshold" && ParseFloat(value, f)) bloomThreshold = std::clamp(f, 0.2f, 4.0f);
        else if (key == "bloomRadius" && ParseFloat(value, f)) bloomLook.radius = std::clamp(f, 0.0f, 1.0f);
        else if (key == "bloomBlend" && ParseInt(value, i)) bloomLook.blend = std::clamp(i, 0, 1);
        else if (key == "bloomDepthMask" && ParseFloat(value, f)) bloomLook.depthMask = std::clamp(f, 0.0f, 1.0f);
        else if (key == "bloomKernelPreset" && ParseInt(value, i)) bloomKernelPreset = std::clamp(i, 0, 4);
        else if (key == "bloomKernel") {
            // bloomKernel=rays,rotation,spikeLength,spikeIntensity,glowRadius,glowIntensity,dispersion,anamorphic
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos <= value.size()) {
                const size_t comma = value.find(',', pos);
                const size_t end = comma == std::string::npos ? value.size() : comma;
                parts.push_back(Trim(value.substr(pos, end - pos)));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            float v[8];
            bool ok = parts.size() == 8;
            for (size_t n = 0; ok && n < 8; ++n) ok = ParseFloat(parts[n], v[n]) && std::isfinite(v[n]);
            if (ok) {
                BloomKernel k;
                int rays = (int)v[0];
                if (rays < 0) rays = 0;
                if (rays > 16) rays = 16;
                k.rays = rays & ~1;
                k.rotation = std::clamp(v[1], 0.0f, 180.0f);
                k.spikeLength = std::clamp(v[2], 0.0f, 1.0f);
                k.spikeIntensity = std::clamp(v[3], 0.0f, 1.0f);
                k.glowRadius = std::clamp(v[4], 0.0f, 1.0f);
                k.glowIntensity = std::clamp(v[5], 0.0f, 1.0f);
                k.dispersion = std::clamp(v[6], 0.0f, 1.0f);
                k.anamorphic = std::clamp(v[7], 0.0f, 1.0f);
                bloomLook.kernel = k;
            }
        }
        else if (key == "colorLut") colorLut = value;
        else if (key == "lutIntensity" && ParseFloat(value, f)) lutIntensity = f;
        else if (key == "videoResolution" && ParseInt(value, i)) video.resolution = i;
        else if (key == "videoFps" && ParseInt(value, i)) video.fps = i;
        else if (key == "videoBitrate" && ParseInt(value, i)) video.bitrateMbps = i;
        else if (key == "videoQuality" && ParseInt(value, i)) video.quality = i;
        else if (key == "videoRenderer" && ParseInt(value, i)) video.renderer = i;
        else if (key == "videoBloom" && ParseBool(value, b)) video.bloom = b;
        else if (key == "videoBloomConvolution" && ParseBool(value, b)) video.bloomConvolution = b;
        else if (key == "videoVolumetric" && ParseBool(value, b)) video.volumetric = b;
        else if (key == "videoVolumetricDensity" && ParseFloat(value, f)) video.volumetricDensity = f;
        else if (key == "videoVolumetricFull" && ParseBool(value, b)) video.volumetricFull = b;
        else if (key == "videoDof" && ParseBool(value, b)) video.dof = b;
        else if (key == "videoDofAperture" && ParseFloat(value, f)) video.dofAperture = f;
        else if (key == "videoAlpha" && ParseBool(value, b)) video.alpha = b;
        else if (key == "videoProbe") {
            // "<key hex>:<seconds per frame>"
            const size_t colon = value.find(':');
            if (colon != std::string::npos) {
                VideoProbe pr;
                char* end = nullptr;
                pr.key = std::strtoull(value.c_str(), &end, 16);
                if (ParseFloat(value.substr(colon + 1), f) && f > 0.0f && std::isfinite(f)) {
                    pr.secondsPerFrame = f;
                    SetVideoProbe(pr.key, pr.secondsPerFrame);
                }
            }
        }
        else if (key == "leaderboardUrl") leaderboardUrl = value;
        else if (key == "updateFeedUrl") updateFeedUrl = value;
        else if (key == "galleryIndex") {
            if (!value.empty()) galleryIndexes.push_back(value);
        }
        else if (key == "characterScale") {
            // characterScale=<scale>|<character id>
            const size_t bar = value.find('|');
            if (bar != std::string::npos && ParseFloat(value.substr(0, bar), f) && f > 0.0f && std::isfinite(f))
                characterScales[value.substr(bar + 1)] = std::clamp(f, 0.25f, 4.0f);
        }
        else if (key == "characterShader") {
            // characterShader=<pack id>|<key>:<value>,<key>:<value>|<character id>
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            if (b != std::string::npos && a > 0) {
                ShaderChoice c;
                c.pack = value.substr(0, a);
                const std::string list = value.substr(a + 1, b - a - 1);
                size_t pos = 0;
                while (pos < list.size()) {
                    size_t end = list.find(',', pos);
                    if (end == std::string::npos) end = list.size();
                    const std::string item = list.substr(pos, end - pos);
                    const size_t colon = item.find(':');
                    if (colon != std::string::npos && ParseFloat(item.substr(colon + 1), f) && std::isfinite(f))
                        c.params[item.substr(0, colon)] = f;
                    pos = end + 1;
                }
                ShaderChoice& dst = characterShaders[value.substr(b + 1)];   // keeps memos loaded before
                dst.pack = std::move(c.pack);
                dst.params = std::move(c.params);
            }
        }
        else if (key == "characterShaderMemo") {
            // characterShaderMemo=<pack id>|<key>:<value>,...|<texture folder or ->|<character id>: the last settings of a pack
            // this character is not using right now (restored when it is picked again)
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            const size_t c2 = b == std::string::npos ? b : value.find('|', b + 1);
            if (a != std::string::npos && a > 0 && c2 != std::string::npos && c2 + 1 < value.size()) {
                ShaderMemo m;
                const std::string list = value.substr(a + 1, b - a - 1);
                size_t pos = 0;
                while (pos < list.size()) {
                    size_t end = list.find(',', pos);
                    if (end == std::string::npos) end = list.size();
                    const std::string item = list.substr(pos, end - pos);
                    const size_t colon = item.find(':');
                    if (colon != std::string::npos && ParseFloat(item.substr(colon + 1), f) && std::isfinite(f))
                        m.params[item.substr(0, colon)] = f;
                    pos = end + 1;
                }
                const std::string folder = value.substr(b + 1, c2 - b - 1);
                if (folder != "-") m.textureFolder = folder;
                characterShaders[value.substr(c2 + 1)].remembered[value.substr(0, a)] = std::move(m);
            }
        }
        else if (key == "characterShaderTextures") {
            // characterShaderTextures=<utf-8 folder>|<character id>: the shader choice's per-character
            // texture folder (a separate line, the ids in characterShader= are already last)
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size()) {
                ShaderChoice& c = characterShaders[value.substr(bar + 1)];
                c.textureFolder = value.substr(0, bar);
            }
        }
        else if (key == "characterShaderMaterial") {
            // characterShaderMaterial=<class 0..5 or -1 = default shading>|<character id>|<material name>
            // (the name last: PMX material names may contain '|', library ids are paths and cannot)
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            if (b != std::string::npos && b > a + 1 && b + 1 < value.size() && ParseInt(value.substr(0, a), i) &&
                i >= kMaterialPackOff && i <= (int)PackClass::Weapon)
                characterShaders[value.substr(a + 1, b - a - 1)].materials[value.substr(b + 1)] = i;
        }
        else if (key == "characterShaderSuggested") {
            // characterShaderSuggested=<pack id>|<character id>: this pack was already suggested for the character
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size()) characterShaderSuggested.insert(value);
        }
        else if (key == "characterMmeSuggested") {
            // characterMmeSuggested=<character id>: the model's own MME material was already suggested
            if (!value.empty()) characterMmeSuggested.insert(value);
        }
        else if (key == "characterMme" || key == "stageMme") {
            // characterMme=<mode>|<file or ->|<character id> / stageMme=<mode>|<file or ->|<stage id>
            // (mode = none|auto|file; the id last, like the other lines: library ids hold no '|')
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            if (b != std::string::npos && b + 1 < value.size()) {
                MmeChoice c;
                if (ParseMmeMode(value.substr(0, a), c.mode)) {
                    const std::string file = value.substr(a + 1, b - a - 1);
                    if (c.mode == MmeChoice::File && !file.empty() && file != "-") c.file = Utf8ToPath(file);
                    if (key == "characterMme") SetCharacterMme(value.substr(b + 1), c);
                    else SetStageMme(value.substr(b + 1), c);
                }
            }
        }
        else if (key == "characterMaterial") {
            // characterMaterial=<ray|default>|<emd path or empty>|<character id> (the id last, like the other lines)
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            if (b != std::string::npos && b + 1 < value.size()) {
                MaterialChoice c;
                c.ray = value.compare(0, a, "ray") == 0;
                c.emd = value.substr(a + 1, b - a - 1);
                if (c.ray) characterMaterials[value.substr(b + 1)] = c;
            }
        }
        else if (key == "characterMaterialSuggested") {
            // characterMaterialSuggested=<character id>: Ray-MMD material data was already suggested
            if (!value.empty()) characterMaterialSuggested.insert(value);
        }
        else if (key == "stageVariant") {
            // stageVariant=<variant id>|<stage id> (the stage id last, like the other per-id lines)
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size())
                stageVariants[value.substr(bar + 1)] = value.substr(0, bar);
        }
        else if (key == "paramGroupOpen") {
            // paramGroupOpen=<pack>|<group>|0|1
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? std::string::npos : value.find('|', a + 1);
            if (a != std::string::npos && b != std::string::npos && a > 0 && b > a + 1 && b + 1 < value.size()) {
                const std::string stateStr = value.substr(b + 1);
                bool open = (stateStr == "1" || stateStr == "true");
                paramGroupOpen[value.substr(0, b)] = open;
            }
        }
        else if (key == "characterShaderAuto") {
            // retired (the app no longer auto-applies packs): ignored
        }
        else if (key == "packTextureFolder") {
            // packTextureFolder=<pack id>|<utf-8 folder>
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size())
                packTextureFolders[value.substr(0, bar)] = value.substr(bar + 1);
        }
        else if (key == "effect") {
            // effect=<pack id>|<enabled 0/1>|<key>:<value>,<key>:<value>|<texture folder or ->
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            const size_t c = b == std::string::npos ? b : value.find('|', b + 1);
            if (a != std::string::npos && a > 0 && b != std::string::npos) {
                EffectStackEntry e;
                e.pack = value.substr(0, a);
                int en = 1;
                if (ParseInt(value.substr(a + 1, b - a - 1), en)) e.enabled = en != 0;
                if (c != std::string::npos) {
                    const std::string list = value.substr(b + 1, c - b - 1);
                    size_t pos = 0;
                    while (pos < list.size()) {
                        size_t end = list.find(',', pos);
                        if (end == std::string::npos) end = list.size();
                        const std::string item = list.substr(pos, end - pos);
                        const size_t colon = item.find(':');
                        if (colon != std::string::npos && ParseFloat(item.substr(colon + 1), f) && std::isfinite(f))
                            e.params[item.substr(0, colon)] = f;
                        pos = end + 1;
                    }
                    const std::string folder = value.substr(c + 1);
                    if (!folder.empty() && folder != "-") e.textureFolder = folder;
                }
                effectStack.push_back(std::move(e));
            }
        }
        else if (key == "world") {
            // world=<pack id>|<enabled 0/1>|<key>:<value>,<key>:<value>|<texture folder or -> (same as effect=)
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            const size_t c = b == std::string::npos ? b : value.find('|', b + 1);
            if (a != std::string::npos && a > 0 && b != std::string::npos) {
                EffectStackEntry e;
                e.pack = value.substr(0, a);
                int en = 1;
                if (ParseInt(value.substr(a + 1, b - a - 1), en)) e.enabled = en != 0;
                if (c != std::string::npos) {
                    const std::string list = value.substr(b + 1, c - b - 1);
                    size_t pos = 0;
                    while (pos < list.size()) {
                        size_t end = list.find(',', pos);
                        if (end == std::string::npos) end = list.size();
                        const std::string item = list.substr(pos, end - pos);
                        const size_t colon = item.find(':');
                        if (colon != std::string::npos && ParseFloat(item.substr(colon + 1), f) && std::isfinite(f))
                            e.params[item.substr(0, colon)] = f;
                        pos = end + 1;
                    }
                    const std::string folder = value.substr(c + 1);
                    if (!folder.empty() && folder != "-") e.textureFolder = folder;
                }
                worldStack.push_back(std::move(e));
            }
        }
        else if (key == "mmeEffect") {
            // mmeEffect=<enabled 0|1>|<path> (path last: effect paths may hold '|')
            const size_t a = value.find('|');
            if (a != std::string::npos && a + 1 < value.size()) {
                MmeEffectEntry e;
                int en = 1;
                if (ParseInt(value.substr(0, a), en)) e.enabled = en != 0;
                const std::string path = value.substr(a + 1);
                if (!path.empty() && path != "-") e.file = Utf8ToPath(path);
                if (!e.file.empty()) mmeEffects.push_back(std::move(e));
            }
        }
        else if (key == "shaderSort") {
            const size_t sep = value.find_first_of(",|");
            if (sep != std::string::npos) {
                if (ParseInt(value.substr(0, sep), i)) shaderSort[0] = std::clamp(i, 0, 3);
                if (ParseInt(value.substr(sep + 1), i)) shaderSort[1] = std::clamp(i, 0, 3);
            } else if (ParseInt(value, i)) {
                shaderSort[0] = std::clamp(i, 0, 3);
            }
        }
        else if (key == "shaderType") {
            const size_t sep = value.find_first_of(",|");
            if (sep != std::string::npos) {
                if (ParseInt(value.substr(0, sep), i)) shaderType[0] = std::clamp(i, 0, 4);
                if (ParseInt(value.substr(sep + 1), i)) shaderType[1] = std::clamp(i, 0, 4);
            } else if (ParseInt(value, i)) {
                shaderType[0] = std::clamp(i, 0, 4);
            }
        }
        else if (key == "playPanelTab" && ParseInt(value, i)) playPanelTab = std::clamp(i, 0, 2);
        else if (key == "lastCharacter") lastCharacter = value;
        else if (key == "lastStage") lastStage = value;
        else if (key == "lastSong") lastSong = value;
        else if (key == "songCamera") {   // songCamera=<camera file name>|<song id> (the id last: ids may hold '|')
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size())
                songCameras[value.substr(bar + 1)] = value.substr(0, bar);
        }
        else if (key == "songStage") {   // songStage=<stage id|->|<song id> (the id last: ids may hold '|')
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size())
                songStages[value.substr(bar + 1)] = value.substr(0, bar);
        }
        else if (key == "songAudio") {   // songAudio=<audio file name>|<song id> (the id last: ids may hold '|')
            const size_t bar = value.find('|');
            if (bar != std::string::npos && bar > 0 && bar + 1 < value.size())
                songAudios[value.substr(bar + 1)] = value.substr(0, bar);
        }
        else if (key == "songVideo") {
            // songVideo=<overlay|background|off>[:<black|screen|add|normal>]|<file name>|<song id>
            // (the file name may be empty; the song id goes last because song ids may hold '|')
            const size_t bar1 = value.find('|');
            if (bar1 != std::string::npos && bar1 > 0) {
                const size_t bar2 = value.find('|', bar1 + 1);
                if (bar2 != std::string::npos && bar2 + 1 < value.size())
                    songVideos[value.substr(bar2 + 1)] = value.substr(0, bar2);
            }
        }
        else if (key == "songFormationPart" || key == "songFormationCenter") {
            // songFormationPart=<dance file>|<character id>|<song id>, songFormationCenter=<dance file>|<song id>
            // (file names and library paths hold no '|'; the song id goes last)
            const size_t bar1 = value.find('|');
            if (bar1 != std::string::npos && bar1 > 0) {
                const std::string dance = value.substr(0, bar1);
                if (key == "songFormationCenter") {
                    if (bar1 + 1 < value.size()) songFormations[value.substr(bar1 + 1)].centerDance = dance;
                } else {
                    const size_t bar2 = value.find('|', bar1 + 1);
                    if (bar2 != std::string::npos && bar2 > bar1 + 1 && bar2 + 1 < value.size())
                        songFormations[value.substr(bar2 + 1)].byDance[dance] = value.substr(bar1 + 1, bar2 - bar1 - 1);
                }
            }
        }
        else if (key == "cameraInvertX" && ParseBool(value, b)) cameraInvertX = b;
        else if (key == "cameraInvertY" && ParseBool(value, b)) cameraInvertY = b;
        else if (key == "cameraRotateSpeed" && ParseFloat(value, f)) cameraRotateSpeed = std::clamp(f, 0.05f, 10.0f);
        else if (key == "cameraPanSpeed" && ParseFloat(value, f)) cameraPanSpeed = std::clamp(f, 0.05f, 10.0f);
        else if (key == "cameraZoomSpeed" && ParseFloat(value, f)) cameraZoomSpeed = std::clamp(f, 0.05f, 10.0f);
        else if (key == "cameraFovSpeed" && ParseFloat(value, f)) cameraFovSpeed = std::clamp(f, 0.05f, 10.0f);
        else if (key == "cameraFlySpeed" && ParseFloat(value, f)) cameraFlySpeed = std::clamp(f, 0.05f, 20.0f);
        else if (key == "editorPrecisionFactor" && ParseFloat(value, f)) editorPrecisionFactor = std::clamp(f, 0.01f, 1.0f);
        else if (key == "showEnglishNames" && ParseBool(value, b)) showEnglishNames = b;
        else if (key == "boneOverlayScale" && ParseFloat(value, f)) boneOverlayScale = std::clamp(f, 0.5f, 2.5f);
        else if (key.rfind("key", 0) == 0 && key.size() > 4) {
            // key<Action>=<ImGuiKey int>
            if (ParseInt(value, i) && i >= ImGuiKey_NamedKey_BEGIN && i < ImGuiKey_NamedKey_END) keys[key.substr(3)] = i;
        }
        else if (key == "recentProject" && !value.empty() && recentProjects.size() < 8) recentProjects.push_back(value);
        else if (key == "windowWidth" && ParseInt(value, i)) windowWidth = i;
        else if (key == "windowHeight" && ParseInt(value, i)) windowHeight = i;
    }

    // Clamp msaa to {1,2,4,8}, else 4.
    if (msaa != 1 && msaa != 2 && msaa != 4 && msaa != 8) msaa = 4;
    renderScale = std::clamp(renderScale, 0.5f, 2.0f);
    graphicsPreset = std::clamp(graphicsPreset, 0, 4);
    if (shadowMapSize != 1024 && shadowMapSize != 2048 && shadowMapSize != 4096) shadowMapSize = 2048;
    lighting = std::clamp(lighting, 0, 3);   // the ordinary presets; "스테이지 조명" (4) is a run-time choice
    exposure = std::clamp(exposure, 0.5f, 2.0f);
    renderPath = std::clamp(renderPath, 0, 2);
    studioRenderer = std::clamp(studioRenderer, 0, 3);
    studioRibbonTab = std::clamp(studioRibbonTab, 0, 5);
    upscaler = std::clamp(upscaler, 0, 3);
    upscalerQuality = std::clamp(upscalerQuality, 0, 4);
    if (dlssFrameGen != 2 && dlssFrameGen != 3 && dlssFrameGen != 4) dlssFrameGen = 0;
    dlssModel = std::clamp(dlssModel, 0, 3);
    rtaoRays = std::clamp(rtaoRays, 1, 64);
    ptSamples = std::clamp(ptSamples, 1, 4);
    ptBounces = std::clamp(ptBounces, 1, 6);
    dofAperture = std::clamp(dofAperture, 0.2f, 3.0f);
    volumetricDensity = std::clamp(volumetricDensity, 0.25f, 4.0f);
    volumetricLightGain = std::clamp(volumetricLightGain, 0.0f, 8.0f);
    volumetricAmbientGain = std::clamp(volumetricAmbientGain, 0.0f, 2.0f);
    volumetricRange = volumetricRange < 50.0f ? 0.0f : std::min(volumetricRange, 1000.0f);
    volumetricMode = std::clamp(volumetricMode, 0, 1);
    lutIntensity = std::clamp(lutIntensity, 0.0f, 1.0f);
    volume = std::clamp(volume, 0.0f, 1.0f);
    video.Clamp();
    windowWidth = std::clamp(windowWidth, 640, 7680);
    windowHeight = std::clamp(windowHeight, 360, 4320);
    return true;
}

float AppSettings::CharacterScale(const std::string& id) const {
    auto it = characterScales.find(id);
    return it == characterScales.end() ? 1.0f : it->second;
}

void AppSettings::SetCharacterScale(const std::string& id, float scale) {
    if (std::fabs(scale - 1.0f) < 0.005f) characterScales.erase(id);
    else characterScales[id] = std::clamp(scale, 0.25f, 4.0f);
}

std::string AppSettings::StageVariant(const std::string& stageId) const {
    const auto it = stageVariants.find(stageId);
    return it == stageVariants.end() ? std::string() : it->second;
}

void AppSettings::SetStageVariant(const std::string& stageId, const std::string& variantId) {
    if (variantId.empty()) stageVariants.erase(stageId);
    else stageVariants[stageId] = variantId;
}

namespace {
bool SamePathText(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (std::tolower((unsigned char)x) != std::tolower((unsigned char)y)) return false;
    }
    return true;
}
} // namespace

void AppSettings::RemoveRecentProject(const std::string& path) {
    recentProjects.erase(std::remove_if(recentProjects.begin(), recentProjects.end(),
                                        [&](const std::string& r) { return SamePathText(r, path); }),
                         recentProjects.end());
}

void AppSettings::AddRecentProject(const std::string& path) {
    RemoveRecentProject(path);
    recentProjects.insert(recentProjects.begin(), path);
    if (recentProjects.size() > 8) recentProjects.resize(8);
}

bool AppSettings::Save(const std::filesystem::path& file) const {
    if (readOnly) return true;   // --classic: the session's look must never reach the ini
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"w") != 0 || !f) return false;

    std::fprintf(f, "libraryPath=%s\n", libraryPath.c_str());
    std::fprintf(f, "nickname=%s\n", nickname.c_str());
    std::fprintf(f, "theme=%d\n", theme);
    std::fprintf(f, "vsync=%d\n", vsync ? 1 : 0);
    std::fprintf(f, "msaa=%d\n", msaa);
    std::fprintf(f, "renderScale=%.3f\n", renderScale);
    std::fprintf(f, "volume=%.3f\n", volume);
    std::fprintf(f, "drawEdges=%d\n", drawEdges ? 1 : 0);
    std::fprintf(f, "motionLighting=%d\n", motionLighting ? 1 : 0);
    std::fprintf(f, "physics=%d\n", physics ? 1 : 0);
    std::fprintf(f, "mcp=%d\n", mcpEnabled ? 1 : 0);
    std::fprintf(f, "graphicsPreset=%d\n", graphicsPreset);
    std::fprintf(f, "shadows=%d\nssao=%d\nssr=%d\nbloom=%d\ntaa=%d\n", shadows ? 1 : 0, ssao ? 1 : 0, ssr ? 1 : 0,
                 bloom ? 1 : 0, taa ? 1 : 0);
    std::fprintf(f, "autoLuminous=%d\n", autoLuminous ? 1 : 0);
    std::fprintf(f, "screenLights=%d\n", screenLights ? 1 : 0);
    std::fprintf(f, "mmeEnabled=%d\n", mmeEnabled ? 1 : 0);
    std::fprintf(f, "rayMmdEnabled=%d\n", rayMmdEnabled ? 1 : 0);
    std::fprintf(f, "mmeWorkflow=%s\n", mmeWorkflow == 1 ? "auto" : "manual");
    std::fprintf(f, "shadowMapSize=%d\n", shadowMapSize);
    std::fprintf(f, "lighting=%d\n", lighting);
    std::fprintf(f, "exposure=%.3f\n", exposure);
    std::fprintf(f, "renderPath=%d\n", renderPath);
    std::fprintf(f, "studioRenderer=%d\n", studioRenderer);
    std::fprintf(f, "studioRibbonTab=%d\n", studioRibbonTab);
    std::fprintf(f, "studioRibbonCollapsed=%d\n", studioRibbonCollapsed ? 1 : 0);
    std::fprintf(f, "upscaler=%d\n", upscaler);
    std::fprintf(f, "upscalerQuality=%d\n", upscalerQuality);
    std::fprintf(f, "dlssFrameGen=%d\n", dlssFrameGen);
    static const char* const kDlssModels[] = {"auto", "cnn", "transformer", "transformer2"};
    std::fprintf(f, "dlssModel=%s\n", kDlssModels[std::clamp(dlssModel, 0, 3)]);
    std::fprintf(f, "dlssRayReconstruction=%d\n", dlssRayReconstruction ? 1 : 0);
    std::fprintf(f, "rtaoRays=%d\n", rtaoRays);
    std::fprintf(f, "ptSamples=%d\n", ptSamples);
    std::fprintf(f, "ptBounces=%d\n", ptBounces);
    std::fprintf(f, "dof=%d\ndofAperture=%.3f\n", dof ? 1 : 0, dofAperture);
    std::fprintf(f, "volumetric=%d\nvolumetricDensity=%.3f\nvolumetricFull=%d\n", volumetric ? 1 : 0, volumetricDensity,
                 volumetricFull ? 1 : 0);
    std::fprintf(f, "volumetricLightGain=%.3f\nvolumetricAmbientGain=%.3f\n", volumetricLightGain, volumetricAmbientGain);
    std::fprintf(f, "volumetricRange=%.0f\n", volumetricRange);
    std::fprintf(f, "volumetricMode=%d\nvolumetricMultiScatter=%d\n", volumetricMode, volumetricMultiScatter ? 1 : 0);
    std::fprintf(f, "bloomConvolution=%d\n", bloomConvolution ? 1 : 0);
    std::fprintf(f, "bloomIntensity=%.3f\nbloomThreshold=%.3f\n", bloomIntensity, bloomThreshold);
    std::fprintf(f, "bloomRadius=%.3f\nbloomBlend=%d\nbloomDepthMask=%.3f\n", bloomLook.radius, bloomLook.blend,
                 bloomLook.depthMask);
    std::fprintf(f, "bloomKernelPreset=%d\n", bloomKernelPreset);
    std::fprintf(f, "bloomKernel=%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", bloomLook.kernel.rays,
                 bloomLook.kernel.rotation, bloomLook.kernel.spikeLength, bloomLook.kernel.spikeIntensity,
                 bloomLook.kernel.glowRadius, bloomLook.kernel.glowIntensity, bloomLook.kernel.dispersion,
                 bloomLook.kernel.anamorphic);
    std::fprintf(f, "colorLut=%s\nlutIntensity=%.3f\n", colorLut.c_str(), lutIntensity);
    std::fprintf(f, "videoResolution=%d\nvideoFps=%d\nvideoBitrate=%d\nvideoQuality=%d\n", video.resolution, video.fps,
                 video.bitrateMbps, video.quality);
    std::fprintf(f, "videoRenderer=%d\nvideoBloom=%d\nvideoBloomConvolution=%d\nvideoVolumetric=%d\n", video.renderer,
                 video.bloom ? 1 : 0, video.bloomConvolution ? 1 : 0, video.volumetric ? 1 : 0);
    std::fprintf(f, "videoVolumetricDensity=%.3f\nvideoVolumetricFull=%d\nvideoDof=%d\nvideoDofAperture=%.3f\n",
                 video.volumetricDensity, video.volumetricFull ? 1 : 0,
                 video.dof ? 1 : 0, video.dofAperture);
    std::fprintf(f, "videoAlpha=%d\n", video.alpha ? 1 : 0);
    for (const VideoProbe& pr : videoProbes)
        std::fprintf(f, "videoProbe=%llx:%.4f\n", (unsigned long long)pr.key, pr.secondsPerFrame);
    std::fprintf(f, "leaderboardUrl=%s\n", leaderboardUrl.c_str());
    std::fprintf(f, "updateFeedUrl=%s\n", updateFeedUrl.c_str());
    for (const std::string& url : galleryIndexes) std::fprintf(f, "galleryIndex=%s\n", url.c_str());
    for (const auto& [id, scale] : characterScales) std::fprintf(f, "characterScale=%.3f|%s\n", scale, id.c_str());
    for (const auto& [id, c] : characterShaders) {
        std::string list;
        for (const auto& [k, v] : c.params) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4g", v);
            list += (list.empty() ? "" : ",") + k + ":" + buf;
        }
        if (!c.pack.empty()) {
            std::fprintf(f, "characterShader=%s|%s|%s\n", c.pack.c_str(), list.c_str(), id.c_str());
            if (!c.textureFolder.empty())
                std::fprintf(f, "characterShaderTextures=%s|%s\n", c.textureFolder.c_str(), id.c_str());
        }
        for (const auto& [name, cls] : c.materials)
            std::fprintf(f, "characterShaderMaterial=%d|%s|%s\n", cls, id.c_str(), name.c_str());
        for (const auto& [packId, memo] : c.remembered) {
            std::string mlist;
            for (const auto& [k, v] : memo.params) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.4g", v);
                mlist += (mlist.empty() ? "" : ",") + k + ":" + buf;
            }
            std::fprintf(f, "characterShaderMemo=%s|%s|%s|%s\n", packId.c_str(), mlist.c_str(),
                         memo.textureFolder.empty() ? "-" : memo.textureFolder.c_str(), id.c_str());
        }
    }
    for (const auto& [id, dir] : packTextureFolders)
        std::fprintf(f, "packTextureFolder=%s|%s\n", id.c_str(), dir.c_str());
    for (const std::string& key : characterShaderSuggested) std::fprintf(f, "characterShaderSuggested=%s\n", key.c_str());
    for (const std::string& id : characterMmeSuggested) std::fprintf(f, "characterMmeSuggested=%s\n", id.c_str());
    for (const auto& [id, c] : characterMmes)
        std::fprintf(f, "characterMme=%s|%s|%s\n", MmeModeName(c.mode),
                     c.file.empty() ? "-" : PathToUtf8(c.file).c_str(), id.c_str());
    for (const auto& [id, c] : stageMmes)
        std::fprintf(f, "stageMme=%s|%s|%s\n", MmeModeName(c.mode),
                     c.file.empty() ? "-" : PathToUtf8(c.file).c_str(), id.c_str());
    for (const auto& [id, m] : characterMaterials)
        if (m.ray) std::fprintf(f, "characterMaterial=ray|%s|%s\n", m.emd.c_str(), id.c_str());
    for (const auto& [id, v] : stageVariants) std::fprintf(f, "stageVariant=%s|%s\n", v.c_str(), id.c_str());
    for (const std::string& id : characterMaterialSuggested)
        std::fprintf(f, "characterMaterialSuggested=%s\n", id.c_str());
    for (const EffectStackEntry& e : effectStack) {
        std::string list;
        for (const auto& [k, v] : e.params) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4g", v);
            list += (list.empty() ? "" : ",") + k + ":" + buf;
        }
        std::fprintf(f, "effect=%s|%d|%s|%s\n", e.pack.c_str(), e.enabled ? 1 : 0, list.c_str(),
                     e.textureFolder.empty() ? "-" : e.textureFolder.c_str());
    }
    for (const EffectStackEntry& e : worldStack) {
        std::string list;
        for (const auto& [k, v] : e.params) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4g", v);
            list += (list.empty() ? "" : ",") + k + ":" + buf;
        }
        std::fprintf(f, "world=%s|%d|%s|%s\n", e.pack.c_str(), e.enabled ? 1 : 0, list.c_str(),
                     e.textureFolder.empty() ? "-" : e.textureFolder.c_str());
    }
    for (const MmeEffectEntry& e : mmeEffects)
        std::fprintf(f, "mmeEffect=%d|%s\n", e.enabled ? 1 : 0, PathToUtf8(e.file).c_str());
    for (const auto& [k, v] : paramGroupOpen)
        std::fprintf(f, "paramGroupOpen=%s|%d\n", k.c_str(), v ? 1 : 0);
    std::fprintf(f, "shaderSort=%d,%d\n", shaderSort[0], shaderSort[1]);
    std::fprintf(f, "shaderType=%d,%d\n", shaderType[0], shaderType[1]);
    std::fprintf(f, "playPanelTab=%d\n", playPanelTab);
    std::fprintf(f, "lastCharacter=%s\n", lastCharacter.c_str());
    std::fprintf(f, "lastStage=%s\n", lastStage.c_str());
    std::fprintf(f, "lastSong=%s\n", lastSong.c_str());
    for (const auto& [songId, cam] : songCameras) std::fprintf(f, "songCamera=%s|%s\n", cam.c_str(), songId.c_str());
    for (const auto& [songId, stage] : songStages) std::fprintf(f, "songStage=%s|%s\n", stage.c_str(), songId.c_str());
    for (const auto& [songId, audio] : songAudios) std::fprintf(f, "songAudio=%s|%s\n", audio.c_str(), songId.c_str());
    for (const auto& [songId, video] : songVideos) std::fprintf(f, "songVideo=%s|%s\n", video.c_str(), songId.c_str());
    for (const auto& [songId, form] : songFormations) {
        if (!form.centerDance.empty()) std::fprintf(f, "songFormationCenter=%s|%s\n", form.centerDance.c_str(), songId.c_str());
        for (const auto& [dance, cid] : form.byDance)
            std::fprintf(f, "songFormationPart=%s|%s|%s\n", dance.c_str(), cid.c_str(), songId.c_str());
    }
    std::fprintf(f, "cameraInvertX=%d\ncameraInvertY=%d\n", cameraInvertX ? 1 : 0, cameraInvertY ? 1 : 0);
    std::fprintf(f, "cameraRotateSpeed=%.3f\ncameraPanSpeed=%.3f\ncameraZoomSpeed=%.3f\ncameraFovSpeed=%.3f\ncameraFlySpeed=%.3f\n",
                 cameraRotateSpeed, cameraPanSpeed, cameraZoomSpeed, cameraFovSpeed, cameraFlySpeed);
    std::fprintf(f, "editorPrecisionFactor=%.3f\n", editorPrecisionFactor);
    std::fprintf(f, "showEnglishNames=%d\n", showEnglishNames ? 1 : 0);
    std::fprintf(f, "boneOverlayScale=%.3f\n", boneOverlayScale);
    for (const auto& [action, key] : keys) std::fprintf(f, "key%s=%d\n", action.c_str(), key);
    for (const std::string& r : recentProjects) std::fprintf(f, "recentProject=%s\n", r.c_str());
    std::fprintf(f, "windowWidth=%d\n", windowWidth);
    std::fprintf(f, "windowHeight=%d\n", windowHeight);
    std::fclose(f);
    return true;
}

} // namespace mmdx
