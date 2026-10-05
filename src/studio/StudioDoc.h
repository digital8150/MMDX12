#pragma once
// Studio document: the models of the scene with their editable motions, the camera motion, the undo
// history and the editor state (selection, timeline view). Owned by App while screen_ == Studio.
// Motions are keyed by bone/morph name (StudioMotion.h) and re-bound to the model after every edit.
#include "anim/Motion.h"
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"
#include "studio/CommandStack.h"
#include "studio/StudioMotion.h"
#include "studio/UiTimeline.h"
#include <cmath>
#include <filesystem>
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
    bool isStage = false;          // stage part: no physics, drawn before the characters
    bool visible = true;
    std::shared_ptr<const PmxModel> pmx;
    std::unique_ptr<ModelInstance> inst;
    std::unique_ptr<GpuModel> gpu;
    MotionData motion;
    uint64_t motionVersion = 1;    // bumped by every edit of `motion`
    uint64_t boundVersion = 0;     // motionVersion `bound` was built from
    std::shared_ptr<BoundMotion> bound;
};

// Timeline row ids: kind in the top byte, then a 24-bit group and a 32-bit index.
enum class RowKind : uint8_t { Group = 1, Bone = 2, Morph = 3, Camera = 4 };
inline uint64_t MakeRowId(RowKind k, uint32_t group, uint32_t index) {
    return ((uint64_t)k << 56) | ((uint64_t)(group & 0xFFFFFF) << 32) | index;
}
inline RowKind RowKindOf(uint64_t id) { return (RowKind)(id >> 56); }
inline uint32_t RowIndexOf(uint64_t id) { return (uint32_t)id; }
inline uint32_t RowGroupOf(uint64_t id) { return (uint32_t)(id >> 32) & 0xFFFFFF; }

// A key in the timeline selection: row (bone/morph/camera rows only, never groups) + frame.
using KeyId = std::pair<uint64_t, int>;

struct ClipboardKey {
    uint64_t row = 0;
    int offset = 0;  // frame relative to the first copied key
    BoneKf bone; MorphKf morph; CameraKf camera;
};

struct StudioDoc {
    std::vector<std::unique_ptr<StudioModel>> models;  // stage parts first, then characters
    MotionData camera;                                 // camera keys only
    uint64_t cameraVersion = 1, cameraEvalVersion = 0;
    std::shared_ptr<CameraMotion> cameraEval;
    std::filesystem::path audioPath;
    bool hasAudio = false;
    float audioEndFrame = 0;
    CommandStack history;
    uint64_t savedVersion = 0;  // history.Version() at the last export (dirty marker)

    // playback
    double time = 0;            // seconds
    bool playing = false;
    float physicsFrame = -1;    // frame of the last physics step (-1: reset)
    bool useMotionCamera = true;

    // editor
    int selectedModel = -1;     // index into models; -1 = camera
    TimelineView view;
    int followFrame = -1;             // frame the timeline last scrolled to show (follows playback and seeks)
    std::set<KeyId> selection;
    std::set<uint64_t> collapsed;     // collapsed group rows (per selected model; cleared on switch)
    std::vector<ClipboardKey> clipboard;
    int clipboardModel = -2;          // model the clipboard came from (-1 camera, -2 empty)
    int curveChannel = 3;             // inspector: bone 0..3 (X,Y,Z,R), camera 0..5
    std::vector<TimelineRow> rows;    // cached timeline rows
    uint64_t rowsKey = ~0ull;         // inputs the cache was built from
    bool curveEditing = false;        // a curve edit is in progress (one undo step per drag)
    std::vector<struct TrackState> curveBefore;  // tracks before the edit started

    int Frame() const { return (int)std::floor(time * kMmdFps + 1e-4); }
    int EndFrame() const;             // last key over all motions, the audio length, at least 300
    StudioModel* Selected() { return selectedModel >= 0 && selectedModel < (int)models.size() ? models[selectedModel].get() : nullptr; }
    void TouchModel(int model);       // after editing a motion (model -1 = camera)
};

// Track snapshots: the generic undoable edit. Holds whole tracks before and after an edit, which keeps
// every operation (move, delete, paste, curve, insert) trivially reversible.
struct TrackState {
    int model = -1;                // -1 camera
    RowKind kind = RowKind::Bone;  // Bone, Morph or Camera
    std::string name;              // bone/morph name (unused for the camera)
    bool existed = false;          // the track existed (absent tracks are erased again)
    std::vector<BoneKf> bones;
    std::vector<MorphKf> morphs;
    std::vector<CameraKf> cameras;
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

private:
    StudioDoc& doc_;
    std::string name_;
    std::vector<TrackState> before_, after_;
};

// Replaces a whole motion (VMD import): also covers IK tracks, which TrackState does not.
class MotionSwapCommand : public Command {
public:
    MotionSwapCommand(StudioDoc& doc, int model, std::string name, MotionData before, MotionData after)
        : doc_(doc), model_(model), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return name_; }

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

// CPU side of a studio scene (worker thread).
struct StudioPackageModel {
    std::shared_ptr<PmxModel> pmx;
    std::vector<ImageRGBA8> textures;
    std::string name, libraryId;
    bool isStage = false;
    MotionData motion;
};
struct StudioPackage {
    std::vector<StudioPackageModel> models;  // stage parts first
    MotionData camera;
    std::filesystem::path audioPath;
};
// `character` is required, `stage` and `song` may be null. The song's dance + facial VMDs become the
// character's motion (names canonicalised to the model), its camera VMD the camera track.
bool LoadStudioPackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset* song,
                       StudioPackage& out, LoadProgress* progress, std::string* error);

} // namespace mmdx::studio
