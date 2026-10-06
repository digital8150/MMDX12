#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "app/VideoConfig.h"
#include "render/ShaderPack.h"

namespace mmdx {

// Persisted as UTF-8 "key=value" lines in mmdx12.ini next to the executable.
// Unknown keys are ignored; malformed values keep the default.
struct AppSettings {
    std::string libraryPath;   // empty => auto: first existing of <exe>/library, then FindUpward(exe, "library")
    std::string nickname;
    int language = 0;          // Language (core/I18n.h): 0 auto (system), 1 Korean, 2 English, 3 Japanese, 4 Chinese
    bool vsync = true;
    int msaa = 4;              // 1, 2, 4, 8
    float renderScale = 1.0f;  // 0.5 .. 2.0
    float volume = 0.8f;       // 0 .. 1
    bool drawEdges = true;
    bool motionLighting = true;   // play mode: the camera VMD's light / self-shadow tracks (when not just defaults)
    bool physics = true;       // rigid-body physics (hair, skirts) on the character
    // Graphics quality: 0 low, 1 medium, 2 high, 3 ultra, 4 custom (toggles below as set).
    int graphicsPreset = 2;
    bool shadows = true, ssao = true, ssr = true, bloom = true, taa = false;
    int shadowMapSize = 2048;
    int lighting = 0;          // LightingPreset (app/Lighting.h)
    float exposure = 1.0f;     // 0.5 .. 2.0
    int renderPath = 0;        // RenderPath: 0 raster, 1 ray traced, 2 path traced
    int upscaler = 0;          // UpscalerKind: 0 none, 1 DLSS, 2 FSR, 3 XeSS
    int upscalerQuality = 1;   // UpscalerQuality: 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    int ptSamples = 1;         // 1, 2, 4
    int ptBounces = 3;         // 1..6
    // Post effects (independent of graphicsPreset; the benchmark turns them all off).
    bool dof = false;
    float dofAperture = 1.0f;        // 0.2 .. 3.0
    bool volumetric = false;
    float volumetricDensity = 1.0f;  // 0.25 .. 4.0
    bool bloomConvolution = false;
    std::string colorLut;            // ColorLutEntry::id, empty = none
    float lutIntensity = 1.0f;       // 0 .. 1
    VideoRenderConfig video;         // video render (lobby dialog)
    std::vector<VideoProbe> videoProbes;  // measured sample renders, oldest first
    std::string leaderboardUrl = "https://home.codingbot.kr/api/benchmark";
    std::string updateFeedUrl;               // auto-update feed (empty: the built-in default)
    std::string lastCharacter, lastStage, lastSong;  // asset ids
    std::map<std::string, float> characterScales;    // character id -> display scale (absent = 1)
    float CharacterScale(const std::string& id) const;
    void SetCharacterScale(const std::string& id, float scale);  // ~1 removes the entry
    std::map<std::string, ShaderChoice> characterShaders;  // character id -> shader pack (absent = default shading)
    ShaderChoice CharacterShader(const std::string& id) const;
    void SetCharacterShader(const std::string& id, const ShaderChoice& choice);  // empty pack removes the entry
    int windowWidth = 1600, windowHeight = 900;
    std::vector<std::string> recentProjects;         // studio projects (UTF-8 absolute paths), newest first, max 8
    void AddRecentProject(const std::string& path);  // moves it to the front (case-insensitive match)
    void RemoveRecentProject(const std::string& path);

    const VideoProbe* FindVideoProbe(uint64_t key) const;
    void SetVideoProbe(uint64_t key, double secondsPerFrame);  // replaces an entry with the same key

    bool Load(const std::filesystem::path& file);   // false if missing/unreadable (defaults kept)
    bool Save(const std::filesystem::path& file) const;
};

} // namespace mmdx
