#pragma once
// Runtime pose of a PMX model: bone hierarchy evaluation (append/IK), morphs, rigid-body
// physics, and the per-frame data the GPU needs (skinning matrices + vertex morph deltas).
// Matrices follow DirectXMath row-vector convention: v' = v * M.
#include "anim/Physics.h"
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <memory>
#include <string_view>
#include <vector>

namespace mmdx {

using DirectX::XMFLOAT4X4;

class ModelInstance {
public:
    explicit ModelInstance(std::shared_ptr<const PmxModel> model);
    ~ModelInstance();

    const PmxModel& Model() const { return *model_; }
    const std::shared_ptr<const PmxModel>& ModelPtr() const { return model_; }

    // Clears animation translations/rotations, morph weights, and re-enables all IK.
    void ResetPose();

    // Animation input (bone-local, relative to rest pose), as produced by VMD evaluation.
    void SetBoneAnim(int bone, const XMFLOAT3& translation, const XMFLOAT4& rotation);
    void SetMorphWeight(int morph, float weight);
    void SetIkEnabled(int ikBone, bool enabled);

    // Rigid-body physics. Off by default (stages, thumbnails); the world is built on first enable.
    // Models without simulated bodies stay a no-op.
    void EnablePhysics(bool enabled);
    bool PhysicsEnabled() const { return physicsEnabled_; }
    // The next UpdatePose teleports the bodies to the animated pose and lets them settle
    // instead of simulating the jump. Call after seeks, loops, and on the first frame.
    void ResetPhysics() { physicsResetPending_ = true; }

    // Evaluates morphs -> bones (hierarchy, append, IK) -> physics advanced by physicsDt seconds
    // (if enabled) -> after-physics bones -> skinning matrices.
    void UpdatePose(float physicsDt = 0.0f);

    // Per bone: inverse(bind) * world, i.e. translate(-restPosition) * boneWorld.
    const std::vector<XMFLOAT4X4>& SkinMatrices() const { return skin_; }
    // Per vertex position offsets from vertex morphs (size == vertex count).
    const std::vector<XMFLOAT3>& VertexMorphDeltas() const { return morphDelta_; }
    // Incremented every time VertexMorphDeltas() content changes.
    uint64_t MorphVersion() const { return morphVersion_; }

    XMFLOAT3 BoneWorldPosition(int bone) const;
    const XMFLOAT4X4& BoneWorld(int bone) const { return bones_[bone].world; }

private:
    struct BoneState {
        XMFLOAT3 animT{}; XMFLOAT4 animR{0, 0, 0, 1};    // from motion
        XMFLOAT3 morphT{}; XMFLOAT4 morphR{0, 0, 0, 1};  // from bone morphs
        XMFLOAT4 ikR{0, 0, 0, 1};                        // from IK solve
        XMFLOAT3 appendT{}; XMFLOAT4 appendR{0, 0, 0, 1};// resolved append (付与) result
        XMFLOAT4X4 local{}, world{};
        bool ikEnabled = true;
    };

    void ApplyMorphs();
    void AddMorph(int morph, float weight, int depth);
    void UpdateLocal(int bone);
    void UpdateWorld(int bone);
    void UpdateWorldRecursive(int bone);
    void SolveIk(int ikBone);
    void EvaluateAppendAndIk(int bone);
    void RunPhysics(float dt);
    XMFLOAT4 AnimRotation(int bone) const;     // morphR * animR (before append/IK)
    XMFLOAT3 AnimTranslation(int bone) const;

    std::shared_ptr<const PmxModel> model_;
    std::vector<BoneState> bones_;
    std::vector<int> order_;                // evaluation order: before-physics bones by layer, then after-physics
    size_t afterPhysicsBegin_ = 0;          // first index in order_ of an after-physics bone
    std::vector<std::vector<int>> children_;
    std::vector<float> morphWeight_;
    std::vector<float> appliedMorphWeight_; // last weights used to build morphDelta_
    std::vector<float> pendingVertexWeight_;// scratch: effective vertex-morph weights this frame
    std::vector<int> dfsStack_;             // scratch for UpdateWorldRecursive
    std::vector<XMFLOAT3> morphDelta_;
    uint64_t morphVersion_ = 0;
    std::vector<XMFLOAT4X4> skin_;

    std::unique_ptr<PhysicsWorld> physics_;
    bool physicsEnabled_ = false;
    bool physicsResetPending_ = true;
    std::vector<XMFLOAT4X4> physicsPose_;   // scratch: animated bone worlds handed to physics
    std::vector<PhysicsWorld::BoneResult> physicsOut_;  // scratch: simulated bone worlds
    std::vector<int> physicsResult_;        // scratch: per bone, index into physicsOut_ or -1
};

} // namespace mmdx
