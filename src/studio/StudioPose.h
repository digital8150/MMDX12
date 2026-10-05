#pragma once
// Studio pose layer: bone/morph values edited in the viewport that override the motion at one frame until they are
// registered as keys or discarded (MMD's "unregistered pose"). Plus pose operations that work on whole poses:
// left/right mirroring and VPD pose files.
// Bone values are the motion's local animation values (VMD key semantics: translation offset from the rest position
// in the parent's frame, rotation relative to the rest orientation).
#include "asset/PmxModel.h"
#include "asset/VpdFile.h"
#include <DirectXMath.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mmdx::studio {

struct PoseBone {
    DirectX::XMFLOAT3 t{};
    DirectX::XMFLOAT4 r{0, 0, 0, 1};
};

struct PoseLayer {
    int frame = -1;                 // frame the edits were made at (-1 while empty)
    std::map<int, PoseBone> bones;  // bone index -> overriding value
    std::map<int, float> morphs;    // morph index -> overriding weight
    bool Empty() const { return bones.empty() && morphs.empty(); }
};
bool SamePoseLayer(const PoseLayer& a, const PoseLayer& b);  // exact compare (frame, keys and values)

// 左 <-> 右 everywhere in the name (both swapped at once). Names without 左/右: ASCII side markers
// ("_L"/"_R", ".L"/".R", "_l"/"_r", ".l"/".r" as a suffix; "Left"/"Right", "left"/"right" first occurrence).
// Returns `name` unchanged when it has no side marker.
std::string MirrorBoneName(const std::string& name);
// Index of the bone on the other side, or `bone` itself (centre bones, or the mirrored name does not exist).
int MirrorBoneIndex(const PmxModel& model, int bone);
// Reflection across the model's YZ plane (x -> -x): t = (-x, y, z), r = (x, -y, -z, w).
PoseBone MirrorPose(const PoseBone& p);

// Mirror operation. `current` holds the effective value of every bone (size == model.bones.size()).
// For every bone b in scope (all bones, or `onlyBones`), the bone m = MirrorBoneIndex(b) receives MirrorPose(current[b])
// (all reads use `current`, so selecting both sides swaps them; a whole-model scope flips the pose). Components the
// target bone cannot take keep current[m]: translation needs PmxBone_Movable, rotation PmxBone_Rotatable.
// Only bones whose result differs from current[m] (any component by more than 1e-6) are written to `out.bones`
// (existing entries for other bones are kept). Returns the number of bones written.
int MirrorPoseInto(const PmxModel& model, const std::vector<PoseBone>& current, const std::set<int>* onlyBones,
                   PoseLayer& out);

// VPD export. Bones: with `onlyBones`, exactly those bones (any value); otherwise every bone that has
// PmxBone_Rotatable or PmxBone_Movable and a non-identity value (|t| component > 1e-6 or rotation not within 1e-6
// of identity, either sign of w). Morphs (whole-model scope only): weights != 0 from `morphWeights` (size == morphs).
// Bones are listed in bone index order, morphs in morph index order. modelName = model.name.
VpdPose MakeVpdPose(const PmxModel& model, const std::vector<PoseBone>& current, const std::vector<float>& morphWeights,
                    const std::set<int>* onlyBones);
// VPD import into a pose layer: every VPD bone found by name (PmxModel::FindBone) and in scope (`onlyBones` or all)
// is written to out.bones (rotation normalised); morphs (whole-model scope only) found by name go to out.morphs.
// Names not found are appended to `missing` (if non-null). Returns the number of bones + morphs written.
int ApplyVpdPose(const PmxModel& model, const VpdPose& pose, const std::set<int>* onlyBones, PoseLayer& out,
                 std::vector<std::string>* missing);

} // namespace mmdx::studio
