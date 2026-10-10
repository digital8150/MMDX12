#include "asset/RestPose.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace DirectX;

namespace mmdx {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kHangingHorizontal = 0.2f;  // horizontal share of the upper arm below which it counts as hanging

struct Side {
    const char* arm;
    const char* elbow;
    const char* elbowAlt;
};
constexpr Side kSides[2] = {{"左腕", "左ひじ", "左肘"}, {"右腕", "右ひじ", "右肘"}};

struct ArmBones {
    int arm = -1, elbow = -1;
    bool ok() const { return arm >= 0 && elbow >= 0; }
};

ArmBones FindArm(const PmxModel& m, int side) {
    ArmBones a;
    a.arm = m.FindBone(kSides[side].arm);
    if (a.arm < 0) return a;
    a.elbow = m.FindBone(kSides[side].elbow);
    if (a.elbow < 0) a.elbow = m.FindBone(kSides[side].elbowAlt);
    if (a.elbow == a.arm) a.elbow = -1;
    return a;
}

XMVECTOR Pos(const PmxBone& b) { return XMLoadFloat3(&b.position); }

// Angle below horizontal of the arm -> elbow segment, in degrees. hanging: nearly vertical, no usable heading.
void ArmAngle(const PmxModel& m, const ArmBones& a, float& deg, bool& hanging) {
    const XMVECTOR d = XMVector3Normalize(Pos(m.bones[a.elbow]) - Pos(m.bones[a.arm]));
    const float h = std::sqrt(XMVectorGetX(d) * XMVectorGetX(d) + XMVectorGetZ(d) * XMVectorGetZ(d));
    deg = std::atan2(-XMVectorGetY(d), h) * 180.0f / kPi;
    hanging = h < kHangingHorizontal;
}

// Is `bone` the root or a descendant of `root`?
bool IsUnder(const PmxModel& m, int bone, int root) {
    for (int guard = 0; bone >= 0 && guard < (int)m.bones.size(); ++guard) {
        if (bone == root) return true;
        bone = m.bones[bone].parentIndex;
    }
    return false;
}

// PMX Euler angles: Z, then X, then Y (XMMatrixRotationRollPitchYaw(x, y, z)).
XMFLOAT3 EulerFromMatrix(const XMMATRIX& mat) {
    XMFLOAT4X4 M;
    XMStoreFloat4x4(&M, mat);
    XMFLOAT3 e;
    const float sx = std::clamp(-M._32, -1.0f, 1.0f);
    e.x = std::asin(sx);
    if (std::fabs(sx) > 0.99999f) {  // gimbal lock: fold the roll into the yaw
        e.z = 0;
        e.y = std::atan2(-M._13, M._11);
    } else {
        e.y = std::atan2(M._31, M._33);
        e.z = std::atan2(M._12, M._22);
    }
    return e;
}

XMVECTOR Rotate(const XMMATRIX& R, XMVECTOR v) { return XMVector3TransformNormal(v, R); }

// Rotation about the arm bone that turns the upper arm to targetDeg below horizontal, keeping its heading.
bool ArmRotation(const PmxModel& m, const ArmBones& a, float targetDeg, XMMATRIX& rot) {
    const XMVECTOR p = Pos(m.bones[a.arm]);
    const XMVECTOR d = XMVector3Normalize(Pos(m.bones[a.elbow]) - p);
    const XMVECTOR horiz = XMVectorSet(XMVectorGetX(d), 0, XMVectorGetZ(d), 0);
    if (XMVectorGetX(XMVector3Length(horiz)) < kHangingHorizontal) return false;
    const float ang = targetDeg * kPi / 180.0f;
    const XMVECTOR t = XMVector3Normalize(XMVector3Normalize(horiz) * std::cos(ang) + XMVectorSet(0, -std::sin(ang), 0, 0));
    const XMVECTOR axis = XMVector3Cross(d, t);
    const float len = XMVectorGetX(XMVector3Length(axis));
    if (len < 1e-5f) return false;
    const float angle = std::atan2(len, XMVectorGetX(XMVector3Dot(d, t)));
    rot = XMMatrixRotationAxis(axis / len, angle);  // about the origin; callers move the pivot there
    return true;
}

}  // namespace

bool ArmRest::tPose() const {
    if (!valid) return false;
    for (int i = 0; i < 2; ++i)
        if (!hanging[i] && deg[i] < kTPoseMaxArmDeg) return true;
    return false;
}

ArmRest MeasureArmRest(const PmxModel& model) {
    ArmRest r;
    const ArmBones l = FindArm(model, 0), rt = FindArm(model, 1);
    if (!l.ok() || !rt.ok()) return r;
    ArmAngle(model, l, r.deg[0], r.hanging[0]);
    ArmAngle(model, rt, r.deg[1], r.hanging[1]);
    r.valid = true;
    return r;
}

RestPoseFix NormalizeArmRestPose(PmxModel& model) {
    RestPoseFix fix;
    fix.before = MeasureArmRest(model);
    if (!fix.before.valid || !fix.before.tPose()) return fix;

    const size_t nb = model.bones.size();
    for (int side = 0; side < 2; ++side) {
        if (fix.before.hanging[side] || fix.before.deg[side] >= kTPoseMaxArmDeg) continue;
        const ArmBones a = FindArm(model, side);
        XMMATRIX R;
        if (!ArmRotation(model, a, kMmdArmAngleDeg, R)) continue;
        const XMVECTOR pivot = Pos(model.bones[a.arm]);

        // The arm chain: everything under the arm bone, plus arm IK bones (and their children) that chase it.
        std::vector<char> inChain(nb, 0);
        for (size_t i = 0; i < nb; ++i) inChain[i] = IsUnder(model, (int)i, a.arm) ? 1 : 0;
        for (size_t i = 0; i < nb; ++i) {
            const PmxBone& b = model.bones[i];
            if ((b.flags & PmxBone_IK) && b.ikTargetIndex >= 0 && (size_t)b.ikTargetIndex < nb && inChain[b.ikTargetIndex] &&
                !inChain[i] && !IsUnder(model, a.arm, (int)i))
                for (size_t j = 0; j < nb; ++j)
                    if (IsUnder(model, (int)j, (int)i)) inChain[j] = 1;
        }

        auto rotatePoint = [&](XMVECTOR p) { return XMVector3TransformCoord(p - pivot, R) + pivot; };

        // Bones: rest positions and the direction vectors some bone kinds carry.
        for (size_t i = 0; i < nb; ++i) {
            if (!inChain[i]) continue;
            PmxBone& b = model.bones[i];
            if ((int)i != a.arm) {
                XMStoreFloat3(&b.position, rotatePoint(Pos(b)));
                ++fix.bonesMoved;
            }
            auto rotateDir = [&](XMFLOAT3& v) { XMStoreFloat3(&v, Rotate(R, XMLoadFloat3(&v))); };
            if (!(b.flags & PmxBone_TailIsBone)) rotateDir(b.tailOffset);
            if (b.flags & PmxBone_FixedAxis) rotateDir(b.fixedAxis);
            if (b.flags & PmxBone_LocalAxis) {
                rotateDir(b.localAxisX);
                rotateDir(b.localAxisZ);
            }
        }

        // Vertices: linear blend of the rotation by the share of the vertex's weight on the chain (what skinning would do).
        std::vector<float> share(model.vertices.size(), 0.0f);
        for (size_t vi = 0; vi < model.vertices.size(); ++vi) {
            PmxVertex& v = model.vertices[vi];
            float on = 0, total = 0;
            for (int k = 0; k < 4; ++k) {
                const int bi = v.boneIndex[k];
                const float w = v.deform == PmxDeform::BDEF1 && k == 0 ? 1.0f : v.boneWeight[k];
                if (bi < 0 || (size_t)bi >= nb || w <= 0) continue;
                total += w;
                if (inChain[bi]) on += w;
            }
            const float s = total > 1e-6f ? std::clamp(on / total, 0.0f, 1.0f) : 0.0f;
            if (s <= 0) continue;
            share[vi] = s;
            ++fix.verticesMoved;
            auto blendPoint = [&](XMFLOAT3& p) {
                const XMVECTOR q = XMLoadFloat3(&p);
                XMStoreFloat3(&p, XMVectorLerp(q, rotatePoint(q), s));
            };
            blendPoint(v.position);
            const XMVECTOR n = XMLoadFloat3(&v.normal);
            XMStoreFloat3(&v.normal, XMVector3Normalize(XMVectorLerp(n, Rotate(R, n), s)));
            if (v.deform == PmxDeform::SDEF) {
                blendPoint(v.sdefC);
                blendPoint(v.sdefR0);
                blendPoint(v.sdefR1);
            }
        }
        for (PmxMorph& morph : model.morphs) {
            if (morph.type != PmxMorphType::Vertex) continue;
            for (PmxMorph::VertexOffset& o : morph.vertexOffsets) {
                if (o.vertex < 0 || (size_t)o.vertex >= share.size() || share[o.vertex] <= 0) continue;
                const XMVECTOR v = XMLoadFloat3(&o.offset);
                XMStoreFloat3(&o.offset, XMVectorLerp(v, Rotate(R, v), share[o.vertex]));
            }
        }

        // Physics: bodies riding the chain turn with it; a joint follows the body it drives (B).
        std::vector<char> bodyMoved(model.rigidBodies.size(), 0);
        for (size_t i = 0; i < model.rigidBodies.size(); ++i) {
            PmxRigidBody& rb = model.rigidBodies[i];
            if (rb.boneIndex < 0 || (size_t)rb.boneIndex >= nb || !inChain[rb.boneIndex]) continue;
            bodyMoved[i] = 1;
            ++fix.bodiesMoved;
            XMStoreFloat3(&rb.position, rotatePoint(XMLoadFloat3(&rb.position)));
            const XMMATRIX orient = XMMatrixRotationRollPitchYaw(rb.rotation.x, rb.rotation.y, rb.rotation.z) * R;
            rb.rotation = EulerFromMatrix(orient);
        }
        for (PmxJoint& j : model.joints) {
            if (j.rigidBodyB < 0 || (size_t)j.rigidBodyB >= bodyMoved.size() || !bodyMoved[j.rigidBodyB]) continue;
            ++fix.jointsMoved;
            XMStoreFloat3(&j.position, rotatePoint(XMLoadFloat3(&j.position)));
            const XMMATRIX orient = XMMatrixRotationRollPitchYaw(j.rotation.x, j.rotation.y, j.rotation.z) * R;
            j.rotation = EulerFromMatrix(orient);
        }
        fix.applied = true;
    }
    if (fix.applied)
        LOG_INFO("rest pose: %s T-pose arms (%.0f / %.0f deg below horizontal) turned to %.0f deg: %d bones, %d vertices, %d bodies, %d joints",
                 model.name.c_str(), fix.before.deg[0], fix.before.deg[1], kMmdArmAngleDeg, fix.bonesMoved, fix.verticesMoved,
                 fix.bodiesMoved, fix.jointsMoved);
    return fix;
}

}  // namespace mmdx
