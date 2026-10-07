#include "studio/StudioDoc.h"
#include "anim/ModelInstance.h"
#include "render/GpuModel.h"
#include <algorithm>

namespace mmdx::studio {

StudioModel::StudioModel() = default;
StudioModel::~StudioModel() = default;

void StudioModel::BuildRowGroups() {
    // Mirrors StudioRebuildRows: display frame g lists items; unlisted bones go to group frames.size(), morphs to + 1.
    const uint32_t frames = (uint32_t)pmx->displayFrames.size();
    boneRowGroup.assign(pmx->bones.size(), frames);
    morphRowGroup.assign(pmx->morphs.size(), frames + 1);
    std::vector<char> boneSet(pmx->bones.size(), 0), morphSet(pmx->morphs.size(), 0);
    for (uint32_t g = 0; g < frames; ++g) {
        for (const PmxDisplayFrame::Item& it : pmx->displayFrames[g].items) {
            if (it.index < 0) continue;
            const size_t i = (size_t)it.index;
            if (it.morph) {
                if (i < morphSet.size() && !morphSet[i]) { morphSet[i] = 1; morphRowGroup[i] = g; }
            } else if (i < boneSet.size() && !boneSet[i]) {
                boneSet[i] = 1;
                boneRowGroup[i] = g;
            }
        }
    }
}

uint64_t CanonicalRow(const StudioModel& m, RowKind kind, uint32_t index) {
    if (kind == RowKind::Bone && index < m.boneRowGroup.size()) return MakeRowId(kind, m.boneRowGroup[index], index);
    if (kind == RowKind::Morph && index < m.morphRowGroup.size()) return MakeRowId(kind, m.morphRowGroup[index], index);
    return 0;
}

int StudioDoc::EndFrame() const {
    int end = std::max(camera.EndFrame(), (int)std::ceil(audioEndFrame));
    for (const auto& m : models) end = std::max(end, m->motion.EndFrame());
    return std::max(end, 300);
}

int StudioDoc::IndexOfUid(uint32_t uid) const {
    for (size_t i = 0; i < models.size(); ++i)
        if (models[i]->uid == uid) return (int)i;
    return -1;
}

void StudioDoc::TouchModel(int model) {
    if (model < 0) ++cameraVersion;
    else if (model < (int)models.size()) ++models[model]->motionVersion;
    rowsKey = ~0ull;  // timeline rows show keys: rebuild
}

TrackState CaptureTrack(StudioDoc& doc, int model, RowKind kind, const std::string& name) {
    TrackState s;
    s.model = model;
    s.kind = kind;
    s.name = name;
    if (IsCameraKind(kind)) {
        s.existed = true;
        if (kind == RowKind::Light) s.lights = doc.camera.light;
        else if (kind == RowKind::Shadow) s.shadows = doc.camera.shadow;
        else s.cameras = doc.camera.camera;
        return s;
    }
    if (kind == RowKind::Spot) {
        const SpotLight* spot = doc.lighting.Spot(name);
        s.existed = spot != nullptr;
        if (spot) s.spots = spot->keys;
        return s;
    }
    MotionData& m = doc.models[model]->motion;
    if (kind == RowKind::Bone) {
        auto it = m.bones.find(name);
        s.existed = it != m.bones.end();
        if (s.existed) s.bones = it->second;
    } else {
        auto it = m.morphs.find(name);
        s.existed = it != m.morphs.end();
        if (s.existed) s.morphs = it->second;
    }
    return s;
}

void RestoreTrack(StudioDoc& doc, const TrackState& s) {
    if (IsCameraKind(s.kind)) {
        if (s.kind == RowKind::Light) doc.camera.light = s.lights;
        else if (s.kind == RowKind::Shadow) doc.camera.shadow = s.shadows;
        else doc.camera.camera = s.cameras;
        doc.TouchModel(-1);
        return;
    }
    if (s.kind == RowKind::Spot) {
        SpotLight* spot = doc.lighting.Spot(s.name);
        if (spot) spot->keys = s.spots;  // an emptied track keeps the spot (its values drive it again)
        doc.rowsKey = ~0ull;
        return;
    }
    if (s.model >= 0 && s.model < (int)doc.models.size()) {
        MotionData& m = doc.models[s.model]->motion;
        // A track that ends up empty is removed, so an undone insert leaves no empty track behind.
        if (s.kind == RowKind::Bone) {
            if (s.bones.empty()) m.bones.erase(s.name);
            else m.bones[s.name] = s.bones;
        } else {
            if (s.morphs.empty()) m.morphs.erase(s.name);
            else m.morphs[s.name] = s.morphs;
        }
    }
    doc.TouchModel(s.model);  // spot rows returned above
}

} // namespace mmdx::studio
