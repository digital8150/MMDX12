#pragma once
// Physics bake (P2.2): simulate a model's rigid-body physics over a frame range and turn the simulated
// bones into ordinary linear bone keys, so playback / export need no physics. App-independent (the
// undoing and the UI live in the App); unit-tested by tools/bake_test.cpp.
#include "anim/Motion.h"
#include "asset/PmxModel.h"
#include "studio/PhysicsEnv.h"
#include "studio/StudioMotion.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mmdx::studio {

struct BakeOptions {
    int start = 0;
    int end = 0;
    int step = 1;               // key every N frames (>= 1; `end` is always keyed)
    int preroll = 60;           // frames simulated before `start` so the first key is settled
    std::vector<int> bones;     // bone indices to bake; empty = every physics-driven bone
    PhysicsEnv env;             // the project's gravity / wind (the viewport simulates with the same)
};

struct BakeResult {
    std::map<std::string, std::vector<BoneKf>> bones;  // bone name -> baked (linear) keys, sorted by frame
};

// Bones a simulated rigid body drives (physicsMode != 0 and jointed, as PhysicsWorld keeps them).
std::vector<int> PhysicsDrivenBones(const PmxModel& pmx);

// Runs the model's own physics over [start, end] and writes each baked bone's VMD-local rotation
// (and its translation when it moves) as linear keys. Uses its own ModelInstance + Bullet world (the
// viewport's is untouched); `pmx` and `motion` are read-only. `cancel` / `progress` may be null.
// Returns false when cancelled.
bool BakePhysics(std::shared_ptr<const PmxModel> pmx, const BoundMotion& motion,
                 const DirectX::XMFLOAT4X4& root, const BakeOptions& options, BakeResult& out,
                 const std::atomic<bool>* cancel, std::atomic<float>* progress);

} // namespace mmdx::studio
