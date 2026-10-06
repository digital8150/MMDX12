// Bone evaluation follows the reference behaviour of saba (MIT, benikabocha/saba) ported to
// DirectXMath. Quaternion note: XMQuaternionMultiply(a, b) == "rotate by a, then by b",
// i.e. the glm product b * a.
#include "anim/ModelInstance.h"
#include <algorithm>
#include <cmath>
#include <limits>

using namespace DirectX;

namespace mmdx {

namespace {

constexpr float kPi = 3.14159265358979f;

XMFLOAT4 QuatIdentity() { return {0, 0, 0, 1}; }

XMVECTOR Translation(const XMFLOAT4X4& m) { return XMVectorSet(m._41, m._42, m._43, 1.0f); }

float WrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// Euler angles (x, y, z) for q = Rz then Ry then Rx (column form Rx*Ry*Rz), choosing among the two
// equivalent solutions the one closest to `before` to keep IK solutions continuous.
XMFLOAT3 DecomposeXYZ(FXMVECTOR q, const XMFLOAT3& before) {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(q));
    // Column-vector matrix element C(r,c) == row-vector m.m[c][r].
    const float sy = std::clamp(m.m[2][0], -1.0f, 1.0f);
    XMFLOAT3 r;
    r.y = std::asin(sy);
    if (1.0f - std::fabs(sy) < 1e-6f) {  // gimbal lock: keep x, solve z
        r.x = 0.0f;
        r.z = std::atan2(m.m[0][1], m.m[1][1]);
    } else {
        r.x = std::atan2(-m.m[2][1], m.m[2][2]);
        r.z = std::atan2(-m.m[1][0], m.m[0][0]);
    }
    const XMFLOAT3 alt{WrapAngle(r.x + kPi), WrapAngle(kPi - r.y), WrapAngle(r.z + kPi)};
    auto err = [&](const XMFLOAT3& v) {
        return std::fabs(WrapAngle(v.x - before.x)) + std::fabs(WrapAngle(v.y - before.y)) + std::fabs(WrapAngle(v.z - before.z));
    };
    return err(alt) < err(r) ? alt : r;
}

XMVECTOR QuatFromXYZ(const XMFLOAT3& e) {
    const XMVECTOR qx = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), e.x);
    const XMVECTOR qy = XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), e.y);
    const XMVECTOR qz = XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), e.z);
    return XMQuaternionMultiply(XMQuaternionMultiply(qz, qy), qx);
}

}  // namespace

ModelInstance::ModelInstance(std::shared_ptr<const PmxModel> model) : model_(std::move(model)) {
    const auto& bones = model_->bones;
    const size_t n = bones.size();
    bones_.resize(n);
    children_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const int p = bones[i].parentIndex;
        if (p >= 0 && p < (int)n && p != (int)i) children_[p].push_back((int)i);
    }
    order_.resize(n);
    for (size_t i = 0; i < n; ++i) order_[i] = (int)i;
    std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) {
        const bool pa = bones[a].flags & PmxBone_AfterPhysics, pb = bones[b].flags & PmxBone_AfterPhysics;
        if (pa != pb) return pb;  // before-physics bones first
        return bones[a].deformLayer < bones[b].deformLayer;
    });
    afterPhysicsBegin_ = (size_t)(std::find_if(order_.begin(), order_.end(), [&](int b) {
        return (bones[b].flags & PmxBone_AfterPhysics) != 0;
    }) - order_.begin());
    morphWeight_.assign(model_->morphs.size(), 0.0f);
    appliedMorphWeight_.assign(model_->morphs.size(), 0.0f);
    morphDelta_.assign(model_->vertices.size(), XMFLOAT3{0, 0, 0});
    for (const PmxMorph& m : model_->morphs)
        if (m.type == PmxMorphType::Material && !m.materialOffsets.empty()) {
            appliedMaterialWeight_.assign(model_->morphs.size(), 0.0f);
            RebuildMaterialFactors();
            break;
        }
    skin_.resize(std::max<size_t>(n, 1));
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    for (auto& m : skin_) m = identity;
    ResetPose();
    UpdatePose();
}

ModelInstance::~ModelInstance() = default;

void ModelInstance::EnablePhysics(bool enabled) {
    if (enabled && !physics_ && !model_->rigidBodies.empty()) {
        std::vector<XMFLOAT4X4> bind(bones_.size());
        for (size_t i = 0; i < bind.size(); ++i) {
            const XMFLOAT3& p = model_->bones[i].position;
            XMStoreFloat4x4(&bind[i], XMMatrixTranslation(p.x, p.y, p.z));
        }
        physics_ = std::make_unique<PhysicsWorld>(*model_, bind);
    }
    if (enabled && !physicsEnabled_) physicsResetPending_ = true;
    physicsEnabled_ = enabled;
}

void ModelInstance::ResetPose() {
    for (BoneState& b : bones_) {
        b.animT = {0, 0, 0};
        b.animR = QuatIdentity();
        b.ikEnabled = true;
    }
    std::fill(morphWeight_.begin(), morphWeight_.end(), 0.0f);
}

void ModelInstance::SetBoneAnim(int bone, const XMFLOAT3& t, const XMFLOAT4& r) {
    if (bone < 0 || bone >= (int)bones_.size()) return;
    bones_[bone].animT = t;
    bones_[bone].animR = r;
}

void ModelInstance::SetMorphWeight(int morph, float weight) {
    if (morph >= 0 && morph < (int)morphWeight_.size()) morphWeight_[morph] = weight;
}

void ModelInstance::SetIkEnabled(int ikBone, bool enabled) {
    if (ikBone >= 0 && ikBone < (int)bones_.size()) bones_[ikBone].ikEnabled = enabled;
}

XMFLOAT4 ModelInstance::AnimRotation(int bone) const {
    XMFLOAT4 r;
    XMStoreFloat4(&r, XMQuaternionMultiply(XMLoadFloat4(&bones_[bone].morphR), XMLoadFloat4(&bones_[bone].animR)));
    return r;
}

XMFLOAT3 ModelInstance::AnimTranslation(int bone) const {
    const BoneState& b = bones_[bone];
    return {b.animT.x + b.morphT.x, b.animT.y + b.morphT.y, b.animT.z + b.morphT.z};
}

// ---------------------------------------------------------------------------- morphs

void ModelInstance::AddMorph(int morph, float weight, int depth) {
    if (morph < 0 || morph >= (int)model_->morphs.size() || depth > 8) return;
    const PmxMorph& m = model_->morphs[morph];
    switch (m.type) {
    case PmxMorphType::Vertex:
        // Accumulated into a scratch weight list; deltas are rebuilt in ApplyMorphs.
        pendingVertexWeight_[morph] += weight;
        break;
    case PmxMorphType::Group:
        for (const auto& g : m.groupOffsets) AddMorph(g.morph, weight * g.weight, depth + 1);
        break;
    case PmxMorphType::Bone:
        for (const auto& b : m.boneOffsets) {
            if (b.bone < 0 || b.bone >= (int)bones_.size()) continue;
            BoneState& s = bones_[b.bone];
            s.morphT.x += b.translation.x * weight;
            s.morphT.y += b.translation.y * weight;
            s.morphT.z += b.translation.z * weight;
            const XMVECTOR q = XMQuaternionSlerp(XMQuaternionIdentity(), XMLoadFloat4(&b.rotation), weight);
            XMStoreFloat4(&s.morphR, XMQuaternionMultiply(XMLoadFloat4(&s.morphR), q));
        }
        break;
    case PmxMorphType::Material:
        // Accumulated like vertex morphs; the factors are rebuilt in ApplyMorphs when they change.
        if (!pendingMaterialWeight_.empty()) pendingMaterialWeight_[morph] += weight;
        break;
    default:
        break;  // UV / flip / impulse morphs: not supported
    }
}

void ModelInstance::RebuildMaterialFactors() {
    const size_t n = model_->materials.size();
    PmxMorph::MaterialOffset one{}, zero{};
    one.diffuse = {1, 1, 1, 1};
    one.specular = {1, 1, 1};
    one.specularPower = 1;
    one.ambient = {1, 1, 1};
    one.edgeColor = {1, 1, 1, 1};
    one.edgeSize = 1;
    one.textureFactor = one.sphereFactor = one.toonFactor = {1, 1, 1, 1};
    materialMul_.assign(n, one);
    materialAdd_.assign(n, zero);
    const auto mul4 = [](XMFLOAT4& a, const XMFLOAT4& o, float w) {
        a.x *= 1.0f + (o.x - 1.0f) * w; a.y *= 1.0f + (o.y - 1.0f) * w;
        a.z *= 1.0f + (o.z - 1.0f) * w; a.w *= 1.0f + (o.w - 1.0f) * w;
    };
    const auto mul3 = [](XMFLOAT3& a, const XMFLOAT3& o, float w) {
        a.x *= 1.0f + (o.x - 1.0f) * w; a.y *= 1.0f + (o.y - 1.0f) * w; a.z *= 1.0f + (o.z - 1.0f) * w;
    };
    const auto add4 = [](XMFLOAT4& a, const XMFLOAT4& o, float w) {
        a.x += o.x * w; a.y += o.y * w; a.z += o.z * w; a.w += o.w * w;
    };
    const auto add3 = [](XMFLOAT3& a, const XMFLOAT3& o, float w) { a.x += o.x * w; a.y += o.y * w; a.z += o.z * w; };
    for (size_t i = 0; i < appliedMaterialWeight_.size(); ++i) {
        const float w = appliedMaterialWeight_[i];
        if (w == 0.0f) continue;
        for (const PmxMorph::MaterialOffset& o : model_->morphs[i].materialOffsets) {
            size_t first = 0, last = n;  // -1: every material
            if (o.material >= 0) {
                if ((size_t)o.material >= n) continue;
                first = (size_t)o.material;
                last = first + 1;
            }
            for (size_t m = first; m < last; ++m) {
                if (o.operation == 0) {
                    PmxMorph::MaterialOffset& a = materialMul_[m];
                    mul4(a.diffuse, o.diffuse, w);
                    mul3(a.specular, o.specular, w);
                    a.specularPower *= 1.0f + (o.specularPower - 1.0f) * w;
                    mul3(a.ambient, o.ambient, w);
                    mul4(a.edgeColor, o.edgeColor, w);
                    a.edgeSize *= 1.0f + (o.edgeSize - 1.0f) * w;
                    mul4(a.textureFactor, o.textureFactor, w);
                    mul4(a.sphereFactor, o.sphereFactor, w);
                    mul4(a.toonFactor, o.toonFactor, w);
                } else {
                    PmxMorph::MaterialOffset& a = materialAdd_[m];
                    add4(a.diffuse, o.diffuse, w);
                    add3(a.specular, o.specular, w);
                    a.specularPower += o.specularPower * w;
                    add3(a.ambient, o.ambient, w);
                    add4(a.edgeColor, o.edgeColor, w);
                    a.edgeSize += o.edgeSize * w;
                    add4(a.textureFactor, o.textureFactor, w);
                    add4(a.sphereFactor, o.sphereFactor, w);
                    add4(a.toonFactor, o.toonFactor, w);
                }
            }
        }
    }
}

void ModelInstance::ApplyMorphs() {
    for (BoneState& b : bones_) {
        b.morphT = {0, 0, 0};
        b.morphR = QuatIdentity();
    }
    pendingVertexWeight_.assign(morphWeight_.size(), 0.0f);
    if (!appliedMaterialWeight_.empty()) pendingMaterialWeight_.assign(morphWeight_.size(), 0.0f);
    for (size_t i = 0; i < morphWeight_.size(); ++i)
        if (morphWeight_[i] != 0.0f) AddMorph((int)i, morphWeight_[i], 0);
    if (!appliedMaterialWeight_.empty() && pendingMaterialWeight_ != appliedMaterialWeight_) {
        appliedMaterialWeight_.swap(pendingMaterialWeight_);
        RebuildMaterialFactors();
        ++materialVersion_;
    }

    if (pendingVertexWeight_ == appliedMorphWeight_) return;
    const auto& morphs = model_->morphs;
    const int vertexCount = (int)morphDelta_.size();
    for (size_t i = 0; i < morphs.size(); ++i) {
        if (appliedMorphWeight_[i] == 0.0f) continue;
        for (const auto& o : morphs[i].vertexOffsets)
            if (o.vertex >= 0 && o.vertex < vertexCount) morphDelta_[o.vertex] = {0, 0, 0};
    }
    for (size_t i = 0; i < morphs.size(); ++i) {
        const float w = pendingVertexWeight_[i];
        if (w == 0.0f) continue;
        for (const auto& o : morphs[i].vertexOffsets) {
            if (o.vertex < 0 || o.vertex >= vertexCount) continue;
            XMFLOAT3& d = morphDelta_[o.vertex];
            d.x += o.offset.x * w;
            d.y += o.offset.y * w;
            d.z += o.offset.z * w;
        }
    }
    appliedMorphWeight_ = pendingVertexWeight_;
    ++morphVersion_;
}

// ---------------------------------------------------------------------------- bones

void ModelInstance::UpdateLocal(int bone) {
    const PmxBone& pb = model_->bones[bone];
    BoneState& b = bones_[bone];
    XMFLOAT3 t = AnimTranslation(bone);
    if (pb.flags & PmxBone_AppendTranslate) {
        t.x += b.appendT.x; t.y += b.appendT.y; t.z += b.appendT.z;
    }
    XMVECTOR r = XMLoadFloat4(&bones_[bone].animR);
    r = XMQuaternionMultiply(XMLoadFloat4(&b.morphR), r);  // morph, then animation
    r = XMQuaternionMultiply(r, XMLoadFloat4(&b.ikR));      // then IK
    if (pb.flags & PmxBone_AppendRotate) r = XMQuaternionMultiply(XMLoadFloat4(&b.appendR), r);
    XMFLOAT3 offset = pb.position;
    if (pb.parentIndex >= 0 && pb.parentIndex < (int)bones_.size()) {
        const XMFLOAT3& pp = model_->bones[pb.parentIndex].position;
        offset = {offset.x - pp.x, offset.y - pp.y, offset.z - pp.z};
    }
    const XMMATRIX local = XMMatrixMultiply(XMMatrixRotationQuaternion(XMQuaternionNormalize(r)),
                                            XMMatrixTranslation(offset.x + t.x, offset.y + t.y, offset.z + t.z));
    XMStoreFloat4x4(&b.local, local);
}

void ModelInstance::UpdateWorld(int bone) {
    BoneState& b = bones_[bone];
    const int p = model_->bones[bone].parentIndex;
    if (p >= 0 && p < (int)bones_.size() && p != bone)
        XMStoreFloat4x4(&b.world, XMMatrixMultiply(XMLoadFloat4x4(&b.local), XMLoadFloat4x4(&bones_[p].world)));
    else
        XMStoreFloat4x4(&b.world, XMMatrixMultiply(XMLoadFloat4x4(&b.local), XMLoadFloat4x4(&root_)));
}

void ModelInstance::SetRootTransform(const XMFLOAT4X4& m) { root_ = m; }

void ModelInstance::UpdateWorldRecursive(int bone) {
    // Iterative DFS (bone hierarchies can be deep).
    std::vector<int>& stack = dfsStack_;
    stack.clear();
    stack.push_back(bone);
    while (!stack.empty()) {
        const int b = stack.back();
        stack.pop_back();
        UpdateWorld(b);
        for (int c : children_[b]) stack.push_back(c);
    }
}

void ModelInstance::SolveIk(int ikBone) {
    const PmxBone& ik = model_->bones[ikBone];
    const int target = ik.ikTargetIndex;
    const int n = (int)bones_.size();
    if (target < 0 || target >= n || ik.ikLinks.empty()) return;

    struct ChainState { XMFLOAT3 prevAngle{}; float planeAngle = 0; XMFLOAT4 saved{0, 0, 0, 1}; };
    std::vector<ChainState> chain(ik.ikLinks.size());
    for (const PmxIkLink& link : ik.ikLinks) {
        if (link.boneIndex < 0 || link.boneIndex >= n) continue;
        bones_[link.boneIndex].ikR = QuatIdentity();
        UpdateLocal(link.boneIndex);
        UpdateWorldRecursive(link.boneIndex);
    }

    const float limitAngle = ik.ikLimitAngle;
    float bestDist = std::numeric_limits<float>::max();
    for (int iter = 0; iter < std::max(ik.ikLoopCount, 1); ++iter) {
        for (size_t ci = 0; ci < ik.ikLinks.size(); ++ci) {
            const PmxIkLink& link = ik.ikLinks[ci];
            const int node = link.boneIndex;
            if (node < 0 || node >= n || node == target) continue;
            ChainState& cs = chain[ci];
            BoneState& nb = bones_[node];

            const XMVECTOR goalPos = Translation(bones_[ikBone].world);
            const XMVECTOR effPos = Translation(bones_[target].world);
            const XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&nb.world));
            const XMVECTOR goalLocal = XMVector3Normalize(XMVector3TransformCoord(goalPos, inv));
            const XMVECTOR effLocal = XMVector3Normalize(XMVector3TransformCoord(effPos, inv));
            const float dot = std::clamp(XMVectorGetX(XMVector3Dot(effLocal, goalLocal)), -1.0f, 1.0f);
            float angle = std::acos(dot);
            const XMVECTOR animRot = XMLoadFloat4(&nb.animR);
            const XMVECTOR baseRot = XMQuaternionMultiply(XMLoadFloat4(&nb.morphR), animRot);

            // Single-axis limited links (knees) use the dedicated plane solver.
            int planeAxis = -1;
            if (link.hasLimit) {
                const bool x = link.limitMin.x != 0 || link.limitMax.x != 0;
                const bool y = link.limitMin.y != 0 || link.limitMax.y != 0;
                const bool z = link.limitMin.z != 0 || link.limitMax.z != 0;
                if (x && !y && !z) planeAxis = 0;
                else if (y && !x && !z) planeAxis = 1;
                else if (z && !x && !y) planeAxis = 2;
            }

            if (planeAxis >= 0) {
                const float mins[3] = {link.limitMin.x, link.limitMin.y, link.limitMin.z};
                const float maxs[3] = {link.limitMax.x, link.limitMax.y, link.limitMax.z};
                const XMVECTOR axis = planeAxis == 0 ? XMVectorSet(1, 0, 0, 0) : planeAxis == 1 ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
                angle = std::clamp(angle, -limitAngle, limitAngle);
                const float d1 = XMVectorGetX(XMVector3Dot(XMVector3Rotate(effLocal, XMQuaternionRotationAxis(axis, angle)), goalLocal));
                const float d2 = XMVectorGetX(XMVector3Dot(XMVector3Rotate(effLocal, XMQuaternionRotationAxis(axis, -angle)), goalLocal));
                float newAngle = cs.planeAngle + (d1 > d2 ? angle : -angle);
                const float lo = mins[planeAxis], hi = maxs[planeAxis];
                if (iter == 0 && (newAngle < lo || newAngle > hi)) {
                    if (-newAngle > lo && -newAngle < hi) newAngle = -newAngle;
                    else {
                        const float half = (lo + hi) * 0.5f;
                        if (std::fabs(half - newAngle) > std::fabs(half + newAngle)) newAngle = -newAngle;
                    }
                }
                newAngle = std::clamp(newAngle, lo, hi);
                cs.planeAngle = newAngle;
                // ikR such that (base then ikR) == rotation(axis, newAngle)
                const XMVECTOR target = XMQuaternionRotationAxis(axis, newAngle);
                XMStoreFloat4(&nb.ikR, XMQuaternionMultiply(XMQuaternionInverse(baseRot), target));
            } else {
                if (angle < 1e-5f) continue;
                angle = std::clamp(angle, -limitAngle, limitAngle);
                XMVECTOR axis = XMVector3Cross(effLocal, goalLocal);
                if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f) continue;
                axis = XMVector3Normalize(axis);
                const XMVECTOR rot = XMQuaternionRotationAxis(axis, angle);
                // chainRot (glm: ik * base * rot) == rot, then base, then ik
                XMVECTOR chainRot = XMQuaternionMultiply(XMQuaternionMultiply(rot, baseRot), XMLoadFloat4(&nb.ikR));
                if (link.hasLimit) {
                    XMFLOAT3 e = DecomposeXYZ(chainRot, cs.prevAngle);
                    e.x = std::clamp(e.x, link.limitMin.x, link.limitMax.x);
                    e.y = std::clamp(e.y, link.limitMin.y, link.limitMax.y);
                    e.z = std::clamp(e.z, link.limitMin.z, link.limitMax.z);
                    e.x = std::clamp(e.x - cs.prevAngle.x, -limitAngle, limitAngle) + cs.prevAngle.x;
                    e.y = std::clamp(e.y - cs.prevAngle.y, -limitAngle, limitAngle) + cs.prevAngle.y;
                    e.z = std::clamp(e.z - cs.prevAngle.z, -limitAngle, limitAngle) + cs.prevAngle.z;
                    cs.prevAngle = e;
                    chainRot = QuatFromXYZ(e);
                }
                // ikR such that (base then ikR) == chainRot
                XMStoreFloat4(&nb.ikR, XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionInverse(baseRot), chainRot)));
            }
            UpdateLocal(node);
            UpdateWorldRecursive(node);
        }

        const float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(Translation(bones_[target].world), Translation(bones_[ikBone].world))));
        if (dist < bestDist) {
            bestDist = dist;
            for (size_t ci = 0; ci < ik.ikLinks.size(); ++ci) {
                const int node = ik.ikLinks[ci].boneIndex;
                if (node >= 0 && node < n) chain[ci].saved = bones_[node].ikR;
            }
        } else {
            for (size_t ci = 0; ci < ik.ikLinks.size(); ++ci) {
                const int node = ik.ikLinks[ci].boneIndex;
                if (node < 0 || node >= n) continue;
                bones_[node].ikR = chain[ci].saved;
                UpdateLocal(node);
                UpdateWorldRecursive(node);
            }
            break;
        }
    }
}

void ModelInstance::UpdatePose(float physicsDt) {
    ApplyMorphs();
    const auto& pbones = model_->bones;
    const int n = (int)bones_.size();
    for (int b : order_) {
        bones_[b].ikR = QuatIdentity();
        bones_[b].appendT = {0, 0, 0};
        bones_[b].appendR = QuatIdentity();
        UpdateLocal(b);
    }
    for (int b : order_) {
        const int p = pbones[b].parentIndex;
        if (p < 0 || p >= n || p == b) UpdateWorldRecursive(b);
    }
    for (size_t i = 0; i < afterPhysicsBegin_; ++i) EvaluateAppendAndIk(order_[i]);
    if (physicsEnabled_ && physics_ && physics_->HasDynamicBodies()) RunPhysics(physicsDt);
    for (size_t i = afterPhysicsBegin_; i < order_.size(); ++i) EvaluateAppendAndIk(order_[i]);
    for (int i = 0; i < n; ++i) {
        const XMFLOAT3& p = pbones[i].position;
        XMMATRIX m = XMMatrixMultiply(XMMatrixTranslation(-p.x, -p.y, -p.z), XMLoadFloat4x4(&bones_[i].world));
        if (scale_ != 1.0f) m = XMMatrixMultiply(m, XMMatrixScaling(scale_, scale_, scale_));
        XMStoreFloat4x4(&skin_[i], m);
    }
}

void ModelInstance::EvaluateAppendAndIk(int b) {
    const auto& pbones = model_->bones;
    const int n = (int)bones_.size();
    const PmxBone& pb = pbones[b];
    if ((pb.flags & (PmxBone_AppendRotate | PmxBone_AppendTranslate)) && pb.appendParentIndex >= 0 &&
        pb.appendParentIndex < n && pb.appendParentIndex != b) {
        const int a = pb.appendParentIndex;
        const PmxBone& pa = pbones[a];
        const bool aHasAppend = pa.appendParentIndex >= 0 && (pa.flags & (PmxBone_AppendRotate | PmxBone_AppendTranslate));
        BoneState& s = bones_[b];
        if (pb.flags & PmxBone_AppendRotate) {
            XMVECTOR ar;
            if ((pb.flags & PmxBone_AppendLocal) || !aHasAppend) {
                const XMFLOAT4 r = AnimRotation(a);
                ar = XMLoadFloat4(&r);
            } else {
                ar = XMLoadFloat4(&bones_[a].appendR);
            }
            ar = XMQuaternionMultiply(ar, XMLoadFloat4(&bones_[a].ikR));
            XMStoreFloat4(&s.appendR, XMQuaternionSlerp(XMQuaternionIdentity(), ar, pb.appendRatio));
        }
        if (pb.flags & PmxBone_AppendTranslate) {
            XMFLOAT3 at = ((pb.flags & PmxBone_AppendLocal) || !aHasAppend) ? AnimTranslation(a) : bones_[a].appendT;
            s.appendT = {at.x * pb.appendRatio, at.y * pb.appendRatio, at.z * pb.appendRatio};
        }
        UpdateLocal(b);
        UpdateWorldRecursive(b);
    }
    if ((pb.flags & PmxBone_IK) && bones_[b].ikEnabled) {
        SolveIk(b);
        UpdateWorldRecursive(b);
    }
}

// Hands the animated pose to the physics world, steps it, and writes the simulated bones back.
// Bones are revisited parents-first so non-simulated children of simulated bones follow them;
// simulated bones get their local matrix rebuilt so later append/IK passes stay consistent.
void ModelInstance::RunPhysics(float dt) {
    const int n = (int)bones_.size();
    physicsPose_.resize(n);
    for (int i = 0; i < n; ++i) physicsPose_[i] = bones_[i].world;
    if (physicsResetPending_) {
        physics_->Reset(physicsPose_, 1.0f);
        physicsResetPending_ = false;
    } else {
        physics_->Step(physicsPose_, dt);
    }
    physics_->Results(physicsPose_, physicsOut_);
    if (physicsOut_.empty()) return;

    physicsResult_.assign(n, -1);
    for (size_t i = 0; i < physicsOut_.size(); ++i) physicsResult_[physicsOut_[i].bone] = (int)i;
    std::vector<int>& stack = dfsStack_;
    stack.clear();
    for (int i = 0; i < n; ++i) {
        const int p = model_->bones[i].parentIndex;
        if (p < 0 || p >= n || p == i) stack.push_back(i);
    }
    while (!stack.empty()) {
        const int b = stack.back();
        stack.pop_back();
        BoneState& s = bones_[b];
        const int p = model_->bones[b].parentIndex;
        const bool hasParent = p >= 0 && p < n && p != b;
        if (physicsResult_[b] >= 0) {
            s.world = physicsOut_[physicsResult_[b]].world;
            if (hasParent)
                XMStoreFloat4x4(&s.local, XMMatrixMultiply(XMLoadFloat4x4(&s.world),
                                                           XMMatrixInverse(nullptr, XMLoadFloat4x4(&bones_[p].world))));
            else
                XMStoreFloat4x4(&s.local, XMMatrixMultiply(XMLoadFloat4x4(&s.world),
                                                           XMMatrixInverse(nullptr, XMLoadFloat4x4(&root_))));
        } else {
            UpdateWorld(b);
        }
        for (int c : children_[b]) stack.push_back(c);
    }
}

XMFLOAT3 ModelInstance::BoneWorldPosition(int bone) const {
    if (bone < 0 || bone >= (int)bones_.size()) return {0, 0, 0};
    const XMFLOAT4X4& w = bones_[bone].world;
    return {w._41 * scale_, w._42 * scale_, w._43 * scale_};
}

}  // namespace mmdx
