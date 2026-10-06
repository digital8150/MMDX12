#pragma once
// Studio document: the models of the scene with their editable motions, the camera motion, the undo
// history and the editor state (selection, timeline view). Owned by App while screen_ == Studio.
// Motions are keyed by bone/morph name (StudioMotion.h) and re-bound to the model after every edit.
#include "anim/Motion.h"
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"
#include "studio/CommandStack.h"
#include "studio/StudioMotion.h"
#include "studio/StudioPose.h"
#include "studio/StudioProject.h"
#include "studio/UiTimeline.h"
#include <cmath>
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
    uint32_t uid = 0;              // stable id (prop parents, async loads); indices shift when a model is removed
    ModelKind kind = ModelKind::Character;  // stage: no physics, drawn first; prop: follows `attach`, no physics
    PropAttach attach;             // props: parent = uid of the parent model (-1 world)
    PropAttach place;              // characters / stages: world placement (translation, rotationDeg, scale)
    PropAttach placeApplied;       // the placement the instance was last posed with (physics resets when it changes)
    bool visible = true;
    std::shared_ptr<const PmxModel> pmx;
    std::unique_ptr<ModelInstance> inst;
    std::unique_ptr<GpuModel> gpu;
    MotionData motion;
    uint64_t motionVersion = 1;    // bumped by every edit of `motion`
    uint64_t boundVersion = 0;     // motionVersion `bound` was built from
    std::shared_ptr<BoundMotion> bound;
    PoseLayer pose;                // unregistered viewport edits (override the motion at pose.frame)
    // Display-frame group of each bone/morph's first timeline row (CanonicalRow); filled on load.
    std::vector<uint32_t> boneRowGroup, morphRowGroup;
    void BuildRowGroups();
    bool IsStage() const { return kind == ModelKind::Stage; }
    bool IsProp() const { return kind == ModelKind::Prop; }
};

// Timeline row ids: kind in the top byte, then a 24-bit group and a 32-bit index.
enum class RowKind : uint8_t { Group = 1, Bone = 2, Morph = 3, Camera = 4, Light = 5, Shadow = 6 };
inline uint64_t MakeRowId(RowKind k, uint32_t group, uint32_t index) {
    return ((uint64_t)k << 56) | ((uint64_t)(group & 0xFFFFFF) << 32) | index;
}
inline RowKind RowKindOf(uint64_t id) { return (RowKind)(id >> 56); }
inline uint32_t RowIndexOf(uint64_t id) { return (uint32_t)id; }
inline uint32_t RowGroupOf(uint64_t id) { return (uint32_t)(id >> 32) & 0xFFFFFF; }
// Camera, Light and Shadow all live in the camera MotionData (model -1): they are selected, edited and
// undone through it together.
inline bool IsCameraKind(RowKind k) { return k == RowKind::Camera || k == RowKind::Light || k == RowKind::Shadow; }

// The row that represents a bone/morph track in the key selection: a bone listed in two display frames has two rows,
// but its keys are selected through the first one only (CanonicalRow), so counts and edits see each key once.
uint64_t CanonicalRow(const StudioModel& m, RowKind kind, uint32_t index);

// A key in the timeline selection: row (bone/morph/camera rows only, never groups) + frame.
using KeyId = std::pair<uint64_t, int>;

struct ClipboardKey {
    uint64_t row = 0;
    int offset = 0;  // frame relative to the first copied key
    BoneKf bone; MorphKf morph; CameraKf camera; LightKf light; ShadowKf shadow;
};

struct StudioDoc {
    std::vector<std::unique_ptr<StudioModel>> models;  // stage parts first, then characters
    MotionData camera;                                 // camera keys only
    uint64_t cameraVersion = 1, cameraEvalVersion = 0;
    std::shared_ptr<CameraMotion> cameraEval;
    std::filesystem::path audioPath;
    bool hasAudio = false;
    float audioEndFrame = 0;          // timeline frame where the audio ends (includes audioOffset)
    double audioOffset = 0;           // seconds: the audio starts at this timeline time (may be negative)
    CommandStack history;
    // Project state: dirty = an undoable edit (history) or a project edit outside the history (models added/removed,
    // audio, visibility, rename) since the last save.
    std::filesystem::path projectPath;  // .mmdxproj, empty = never saved ("untitled")
    uint64_t savedVersion = 0;          // history.Version() at the last save
    uint64_t projectVersion = 1;        // bumped by project edits outside the history
    uint64_t savedProjectVersion = 1;   // projectVersion at the last save
    uint64_t autosavedStamp = 0;        // ChangeStamp() of the last autosave
    uint32_t nextUid = 1;
    bool Dirty() const { return history.Version() != savedVersion || projectVersion != savedProjectVersion; }
    uint64_t ChangeStamp() const { return history.Version() + (projectVersion << 32); }
    void MarkSaved() { savedVersion = history.Version(); savedProjectVersion = projectVersion; }

    // playback
    double time = 0;            // seconds
    bool playing = false;
    float physicsFrame = -1;    // frame of the last physics step (-1: reset)
    bool useMotionCamera = true;
    bool useLightTrack = true;      // the light track drives the renderer's key light (else the lighting preset)
    bool useShadowTrack = true;     // the self-shadow track drives the shadows (else the render settings)
    bool showCameraPath = true;     // camera path overlay in the viewport (free camera only)
    bool modelGizmo = false;        // viewport gizmo moves / rotates the selected character / stage (no bone picked)
    int viewLayout = 0;             // 0 single perspective view, 1 quad view (perspective + top / front / left, orthographic)
    DirectX::XMFLOAT3 quadCenter[3] = {{0, 10, 0}, {0, 10, 0}, {0, 10, 0}};  // per ortho view (top / front / left): what it looks at
    float quadHeight[3] = {40.0f, 40.0f, 40.0f};  // per ortho view: world units across its height (zoom)
    bool possessCamera = false;     // C4D style: viewport navigation edits the motion camera instead of the free view
    int shading = 0;                // viewport shading (ViewShading): 0 lit, 1 unlit, 2 wireframe (raster path only)
    bool frameMask = true;         // camera view: render the 16:9 frame only and dim the rest (WYSIWYG with the video)
    bool showSafeFrames = false;    // title / action safe rectangles inside the frame
    bool showThirds = false;        // rule-of-thirds guides inside the frame
    bool autoKey = true;           // editing a value in the inspector keys it at the playhead (Adobe / Blender style)

    // editor
    int selectedModel = -1;     // index into models; -1 = camera
    TimelineView view;
    int followFrame = -1;             // frame the timeline last scrolled to show (follows playback and seeks)
    std::set<KeyId> selection;
    std::set<uint64_t> collapsed;     // collapsed group rows (per selected model; cleared on switch)
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
    RowKind curveClipKind = RowKind::Group;  // copied interpolation block: Bone (64 bytes) or Camera (24); Group = empty
    uint8_t curveClip[64] = {};

    // pose editing (viewport)
    std::set<int> selectedBones;      // bones picked in the viewport / on bone rows (selected model)
    int activeBone = -1;              // the gizmo's bone (last picked), -1 none
    int gizmoTool = 0;                // 0 rotate, 1 translate
    bool gizmoLocal = false;          // gizmo axes: bone-local (true) or global
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
};

// Track snapshots: the generic undoable edit. Holds whole tracks before and after an edit, which keeps
// every operation (move, delete, paste, curve, insert) trivially reversible.
struct TrackState {
    int model = -1;                // -1 camera
    RowKind kind = RowKind::Bone;  // Bone, Morph, Camera, Light or Shadow
    std::string name;              // bone/morph name (unused for the camera)
    bool existed = false;          // the track existed (absent tracks are erased again)
    std::vector<BoneKf> bones;
    std::vector<MorphKf> morphs;
    std::vector<CameraKf> cameras;
    std::vector<LightKf> lights;
    std::vector<ShadowKf> shadows;

    size_t Bytes() const {
        return sizeof(TrackState) + name.size() + bones.capacity() * sizeof(BoneKf) + morphs.capacity() * sizeof(MorphKf) +
               cameras.capacity() * sizeof(CameraKf) + lights.capacity() * sizeof(LightKf) +
               shadows.capacity() * sizeof(ShadowKf);
    }
};
TrackState CaptureTrack(StudioDoc& doc, int model, RowKind kind, const std::string& name);
void RestoreTrack(StudioDoc& doc, const TrackState& s);

class TrackEditCommand : public Command {
public:
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
    std::string name, libraryId;
    ModelKind kind = ModelKind::Character;
    bool visible = true;
    PropAttach attach;                       // props: parent = index into StudioPackage::models (-1 world)
    PropAttach place;                        // characters / stages: world placement
    MotionData motion;                       // names canonicalised to pmx
};
struct StudioPackage {
    std::vector<StudioPackageModel> models;  // stage parts first
    MotionData camera;
    std::filesystem::path audioPath;
    double audioOffset = 0;
    // project loads only
    bool fromProject = false;
    ProjectEditor editor;
    std::filesystem::path projectPath;       // the file loaded (recovery: the project it belongs to, may be empty)
    bool recovered = false;                  // loaded from the autosave: opens dirty
    std::vector<std::string> warnings;       // missing models / motions (shown as a toast)
};
// `character` is required, `stage` and `song` may be null. The song's dance + facial VMDs become the
// character's motion (names canonicalised to the model), its camera VMD the camera track.
bool LoadStudioPackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset* song,
                       StudioPackage& out, LoadProgress* progress, std::string* error);
// One model file (PMX, glTF/GLB/VRM, FBX/OBJ) as a character, stage or prop. Characters import with
// asset ModelRole::Character, stages and props with ModelRole::Stage. `out.motion` stays empty.
bool LoadStudioModel(const std::filesystem::path& path, ModelKind kind, const std::string& label,
                     StudioPackageModel& out, LoadProgress* progress, std::string* error);
// Every part of a library stage (failed parts are skipped with a warning; false only when none loads).
bool LoadStudioStage(const StageAsset& stage, std::vector<StudioPackageModel>& out, LoadProgress* progress,
                     std::string* error);
// A library song as studio motions: dance + facial VMDs merged into `dance` (camera/light/shadow cleared, names NOT
// canonicalised), the camera VMD's camera/light/shadow keys into `camera`. False when no VMD could be read.
bool LoadStudioSong(const SongAsset& song, MotionData& dance, MotionData& camera, std::string* error);
// A whole .mmdxproj (LoadProject) with its models and motions. `recovery`: the file is the autosave; projectPath
// becomes its recoveryOf and `recovered` is set. Models whose file fails to load are skipped with a warning (prop
// parents pointing at them become -1, later indices are remapped).
bool LoadStudioProjectPackage(const std::filesystem::path& file, bool recovery, StudioPackage& out,
                              LoadProgress* progress, std::string* error);

} // namespace mmdx::studio
