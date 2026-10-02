#include "anim/Physics.h"
#include "core/Log.h"
#include <btBulletDynamicsCommon.h>
#include <algorithm>

using namespace DirectX;

namespace mmdx {

namespace {

constexpr float kGravity = -9.8f * 10.0f;  // MMD units are roughly 10 cm
constexpr float kSubstep = 1.0f / 120.0f;
constexpr int kMaxSubsteps = 10;
// Some rigs hang 6e9-mass "anchor" weights off the head (e.g. pda_miku's skirt). In float Bullet
// a joint cannot hold such a body and it free-falls, dragging the skirt along. 1e5 still acts as
// an immovable anchor next to 0.1..10 mass cloth and hair bodies.
constexpr float kMaxMass = 1e5f;
constexpr float kTeleportDistance = 10.0f;  // per update; about half a character's height
constexpr float kTeleportSettleSeconds = 0.25f;

// A row-vector DirectXMath matrix stored row-major has the same memory layout as Bullet's
// column-major "OpenGL" matrix of the equivalent column-vector transform.
btTransform ToBt(const XMFLOAT4X4& m) {
    btTransform t;
    t.setFromOpenGLMatrix(&m._11);
    return t;
}

XMFLOAT4X4 FromBt(const btTransform& t) {
    XMFLOAT4X4 m;
    t.getOpenGLMatrix(&m._11);
    m._14 = m._24 = m._34 = 0.0f;
    m._44 = 1.0f;
    return m;
}

// PMX Euler angles: Z, then X, then Y (D3DXMatrixRotationYawPitchRoll), then translate.
XMMATRIX PmxTransform(const XMFLOAT3& position, const XMFLOAT3& rotation) {
    return XMMatrixMultiply(XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z),
                            XMMatrixTranslation(position.x, position.y, position.z));
}

// Kinematic body: its transform is offset * boneWorld, read by Bullet every substep.
struct KinematicMotionState : btMotionState {
    const std::vector<XMFLOAT4X4>* boneWorld = nullptr;
    int bone = -1;
    XMFLOAT4X4 offset{};

    void getWorldTransform(btTransform& t) const override {
        XMMATRIX w = XMLoadFloat4x4(&offset);
        if (bone >= 0 && bone < (int)boneWorld->size()) w = XMMatrixMultiply(w, XMLoadFloat4x4(&(*boneWorld)[bone]));
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, w);
        t = ToBt(m);
    }
    void setWorldTransform(const btTransform&) override {}
};

}  // namespace

struct PhysicsWorld::Impl {
    struct Body {
        std::unique_ptr<btCollisionShape> shape;
        std::unique_ptr<btMotionState> motion;
        std::unique_ptr<btRigidBody> rb;
        int bone = -1;
        uint8_t mode = 0;
        XMFLOAT4X4 offset{}, invOffset{};  // rbWorld = offset * boneWorld
    };

    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    // Pairs collide through Bullet's default group/mask test, which is the PMX rule. Like MMD there
    // is no ground plane: motions and stages disagree about the floor height (some dance below y=0).
    btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &config};

    std::vector<Body> bodies;
    std::vector<std::unique_ptr<btTypedConstraint>> joints;
    std::vector<XMFLOAT4X4> boneWorld;  // read by the kinematic motion states

    ~Impl() {
        for (auto& j : joints) world.removeConstraint(j.get());
        for (auto& b : bodies) world.removeRigidBody(b.rb.get());
    }

    btTransform BodyTransform(const Body& b) const {
        XMMATRIX w = XMLoadFloat4x4(&b.offset);
        if (b.bone >= 0 && b.bone < (int)boneWorld.size()) w = XMMatrixMultiply(w, XMLoadFloat4x4(&boneWorld[b.bone]));
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, w);
        return ToBt(m);
    }
};

PhysicsWorld::PhysicsWorld(const PmxModel& model, const std::vector<XMFLOAT4X4>& bindWorld)
    : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    s.boneWorld = bindWorld;
    s.world.setGravity(btVector3(0, kGravity, 0));

    const int boneCount = (int)bindWorld.size();
    const int bodyTotal = (int)model.rigidBodies.size();
    // A simulated body without any joint can only fall away (truncated files lose their last
    // joints, e.g. 雪ミク 2017), so such bodies follow their bone instead.
    std::vector<bool> jointed(bodyTotal, false);
    for (const PmxJoint& pj : model.joints) {
        if (pj.rigidBodyA >= 0 && pj.rigidBodyA < bodyTotal) jointed[pj.rigidBodyA] = true;
        if (pj.rigidBodyB >= 0 && pj.rigidBodyB < bodyTotal) jointed[pj.rigidBodyB] = true;
    }
    int orphans = 0;
    s.bodies.reserve(bodyTotal);
    for (const PmxRigidBody& pr : model.rigidBodies) {
        Impl::Body b;
        b.bone = (pr.boneIndex >= 0 && pr.boneIndex < boneCount) ? pr.boneIndex : -1;
        b.mode = pr.physicsMode <= 2 ? pr.physicsMode : 1;
        if (b.mode != 0 && !jointed[s.bodies.size()]) {
            b.mode = 0;
            ++orphans;
        }

        const float sx = std::max(pr.size.x, 1e-3f), sy = std::max(pr.size.y, 1e-3f), sz = std::max(pr.size.z, 1e-3f);
        switch (pr.shape) {
        case 0: b.shape = std::make_unique<btSphereShape>(sx); break;
        case 1: b.shape = std::make_unique<btBoxShape>(btVector3(sx, sy, sz)); break;
        default: b.shape = std::make_unique<btCapsuleShape>(sx, sy); break;
        }

        const XMMATRIX rbBind = PmxTransform(pr.position, pr.rotation);
        const XMMATRIX boneBind = b.bone >= 0 ? XMLoadFloat4x4(&bindWorld[b.bone]) : XMMatrixIdentity();
        const XMMATRIX offset = XMMatrixMultiply(rbBind, XMMatrixInverse(nullptr, boneBind));
        XMStoreFloat4x4(&b.offset, offset);
        XMStoreFloat4x4(&b.invOffset, XMMatrixInverse(nullptr, offset));

        btScalar mass = 0;
        btVector3 inertia(0, 0, 0);
        if (b.mode != 0) {
            mass = std::clamp(pr.mass, 1e-3f, kMaxMass);
            b.shape->calculateLocalInertia(mass, inertia);
        }
        XMFLOAT4X4 bindM;
        XMStoreFloat4x4(&bindM, rbBind);
        if (b.mode == 0) {
            auto ms = std::make_unique<KinematicMotionState>();
            ms->boneWorld = &s.boneWorld;
            ms->bone = b.bone;
            ms->offset = b.offset;
            b.motion = std::move(ms);
        } else {
            b.motion = std::make_unique<btDefaultMotionState>(ToBt(bindM));
        }

        btRigidBody::btRigidBodyConstructionInfo info(mass, b.motion.get(), b.shape.get(), inertia);
        info.m_linearDamping = pr.linearDamping;
        info.m_angularDamping = pr.angularDamping;
        info.m_restitution = pr.restitution;
        info.m_friction = pr.friction;
        info.m_additionalDamping = true;
        b.rb = std::make_unique<btRigidBody>(info);
        b.rb->setSleepingThresholds(0.01f, XMConvertToRadians(0.1f));
        b.rb->setActivationState(DISABLE_DEACTIVATION);
        if (b.mode == 0) b.rb->setCollisionFlags(b.rb->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        b.rb->setWorldTransform(ToBt(bindM));

        const int group = 1 << std::min<int>(pr.group, 15);
        s.world.addRigidBody(b.rb.get(), group, pr.collisionMask);
        if (b.mode != 0 && b.bone >= 0) ++dynamicCount_;
        s.bodies.push_back(std::move(b));
    }

    const int bodyCount = (int)s.bodies.size();
    for (const PmxJoint& pj : model.joints) {
        if (pj.rigidBodyA < 0 || pj.rigidBodyA >= bodyCount || pj.rigidBodyB < 0 || pj.rigidBodyB >= bodyCount ||
            pj.rigidBodyA == pj.rigidBodyB)
            continue;
        btRigidBody& a = *s.bodies[pj.rigidBodyA].rb;
        btRigidBody& b = *s.bodies[pj.rigidBodyB].rb;
        XMFLOAT4X4 jm;
        XMStoreFloat4x4(&jm, PmxTransform(pj.position, pj.rotation));
        const btTransform jointWorld = ToBt(jm);
        const btTransform frameA = a.getWorldTransform().inverse() * jointWorld;
        const btTransform frameB = b.getWorldTransform().inverse() * jointWorld;
        auto c = std::make_unique<btGeneric6DofSpringConstraint>(a, b, frameA, frameB, true);
        c->setLinearLowerLimit(btVector3(pj.linearMin.x, pj.linearMin.y, pj.linearMin.z));
        c->setLinearUpperLimit(btVector3(pj.linearMax.x, pj.linearMax.y, pj.linearMax.z));
        c->setAngularLowerLimit(btVector3(pj.angularMin.x, pj.angularMin.y, pj.angularMin.z));
        c->setAngularUpperLimit(btVector3(pj.angularMax.x, pj.angularMax.y, pj.angularMax.z));
        const float springs[6] = {pj.springLinear.x, pj.springLinear.y, pj.springLinear.z,
                                  pj.springAngular.x, pj.springAngular.y, pj.springAngular.z};
        for (int i = 0; i < 6; ++i) {
            if (springs[i] == 0.0f) continue;
            c->enableSpring(i, true);
            c->setStiffness(i, springs[i]);
        }
        s.world.addConstraint(c.get());
        s.joints.push_back(std::move(c));
    }
    LOG_INFO("physics: %d rigid bodies (%d driving bones, %d unjointed made kinematic), %d joints", bodyCount,
             dynamicCount_, orphans, (int)s.joints.size());
}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::Reset(const std::vector<XMFLOAT4X4>& boneWorld, float settleSeconds) {
    Impl& s = *impl_;
    s.boneWorld = boneWorld;
    for (Impl::Body& b : s.bodies) {
        const btTransform t = s.BodyTransform(b);
        b.rb->setWorldTransform(t);
        b.rb->setInterpolationWorldTransform(t);
        b.rb->setLinearVelocity(btVector3(0, 0, 0));
        b.rb->setAngularVelocity(btVector3(0, 0, 0));
        b.rb->setInterpolationLinearVelocity(btVector3(0, 0, 0));
        b.rb->setInterpolationAngularVelocity(btVector3(0, 0, 0));
        b.rb->clearForces();
        if (b.mode != 0) b.motion->setWorldTransform(t);
    }
    s.world.getBroadphase()->resetPool(&s.dispatcher);
    s.world.getConstraintSolver()->reset();
    for (float t = 0; t < settleSeconds; t += 1.0f / 60.0f) s.world.stepSimulation(1.0f / 60.0f, kMaxSubsteps, kSubstep);
}

void PhysicsWorld::Step(const std::vector<XMFLOAT4X4>& boneWorld, float dt) {
    Impl& s = *impl_;
    // Some dance motions teleport the model between cuts. Simulating that would drag every body
    // across the jump, so a kinematic body moving that far in one update resets like a seek.
    for (const Impl::Body& b : s.bodies) {
        if (b.mode != 0 || b.bone < 0) continue;
        const XMFLOAT4X4& from = s.boneWorld[b.bone];
        const XMFLOAT4X4& to = boneWorld[b.bone];
        const float dx = to._41 - from._41, dy = to._42 - from._42, dz = to._43 - from._43;
        if (dx * dx + dy * dy + dz * dz > kTeleportDistance * kTeleportDistance) {
            Reset(boneWorld, kTeleportSettleSeconds);
            return;
        }
    }
    s.boneWorld = boneWorld;
    if (dt > 0) s.world.stepSimulation(dt, kMaxSubsteps, kSubstep);
}

void PhysicsWorld::Results(const std::vector<XMFLOAT4X4>& boneWorld, std::vector<BoneResult>& out) const {
    out.clear();
    for (const Impl::Body& b : impl_->bodies) {
        if (b.mode == 0 || b.bone < 0) continue;
        btTransform t;
        b.motion->getWorldTransform(t);  // interpolated between substeps
        const XMFLOAT4X4 rbWorld = FromBt(t);
        BoneResult r;
        r.bone = b.bone;
        XMStoreFloat4x4(&r.world, XMMatrixMultiply(XMLoadFloat4x4(&b.invOffset), XMLoadFloat4x4(&rbWorld)));
        if (b.mode == 2) {
            const XMFLOAT4X4& anim = boneWorld[b.bone];
            r.world._41 = anim._41; r.world._42 = anim._42; r.world._43 = anim._43;
        }
        out.push_back(r);
    }
}

}  // namespace mmdx
