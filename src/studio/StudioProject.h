#pragma once
// Studio project file (.mmdxproj): UTF-8 JSON that only *references* standard files, so the work stays usable in
// MMD (PMM has no public spec). Model files are referenced by path (relative to the project folder when possible);
// each model's motion and the camera/light/self-shadow tracks are written as standard VMD files next to the project
// ("<stem> - <model name>.vmd", "<stem> - camera.vmd"). Every file is written to a temp file and renamed over the
// target, so a crash mid-save never leaves a half-written project.
// Independent of the App and the renderer (unit-tested by tools/studio_project_test.cpp).
#include "asset/MaterialChoice.h"
#include "asset/StageManifest.h"
#include "render/ShaderPack.h"
#include "studio/PhysicsEnv.h"
#include "studio/SceneLight.h"
#include "studio/StudioEffects.h"
#include "studio/StudioMotion.h"
#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mmdx::studio {

// Version 2: the studio lights are scene objects (editor "lights"). Version 1 projects carried a lighting source +
// spot rig ("lighting", "useLightTrack"); LoadProject converts them to scene lights.
// Version 3: the studio's effect stack ("effects": pack, enabled, params, texture folder, keyed params). Version 1 / 2
// files load with no effects. Older builds reject version 3.
// Version 4: MMDX stage entities ("stages": variant, param base values, keyed params, the params whose manifest cue the
// project replaced) and a part model's / light's "stageUid". A version 3 file's per-part "stageVariant" becomes one
// StudioStage per stage (StudioLoader). Older builds reject version 4.
// Version 5: camera entities ("cameras": uid, name, visible, stageUid, motion), the stage screens' extra cameras. The
// main camera stays the top-level "camera" (its light / self-shadow / focus tracks) and is the only output camera.
// Older builds reject version 5; a version <= 4 file has the main camera only.
// Version 6: a stage entity's screen overrides ("screens": id -> {source, file, camera, brightness}).
inline constexpr int kProjectFormatVersion = 6;
inline constexpr const char* kProjectFormatName = "mmdx12-studio-project";
inline constexpr const wchar_t* kProjectExtension = L".mmdxproj";

enum class ModelKind : uint8_t { Character = 0, Stage = 1, Prop = 2, Effect = 3 };   // Effect: M5c MME effect object
const char* ModelKindName(ModelKind k);                    // "character" | "stage" | "prop"
bool ParseModelKind(const std::string& s, ModelKind& out);  // false for anything else (out untouched)

// Accessory placement (MMD accessory parenting): the prop's root follows a bone of another model with an offset.
// World matrix of the prop's origin = PropOffsetMatrix(a) * parent bone world (row vectors, v * M).
struct PropAttach {
    int parent = -1;               // ProjectData / StudioPackage: index into `models`; StudioModel: uid of the parent.
                                   // -1 = the world origin
    std::string bone;              // parent bone name; empty = the parent model's origin
    DirectX::XMFLOAT3 translation{0, 0, 0};
    DirectX::XMFLOAT3 rotationDeg{0, 0, 0};  // Euler degrees, XMMatrixRotationRollPitchYaw(x, y, z) order
    float scale = 1.0f;
    bool operator==(const PropAttach& o) const {
        return parent == o.parent && bone == o.bone && translation.x == o.translation.x &&
               translation.y == o.translation.y && translation.z == o.translation.z &&
               rotationDeg.x == o.rotationDeg.x && rotationDeg.y == o.rotationDeg.y && rotationDeg.z == o.rotationDeg.z &&
               scale == o.scale;
    }
};
// Scaling(scale) * RotationRollPitchYaw(rad(x), rad(y), rad(z)) * Translation(translation).
DirectX::XMMATRIX PropOffsetMatrix(const PropAttach& a);

// One placed world entity (docs/effect_ecosystem.md §2 item 7): a world pack with its keyed params and placement.
struct StudioWorld {
    uint32_t uid = 0;                 // StudioDoc::nextWorldUid, not saved
    std::string name;                 // outliner label
    StudioEffect fx;                  // pack id, enabled, static params, keyed param tracks
    PropAttach place;                 // translation / rotationDeg / scale (parent unused)
    size_t ApproxBytes() const;
    bool operator==(const StudioWorld& o) const;   // ignores uid
};

// One MMDX stage in the studio (docs/stage_format.md "Studio"): one entity per loaded stage, whose parts stay models
// (StudioModel::stageManifest / stagePart / stageUid). The installed stage is never written: the variant, the param base
// values and the keyed params are this project's overrides. Pure data, saved in the .mmdxproj ("stages").
// S5: one screen's per-project override ("screens": {id: {...}}). An absent member keeps the manifest's value.
struct StageScreenOverride {
    std::optional<std::string> source;      // "none" | "image" | "video" | "camera"
    std::optional<std::string> file;        // image / video file (relative to the project folder in the file)
    std::optional<std::string> camera;      // stage camera name, or "uid:<n>" for a studio camera
    std::optional<float> brightness;
    bool operator==(const StageScreenOverride& o) const {
        return source == o.source && file == o.file && camera == o.camera && brightness == o.brightness;
    }
};

struct StudioStage {
    uint32_t uid = 0;                          // StudioDoc::nextStageUid, saved (part models / lights refer to it)
    std::shared_ptr<const StageManifest> manifest;   // the parsed stage.json (null when it could not be read)
    std::string stageId;                       // library id of the stage folder (empty = the manifest's own id)
    std::filesystem::path manifestPath;        // its stage.json (saved relative to the project folder)
    std::string variant;                       // variant id (empty = the manifest's default)
    StageParamValues base;                     // the user's base values (absent key = the variant / default)
    std::vector<EffectParamTrack> tracks;      // one track per keyed param component ("key" / "key#0..3" for a colour)
    std::set<std::string> cuesOff;             // params whose manifest cue the user replaced (imported / keyed)
    // S5: the project's screen overrides by screen id (not keyable). Absent id = the manifest's values.
    std::map<std::string, StageScreenOverride> screens;
    size_t ApproxBytes() const;
    bool operator==(const StudioStage& o) const;   // ignores uid
};

// S4: an extra (non-main) studio camera. The main camera stays ProjectData::camera (uid 0: it keeps the light /
// self-shadow / focus tracks and the project's camera VMD); an extra camera has camera keys only and its own VMD.
struct StudioCamera {
    uint32_t uid = 0;                 // >= 1 (StudioDoc::nextCameraUid), saved (the switch track refers to it)
    std::string name;                 // outliner label, unique in the project
    MotionData track;                 // camera keys only (light / shadow / focus stay the main camera's)
    bool visible = true;
    uint32_t stageUid = 0;            // the MMDX stage whose stage.json "cameras" entry it came from (0 = user camera)
    size_t ApproxBytes() const;
    bool operator==(const StudioCamera& o) const;   // ignores uid
};

struct ProjectModel {    std::string name;                  // outliner label (UTF-8)
    ModelKind kind = ModelKind::Character;
    std::filesystem::path path;        // model file, absolute
    std::filesystem::path fx;          // M5c: effect-object effects ("fx"; kind Effect only)
    std::string libraryId;             // library character id (saved display scale); may be empty
    bool visible = true;
    PropAttach attach;                 // used for props only
    PropAttach place;                  // characters / stages: world placement (translation / rotationDeg / scale; parent unused)
    bool physicsOff = false;           // P2.2: rigid-body physics off for this model ("physics": false, absent = on)
    ShaderChoice shader;               // characters: shader pack ("shader", absent = the default shading)
    MmeChoice mme;                     // M6a: MME object effect choice ("mme", absent = characters None / stages Auto)
    MaterialChoice material;           // P5: per-material PBR source ("material", absent = Default)
    std::string stageVariant;          // S1b: an MMDX stage part's variant ("stageVariant", absent = the default; read
                                       // for version <= 3 files, converted to its StudioStage on load)
    uint32_t stageUid = 0;             // S3: the StudioStage the model is a part of (0 = not an MMDX stage part)
    MotionData motion;                 // its keys; motion.modelName is written as the VMD header model name
};

struct FrameAspectPreset {
    const char* label;
    int w;
    int h;
    float ratio;
};

inline constexpr FrameAspectPreset kAspectPresets[] = {
    {"16:9", 16, 9, 16.0f / 9.0f},
    {"9:16", 9, 16, 9.0f / 16.0f},
    {"4:3", 4, 3, 4.0f / 3.0f},
    {"1:1", 1, 1, 1.0f},
    {"4:5", 4, 5, 4.0f / 5.0f},
    {"2.39:1", 239, 100, 2.39f},
};
inline constexpr int kAspectPresetCount = 6;

inline int AspectIndexFromWH(int w, int h) {
    for (int i = 0; i < kAspectPresetCount; ++i) {
        if (kAspectPresets[i].w == w && kAspectPresets[i].h == h) return i;
    }
    return 0;
}

inline int AspectIndexFromLabel(const std::string& s) {
    for (int i = 0; i < kAspectPresetCount; ++i) {
        if (s == kAspectPresets[i].label) return i;
    }
    if (s == "239:100") return 5;
    return -1;
}

inline float AspectRatioOf(int w, int h) {
    const int idx = AspectIndexFromWH(w, h);
    return kAspectPresets[idx].ratio;
}

// Editor state restored with the project (view, playback options, free camera).
struct ProjectEditor {
    int frame = 0;
    int selectedModel = -1;            // index into models, -1 = camera
    bool useMotionCamera = true, useShadowTrack = true, showCameraPath = true;
    int aspectW = 16, aspectH = 9;     // frame aspect preset (defaults to 16:9)
    // The scene lights ("lights", see SceneLight.h). A spot's targetUid is 1 + the index into ProjectData::models here
    // (0 = the performer): the App converts it from / to the studio's model uids. A version 2 file without "lights"
    // has none; a version 1 file's "lighting" / "useLightTrack" are converted by LoadProject.
    std::vector<SceneLight> lights;
    bool loop = false, physics = true;
    int rangeStart = -1, rangeEnd = -1;
    float pxPerFrame = 6.0f;
    DirectX::XMFLOAT3 camTarget{0, 10, 0};
    float camYaw = 0.0f, camPitch = 0.1f, camDistance = 45.0f, camFovDeg = 30.0f;
};

// M6b: one entry of the studio's own MME post stack (project "mmeEffects"). The path is resolved to the project
// folder on load (absolute in assets).
struct MmePostEntry {
    std::filesystem::path path;
    bool enabled = true;
};

struct ProjectData {
    std::vector<ProjectModel> models;
    MotionData camera;                 // camera, light and self-shadow keys (camera VMD) + focus keys (editor "focus"); bones/morphs ignored
    std::filesystem::path audioPath;   // absolute; empty = no audio
    double audioOffset = 0.0;          // seconds: the audio starts at this timeline time (may be negative)
    // Background AVI (背景AVI): absolute path (empty = none), start offset in seconds, and whether it is also drawn
    // as the screen background (default false: it only feeds screen.bmp materials).
    std::filesystem::path bgVideoPath;
    double bgVideoOffset = 0.0;
    bool bgVideoAsBackground = false;
    // The gravity and the wind ("physicsWorld": {"gravity": {acceleration, direction}, "wind": {direction, strength,
    // gust, gustFrequency, jitter}}). A file without it loads MMD's default gravity and no wind (as it was simulated).
    PhysicsEnv physicsEnv;
    ProjectEditor editor;
    // M6b: the project's own MME post stack (docs/mme/mme_design.md §2.10/§2.13). While the studio is open this
    // replaces the play-mode stack (RenderSettings::mmeEffects); offline renders from the studio use it. Empty = none.
    std::vector<MmePostEntry> mmePost;
    // Version 3: the project's effect stack (order = stack order). Empty = none.
    std::vector<StudioEffect> effects;
    // Version 3: placed world entities ("worlds"). Empty = none.
    std::vector<StudioWorld> worlds;
    // Version 4: the project's MMDX stage entities ("stages"). Empty = none.
    std::vector<StudioStage> stages;
    // Version 5: the extra cameras ("cameras", for the stage screens). The main camera stays `camera` above. Empty = a
    // version <= 4 project (main camera only).
    std::vector<StudioCamera> cameras;
    std::filesystem::path recoveryOf;  // autosave (recovery) files only: the project it belongs to (empty: never saved)
};

// Writes `file` (JSON) and the motion VMDs next to it. Models without keys and an empty camera write no VMD
// ("motion": null). VMD names: "<stem> - <SanitizeFileName(name)>.vmd", camera "<stem> - camera.vmd"; duplicates
// get " (2)", " (3)"... (the camera is named first). Paths in the JSON are relative to the project folder (generic
// '/' separators) unless they are on another drive. Every file goes through WriteFileAtomic / a temp file + rename.
// After a successful save, VMD files that the previous version of `file` referenced, that live in the same folder,
// start with "<stem> - " and are no longer referenced are deleted (renamed / removed models leave no stale files).
// Creates the folder if needed. Returns false with *error (UTF-8) on any write failure.
bool SaveProject(const std::filesystem::path& file, const ProjectData& data, std::string* error);

// Reads `file` and the VMDs it references into `out` (paths absolute). Model motions keep their VMD track names
// (not canonicalised: that needs the PMX). A missing or unreadable VMD adds a line to *warnings and leaves that
// motion empty. Fails (false, *error) when the file is missing, not JSON, not a project (format name), or has a
// newer version than kProjectFormatVersion. Unknown keys are ignored; missing keys keep their defaults.
bool LoadProject(const std::filesystem::path& file, ProjectData& out, std::string* error,
                 std::vector<std::string>* warnings);

// Writes `bytes` to `file` through "<file>.tmp" + MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
bool WriteFileAtomic(const std::filesystem::path& file, const std::string& bytes, std::string* error);
// Saves `vmd` atomically (SaveVmd to "<file>.tmp", then the same rename).
bool SaveVmdAtomic(const std::filesystem::path& file, const VmdMotion& vmd, std::string* error);
// Replaces \ / : * ? " < > | and control characters with '_', trims spaces/dots at both ends; "" -> "model".
std::string SanitizeFileName(const std::string& utf8);

} // namespace mmdx::studio
