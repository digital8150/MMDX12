#include "studio/StudioDoc.h"
#include "anim/ModelInstance.h"
#include "render/GpuModel.h"
#include <algorithm>

namespace mmdx::studio {

StudioModel::StudioModel() = default;
StudioModel::~StudioModel() = default;

int StudioDoc::EndFrame() const {
    int end = std::max(camera.EndFrame(), (int)std::ceil(audioEndFrame));
    for (const auto& m : models) end = std::max(end, m->motion.EndFrame());
    return std::max(end, 300);
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
    if (kind == RowKind::Camera) {
        s.existed = true;
        s.cameras = doc.camera.camera;
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
    if (s.kind == RowKind::Camera) {
        doc.camera.camera = s.cameras;
    } else if (s.model >= 0 && s.model < (int)doc.models.size()) {
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
    doc.TouchModel(s.kind == RowKind::Camera ? -1 : s.model);
}

} // namespace mmdx::studio
