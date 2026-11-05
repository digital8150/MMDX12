#include "studio/PhysicsBake.h"
#include "anim/ModelInstance.h"
#include <algorithm>
#include <cmath>
#include <set>

using namespace DirectX;

namespace mmdx::studio {

namespace {

// Linear interpolation block (VMD layout): every channel's curve is {20, 20, 107, 107}.
BoneKf MakeLinearKey(int frame, const XMFLOAT3& t, const XMFLOAT4& r) {
    BoneKf k;
    k.frame = frame;
    k.t = t;
    k.r = r;
    FillLinearInterp(k.interp);
    return k;
}

// The rotation part of a matrix as a unit quaternion.
XMFLOAT4 MatrixRotation(const XMFLOAT4X4& m) {
    XMFLOAT4X4 r = m;
    r._14 = r._24 = r._34 = 0.0f; r._44 = 1.0f;
    r._41 = r._42 = r._43 = 0.0f;
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionRotationMatrix(XMLoadFloat4x4(&r))));
    return q;
}

} // namespace

std::vector<int> PhysicsDrivenBones(const PmxModel& pmx) {
    const int bodyCount = (int)pmx.rigidBodies.size();
    // A simulated body without a joint can only fall away; PhysicsWorld makes it kinematic (mode 0),
    // so it never drives a bone. Mirror that here.
    std::vector<bool> jointed(bodyCount, false);
    for (const PmxJoint& pj : pmx.joints) {
        if (pj.rigidBodyA >= 0 && pj.rigidBodyA < bodyCount) jointed[pj.rigidBodyA] = true;
        if (pj.rigidBodyB >= 0 && pj.rigidBodyB < bodyCount) jointed[pj.rigidBodyB] = true;
    }
    std::set<int> bones;
    for (int i = 0; i < bodyCount; ++i) {
        const PmxRigidBody& r = pmx.rigidBodies[(size_t)i];
        if (r.physicsMode == 0 || !jointed[(size_t)i]) continue;
        if (r.boneIndex >= 0 && r.boneIndex < (int)pmx.bones.size()) bones.insert(r.boneIndex);
    }
    return std::vector<int>(bones.begin(), bones.end());
}

bool BakePhysics(std::shared_ptr<const PmxModel> pmx, const BoundMotion& motion,
                 const XMFLOAT4X4& root, const BakeOptions& options, BakeResult& out,
                 const std::atomic<bool>* cancel, std::atomic<float>* progress) {
    out.bones.clear();
    if (!pmx) return true;
    const PmxModel& model = *pmx;

    std::vector<int> bones = options.bones;
    if (bones.empty()) bones = PhysicsDrivenBones(model);
    std::sort(bones.begin(), bones.end());
    bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
    if (bones.empty()) return true;

    const int start = std::max(0, options.start);
    const int end = std::max(start, options.end);
    const int step = std::max(1, options.step);
    const int preroll = std::max(0, options.preroll);

    ModelInstance inst(pmx);
    inst.SetRootTransform(root);
    inst.EnablePhysics(true);
    if (!inst.PhysicsEnabled()) return true;  // no simulated bodies: nothing to bake
    const PhysicsEnv& env = options.env;      // the same gravity / wind as StudioUpdateModel
    inst.SetGravity(env.gravity, env.gravityDir);
    inst.SetWind(env.windDir, env.windStrength, env.windGust, env.windGustFreq, env.windJitter);
    const int preFrom = std::max(0, start - preroll);
    inst.ResetPhysics((float)preFrom / kMmdFps);  // the gusts follow the timeline, as in the viewport and renders

    const XMFLOAT4 rootRot = MatrixRotation(root);
    const XMFLOAT3 rootPos{root._41, root._42, root._43};
    const float span = (float)std::max(1, end - start);

    for (int f = preFrom; f <= end; ++f) {
        if (cancel && cancel->load()) return false;
        motion.Evaluate((float)f, inst);
        inst.UpdatePose(1.0f / kMmdFps);
        if (f < start || ((f - start) % step != 0 && f != end)) continue;  // every `step` frames, and always `end`
        if (progress) progress->store((float)(f - start) / span, std::memory_order_relaxed);

        for (int b : bones) {
            const PmxBone& pb = model.bones[(size_t)b];
            const int p = pb.parentIndex;
            const bool hasParent = p >= 0 && p < (int)model.bones.size() && p != b;

            // Parent world rotation and position (the root for a root bone).
            const XMFLOAT4 parentRot = hasParent ? MatrixRotation(inst.BoneWorld(p)) : rootRot;
            const XMFLOAT3 parentPos = hasParent ? inst.BoneWorldPosition(p) : rootPos;
            const XMFLOAT3 parentBind = hasParent ? model.bones[(size_t)p].position : XMFLOAT3{0, 0, 0};

            // rotation: W = local * P  =>  local = W * inverse(P). MMD bones have no rest rotation.
            const XMFLOAT4 wRot = MatrixRotation(inst.BoneWorld(b));
            XMFLOAT4 r;
            XMStoreFloat4(&r, XMQuaternionMultiply(XMLoadFloat4(&wRot), XMQuaternionInverse(XMLoadFloat4(&parentRot))));

            // translation: the world offset into the parent's frame, minus the bind offset.
            const XMFLOAT3 wbPos = inst.BoneWorldPosition(b);
            const XMFLOAT3 bindOffset{pb.position.x - parentBind.x, pb.position.y - parentBind.y,
                                      pb.position.z - parentBind.z};
            const XMVECTOR offset = XMVectorSet(wbPos.x - parentPos.x, wbPos.y - parentPos.y, wbPos.z - parentPos.z, 0.0f);
            XMFLOAT3 t;
            XMStoreFloat3(&t, XMVector3Rotate(offset, XMQuaternionInverse(XMLoadFloat4(&parentRot))));
            t.x -= bindOffset.x; t.y -= bindOffset.y; t.z -= bindOffset.z;
            if (t.x * t.x + t.y * t.y + t.z * t.z < 1e-8f) t = {0, 0, 0};

            out.bones[model.bones[(size_t)b].name].push_back(MakeLinearKey(f, t, r));
        }
    }
    if (progress) progress->store(1.0f, std::memory_order_relaxed);
    return true;
}

} // namespace mmdx::studio
