#pragma once
// PMX rigid-body physics (Bullet). One world per model, built from PmxModel::rigidBodies and
// joints, following the reference behaviour of saba (MIT, benikabocha/saba):
//   mode 0 (follow bone)   kinematic body driven by its bone,
//   mode 1 (physics)       simulated body that drives its bone,
//   mode 2 (physics+bone)  simulated rotation, bone keeps its animated position.
// Bullet types stay inside Physics.cpp. Matrices are DirectXMath row-vector (v' = v * M).
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <memory>
#include <vector>

namespace mmdx {

class PhysicsWorld {
public:
    // bindWorld: per-bone world matrices of the rest pose (index == bone index).
    PhysicsWorld(const PmxModel& model, const std::vector<DirectX::XMFLOAT4X4>& bindWorld);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // True if at least one simulated body drives a bone (otherwise physics is a no-op).
    bool HasDynamicBodies() const { return dynamicCount_ > 0; }

    // Teleports every body to the pose implied by boneWorld, clears velocities, then lets the
    // simulated bodies settle for `settleSeconds` with that pose held.
    void Reset(const std::vector<DirectX::XMFLOAT4X4>& boneWorld, float settleSeconds);

    // Moves kinematic bodies to boneWorld and advances the simulation by dt seconds
    // (fixed 1/120 s substeps). dt <= 0 only refreshes the kinematic targets. If a kinematic body
    // jumps more than 10 units (a teleport in the motion), this resets instead of simulating.
    void Step(const std::vector<DirectX::XMFLOAT4X4>& boneWorld, float dt);

    // Per simulated body attached to a bone: the bone's new world matrix.
    struct BoneResult { int bone; DirectX::XMFLOAT4X4 world; };
    // boneWorld is the animated pose (mode 2 bodies keep its translation).
    void Results(const std::vector<DirectX::XMFLOAT4X4>& boneWorld, std::vector<BoneResult>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int dynamicCount_ = 0;
};

} // namespace mmdx
