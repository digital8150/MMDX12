// anim_probe <model.pmx> <motion.vmd> [camera.vmd]
// Binds a motion, evaluates a few frames and checks IK convergence (effector vs IK goal distance),
// then plays the motion with physics and flags exploding rigs. PHYS_DUMP=1 lists bodies + joints.
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace mmdx;

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { printf("usage: anim_probe <model.pmx> <motion.vmd> [camera.vmd]\n"); return 2; }
    auto pmx = std::make_shared<PmxModel>();
    std::string err;
    if (!LoadModelFile(argv[1], ModelRole::Character, *pmx, &err)) { printf("pmx: %s\n", err.c_str()); return 1; }
    VmdMotion vmd;
    if (!LoadVmd(argv[2], vmd, &err)) { printf("vmd: %s\n", err.c_str()); return 1; }
    auto motion = BoundMotion::Bind(*pmx, {&vmd});
    printf("model '%s' bones=%zu morphs=%zu | motion keys=%zu bound tracks bones=%d morphs=%d end=%.0f\n",
           pmx->name.c_str(), pmx->bones.size(), pmx->morphs.size(), vmd.boneKeys.size(),
           motion->BoundBoneCount(), motion->BoundMorphCount(), motion->EndFrame());

    {   // material morphs / SDEF: counts, and each material morph alone at weight 1 must give the
        // PMX result (multiply: factor = offset, add: factor = offset) on the materials it targets
        size_t matMorphs = 0, sdef = 0, bad = 0;
        for (const PmxVertex& v : pmx->vertices) sdef += v.deform == PmxDeform::SDEF;
        ModelInstance probe(pmx);
        for (size_t i = 0; i < pmx->morphs.size(); ++i) {
            const PmxMorph& mo = pmx->morphs[i];
            if (mo.type != PmxMorphType::Material) continue;
            ++matMorphs;
            probe.ResetPose();
            probe.SetMorphWeight((int)i, 1.0f);
            probe.UpdatePose();
            for (const auto& o : mo.materialOffsets) {
                const size_t first = o.material < 0 ? 0 : (size_t)o.material;
                const size_t last = o.material < 0 ? pmx->materials.size() : first + 1;
                for (size_t m = first; m < last && m < probe.MaterialMul().size(); ++m) {
                    // a morph may list one material twice: only check the single-entry case
                    size_t entries = 0;
                    for (const auto& o2 : mo.materialOffsets)
                        entries += (o2.material < 0 || (size_t)o2.material == m) ? 1 : 0;
                    if (entries != 1) continue;
                    const auto& f = o.operation == 0 ? probe.MaterialMul()[m] : probe.MaterialAdd()[m];
                    if (std::fabs(f.diffuse.w - o.diffuse.w) > 1e-5f || std::fabs(f.edgeSize - o.edgeSize) > 1e-5f ||
                        std::fabs(f.textureFactor.x - o.textureFactor.x) > 1e-5f)
                        ++bad;
                }
            }
            if (getenv("MATMORPH_DUMP"))
                printf("MATMORPH %zu '%s' offsets=%zu first: mat=%d op=%d diffuse.a=%.2f\n", i, mo.name.c_str(),
                       mo.materialOffsets.size(), mo.materialOffsets.empty() ? -2 : mo.materialOffsets[0].material,
                       mo.materialOffsets.empty() ? -1 : mo.materialOffsets[0].operation,
                       mo.materialOffsets.empty() ? 0.f : mo.materialOffsets[0].diffuse.w);
        }
        probe.ResetPose();
        probe.UpdatePose();
        bool identity = true;
        for (size_t m = 0; m < probe.MaterialMul().size(); ++m)
            identity &= probe.MaterialMul()[m].diffuse.x == 1.0f && probe.MaterialAdd()[m].diffuse.w == 0.0f;
        printf("material morphs=%zu (factor mismatches=%zu, reset to identity: %s) | SDEF vertices=%zu\n", matMorphs, bad,
               identity ? "ok" : "FAIL", sdef);
        if (bad || !identity) return 1;
    }

    if (getenv("PHYS_DUMP")) {  // list rigid bodies and joints, then exit
        for (size_t i = 0; i < pmx->rigidBodies.size(); ++i) {
            const auto& r = pmx->rigidBodies[i];
            printf("RB %zu %s bone=%d(%s) mode=%d shape=%d size=%.2f %.2f %.2f pos=%.2f %.2f %.2f rot=%.2f %.2f %.2f "
                   "mass=%.3f damp=%.2f %.2f rest=%.2f fr=%.2f grp=%d mask=%04x\n",
                   i, r.name.c_str(), r.boneIndex, r.boneIndex >= 0 ? pmx->bones[r.boneIndex].name.c_str() : "-",
                   r.physicsMode, r.shape, r.size.x, r.size.y, r.size.z, r.position.x, r.position.y, r.position.z,
                   r.rotation.x, r.rotation.y, r.rotation.z, r.mass, r.linearDamping, r.angularDamping, r.restitution,
                   r.friction, r.group, r.collisionMask);
        }
        for (size_t i = 0; i < pmx->joints.size(); ++i) {
            const auto& j = pmx->joints[i];
            printf("J %zu %s %d-%d lin %.2f..%.2f %.2f..%.2f %.2f..%.2f ang %.2f..%.2f %.2f..%.2f %.2f..%.2f "
                   "sl %.1f %.1f %.1f sa %.1f %.1f %.1f\n",
                   i, j.name.c_str(), j.rigidBodyA, j.rigidBodyB, j.linearMin.x, j.linearMax.x, j.linearMin.y,
                   j.linearMax.y, j.linearMin.z, j.linearMax.z, j.angularMin.x, j.angularMax.x, j.angularMin.y,
                   j.angularMax.y, j.angularMin.z, j.angularMax.z, j.springLinear.x, j.springLinear.y,
                   j.springLinear.z, j.springAngular.x, j.springAngular.y, j.springAngular.z);
        }
        return 0;
    }
    ModelInstance inst(pmx);
    const char* watch[] = {"センター", "頭", "左足首", "右足首", "左つま先", "右つま先"};
    for (float f : {0.0f, 300.0f, 900.0f, 1800.0f}) {
        motion->Evaluate(f, inst);
        inst.UpdatePose();
        printf("frame %.0f\n", f);
        for (const char* name : watch) {
            const int b = pmx->FindBone(name);
            if (b < 0) continue;
            const auto p = inst.BoneWorldPosition(b);
            printf("  %-10s (%7.2f %7.2f %7.2f)\n", name, p.x, p.y, p.z);
        }
        for (size_t i = 0; i < pmx->bones.size(); ++i) {
            const PmxBone& b = pmx->bones[i];
            if (!(b.flags & PmxBone_IK) || b.ikTargetIndex < 0) continue;
            const auto g = inst.BoneWorldPosition((int)i);
            const auto e = inst.BoneWorldPosition(b.ikTargetIndex);
            const float d = std::sqrt((g.x - e.x) * (g.x - e.x) + (g.y - e.y) * (g.y - e.y) + (g.z - e.z) * (g.z - e.z));
            printf("  IK %-10s -> %-10s dist=%.4f\n", b.name.c_str(), pmx->bones[b.ikTargetIndex].name.c_str(), d);
        }
    }
    {   // CPU-skin the whole mesh at frame 900 and report its bounds (sanity check for GPU skinning).
        motion->Evaluate(900.0f, inst);
        inst.UpdatePose();
        using namespace DirectX;
        XMVECTOR lo = XMVectorReplicate(1e9f), hi = XMVectorReplicate(-1e9f);
        const auto& skin = inst.SkinMatrices();
        const auto& morph = inst.VertexMorphDeltas();
        for (size_t i = 0; i < pmx->vertices.size(); ++i) {
            const PmxVertex& v = pmx->vertices[i];
            XMMATRIX m = XMMatrixSet(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            for (int k = 0; k < 4; ++k)
                if (v.boneIndex[k] >= 0 && v.boneWeight[k] != 0) m += XMLoadFloat4x4(&skin[v.boneIndex[k]]) * v.boneWeight[k];
            XMVECTOR p = XMVector3Transform(XMVectorSet(v.position.x + morph[i].x, v.position.y + morph[i].y, v.position.z + morph[i].z, 1), m);
            lo = XMVectorMin(lo, p); hi = XMVectorMax(hi, p);
        }
        XMFLOAT3 a, b; XMStoreFloat3(&a, lo); XMStoreFloat3(&b, hi);
        printf("CPU-skinned bounds @900: (%.1f %.1f %.1f) - (%.1f %.1f %.1f)\n", a.x, a.y, a.z, b.x, b.y, b.z);
    }
    {   // Scan the whole motion (half-frame steps) for non-finite or exploding skin matrices.
        int bad = 0;
        for (float f = 0; f <= motion->EndFrame() && bad < 5; f += 0.5f) {
            motion->Evaluate(f, inst);
            inst.UpdatePose();
            const auto& skin = inst.SkinMatrices();
            for (size_t b = 0; b < pmx->bones.size(); ++b) {
                const float* m = &skin[b]._11;
                bool ok = true;
                for (int k = 0; k < 16; ++k) ok &= std::isfinite(m[k]) && std::fabs(m[k]) < 1e4f;
                if (!ok) {
                    printf("BAD frame %.1f bone %zu '%s' row0 %f %f %f\n", f, b, pmx->bones[b].name.c_str(), m[0], m[1], m[2]);
                    ++bad;
                    break;
                }
            }
        }
        printf("scan: %d bad frames\n", bad);
    }
    const int iters = 600;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) { motion->Evaluate((float)i * 0.5f, inst); inst.UpdatePose(); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / iters;
    printf("evaluate+UpdatePose: %.3f ms/frame\n", ms);

    if (!pmx->rigidBodies.empty()) {
        // Play the whole motion at 60 fps with physics; flag non-finite matrices and bodies that
        // fly away (a simulated bone farther than 40 units from the center bone = an exploded rig).
        inst.EnablePhysics(true);
        inst.ResetPhysics();
        int center = pmx->FindBone("センター");
        if (center < 0) center = 0;
        int bad = 0;
        float worst = 0;
        std::vector<bool> simulated(pmx->bones.size(), false);
        for (const PmxRigidBody& r : pmx->rigidBodies)
            if (r.physicsMode != 0 && r.boneIndex >= 0 && r.boneIndex < (int)simulated.size()) simulated[r.boneIndex] = true;
        auto p0 = std::chrono::steady_clock::now();
        int steps = 0;
        for (float f = 0; f <= motion->EndFrame() && bad < 5; f += 0.5f, ++steps) {
            motion->Evaluate(f, inst);
            inst.UpdatePose(1.0f / 60.0f);
            const auto c = inst.BoneWorldPosition(center);
            for (size_t b = 0; b < pmx->bones.size(); ++b) {
                if (!simulated[b]) continue;
                const auto p = inst.BoneWorldPosition((int)b);
                const float d = std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) + (p.z - c.z) * (p.z - c.z));
                worst = std::isfinite(d) ? std::max(worst, d) : 1e30f;
                if (!std::isfinite(d) || d > 40.0f) {
                    printf("PHYSICS BAD frame %.1f bone %zu '%s' dist %.1f\n", f, b, pmx->bones[b].name.c_str(), d);
                    ++bad;
                    break;
                }
            }
        }
        const double pms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count() / std::max(steps, 1);
        printf("physics scan: %d bad frames, max simulated-bone distance from center %.1f, %.3f ms/frame (evaluate+pose+physics)\n",
               bad, worst, pms);
    }

    if (argc >= 4) {
        VmdMotion cam;
        if (!LoadVmd(argv[3], cam, &err)) { printf("camera: %s\n", err.c_str()); return 1; }
        auto cm = CameraMotion::Create(cam);
        if (!cm) { printf("camera: no keys\n"); return 1; }
        for (float f : {0.0f, 300.0f, 900.0f}) {
            CameraPose p = cm->Evaluate(f);
            DirectX::XMFLOAT3 eye;
            CameraMotion::ToView(p, nullptr, &eye);
            printf("cam %.0f: target(%.1f %.1f %.1f) rot(%.2f %.2f %.2f) dist %.1f fov %.0f eye(%.1f %.1f %.1f)\n", f,
                   p.target.x, p.target.y, p.target.z, p.rotation.x, p.rotation.y, p.rotation.z, p.distance, p.fovDeg,
                   eye.x, eye.y, eye.z);
        }
    }
    return 0;
}
