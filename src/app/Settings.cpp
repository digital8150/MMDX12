#include "app/Settings.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
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
    if (choice.pack.empty() && choice.remembered.empty()) characterShaders.erase(id);   // nothing left to keep
    else characterShaders[id] = choice;
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

} // namespace

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
        else if (key == "language" && ParseInt(value, i)) language = std::clamp(i, 0, 4);
        else if (key == "vsync" && ParseBool(value, b)) vsync = b;
        else if (key == "msaa" && ParseInt(value, i)) msaa = i;
        else if (key == "renderScale" && ParseFloat(value, f)) renderScale = f;
        else if (key == "volume" && ParseFloat(value, f)) volume = f;
        else if (key == "drawEdges" && ParseBool(value, b)) drawEdges = b;
        else if (key == "motionLighting" && ParseBool(value, b)) motionLighting = b;
        else if (key == "physics" && ParseBool(value, b)) physics = b;
        else if (key == "graphicsPreset" && ParseInt(value, i)) graphicsPreset = i;
        else if (key == "shadows" && ParseBool(value, b)) shadows = b;
        else if (key == "ssao" && ParseBool(value, b)) ssao = b;
        else if (key == "ssr" && ParseBool(value, b)) ssr = b;
        else if (key == "bloom" && ParseBool(value, b)) bloom = b;
        else if (key == "taa" && ParseBool(value, b)) taa = b;
        else if (key == "shadowMapSize" && ParseInt(value, i)) shadowMapSize = i;
        else if (key == "lighting" && ParseInt(value, i)) lighting = i;
        else if (key == "exposure" && ParseFloat(value, f)) exposure = f;
        else if (key == "renderPath" && ParseInt(value, i)) renderPath = i;
        else if (key == "upscaler" && ParseInt(value, i)) upscaler = i;
        else if (key == "upscalerQuality" && ParseInt(value, i)) upscalerQuality = i;
        else if (key == "ptSamples" && ParseInt(value, i)) ptSamples = i;
        else if (key == "ptBounces" && ParseInt(value, i)) ptBounces = i;
        else if (key == "dof" && ParseBool(value, b)) dof = b;
        else if (key == "dofAperture" && ParseFloat(value, f)) dofAperture = f;
        else if (key == "volumetric" && ParseBool(value, b)) volumetric = b;
        else if (key == "volumetricDensity" && ParseFloat(value, f)) volumetricDensity = f;
        else if (key == "bloomConvolution" && ParseBool(value, b)) bloomConvolution = b;
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
        else if (key == "videoDof" && ParseBool(value, b)) video.dof = b;
        else if (key == "videoDofAperture" && ParseFloat(value, f)) video.dofAperture = f;
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
        else if (key == "lastCharacter") lastCharacter = value;
        else if (key == "lastStage") lastStage = value;
        else if (key == "lastSong") lastSong = value;
        else if (key == "recentProject" && !value.empty() && recentProjects.size() < 8) recentProjects.push_back(value);
        else if (key == "windowWidth" && ParseInt(value, i)) windowWidth = i;
        else if (key == "windowHeight" && ParseInt(value, i)) windowHeight = i;
    }

    // Clamp msaa to {1,2,4,8}, else 4.
    if (msaa != 1 && msaa != 2 && msaa != 4 && msaa != 8) msaa = 4;
    renderScale = std::clamp(renderScale, 0.5f, 2.0f);
    graphicsPreset = std::clamp(graphicsPreset, 0, 4);
    if (shadowMapSize != 1024 && shadowMapSize != 2048 && shadowMapSize != 4096) shadowMapSize = 2048;
    lighting = std::clamp(lighting, 0, 3);
    exposure = std::clamp(exposure, 0.5f, 2.0f);
    renderPath = std::clamp(renderPath, 0, 2);
    upscaler = std::clamp(upscaler, 0, 3);
    upscalerQuality = std::clamp(upscalerQuality, 0, 4);
    ptSamples = std::clamp(ptSamples, 1, 4);
    ptBounces = std::clamp(ptBounces, 1, 6);
    dofAperture = std::clamp(dofAperture, 0.2f, 3.0f);
    volumetricDensity = std::clamp(volumetricDensity, 0.25f, 4.0f);
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
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"w") != 0 || !f) return false;

    std::fprintf(f, "libraryPath=%s\n", libraryPath.c_str());
    std::fprintf(f, "nickname=%s\n", nickname.c_str());
    std::fprintf(f, "vsync=%d\n", vsync ? 1 : 0);
    std::fprintf(f, "msaa=%d\n", msaa);
    std::fprintf(f, "renderScale=%.3f\n", renderScale);
    std::fprintf(f, "volume=%.3f\n", volume);
    std::fprintf(f, "drawEdges=%d\n", drawEdges ? 1 : 0);
    std::fprintf(f, "motionLighting=%d\n", motionLighting ? 1 : 0);
    std::fprintf(f, "physics=%d\n", physics ? 1 : 0);
    std::fprintf(f, "graphicsPreset=%d\n", graphicsPreset);
    std::fprintf(f, "shadows=%d\nssao=%d\nssr=%d\nbloom=%d\ntaa=%d\n", shadows ? 1 : 0, ssao ? 1 : 0, ssr ? 1 : 0,
                 bloom ? 1 : 0, taa ? 1 : 0);
    std::fprintf(f, "shadowMapSize=%d\n", shadowMapSize);
    std::fprintf(f, "lighting=%d\n", lighting);
    std::fprintf(f, "exposure=%.3f\n", exposure);
    std::fprintf(f, "renderPath=%d\n", renderPath);
    std::fprintf(f, "upscaler=%d\n", upscaler);
    std::fprintf(f, "upscalerQuality=%d\n", upscalerQuality);
    std::fprintf(f, "ptSamples=%d\n", ptSamples);
    std::fprintf(f, "ptBounces=%d\n", ptBounces);
    std::fprintf(f, "dof=%d\ndofAperture=%.3f\n", dof ? 1 : 0, dofAperture);
    std::fprintf(f, "volumetric=%d\nvolumetricDensity=%.3f\n", volumetric ? 1 : 0, volumetricDensity);
    std::fprintf(f, "bloomConvolution=%d\n", bloomConvolution ? 1 : 0);
    std::fprintf(f, "colorLut=%s\nlutIntensity=%.3f\n", colorLut.c_str(), lutIntensity);
    std::fprintf(f, "videoResolution=%d\nvideoFps=%d\nvideoBitrate=%d\nvideoQuality=%d\n", video.resolution, video.fps,
                 video.bitrateMbps, video.quality);
    std::fprintf(f, "videoRenderer=%d\nvideoBloom=%d\nvideoBloomConvolution=%d\nvideoVolumetric=%d\n", video.renderer,
                 video.bloom ? 1 : 0, video.bloomConvolution ? 1 : 0, video.volumetric ? 1 : 0);
    std::fprintf(f, "videoVolumetricDensity=%.3f\nvideoDof=%d\nvideoDofAperture=%.3f\n", video.volumetricDensity,
                 video.dof ? 1 : 0, video.dofAperture);
    for (const VideoProbe& pr : videoProbes)
        std::fprintf(f, "videoProbe=%llx:%.4f\n", (unsigned long long)pr.key, pr.secondsPerFrame);
    std::fprintf(f, "leaderboardUrl=%s\n", leaderboardUrl.c_str());
    std::fprintf(f, "updateFeedUrl=%s\n", updateFeedUrl.c_str());
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
    std::fprintf(f, "lastCharacter=%s\n", lastCharacter.c_str());
    std::fprintf(f, "lastStage=%s\n", lastStage.c_str());
    std::fprintf(f, "lastSong=%s\n", lastSong.c_str());
    for (const std::string& r : recentProjects) std::fprintf(f, "recentProject=%s\n", r.c_str());
    std::fprintf(f, "windowWidth=%d\n", windowWidth);
    std::fprintf(f, "windowHeight=%d\n", windowHeight);
    std::fclose(f);
    return true;
}

} // namespace mmdx
