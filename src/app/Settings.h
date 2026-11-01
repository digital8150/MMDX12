#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/VideoConfig.h"
#include "asset/MaterialChoice.h"
#include "render/ShaderPack.h"

namespace mmdx {

// Persisted as UTF-8 "key=value" lines in mmdx12.ini next to the executable.
// Unknown keys are ignored; malformed values keep the default.
struct AppSettings {
    std::string libraryPath;   // empty => auto: first existing of <exe>/library, then FindUpward(exe, "library")
    std::string nickname;
    int theme = 0;             // colour theme: 0 auto (follow Windows), 1 light, 2 dark
    int language = 0;          // Language (core/I18n.h): 0 auto (system), 1 Korean, 2 English, 3 Japanese, 4 Chinese
    bool vsync = true;
    int msaa = 4;              // 1, 2, 4, 8
    float renderScale = 1.0f;  // 0.5 .. 2.0
    float volume = 0.8f;       // 0 .. 1
    bool drawEdges = true;
    bool motionLighting = true;   // play mode: the camera VMD's light / self-shadow tracks (when not just defaults)
    bool physics = true;       // rigid-body physics (hair, skirts) on the character
    bool mcpEnabled = true;    // named pipe server for MCP control
    // Graphics quality: 0 low, 1 medium, 2 high, 3 ultra, 4 custom (toggles below as set).
    int graphicsPreset = 2;
    bool shadows = true, ssao = true, ssr = true, bloom = true, taa = false;
    // そぼろ AutoLuminous convention: PMX materials the author marked (black specular, power >= 100) glow. Default on.
    bool autoLuminous = true;
    // S8b: stage screens light the raster / RT scene as area lights (ini screenLights=0|1, CLI --screen-lights).
    bool screenLights = true;
    bool readOnly = false;     // runtime only, never saved: Save() is a no-op (--classic runs)
    // Global MME switch (ini mmeEnabled=0|1, CLI --mme-global). Off = no MME object effect, no MME post stack, no
    // MME effect objects, everywhere (play / lobby / studio / offline). Does not cover the Ray-MMD PBR material
    // source (see rayMmdEnabled). Saved per-model choices are kept, they just do not apply.
    bool mmeEnabled = true;
    // Ray-MMD PBR material source switch (ini rayMmdEnabled=0|1, CLI --ray-global). Off = every model's material
    // source resolves to Default; on = the saved / auto / CLI choice. Independent of mmeEnabled.
    bool rayMmdEnabled = true;
    // MME workflow (ini mmeWorkflow=manual|auto, CLI --mme-workflow). Manual (0) = today's per-model behaviour.
    // Auto (1) = every model always gets the automatic choices, ignoring the saved per-model ones.
    int mmeWorkflow = 0;       // 0 manual, 1 auto
    int shadowMapSize = 2048;
    int lighting = 0;          // LightingPreset (app/Lighting.h)
    float exposure = 1.0f;     // 0.5 .. 2.0
    int renderPath = 0;        // RenderPath: 0 raster, 1 ray traced, 2 path traced
    int studioRenderer = 0;    // studio viewport (lit single view): 0 raster, 1 ray traced, 2 path traced, 3 GI preview
    // Studio ribbon (UiStudioRibbon.cpp): the selected tab (0 home, 1 anim, 2 camera, 3 light, 4 render, 5 view) and
    // whether the tool row is collapsed. Scripted / --frames runs start expanded on the home tab.
    int studioRibbonTab = 0;
    bool studioRibbonCollapsed = false;
    int upscaler = 0;          // UpscalerKind: 0 none, 1 DLSS, 2 FSR, 3 XeSS
    int upscalerQuality = 1;   // UpscalerQuality: 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    // DLSS Frame Generation: 0 off, 2 / 3 / 4 (multiplier). Only applies to the live play scene.
    int dlssFrameGen = 0;
    int dlssModel = 0;         // DlssModel: 0 auto (driver default), 1 cnn (preset E), 2 transformer (K), 3 transformer2 (M / L)
    bool dlssRayReconstruction = false;  // DLSS-RR: replaces the ray-traced denoising/filtering and the upscaler (RT/PT + DLSS only)
    int rtaoRays = 4;   // 4, 8, 16
    int ptSamples = 1;         // 1, 2, 4
    int ptBounces = 3;         // 1..6
    // Post effects (independent of graphicsPreset; the benchmark turns them all off).
    bool dof = false;
    float dofAperture = 1.0f;        // 0.2 .. 3.0
    bool volumetric = false;
    float volumetricDensity = 1.0f;  // 0.25 .. 4.0
    // Haze light shares (ini volumetricLightGain= / volumetricAmbientGain=): multiply every spot / point light's
    // in-scattering ("빛줄기 강도", 0..8) and the sun + sky's ("환경 산란", 0..2). 1 / 1 = the lights' own values. Shafts read
    // only when the sun / sky do not fill the whole medium: an arena wants a low ambient share and a high light gain.
    float volumetricLightGain = 1.0f;
    float volumetricAmbientGain = 1.0f;
    // "안개 범위" (ini volumetricRange=): the haze sphere's radius around the origin, 0 = automatic (the stage's
    // environment.fog.radius, else 180 grown to hold the lights that show in the haze), 50 .. 1000 = fixed.
    float volumetricRange = 0.0f;
    // S9d: the real-time volumetric mode: 0 = froxel grid (default), 1 = the per-pixel march (A/B, fallback) and the
    // multiple-scatter approximation switch (froxel injector).
    int volumetricMode = 0;
    bool volumetricMultiScatter = true;
    // S9c: offline GI computes the medium inside the paths (full) instead of the single-scatter pass (fast).
    bool volumetricFull = true;
    // S9: the fog's Henyey-Greenstein anisotropy (a stage "environment.fog.anisotropy" sets it); -1 = the built-in
    // constants of the volumetric pass (sun 0.55, punctual 0.3).
    float volumetricAnisotropy = -1.0f;
    bool bloomConvolution = false;
    float bloomIntensity = 0.35f;    // 0 .. 1.5
    float bloomThreshold = 1.1f;     // 0.2 .. 4
    BloomLook bloomLook;
    int bloomKernelPreset = 0;       // 0..3 = BloomKernelPreset(i), 4 = custom
    std::string colorLut;            // ColorLutEntry::id, empty = none
    float lutIntensity = 1.0f;       // 0 .. 1
    VideoRenderConfig video;         // video render (lobby dialog)
    std::vector<VideoProbe> videoProbes;  // measured sample renders, oldest first
    std::string leaderboardUrl = "https://home.codingbot.kr/api/benchmark";
    std::string updateFeedUrl;               // auto-update feed (empty: the built-in default)
    // Open gallery (docs/effect_ecosystem.md §3): additional index URLs, kept in ini "galleryIndex=<url>" lines.
    // The official index (ShaderPackStore::kDefaultIndexUrl) is implicit and always fetched first, never stored.
    std::vector<std::string> galleryIndexes;
    std::string lastCharacter, lastStage, lastSong;  // asset ids
    std::map<std::string, float> characterScales;    // character id -> display scale (absent = 1)
    float CharacterScale(const std::string& id) const;
    void SetCharacterScale(const std::string& id, float scale);  // ~1 removes the entry
    std::map<std::string, ShaderChoice> characterShaders;  // character id -> shader pack (absent = default shading)
    ShaderChoice CharacterShader(const std::string& id) const;
    void SetCharacterShader(const std::string& id, const ShaderChoice& choice);  // empty pack removes the entry
    // M6a: per-character / per-stage MME object effect choice (render/ShaderPack.h MmeChoice). Absent = the default
    // (characters None, stages Auto). Persisted as "characterMme=<mode>|<file or ->|<id>" / "stageMme=..."
    std::map<std::string, MmeChoice> characterMmes;
    std::map<std::string, MmeChoice> stageMmes;
    MmeChoice CharacterMme(const std::string& id) const;
    void SetCharacterMme(const std::string& id, const MmeChoice& choice);   // None removes the entry
    MmeChoice StageMme(const std::string& id) const;
    void SetStageMme(const std::string& id, const MmeChoice& choice);
    // (pack, character) pairs the app already suggested once (ShaderPack "suggest" fits the model's data): never
    // suggested again. Persisted as "characterShaderSuggested=<pack id>|<character id>" lines (pack ids hold no '|').
    // Old "characterShaderAuto" lines are ignored on load.
    std::set<std::string> characterShaderSuggested;   // "<pack id>|<character id>"
    bool ShaderSuggested(const std::string& packId, const std::string& characterId) const {
        return characterShaderSuggested.count(packId + "|" + characterId) != 0;
    }
    void SetShaderSuggested(const std::string& packId, const std::string& characterId) {
        characterShaderSuggested.insert(packId + "|" + characterId);
    }
    // M6b: character ids the app already suggested an MME object effect for (the model ships an .emd / same-name
    // .fx and has no saved MME choice): never suggested again. Persisted as "characterMmeSuggested=<character id>".
    std::set<std::string> characterMmeSuggested;
    bool MmeSuggested(const std::string& characterId) const { return characterMmeSuggested.count(characterId) != 0; }
    void SetMmeSuggested(const std::string& characterId) { characterMmeSuggested.insert(characterId); }
    // P5 per-character PBR material source (Default / Ray-MMD + optional explicit .emd). Persisted as
    // "characterMaterial=<ray|default>|<emd path or empty>|<character id>" lines (the id last, like the other
    // lines). Absent = Default.
    std::map<std::string, MaterialChoice> characterMaterials;  // character id -> material source
    MaterialChoice CharacterMaterial(const std::string& id) const;
    void SetCharacterMaterial(const std::string& id, const MaterialChoice& choice);  // Default removes the entry
    // Character ids the app already suggested Ray-MMD material data for (a Ray .emd candidate exists, no saved
    // choice): never suggested again. Persisted as "characterMaterialSuggested=<character id>" lines.
    std::set<std::string> characterMaterialSuggested;
    bool MaterialSuggested(const std::string& characterId) const {
        return characterMaterialSuggested.count(characterId) != 0;
    }
    void SetMaterialSuggested(const std::string& characterId) {
        characterMaterialSuggested.insert(characterId);
    }
    // Persisted as "characterShaderTextures=<utf-8 folder>|<character id>" ini lines: the choice's
    // per-character texture folder (a separate line: the ids in characterShader= are already last).
    // Old ini files without it load unchanged.
    std::map<std::string, std::string> packTextureFolders;  // pack id -> user texture folder (game textures), UTF-8
    // Effect packs ("type": "effect"), the user's ordered stack: applied to play mode, the lobby preview and
    // video / still renders (the real-time renderers; GI / PT output skips it). Persisted as
    // "effect=<pack id>|<enabled>|<key>:<value>,...|<texture folder or ->|" ini lines, oldest first.
    std::vector<EffectStackEntry> effectStack;
    // World packs ("type": "world"), the user's ordered stack: placed world entities for play mode / renders.
    // Persisted as "world=" lines in the same format as "effect=", oldest first; --world overrides for one run.
    std::vector<EffectStackEntry> worldStack;
    // M6b: the play-mode MME post stack (RenderSettings::mmeEffects). Persisted as
    // "mmeEffect=<enabled 0|1>|<path>" ini lines, oldest first; --mme overrides it for one run.
    std::vector<MmeEffectEntry> mmeEffects;
    // params.ext: collapsible group open state per pack + group id. Persisted as
    // "paramGroupOpen=<pack>|<group>|0|1" ini lines.
    std::map<std::string, bool> paramGroupOpen;   // "<pack id>|<group id>" -> open
    bool ParamGroupOpen(const std::string& packId, const std::string& groupId, bool defaultOpen = true) const {
        auto it = paramGroupOpen.find(packId + "|" + groupId);
        return it != paramGroupOpen.end() ? it->second : defaultOpen;
    }
    void SetParamGroupOpen(const std::string& packId, const std::string& groupId, bool open) {
        paramGroupOpen[packId + "|" + groupId] = open;
    }
    int shaderSort[2] = {0, 0};   // 0 recommended, 1 name, 2 updated, 3 params
    int shaderType[2] = {0, 0};   // 0 all, 1 surface, 2 effect
    int playPanelTab = 0;         // play-mode 화면 효과 panel: last open tab (0 효과, 1 셰이더, 2 MME · Ray-MMD)
    int windowWidth = 1600, windowHeight = 900;
    std::vector<std::string> recentProjects;         // studio projects (UTF-8 absolute paths), newest first, max 8
    void AddRecentProject(const std::string& path);  // moves it to the front (case-insensitive match)
    void RemoveRecentProject(const std::string& path);

    // A unit song's last cast, keyed by each part's dance file name (part order may change between scans).
    struct SavedFormation {
        std::string centerDance;                        // dance file name of the center part
        std::map<std::string, std::string> byDance;     // dance file name -> character id (cast parts only)
    };
    std::map<std::string, SavedFormation> songFormations; // song id -> saved formation
    std::map<std::string, std::string> songCameras;       // song id -> chosen camera VMD file name (songs with several)
    std::map<std::string, std::string> songStages;        // song id -> chosen stage library id ("-" = studio floor / no stage model; absent = bundled default)
    std::map<std::string, std::string> songAudios;        // song id -> chosen audio file name (songs with several versions)
    // M8b: per-song video override. song id -> "<overlay|background|off>[:<black|screen|add|normal>]|<file name>"
    // (file empty = the classification; "off" = no video layers for this song). See App::ResolveSongVideos.
    std::map<std::string, std::string> songVideos;
    // S1b: the variant picked for an MMDX stage in the lobby, per stage id. Persisted as
    // "stageVariant=<variant id>|<stage id>" lines (the stage id last, like the other per-id lines).
    std::map<std::string, std::string> stageVariants;   // stage id -> variant id
    std::string StageVariant(const std::string& stageId) const;
    void SetStageVariant(const std::string& stageId, const std::string& variantId);   // empty removes the entry

    // --- Program settings (UiSelect's settings popup) ----------------------------------------------
    // Viewport camera control (studio). Multipliers default to 1.0 = the classic MMDX12 feel; the
    // buttons only scale/reflect the gestures, never the motion-camera key values.
    bool cameraInvertX = false;            // invert the horizontal axis of orbit / look
    bool cameraInvertY = false;            // invert the vertical axis of orbit / look
    float cameraRotateSpeed = 1.0f;        // orbit / look rotation multiplier (0.05 .. 10)
    float cameraPanSpeed = 1.0f;           // pan multiplier (0.05 .. 10)
    float cameraZoomSpeed = 1.0f;          // wheel distance zoom + drag-dolly multiplier (0.05 .. 10)
    float cameraFovSpeed = 1.0f;           // wheel FOV zoom multiplier (0.05 .. 10)
    float cameraFlySpeed = 1.0f;           // WASDQE fly speed multiplier (0.05 .. 20)
    // Pose editing: hold Ctrl while dragging a gizmo to move / rotate at a fraction of the pointer
    // motion (1 / editorPrecisionFactor = 10x finer by default).
    float editorPrecisionFactor = 0.1f;    // 0.01 .. 1 (1 = no precision modifier)
    // Interface: prefer the PMX English name (nameEn) in lists / tooltips when one exists, falling back
    // to the original name. Internal ids (VMD / VPD track names) are never changed.
    bool showEnglishNames = false;
    float boneOverlayScale = 1.0f;   // studio bone-overlay marker size (joints / links), 0.5 .. 2.5
    // Rebindable editor / viewport keys: action name -> ImGuiKey value (int). Stored in ini as
    // "key<Action>=<ImGuiKey int>"; absent actions fall back to the built-in defaults (Settings.cpp).
    std::map<std::string, int> keys;
    int KeyFor(const char* action) const;

    const VideoProbe* FindVideoProbe(uint64_t key) const;
    void SetVideoProbe(uint64_t key, double secondsPerFrame);  // replaces an entry with the same key

    bool Load(const std::filesystem::path& file);   // false if missing/unreadable (defaults kept)
    bool Save(const std::filesystem::path& file) const;
};

} // namespace mmdx
