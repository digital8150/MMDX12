#pragma once
// Studio document: the models of the scene with their editable motions, the camera motion, the undo
// history and the editor state (selection, timeline view). Owned by App while screen_ == Studio.
// Motions are keyed by bone/morph name (StudioMotion.h) and re-bound to the model after every edit.
#include "anim/Motion.h"
#include "asset/DdsLoader.h"
#include "asset/ImageLoader.h"
#include "asset/MaterialChoice.h"
#include "asset/PbrMaterial.h"
#include "asset/PmmLoader.h"
#include "asset/PmxModel.h"
#include "asset/StageManifest.h"
#include "asset/StageMaterials.h"
#include "studio/CommandStack.h"
#include "studio/SceneLight.h"
#include "studio/StudioMotion.h"
#include "studio/StudioPose.h"
#include "studio/StudioProject.h"
#include "studio/UiTimeline.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mmdx {
class ModelInstance;
class GpuModel;  // render/GpuModel.h (not included: its ModelRole clashes with asset/ModelImport.h)
struct LoadProgress;
struct CharacterAsset;
struct StageAsset;
struct SongAsset;
}

namespace mmdx::studio {

struct StudioModel {
    StudioModel();
    ~StudioModel();
    std::string name;              // outliner label
    std::string libraryId;         // character id in the library (saved display scale), empty for others
    std::filesystem::path path;
    std::filesystem::path fx;          // M5c: effect-object effects (kind Effect; optional with a mesh)
    std::filesystem::path defaultPackTextureFolder;  // model's MME face texture folder (shader pack default)
    uint32_t uid = 0;              // stable id (prop parents, async loads); indices shift when a model is removed
    ModelKind kind = ModelKind::Character;  // stage: no physics, drawn first; prop: follows `attach`, no physics
    PropAttach attach;             // props: parent = uid of the parent model (-1 world)
    PropAttach place;              // characters / stages: world placement (translation, rotationDeg, scale)
    PropAttach placeApplied;       // the placement the instance was last posed with (physics resets when it changes)
    bool visible = true;
    bool physicsOff = false;       // P2.2: rigid-body physics is off for this model (saved in the project; baked keys replace it)
    ShaderChoice shader;           // characters: shader pack (render/ShaderPack.h); empty = the default shading
    MmeChoice mme;                 // M6a: MME object effect choice; characters default None, stages Auto
    MaterialChoice material;       // P5: per-material PBR source (Default / Ray-MMD + optional explicit .emd)
    // S1b: a part of an MMDX stage keeps the shared manifest, its index in `manifest->models` and (S3) the stage entity
    // it belongs to. The runtime rebuilds the rule -> material binding from it; the stage's variant and param overrides
    // live in StudioDoc::stages (StudioStage), the project saves them there.
    std::shared_ptr<const StageManifest> stageManifest;
    int stagePart = -1;            // index in manifest->models (-1 = not an MMDX stage part)
    uint32_t stageUid = 0;         // the StudioDoc::stages entity this part belongs to (0 = none)
    StageMaterialBinding stageBinding;   // rule -> material map (resolved when the part enters the document)
    StageSurfacePack stageSurfacePack;   // S6: the bundled surface pack of the part (resolved with stageBinding)
    std::shared_ptr<const PmxModel> pmx;
    std::unique_ptr<ModelInstance> inst;
    std::unique_ptr<GpuModel> gpu;
    MotionData motion;
    uint64_t motionVersion = 1;    // bumped by every edit of `motion`
    uint64_t boundVersion = 0;     // motionVersion `bound` was built from
    std::shared_ptr<BoundMotion> bound;
    PoseLayer pose;                // unregistered viewport edits (override the motion at pose.frame)
    // stages: what the static BLAS was last built from (StudioUpdateModel rebuilds it when this changes)
    uint64_t stageMotionApplied = 0;
    float stageFrameApplied = -1.0f;
    PoseLayer stagePoseApplied;
    // Display-frame group of each bone/morph's first timeline row (CanonicalRow); filled on load.
    std::vector<uint32_t> boneRowGroup, morphRowGroup;
    void BuildRowGroups();
    bool IsStage() const { return kind == ModelKind::Stage; }
    // M5c: an effect object is an accessory `.x` + its same-stem `.fx`; it reuses the prop row / accessory key
    // track (the keys drive the controller panel), so IsProp() is true for it too.
    bool IsEffect() const { return kind == ModelKind::Effect; }
    bool IsProp() const { return kind == ModelKind::Prop || kind == ModelKind::Effect; }
};


// Timeline row ids: kind in the top byte, then a 24-bit group and a 32-bit index. Light is the camera VMD's light
// track; SceneLight is the keys of one scene light (SceneLight.h): MakeRowId(RowKind::SceneLight, 0, light uid).
// Focus is the studio's depth-of-field focus track (camera MotionData::focus, saved in the project, not in VMD).
enum class RowKind : uint8_t { Group = 1, Bone = 2, Morph = 3, Camera = 4, Light = 5, Shadow = 6, SceneLight = 7, Focus = 8, Accessory = 9, EffectParam = 10, WorldParam = 11, GraphLane = 12, GraphChannel = 13, StageParam = 14 };
// GraphLane rows are the kTimelineGraphRows rows of an open value / speed graph lane: never a track, no keys. Their id
// group is the parent row's low 24 bits and their index the block's channel in the high byte, the lane index
// (0..kTimelineGraphRows-1) in the low byte: MakeRowId(GraphLane, parentLow24, (channel << 8) | laneIndex);
// `TimelineRow::graphOf` carries the parent id and `graphLaneChannel` the block's channel (kTimelineGraphAllChannels
// for the "all channels" lane). A block = the lane rows of one (parent, channel) and sits directly under its channel row.
// GraphChannel rows are the channel sub-rows of an open graph (one per channel, printed right after the parent): never a
// track, no keys of their own (they show the parent's keys dimmed). Their id group is the parent's low 24 bits and their
// index the channel; `TimelineRow::graphOf` carries the parent id and `graphChannelIndex` the channel.
// Group rows of the effect / world trees (RowKind::Group): the category groups are MakeRowId(Group, 0, 0) ("화면 효과")
// and MakeRowId(Group, 0, 1) ("월드"); an entity group is MakeRowId(Group, 0x400000 | uid, 0) (effect) or
// MakeRowId(Group, 0x800000 | uid, 0) (world); a pack.json group of that entity is the same group word with the index
// 1 + its index in the pack's `groups` ("params without a group" sit directly under the entity). Model display-frame
// groups are MakeRowId(Group, displayFrameIndex, 0) and never set the 0x400000 / 0x800000 bits.
// S3: the stage tree uses the same scheme: the category group is MakeRowId(Group, 0, 2) ("스테이지"), an entity group
// MakeRowId(Group, 0xC00000 | stage uid, 0) and a manifest `groups` entry the same group word with the index 1 + its
// index in the manifest's `groups`.
inline uint64_t MakeRowId(RowKind k, uint32_t group, uint32_t index) {
    return ((uint64_t)k << 56) | ((uint64_t)(group & 0xFFFFFF) << 32) | index;
}
inline RowKind RowKindOf(uint64_t id) { return (RowKind)(id >> 56); }
inline uint32_t RowIndexOf(uint64_t id) { return (uint32_t)id; }
inline uint32_t RowGroupOf(uint64_t id) { return (uint32_t)(id >> 32) & 0xFFFFFF; }
// Camera, Light, Shadow and Focus all live in the camera MotionData (model -1): they are selected, edited and
// undone through it together. (SceneLight rows are offered through the camera target too, but live in the light list.)
// S4: the Camera row edits the SELECTED camera (StudioDoc::selectedCameraUid; Light / Shadow / Focus stay the main
// camera's). The output and the viewport always use the main camera (cuts = MMD's 1-frame key pairs); the extra
// cameras feed the stage screens only.
inline bool IsCameraKind(RowKind k) {
    return k == RowKind::Camera || k == RowKind::Light || k == RowKind::Shadow || k == RowKind::Focus;
}
// The track name of a SceneLight row (StudioTrackOfRow, TrackState::name): the light's uid in decimal.
inline std::string LightTrackName(uint32_t uid) { return std::to_string(uid); }
inline uint32_t LightUidOfTrack(const std::string& name) { return (uint32_t)std::strtoul(name.c_str(), nullptr, 10); }

// The track name of an EffectParam row: "<effect uid>:<param key>".
inline std::string EffectParamTrackName(uint32_t uid, const std::string& param) {
    return std::to_string(uid) + ":" + param;
}
inline bool ParseEffectParamTrackName(const std::string& name, uint32_t& uid, std::string& param) {
    const size_t c = name.find(':');
    if (c == std::string::npos) return false;
    uid = (uint32_t)std::strtoul(name.substr(0, c).c_str(), nullptr, 10);
    param = name.substr(c + 1);
    return true;
}

// The track name of a WorldParam row: "<world uid>:<param key>" (same shape as an effect param).
inline std::string WorldParamTrackName(uint32_t uid, const std::string& param) {
    return std::to_string(uid) + ":" + param;
}
inline bool ParseWorldParamTrackName(const std::string& name, uint32_t& uid, std::string& param) {
    return ParseEffectParamTrackName(name, uid, param);
}

// The track name of a StageParam row: "<stage uid>:<param key>" (a colour param keys per component, "<key>#0..3").
inline std::string StageParamTrackName(uint32_t uid, const std::string& param) {
    return std::to_string(uid) + ":" + param;
}
inline bool ParseStageParamTrackName(const std::string& name, uint32_t& uid, std::string& param) {
    return ParseEffectParamTrackName(name, uid, param);
}

// The row that represents a bone/morph track in the key selection: a bone listed in two display frames has two rows,
// but its keys are selected through the first one only (CanonicalRow), so counts and edits see each key once.
uint64_t CanonicalRow(const StudioModel& m, RowKind kind, uint32_t index);

// A prop's placement at `frame`: without accessory keys the static `attach`, else the evaluated key (translation /
// rotation / scale / visibility / parent / bone), so a keyed prop keeps today's `attach` as its no-key fallback.
PropAttach AccessoryPlacement(const StudioModel& m, float frame);
// A prop's evaluated opacity / visibility at `frame` (1 / true without accessory keys).
float AccessoryOpacity(const StudioModel& m, float frame);
bool AccessoryVisible(const StudioModel& m, float frame);

// A key in the timeline selection: row (bone/morph/camera rows only, never groups) + frame.
using KeyId = std::pair<uint64_t, int>;

struct ClipboardKey {
    uint64_t row = 0;
    int offset = 0;  // frame relative to the first copied key
    BoneKf bone; MorphKf morph; CameraKf camera; LightKf light; ShadowKf shadow; LightKey sceneLight; FocusKf focus;
    AccessoryKf accessory; EffectParamKey effectParam; EffectParamKey worldParam; EffectParamKey stageParam;
};

struct StudioDoc {
    std::vector<std::unique_ptr<StudioModel>> models;  // stage parts first, then characters
    // The MAIN camera (uid 0): its camera keys plus the light / self-shadow / focus tracks. S4 camera entities are
    // `cameras`: the viewport, play-through and renders always use this one (MMD compatible), the extra cameras feed
    // the stage screens only.
    MotionData camera;                                 // camera keys only
    // S4: the extra (non-main) cameras. Camera keys only; the light / shadow / focus tracks stay the main camera's.
    std::vector<StudioCamera> cameras;
    uint32_t nextCameraUid = 1;         // extra camera uids are separate from the model / light uids
    uint32_t selectedCameraUid = 0;     // the camera the camera rows / inspector / possession edit (0 = main)
    uint64_t cameraVersion = 1, cameraEvalVersion = 0;   // the main camera + extra camera keys (row fills, evals)
    std::shared_ptr<CameraMotion> cameraEval;
    // S4: one evaluator per extra camera (index = `cameras`, null when it has no keys), rebuilt with `cameraEvalVersion`.
    std::vector<std::shared_ptr<CameraMotion>> cameraEvals;
    std::filesystem::path audioPath;
    bool hasAudio = false;
    float audioEndFrame = 0;          // timeline frame where the audio ends (includes audioOffset)
    double audioOffset = 0;           // seconds: the audio starts at this timeline time (may be negative)
    bool showWaveform = true;         // show the timeline's audio lane (clip + waveform, drag = offset)
    // Background AVI (背景AVI): an absolute video path (empty = none) whose current frame feeds screen.bmp materials
    // and, with bgVideoAsBackground, is also drawn as the screen background. bgVideoOffset is seconds: the video
    // starts at this timeline time (may be negative, like audioOffset).
    std::filesystem::path bgVideoPath;
    double bgVideoOffset = 0;
    bool bgVideoAsBackground = false;
    CommandStack history;
    // Project state: dirty = an undoable edit (history) or a project edit outside the history (models added/removed,
    // audio, visibility, rename) since the last save.
    std::filesystem::path projectPath;  // .mmdxproj, empty = never saved ("untitled")
    uint64_t savedVersion = 0;          // history.Version() at the last save
    uint64_t projectVersion = 1;        // bumped by project edits outside the history
    uint64_t savedProjectVersion = 1;   // projectVersion at the last save
    uint64_t autosavedStamp = 0;        // ChangeStamp() of the last autosave
    uint32_t nextUid = 1;
    // The scene lights (SceneLight.h): sun, point / spot lights and the ambient light, each with its own keys. List
    // order = timeline order. The undoable edits of the list are LightsCommand; key edits are TrackEditCommand.
    std::vector<SceneLight> lights;
    uint32_t nextLightUid = 1;        // light uids are separate from the model uids
    // M6b: the project's own MME post stack (docs/mme/mme_design.md §2.10); while the studio is open it replaces
    // the play-mode stack (RenderSettings::mmeEffects) and offline renders use it. Saved in the project.
    std::vector<MmePostEntry> mmePost;
    // Version 3: the project's own effect stack (StudioEffects.h). While the studio is open it replaces
    // the play-mode stack (RenderSettings::packEffects) and offline renders use it. Saved in the project.
    std::vector<StudioEffect> effects;
    uint32_t nextEffectUid = 1;
    // Version 3: world entities (StudioWorld above), keyable like effects. Saved in the project.
    std::vector<StudioWorld> worlds;
    uint32_t nextWorldUid = 1;
    // S3: one entity per MMDX stage (StudioStage): variant, param base values and keyed params are project overrides.
    std::vector<StudioStage> stages;
    uint32_t nextStageUid = 1;
    StudioEffect* FindEffect(uint32_t uid) {
        for (StudioEffect& e : effects)
            if (e.uid == uid) return &e;
        return nullptr;
    }
    const StudioEffect* FindEffect(uint32_t uid) const { return const_cast<StudioDoc*>(this)->FindEffect(uid); }
    StudioWorld* FindWorld(uint32_t uid) {
        for (StudioWorld& w : worlds)
            if (w.uid == uid) return &w;
        return nullptr;
    }
    const StudioWorld* FindWorld(uint32_t uid) const { return const_cast<StudioDoc*>(this)->FindWorld(uid); }
    StudioStage* FindStage(uint32_t uid) {
        for (StudioStage& s : stages)
            if (s.uid == uid) return &s;
        return nullptr;
    }
    const StudioStage* FindStage(uint32_t uid) const { return const_cast<StudioDoc*>(this)->FindStage(uid); }
    // The stage entity a model is a part of (nullptr for an ordinary model).
    StudioStage* StageOfModel(const StudioModel& m) { return m.stageUid ? FindStage(m.stageUid) : nullptr; }
    const StudioStage* StageOfModel(const StudioModel& m) const { return const_cast<StudioDoc*>(this)->StageOfModel(m); }
    bool Dirty() const { return history.Version() != savedVersion || projectVersion != savedProjectVersion; }
    uint64_t ChangeStamp() const { return history.Version() + (projectVersion << 32); }
    void MarkSaved() { savedVersion = history.Version(); savedProjectVersion = projectVersion; }

    // playback
    double time = 0;            // seconds
    bool playing = false;
    float physicsFrame = -1;    // frame of the last physics step (-1: reset)
    std::chrono::steady_clock::time_point physicsClock;  // wall time of the last physics step
    bool useMotionCamera = true;
    bool useShadowTrack = true;     // the self-shadow track drives the shadows (else the render settings)
    bool showCameraPath = true;     // camera path overlay in the viewport (free camera only)
    bool modelGizmo = false;        // viewport gizmo moves / rotates the selected character / stage (no bone picked)
    int viewLayout = 0;             // 0 single perspective view, 1 quad view (perspective + top / front / left, orthographic)
    DirectX::XMFLOAT3 quadCenter[3] = {{0, 10, 0}, {0, 10, 0}, {0, 10, 0}};  // per ortho view (top / front / left): what it looks at
    float quadHeight[3] = {40.0f, 40.0f, 40.0f};  // per ortho view: world units across its height (zoom)
    bool possessCamera = false;     // C4D style: viewport navigation edits the motion camera instead of the free view
    int shading = 0;                // viewport shading (ViewShading): 0 lit, 1 unlit, 2 wireframe (raster path only)
    bool frameMask = true;         // camera view: render the frame only and dim the rest (WYSIWYG with the video)
    int aspectW = 16, aspectH = 9; // frame aspect ratio (default 16:9)
    int AspectIndex() const { return AspectIndexFromWH(aspectW, aspectH); }
    const char* AspectLabel() const { return kAspectPresets[AspectIndex()].label; }
    float AspectRatio() const { return AspectRatioOf(aspectW, aspectH); }
    void SetAspect(int w, int h) {
        int idx = AspectIndexFromWH(w, h);
        SetAspectPreset(idx);
    }
    void SetAspectPreset(int idx) {
        idx = std::clamp(idx, 0, kAspectPresetCount - 1);
        if (aspectW != kAspectPresets[idx].w || aspectH != kAspectPresets[idx].h) {
            aspectW = kAspectPresets[idx].w;
            aspectH = kAspectPresets[idx].h;
            ++projectVersion;
        }
    }
    bool showSafeFrames = false;    // title / action safe rectangles inside the frame
    bool showThirds = false;        // rule-of-thirds guides inside the frame
    bool autoKey = true;           // editing a value in the inspector keys it at the playhead (Adobe / Blender style)

    // editor
    int selectedModel = -1;     // index into models; -1 = camera (the camera VMD, or a scene light: selectedLightUid)
    uint32_t selectedLightUid = 0;  // the selected scene light (0 = none); selectedModel stays -1 (-2 is the audio row)
    TimelineView view;
    int followFrame = -1;             // frame the timeline last scrolled to show (follows playback and seeks)
    std::set<KeyId> selection;
    std::set<uint64_t> collapsed;     // collapsed group rows (per selected model; cleared on switch)
    std::set<uint64_t> expanded;      // default-collapsed groups the user opened (entity / pack group rows; cleared with `collapsed`)
    std::set<uint64_t> graphOpen;     // rows whose value / speed graph lanes are shown (not saved to the project)
    std::set<uint64_t> graphLaneOpen; // channel rows whose own graph lane is open (not saved; cleared like graphOpen)
    std::vector<ClipboardKey> clipboard;
    int clipboardModel = -2;          // model the clipboard came from (-1 camera, -2 empty)
    int curveChannel = 3;             // inspector: bone 0..3 (X,Y,Z,R), camera 0..5
    bool curveAllChannels = false;    // inspector: a curve edit / preset applies to every channel of the key
    std::vector<TimelineRow> rows;    // cached timeline rows
    uint64_t rowsKey = ~0ull;         // inputs the cache was built from
    bool curveEditing = false;        // a curve edit is in progress (one undo step per drag)
    std::vector<struct TrackState> curveBefore;  // tracks before the edit started

    // rows, range, playback options
    std::set<uint64_t> selectedRows;  // rows picked on the label column (bone/morph/camera/group rows)
    uint64_t rowAnchor = 0;           // Shift+click range anchor (last plain/Ctrl-clicked row)
    std::map<uint64_t, std::vector<uint64_t>> groupChildren;  // group row -> its bone/morph rows (also when collapsed)
    bool loop = false;                // playback loops over the frame range (or the whole timeline without one)
    bool physics = true;              // physics toggle (initialised from the app settings on load)
    bool physicsAlways = false;       // keep simulating even when paused on the same frame (hair/skirts settle)
    bool showPhysicsBones = false;    // show physics-driven bones in the viewport overlay even when physics is off
    // The gravity and the wind, saved in the project ("physicsWorld"); edits go through PhysicsEnvCommand (one undo
    // step per gesture).
    PhysicsEnv physicsEnv;
    bool showWindVector = true;   // viewport 3D wind-direction arrow
    RowKind curveClipKind = RowKind::Group;  // copied interpolation block: Bone (64 bytes) or Camera (24); Group = empty
    uint8_t curveClip[64] = {};

    // pose editing (viewport)
    std::set<int> selectedBones;      // bones picked in the viewport / on bone rows (selected model)
    int activeBone = -1;              // the gizmo's bone (last picked), -1 none
    int gizmoTool = 0;                // 0 rotate, 1 translate
    bool gizmoLocal = false;          // gizmo axes: bone-local (true) or global
    bool autoIk = false;              // auto-IK posing: dragging a wrist / hand solves the parent chain (not saved)
    bool showBones = true;            // bone overlay
    int inspectorTab = 0;             // 0 keys, 1 bone, 2 morphs (characters only)
    int poseScope = 0;                // VPD / mirror scope: 0 whole model, 1 selected bones
    int pendingSeekFrame = -1;        // set by pose undo/redo: the App seeks there (the pose belongs to that frame)
    int scrollToRow = 0;              // > 0: the timeline scrolls the row `scrollRowId` into view (frames left to try)
    uint64_t scrollRowId = 0;
    char morphFilter[64] = {};
    char boneFilter[64] = {};       // narrows the timeline's bone / morph rows (inspector search field)

    int Frame() const { return (int)std::floor(time * kMmdFps + 1e-4); }
    bool HasRange() const { return view.rangeStart >= 0 && view.rangeEnd >= view.rangeStart; }
    int EndFrame() const;             // last key over all motions, the audio length, at least 300
    StudioModel* Selected() { return selectedModel >= 0 && selectedModel < (int)models.size() ? models[selectedModel].get() : nullptr; }
    const StudioModel* Selected() const { return const_cast<StudioDoc*>(this)->Selected(); }
    void TouchModel(int model);       // after editing a motion (model -1 = camera)
    int IndexOfUid(uint32_t uid) const;  // -1 when no model has it
    SceneLight* FindLight(uint32_t uid) {  // nullptr when no light has it
        for (SceneLight& l : lights)
            if (l.uid == uid) return &l;
        return nullptr;
    }
    const SceneLight* FindLight(uint32_t uid) const { return const_cast<StudioDoc*>(this)->FindLight(uid); }
    // S4: an extra camera (uid 0 = the main camera, which is not in the list: nullptr).
    StudioCamera* FindCamera(uint32_t uid) {
        if (uid == 0) return nullptr;
        for (StudioCamera& c : cameras)
            if (c.uid == uid) return &c;
        return nullptr;
    }
    const StudioCamera* FindCamera(uint32_t uid) const { return const_cast<StudioDoc*>(this)->FindCamera(uid); }
    // The motion a camera uid edits: uid 0 = the main camera (`camera`), else the extra camera's track. A uid that no
    // camera has falls back to the main camera, so an edit never writes into nothing.
    MotionData& CameraTrack(uint32_t uid) {
        StudioCamera* c = FindCamera(uid);
        return c ? c->track : camera;
    }
    const MotionData& CameraTrack(uint32_t uid) const { return const_cast<StudioDoc*>(this)->CameraTrack(uid); }
    // The selected camera's motion (0 = main): what the camera rows / inspector / possession edit.
    MotionData& SelectedCameraTrack() { return CameraTrack(selectedCameraUid); }
    const MotionData& SelectedCameraTrack() const { return CameraTrack(selectedCameraUid); }
    // Camera keys of a uid (uid 0 = the main camera, unknown = empty).
    const std::vector<CameraKf>& CameraKeys(uint32_t uid) const;
    // A name for the camera of `uid` (the outliner / menus): the main camera's or the extra camera's label.
    std::string CameraLabel(uint32_t uid) const;
    void TouchCamera(uint32_t uid);   // after editing a camera's keys (0 = main)
    // S4: rebuilds the evaluators when `cameraVersion` moved (the app calls this once per frame instead of rebuilding
    // `cameraEval` itself): `cameraEval` = the main camera, `cameraEvals[i]` = `cameras[i]` (null = no keys).
    void RefreshCameraEvals();
    // The main camera has keys: what enables camera view / the camera controls (`cameraEval` is its evaluator).
    bool HasCameraKeys() const { return !camera.camera.empty(); }
    // The selected camera's evaluator (null when it has no keys): the path overlay / inspector preview.
    const std::shared_ptr<CameraMotion>& SelectedCameraEval() const;
};

// Track snapshots: the generic undoable edit. Holds whole tracks before and after an edit, which keeps
// every operation (move, delete, paste, curve, insert) trivially reversible.
struct TrackState {
    int model = -1;                // -1 camera
    RowKind kind = RowKind::Bone;  // Bone, Morph, Camera, Light, Shadow, Focus, SceneLight, EffectParam, WorldParam or StageParam
    std::string name;              // bone/morph name (scene light: LightTrackName, effect / world / stage param: their track name); unused for the camera
    uint32_t uid = 0;              // SceneLight: the light's uid; EffectParam / WorldParam: the entity's uid
    uint32_t cameraUid = 0;        // S4: Camera rows: the edited camera's uid (0 = the main camera)
    bool existed = false;          // the track existed (absent tracks are erased again)
    std::vector<BoneKf> bones;
    std::vector<MorphKf> morphs;
    std::vector<CameraKf> cameras;
    std::vector<LightKf> lights;   // the camera VMD's light track
    std::vector<ShadowKf> shadows;
    std::vector<FocusKf> focuses;
    std::vector<LightKey> lightKeys;  // a scene light's keys
    std::vector<AccessoryKf> accessories;
    std::vector<EffectParamKey> effectParamKeys;
    std::vector<EffectParamKey> worldParamKeys;
    std::vector<EffectParamKey> stageParamKeys;
    bool staticFlag = false;       // Bone/Morph: the track was flagged constant (MotionData::staticBones/staticMorphs)

    size_t Bytes() const {
        return sizeof(TrackState) + name.size() + bones.capacity() * sizeof(BoneKf) + morphs.capacity() * sizeof(MorphKf) +
               cameras.capacity() * sizeof(CameraKf) + lights.capacity() * sizeof(LightKf) +
               shadows.capacity() * sizeof(ShadowKf) + focuses.capacity() * sizeof(FocusKf) +
               lightKeys.capacity() * sizeof(LightKey) + accessories.capacity() * sizeof(AccessoryKf) +
               effectParamKeys.capacity() * sizeof(EffectParamKey) +
               worldParamKeys.capacity() * sizeof(EffectParamKey) +
               stageParamKeys.capacity() * sizeof(EffectParamKey);
    }
};
// A SceneLight track is addressed by its uid (`name` = LightTrackName(uid)): restoring one whose light no longer
// exists does nothing; an emptied key list keeps the light (its base values drive it again).
// A Camera track (S4) is addressed by `cameraUid` (0 = the main camera): restoring one whose camera no longer exists
// does nothing.
// An EffectParam track is addressed by its uid and param key (`name` = EffectParamTrackName(uid, param)).
// A StageParam track is addressed by the stage's uid and param key (`name` = StageParamTrackName(uid, param)).
TrackState CaptureTrack(StudioDoc& doc, int model, RowKind kind, const std::string& name);
// The same track as `before` addresses it (including `cameraUid`: the camera row's selected camera may have moved on).
TrackState CaptureTrack(StudioDoc& doc, const TrackState& before);
void RestoreTrack(StudioDoc& doc, const TrackState& s);

// ---- S3: studio stage param values (pure; docs/stage_format.md "Studio") ----
// The base param key of a track name ("key" -> "key"; "key#c" -> "key", comp = c; comp = -1 otherwise).
std::string StageParamBaseKey(const std::string& name, int& comp);
// One component of a stage track without keys: the stage's base value, else the param's default (component c of a
// colour; 0 for an unknown param).
float StageParamStaticValue(const StudioStage& stage, const std::string& name);
// The stage's param overrides at `frame`: `base` with every keyed component's value at `frame` on top, and the params
// whose manifest cue is skipped (cuesOff plus every param that has a track).
struct StudioStageOverrides {
    StageParamValues values;
    std::set<std::string> skipCues;
};
StudioStageOverrides StageOverridesAt(const StudioStage& stage, float frame);
// Turns the manifest's cue tracks into keyable param tracks (colours per component) and marks those params cuesOff, so
// the project's keys replace the stage's own light show. True = something was imported. One StagesCommand step is the
// caller's business.
bool ImportStageCues(StudioStage& stage);
// Drops a param's base value and every one of its tracks (the variant / default drives it again).
bool ResetStageParam(StudioStage& stage, const std::string& key);

// Adds, removes, reorders or edits scene lights (undoable): holds the whole list before and after. Preset
// application is one of these. Key edits of a light go through TrackEditCommand instead.
class LightsCommand : public Command {
public:
    LightsCommand(StudioDoc& doc, std::string name, std::vector<SceneLight> before, std::vector<SceneLight> after)
        : doc_(doc), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { doc_.lights = after_; doc_.rowsKey = ~0ull; }
    void Undo() override { doc_.lights = before_; doc_.rowsKey = ~0ull; }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        const auto list = [](const std::vector<SceneLight>& v) {
            size_t n = 0;
            for (const SceneLight& l : v) n += l.ApproxBytes();
            return n;
        };
        return sizeof(*this) + name_.size() + list(before_) + list(after_);
    }

private:
    StudioDoc& doc_;
    std::string name_;
    std::vector<SceneLight> before_, after_;
};

// Adds, removes, reorders or edits effect entries (undoable): holds the whole list before and after.
// Key edits of an effect go through TrackEditCommand instead.
class EffectsCommand : public Command {
public:
    EffectsCommand(StudioDoc& doc, std::string name, std::vector<StudioEffect> before, std::vector<StudioEffect> after)
        : doc_(doc), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { doc_.effects = after_; doc_.rowsKey = ~0ull; }
    void Undo() override { doc_.effects = before_; doc_.rowsKey = ~0ull; }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        const auto list = [](const std::vector<StudioEffect>& v) {
            size_t n = 0;
            for (const StudioEffect& e : v) n += e.ApproxBytes();
            return n;
        };
        return sizeof(*this) + name_.size() + list(before_) + list(after_);
    }

private:
    StudioDoc& doc_;
    std::string name_;
    std::vector<StudioEffect> before_, after_;
};

// Adds, removes, reorders or edits world entities (undoable): holds the whole list before and after.
// Key edits of a world go through TrackEditCommand instead.
class WorldsCommand : public Command {
public:
    WorldsCommand(StudioDoc& doc, std::string name, std::vector<StudioWorld> before, std::vector<StudioWorld> after)
        : doc_(doc), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { doc_.worlds = after_; doc_.rowsKey = ~0ull; }
    void Undo() override { doc_.worlds = before_; doc_.rowsKey = ~0ull; }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        const auto list = [](const std::vector<StudioWorld>& v) {
            size_t n = 0;
            for (const StudioWorld& w : v) n += w.ApproxBytes();
            return n;
        };
        return sizeof(*this) + name_.size() + list(before_) + list(after_);
    }
private:
    StudioDoc& doc_;
    std::string name_;
    std::vector<StudioWorld> before_, after_;
};

// S4: adds, removes, renames or edits the extra cameras — one undo step over the whole list. Key edits of a camera go
// through TrackEditCommand instead (TrackState::cameraUid); a removed camera whose keys are restored later is skipped.
// The selected camera falls back to the main camera when the selected one is gone.
class CamerasCommand : public Command {
public:
    CamerasCommand(StudioDoc& doc, std::string name, std::vector<StudioCamera> before, std::vector<StudioCamera> after)
        : doc_(doc), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        const auto list = [](const std::vector<StudioCamera>& v) {
            size_t n = 0;
            for (const StudioCamera& c : v) n += c.ApproxBytes();
            return n;
        };
        return sizeof(*this) + name_.size() + list(before_) + list(after_);
    }

private:
    void Apply(const std::vector<StudioCamera>& v) {
        doc_.cameras = v;
        if (doc_.selectedCameraUid != 0 && !doc_.FindCamera(doc_.selectedCameraUid)) doc_.selectedCameraUid = 0;
        doc_.TouchCamera(0);
    }
    StudioDoc& doc_;
    std::string name_;
    std::vector<StudioCamera> before_, after_;
};

class StagesCommand : public Command {
public:
    StagesCommand(StudioDoc& doc, std::string name, std::vector<StudioStage> before, std::vector<StudioStage> after)
        : doc_(doc), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { doc_.stages = after_; doc_.rowsKey = ~0ull; }
    void Undo() override { doc_.stages = before_; doc_.rowsKey = ~0ull; }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        const auto list = [](const std::vector<StudioStage>& v) {
            size_t n = 0;
            for (const StudioStage& s : v) n += s.ApproxBytes();
            return n;
        };
        return sizeof(*this) + name_.size() + list(before_) + list(after_);
    }
private:
    StudioDoc& doc_;
    std::string name_;
    std::vector<StudioStage> before_, after_;
};

class TrackEditCommand : public Command {public:
    TrackEditCommand(StudioDoc& doc, std::string name, std::vector<TrackState> before, std::vector<TrackState> after)
        : doc_(doc), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { for (const auto& s : after_) RestoreTrack(doc_, s); }
    void Undo() override { for (const auto& s : before_) RestoreTrack(doc_, s); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        size_t bytes = sizeof(*this) + name_.size();
        for (const TrackState& s : before_) bytes += s.Bytes();
        for (const TrackState& s : after_) bytes += s.Bytes();
        return bytes;
    }

private:
    StudioDoc& doc_;
    std::string name_;
    std::vector<TrackState> before_, after_;
};

// Replaces a model's pose layer (viewport edits, register, reset, VPD, mirror). Restoring a non-empty layer of another
// frame asks the App to seek there (pendingSeekFrame), since a pose only applies at its own frame.
class PoseEditCommand : public Command {
public:
    PoseEditCommand(StudioDoc& doc, int model, std::string name, PoseLayer before, PoseLayer after)
        : doc_(doc), model_(model), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        return sizeof(*this) + name_.size() + (before_.bones.size() + after_.bones.size()) * 64 +
               (before_.morphs.size() + after_.morphs.size()) * 48;
    }

private:
    void Apply(const PoseLayer& l) {
        if (model_ < 0 || model_ >= (int)doc_.models.size()) return;
        doc_.models[model_]->pose = l;
        if (!l.Empty() && l.frame != doc_.Frame()) doc_.pendingSeekFrame = l.frame;
    }
    StudioDoc& doc_;
    int model_;
    std::string name_;
    PoseLayer before_, after_;
};

// Replaces a whole motion (VMD import): also covers IK tracks, which TrackState does not.
class MotionSwapCommand : public Command {
public:
    MotionSwapCommand(StudioDoc& doc, int model, std::string name, MotionData before, MotionData after)
        : doc_(doc), model_(model), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override { return sizeof(*this) + name_.size() + before_.ApproxBytes() + after_.ApproxBytes(); }

private:
    void Apply(const MotionData& m) {
        if (model_ < 0) doc_.camera = m;
        else if (model_ < (int)doc_.models.size()) doc_.models[model_]->motion = m;
        doc_.TouchModel(model_);
    }
    StudioDoc& doc_;
    int model_;
    std::string name_;
    MotionData before_, after_;
};

// P2.2: the per-model rigid-body physics switch (StudioModel::physicsOff, saved in the project). The next
// frame's StudioUpdateModel re-enables / disables the instance from the flag, so Apply only clears it.
class PhysicsOffCommand : public Command {
public:
    PhysicsOffCommand(StudioDoc& doc, int model, bool before, bool after)
        : doc_(doc), model_(model), before_(before), after_(after) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return "physics"; }
    size_t Bytes() const override { return sizeof(*this); }

private:
    void Apply(bool v) {
        if (model_ < 0 || model_ >= (int)doc_.models.size()) return;
        // StudioUpdateModel calls EnablePhysics from the flag every frame; reset so the change takes at the next pose.
        doc_.models[(size_t)model_]->physicsOff = v;
        doc_.physicsFrame = -1.0f;
        ++doc_.projectVersion;
    }
    StudioDoc& doc_;
    int model_;
    bool before_, after_;
};

// A gravity / wind edit (StudioDoc::physicsEnv before and after). The bodies re-seed at the next pose, so a paused
// viewport shows the new settings settled.
class PhysicsEnvCommand : public Command {
public:
    PhysicsEnvCommand(StudioDoc& doc, std::string name, const PhysicsEnv& before, const PhysicsEnv& after)
        : doc_(doc), name_(std::move(name)), before_(before), after_(after) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override { return sizeof(*this) + name_.size(); }

private:
    void Apply(const PhysicsEnv& e) {
        doc_.physicsEnv = e;
        doc_.physicsFrame = -1.0f;
    }
    StudioDoc& doc_;
    std::string name_;
    PhysicsEnv before_, after_;
};

// The audio offset (the timeline's audio clip drag, the audio menu): seconds before / after. The host re-seeks the
// sound after an undo / redo that changed it.
class AudioOffsetCommand : public Command {
public:
    AudioOffsetCommand(StudioDoc& doc, double before, double after) : doc_(doc), before_(before), after_(after) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return "audio offset"; }
    size_t Bytes() const override { return sizeof(*this); }

private:
    void Apply(double v) {
        doc_.audioEndFrame += (float)((v - doc_.audioOffset) * 30.0);  // the end frame includes the offset
        doc_.audioOffset = v;
        ++doc_.projectVersion;
    }
    StudioDoc& doc_;
    double before_, after_;
};

// One IK bone's enable track (VMD IK on/off keys): small, unlike MotionSwapCommand.
class IkEditCommand : public Command {
public:
    IkEditCommand(StudioDoc& doc, int model, std::string name, std::string ikName, std::vector<IkKf> before,
                  std::vector<IkKf> after)
        : doc_(doc), model_(model), name_(std::move(name)), ik_(std::move(ikName)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override { return sizeof(*this) + name_.size() + ik_.size() + (before_.size() + after_.size()) * sizeof(IkKf); }

private:
    void Apply(const std::vector<IkKf>& keys) {
        if (model_ < 0 || model_ >= (int)doc_.models.size()) return;
        auto& ik = doc_.models[model_]->motion.ik;
        if (keys.empty()) ik.erase(ik_);
        else ik[ik_] = keys;
        doc_.TouchModel(model_);
    }
    StudioDoc& doc_;
    int model_;
    std::string name_, ik_;
    std::vector<IkKf> before_, after_;
};

// CPU side of a studio scene (worker thread).
struct StudioPackageModel {
    std::shared_ptr<PmxModel> pmx;
    std::vector<ImageRGBA8> textures;
    // S11: parallel to `textures`; the DDS blocks the GPU upload prefers (see LoadedModelCpu::compressedTextures).
    std::vector<CompressedImage> compressedTextures;
    std::string name, libraryId;
    ModelKind kind = ModelKind::Character;
    std::filesystem::path fx;                 // M5c: kind Effect only: the effect file (mesh = `pmx`, may be null)
    bool visible = true;
    bool physicsOff = false;                 // P2.2: physics off for this model (project files)
    std::filesystem::path defaultPackTextureFolder;  // model's MME face texture folder (shader pack default)
    std::vector<uint8_t> materialBlend;              // per-material .emd blend hint (asset/MmeHints::MmeBlendHint)
    uint8_t materialBlendDefault = 0;                // hint for materials with no own Obj entry
    PbrModelData pbr;                        // per-material Ray PBR records + decoded maps (work unit P2)
    PropAttach attach;                       // props: parent = index into StudioPackage::models (-1 world)
    PropAttach place;                        // characters / stages: world placement
    ShaderChoice shader;                     // characters: shader pack
    MmeChoice mme;                           // M6a: MME object effect choice
    MaterialChoice material;                 // P5: per-material PBR source (Default / Ray-MMD)
    // S1b: an MMDX stage part carries its manifest (shared with its siblings) and its index in manifest->models; S3 adds
    // the stage entity uid the part belongs to (the package's `stages` carry the variant / overrides).
    std::shared_ptr<const StageManifest> stageManifest;
    int stagePart = -1;
    uint32_t stageUid = 0;
    std::string stageVariant;                // a version <= 3 project's per-part variant (converted to a StudioStage)
    MotionData motion;                       // names canonicalised to pmx
};
struct StudioPackage {
    std::vector<StudioPackageModel> models;  // stage parts first
    MotionData camera;
    // S4: the project's extra cameras (a version <= 4 project: none). A version 5 project's switch track is flattened
    // into `camera` by the loader (the main camera alone is the output).
    std::vector<StudioCamera> cameras;
    std::filesystem::path audioPath;
    double audioOffset = 0;
    // Background AVI (背景AVI), from a project or PMM import.
    std::filesystem::path bgVideoPath;
    double bgVideoOffset = 0;
    bool bgVideoAsBackground = false;
    PhysicsEnv physicsEnv;                   // a project's gravity / wind (library scenes and imports: the defaults)
    // project loads only
    bool fromProject = false;
    bool fromPmm = false;                    // a .pmm import: a NEW unsaved project (no projectPath / recents)
    ProjectEditor editor;
    std::filesystem::path projectPath;       // the file loaded (recovery: the project it belongs to, may be empty)
    bool recovered = false;                  // loaded from the autosave: opens dirty
    std::vector<std::string> warnings;       // missing models / motions (shown as a toast)
    // M6b: the project's own MME post stack (StudioProject.h "mmeEffects"); while the studio is open it replaces
    // the play-mode stack (RenderSettings::mmeEffects).
    std::vector<MmePostEntry> mmePost;
    // Version 3: the project's own effect stack (StudioEffects.h).
    std::vector<StudioEffect> effects;
    // Version 3: the project's world entities (StudioWorld).
    std::vector<StudioWorld> worlds;
    // Version 4: the project's MMDX stage entities (StudioStage; a version <= 3 file's part variants become these).
    std::vector<StudioStage> stages;
    PmmEffectSettings pmmEffects;            // .pmm import: MME controller mapping (App applies it)
    int pmmWidth = 0, pmmHeight = 0;         // .pmm import: output resolution (aspect preset)
    float pmmFps = 0.0f;                     // .pmm import: preferred fps (informational)
    std::filesystem::path pmmSourcePath;     // .pmm import: the file opened (added to the recent list)
};
// `character` is required, `stage` and `song` may be null. The song's dance + facial VMDs become the
// character's motion (names canonicalised to the model), its camera VMD the camera track.
bool LoadStudioPackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset* song,
                       StudioPackage& out, LoadProgress* progress, std::string* error,
                       const MaterialChoiceFn& materialChoice = {}, const std::string& stageVariant = {});
// One model file (PMX, glTF/GLB/VRM, FBX/OBJ) as a character, stage or prop. Characters import with
// asset ModelRole::Character, stages and props with ModelRole::Stage. `out.motion` stays empty.
bool LoadStudioModel(const std::filesystem::path& path, ModelKind kind, const std::string& label,
                     StudioPackageModel& out, LoadProgress* progress, std::string* error,
                     const MaterialChoice& choice = {});
// Every part of a library stage (failed parts are skipped with a warning; false only when none loads).
// S1b: an MMDX stage's manifest rules are applied to each part on the worker thread and the manifest / part index /
// `variant` travel on the package models (`stageManifest`, `stagePart`, `stageVariant`).
bool LoadStudioStage(const StageAsset& stage, std::vector<StudioPackageModel>& out, LoadProgress* progress,
                     std::string* error, const MaterialChoice& choice = {}, const std::string& variant = {});
// A library song as studio motions: dance + facial VMDs merged into `dance` (camera/light/shadow cleared, names NOT
// canonicalised), the camera VMD's camera/light/shadow keys into `camera`. False when no VMD could be read.
bool LoadStudioSong(const SongAsset& song, MotionData& dance, MotionData& camera, std::string* error);
// A whole .mmdxproj (LoadProject) with its models and motions. `recovery`: the file is the autosave; projectPath
// becomes its recoveryOf and `recovered` is set. Models whose file fails to load are skipped with a warning (prop
// parents pointing at them become -1, later indices are remapped).
bool LoadStudioProjectPackage(const std::filesystem::path& file, bool recovery, StudioPackage& out,
                              LoadProgress* progress, std::string* error);
// A .pmm (MMD project) import (asset/PmmLoader.h). Only the camera / light / self-shadow tracks come
// directly (they do not need the models); each model's motion is split into one temporary VMD (under
// the temp dir, never the PMM's own folder) and re-read with LoadVmd, so PMM bone / morph / IK /
// interpolation semantics go through the VMD code. PMM stores no accessory visibility->prop mapping,
// so accessories become props statically placed at their first key; MME effect controllers are
// recognised (MapMmeControllers) and left out. `out.fromPmm` is set; `out.warnings` holds one line
// per unresolved model / unsupported controller. pkg.editor carries the frame range / aspect / fps
// (resolved by the App). `characters` / `stages` are the library, used to resolve a PMM model path by
// file name when neither the absolute path nor the folder relative to the .pmm exists.
bool LoadStudioPmmPackage(const std::filesystem::path& file, const std::vector<CharacterAsset>& characters,
                          const std::vector<StageAsset>& stages, StudioPackage& out, LoadProgress* progress,
                          std::string* error);

} // namespace mmdx::studio
