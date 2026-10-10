// rest_pose_test [<pmx|pmd|dir>...]  : a synthetic self test (no arguments: only that), then measures the upper arm angle of every model (recursively for directories),
// normalises T-pose arms (asset/RestPose.h) and checks the result: angle after, finite data, bone/vertex counts.
// Exit code 1 when a check fails.
#include "asset/ModelImport.h"
#include "asset/RestPose.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <cmath>
#include <DirectXMath.h>
#include <cstdio>
#include <filesystem>

using namespace mmdx;
using namespace DirectX;
namespace fs = std::filesystem;

static bool Finite(const XMFLOAT3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

static int Check(const fs::path& path, bool& anyT) {
    PmxModel m;
    std::string err;
    const ModelFormat f = ModelFormatFromPath(path);
    if (!IsMmdModelFormat(f)) return 0;
    if (!(f == ModelFormat::Pmx ? LoadPmx(path, m, &err) : LoadPmd(path, m, &err))) {
        printf("SKIP %s: %s\n", PathToUtf8(path).c_str(), err.c_str());
        return 0;
    }
    const ArmRest before = MeasureArmRest(m);
    if (!before.valid) {
        printf("NOARM  %s\n", PathToUtf8(path.filename()).c_str());
        return 0;
    }
    // Reference: the vertex centroid weighted to nothing in particular -> track a few probes instead.
    const RestPoseFix fix = NormalizeArmRestPose(m);
    const ArmRest after = MeasureArmRest(m);
    int bad = 0;
    for (const PmxBone& b : m.bones) bad += Finite(b.position) ? 0 : 1;
    for (const PmxVertex& v : m.vertices) bad += (Finite(v.position) && Finite(v.normal)) ? 0 : 1;
    for (const PmxRigidBody& r : m.rigidBodies) bad += (Finite(r.position) && Finite(r.rotation)) ? 0 : 1;
    for (const PmxJoint& j : m.joints) bad += (Finite(j.position) && Finite(j.rotation)) ? 0 : 1;
    const bool tBefore = before.tPose();
    anyT |= tBefore;
    const bool angleOk = !fix.applied || (std::fabs(after.deg[0] - kMmdArmAngleDeg) < 0.5f || before.deg[0] >= kTPoseMaxArmDeg) &&
                                          (std::fabs(after.deg[1] - kMmdArmAngleDeg) < 0.5f || before.deg[1] >= kTPoseMaxArmDeg);
    printf("%s %-40s before L%6.1f R%6.1f  after L%6.1f R%6.1f  moved: bones %d verts %d bodies %d joints %d%s%s\n",
           tBefore ? "T-POSE" : "a-pose", PathToUtf8(path.filename()).c_str(), before.deg[0], before.deg[1], after.deg[0], after.deg[1], fix.bonesMoved,
           fix.verticesMoved, fix.bodiesMoved, fix.jointsMoved, bad ? "  NON-FINITE" : "", angleOk ? "" : "  ANGLE-MISMATCH");
    return (bad || !angleOk) ? 1 : 0;
}


// Synthetic model: a T-pose left arm (raised chain bones, a half-weighted vertex, a vertex morph, a rigid body with an
// Euler rotation, a joint) and an A-pose right arm that must stay untouched.
static int SelfTest() {
    PmxModel m;
    auto bone = [&](const char* name, XMFLOAT3 pos, int parent) {
        PmxBone b;
        b.name = name;
        b.position = pos;
        b.parentIndex = parent;
        m.bones.push_back(b);
        return (int)m.bones.size() - 1;
    };
    const int body = bone("上半身", {0, 15, 0}, -1);
    const int lArm = bone("左腕", {1.5f, 16, 0}, body);
    const int lElbow = bone("左ひじ", {4.5f, 16, 0}, lArm);
    const int lWrist = bone("左手首", {7, 16, 0}, lElbow);
    const int rArm = bone("右腕", {-1.5f, 16, 0}, body);
    const float c = std::cos(0.6f), s = std::sin(0.6f);
    bone("右ひじ", {-1.5f - 3 * c, 16 - 3 * s, 0}, rArm);
    auto vert = [&](XMFLOAT3 p, int b0, int b1, float w0) {
        PmxVertex v;
        v.position = p;
        v.normal = {0, 1, 0};
        v.deform = b1 < 0 ? PmxDeform::BDEF1 : PmxDeform::BDEF2;
        v.boneIndex[0] = b0;
        v.boneIndex[1] = b1;
        v.boneWeight[0] = b1 < 0 ? 1.0f : w0;
        v.boneWeight[1] = b1 < 0 ? 0.0f : 1.0f - w0;
        m.vertices.push_back(v);
    };
    vert({6, 16.5f, 0}, lElbow, -1, 1);       // fully on the arm: rotates rigidly
    vert({1.5f, 17, 0}, lArm, body, 0.5f);    // shoulder: half way
    vert({0, 15, 0}, body, -1, 1);            // torso: stays
    vert({-5, 15, 0}, rArm, -1, 1);           // right arm: stays
    PmxMorph morph;
    morph.type = PmxMorphType::Vertex;
    morph.vertexOffsets = {{0, {0, 1, 0}}, {2, {0, 1, 0}}};
    m.morphs.push_back(morph);
    PmxRigidBody rb;
    rb.boneIndex = lWrist;
    rb.position = {7, 16, 0};
    rb.rotation = {0.2f, 0.3f, 0.4f};
    m.rigidBodies.push_back(rb);
    PmxRigidBody rbTorso;
    rbTorso.boneIndex = body;
    rbTorso.position = {0, 15, 0};
    m.rigidBodies.push_back(rbTorso);
    PmxJoint j;
    j.rigidBodyA = 1;
    j.rigidBodyB = 0;
    j.position = {7, 16, 0};
    m.joints.push_back(j);

    const PmxModel before = m;
    const RestPoseFix fix = NormalizeArmRestPose(m);
    int fails = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) { printf("SELFTEST FAIL: %s\n", what); ++fails; }
    };
    auto dist = [](XMFLOAT3 a, XMFLOAT3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
    const XMFLOAT3 pivot = before.bones[lArm].position;
    const ArmRest after = MeasureArmRest(m);
    expect(fix.applied, "fix applied");
    expect(std::fabs(after.deg[0] - kMmdArmAngleDeg) < 0.1f, "left arm at 38 deg");
    expect(std::fabs(after.deg[1] - fix.before.deg[1]) < 1e-3f, "right arm untouched");
    for (int b : {lElbow, lWrist})
        expect(std::fabs(dist(m.bones[b].position, pivot) - dist(before.bones[b].position, pivot)) < 1e-3f, "chain bone keeps its distance to the shoulder");
    expect(dist(m.bones[lArm].position, pivot) < 1e-6f && dist(m.bones[rArm].position, before.bones[rArm].position) < 1e-6f, "pivot and right arm fixed");
    expect(dist(m.vertices[0].position, m.bones[lElbow].position) > 0.1f && std::fabs(dist(m.vertices[0].position, pivot) - dist(before.vertices[0].position, pivot)) < 1e-3f,
           "arm vertex rotates rigidly");
    expect(dist(m.vertices[2].position, before.vertices[2].position) < 1e-6f && dist(m.vertices[3].position, before.vertices[3].position) < 1e-6f, "torso/right vertices stay");
    {
        // half-weighted vertex ends up midway between its original place and the fully rotated one (not fixed in length by design)
        const XMFLOAT3 o = before.vertices[1].position, n = m.vertices[1].position;
        expect(dist(o, n) > 0.01f && dist(o, n) < dist(before.vertices[0].position, m.vertices[0].position), "shoulder vertex blends");
    }
    expect(std::fabs(m.vertices[0].normal.x * m.vertices[0].normal.x + m.vertices[0].normal.y * m.vertices[0].normal.y + m.vertices[0].normal.z * m.vertices[0].normal.z - 1) < 1e-4f, "normal unit length");
    expect(dist(m.morphs[0].vertexOffsets[1].offset, {0, 1, 0}) < 1e-6f, "morph offset on the torso unchanged");
    expect(dist(m.morphs[0].vertexOffsets[0].offset, {0, 1, 0}) > 0.05f, "morph offset on the arm rotated");
    // Rigid body: position follows the wrist; orientation = old * R (compare the rotated local X axis).
    {
        const PmxRigidBody& a = m.rigidBodies[0];
        expect(dist(a.position, m.bones[lWrist].position) < 1e-3f, "rigid body sits on the wrist");
        const XMMATRIX o = XMMatrixRotationRollPitchYaw(before.rigidBodies[0].rotation.x, before.rigidBodies[0].rotation.y, before.rigidBodies[0].rotation.z);
        const XMMATRIX n = XMMatrixRotationRollPitchYaw(a.rotation.x, a.rotation.y, a.rotation.z);
        // n = o * R  =>  n * o^-1 ... = o^-1 * n is R; rotating the wrist offset with it must reproduce the bone move.
        const XMMATRIX R = XMMatrixInverse(nullptr, o) * n;
        const XMVECTOR rel = XMVectorSet(7 - pivot.x, 16 - pivot.y, 0, 0);
        XMFLOAT3 moved;
        XMStoreFloat3(&moved, XMVector3TransformNormal(rel, R) + XMLoadFloat3(&pivot));
        expect(dist(moved, m.bones[lWrist].position) < 1e-3f, "rigid body Euler rotation matches the chain rotation");
    }
    expect(dist(m.rigidBodies[1].position, before.rigidBodies[1].position) < 1e-6f, "torso body unchanged");
    expect(dist(m.joints[0].position, m.bones[lWrist].position) < 1e-3f, "joint follows its dynamic body");
    expect(fix.bodiesMoved == 1 && fix.jointsMoved == 1 && fix.bonesMoved == 2, "counters");
    // Idempotent: a second pass finds an A-pose.
    PmxModel again = m;
    expect(!NormalizeArmRestPose(again).applied, "second pass is a no-op");
    printf("selftest: %s\n", fails ? "FAILED" : "ok");
    return fails;
}

int wmain(int argc, wchar_t** argv) {
    int failures = SelfTest(), files = 0;
    bool anyT = false;
    for (int i = 1; i < argc; ++i) {
        const fs::path p(argv[i]);
        if (fs::is_directory(p)) {
            for (const auto& e : fs::recursive_directory_iterator(p)) {
                if (!e.is_regular_file()) continue;
                ++files;
                failures += Check(e.path(), anyT);
            }
        } else {
            ++files;
            failures += Check(p, anyT);
        }
    }
    if (argc > 1) printf("%d files, %d failures%s\n", files, failures, anyT ? "" : " (no T-pose model seen)");
    return failures ? 1 : 0;
}
