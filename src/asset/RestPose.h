#pragma once
// Rest-pose normalisation for native MMD models (PMX / PMD).
// MMD motions are authored against an A-pose rig (upper arms about 38 degrees below horizontal) and VMD rotations are
// relative to the model's rest pose. A model that rests in a T-pose (Project Sekai rips, many game rips) would dance with
// its arms raised, so on load the arm chains are rotated down to the A-pose: bone positions, vertices (skin-weighted),
// vertex morphs, rigid bodies and joints. Bone frames in MMD are world-aligned, so VMD / bone-morph data need no change.
#include "asset/PmxModel.h"

namespace mmdx {

constexpr float kMmdArmAngleDeg = 38.0f;   // upper arm below horizontal: 36..42 deg in common MMD rigs (median 38)
constexpr float kTPoseMaxArmDeg = 20.0f;   // an upper arm resting less than this far below horizontal counts as T-pose

struct ArmRest {
    bool valid = false;        // both 腕 -> ひじ pairs were found
    float deg[2] = {0, 0};     // [0] left, [1] right: upper arm below horizontal (positive = down, negative = raised)
    bool hanging[2] = {false, false};  // the arm points (almost) straight down; nothing to correct
    bool tPose() const;        // either arm rests above kTPoseMaxArmDeg's limit and is not hanging
};

// Measures the upper arm direction (腕 -> ひじ) of both sides from the rest positions.
ArmRest MeasureArmRest(const PmxModel& model);

struct RestPoseFix {
    bool applied = false;
    ArmRest before;
    int bonesMoved = 0, verticesMoved = 0, bodiesMoved = 0, jointsMoved = 0;
};

// Rotates every arm that rests as a T-pose down to kMmdArmAngleDeg (the other arm is left alone). No-op for models
// that are already in an A-pose or have no arm bones. Safe to call on any PmxModel; call once per load.
RestPoseFix NormalizeArmRestPose(PmxModel& model);

} // namespace mmdx
