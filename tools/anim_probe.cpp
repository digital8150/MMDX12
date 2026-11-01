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
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace mmdx;

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { printf("usage: anim_probe <model.pmx> <motion.vmd> [camera.vmd]\n"); return 2; }
    auto pmx = std::make_shared<PmxModel>();
    std::string err;
    if (!LoadModelFile(argv[1], ModelRole::Character, *pmx, &err)) { printf("pmx: %s\n", err.c_str()); return 1; }
    VmdMotion vmd;
    if (!LoadVmd(argv[2], vmd, &err)) { printf("vmd: %s\n", err.c_str()); return 1; }
    VmdMotion explicitIk;
    if (getenv("IK_PROBE_EXPLICIT")) {
        VmdIkKey key;
        for (const auto& bone : pmx->bones)
            if (bone.flags & PmxBone_IK) key.ikStates.emplace_back(bone.name, true);
        explicitIk.ikKeys.push_back(std::move(key));
    }
    auto motion = BoundMotion::Bind(*pmx, {&vmd, &explicitIk});
    printf("model '%s' bones=%zu morphs=%zu | motion keys=%zu bound tracks bones=%d morphs=%d end=%.0f\n",
           pmx->name.c_str(), pmx->bones.size(), pmx->morphs.size(), vmd.boneKeys.size(),
           motion->BoundBoneCount(), motion->BoundMorphCount(), motion->EndFrame());

    if (getenv("IK_PROBE")) {
        for (float frame : {0.0f, 300.0f, 900.0f, 1800.0f}) {
            ModelInstance animated(pmx), fk(pmx);
            motion->Evaluate(frame, animated);
            motion->Evaluate(frame, fk);
            for (size_t i = 0; i < pmx->bones.size(); ++i)
                if (pmx->bones[i].flags & PmxBone_IK) fk.SetIkEnabled((int)i, false);
            animated.UpdatePose();
            fk.UpdatePose();
            for (const char* name : {"\u5de6\u8db3\u9996", "\u53f3\u8db3\u9996"}) {
                const int bone = pmx->FindBone(name);
                if (bone < 0) continue;
                const auto a = animated.BoneWorldPosition(bone);
                const auto b = fk.BoneWorldPosition(bone);
                const float d = std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z));
                printf("IK_PROBE frame=%.0f %s animated=(%.3f %.3f %.3f) fk=(%.3f %.3f %.3f) delta=%.4f\n",
                       frame, name, a.x, a.y, a.z, b.x, b.y, b.z, d);
            }
        }
        return 0;
    }

    if (const char* swingEnv = getenv("PHYS_SWING"); swingEnv && *swingEnv) {
        // PHYS_SWING=<seconds>: simulate from frame 0 at 60 fps with physics, then report how far
        // the dynamic-body bones' LOCAL rotations move. Exits after printing.
        using namespace DirectX;
        const float seconds = (float)atof(swingEnv);
        if (seconds <= 0) { printf("PHYS_SWING: bad seconds '%s'\n", swingEnv); return 2; }

        // dynamic bodies (mode 1 or 2, driving a bone), chain grouping by body name
        struct Dyn { int bone; int mode; std::string chain; };
        std::vector<Dyn> dyn;
        auto chainOf = [](const std::string& n) {
            // strip trailing digits / '-' / '_' (e.g. 右袖1-3 -> 右袖, HairL4 -> HairL)
            std::string c = n;
            while (!c.empty()) {
                const char ch = c.back();
                const bool digit = ch >= '0' && ch <= '9';
                const bool sep = ch == '-' || ch == '_';
                // multibyte UTF-8 tails are never ASCII digits/separators, so this is safe
                if (digit || sep) { c.pop_back(); continue; }
                break;
            }
            return c.empty() ? n : c;
        };
        for (const PmxRigidBody& r : pmx->rigidBodies)
            if (r.physicsMode != 0 && r.boneIndex >= 0 && r.boneIndex < (int)pmx->bones.size())
                dyn.push_back({r.boneIndex, r.physicsMode, chainOf(r.name)});

        // rest-pose local rotations (animated pose at frame 0 before physics, as the baseline)
        ModelInstance inst(pmx);
        inst.EnablePhysics(true);
        motion->Evaluate(0.0f, inst);
        inst.ResetPhysics();
        inst.UpdatePose(0.0f);
        std::vector<XMFLOAT4> restLocal(dyn.size());
        auto localRot = [&](int bone) {
            const XMFLOAT4X4& w = inst.BoneWorld(bone);
            const int p = pmx->bones[bone].parentIndex;
            XMFLOAT4X4 l;
            if (p >= 0 && p < (int)pmx->bones.size())
                XMStoreFloat4x4(&l, XMMatrixMultiply(XMLoadFloat4x4(&w),
                                                     XMMatrixInverse(nullptr, XMLoadFloat4x4(&inst.BoneWorld(p)))));
            else
                XMStoreFloat4x4(&l, XMMatrixMultiply(XMLoadFloat4x4(&w), XMMatrixIdentity()));
            XMVECTOR s, rot, tr;
            XMMatrixDecompose(&s, &rot, &tr, XMLoadFloat4x4(&l));
            XMFLOAT4 q; XMStoreFloat4(&q, rot);
            return q;
        };
        auto angleBetween = [](const XMFLOAT4& a, const XMFLOAT4& b) {
            const float d = std::min(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w), 1.0f);
            return 2.0f * acosf(d);
        };
        for (size_t i = 0; i < dyn.size(); ++i) restLocal[i] = localRot(dyn[i].bone);

        // chain accumulators
        struct Acc {
            int bones = 0; int modeMask = 0; double speedSum = 0; long long samples = 0; double maxRange = 0;
            double travelSum = 0; long long travelSamples = 0;
        };
        std::map<std::string, Acc> chains;
        for (const Dyn& d : dyn) chains[d.chain].bones++;
        for (auto& [name, a] : chains)
            for (const Dyn& d : dyn) if (d.chain == name) a.modeMask |= 1 << d.mode;
        double totalSpeed = 0, totalRange = 0, totalTravel = 0;
        long long totalSamples = 0;
        int nanFrames = 0;

        // A second instance with physics disabled, driven by the same motion: the baseline the
        // position metric is measured against (see EnablePhysics in ModelInstance.h).
        ModelInstance plain(pmx);
        plain.EnablePhysics(false);
        motion->Evaluate(0.0f, plain);
        plain.UpdatePose(0.0f);

        const int frames = (int)(seconds * 60.0f);
        std::vector<XMFLOAT4> prevLocal(dyn.size());
        for (int f = 0; f < frames; ++f) {
            const float t = (float)f / 60.0f;
            motion->Evaluate(t, inst);
            inst.UpdatePose(1.0f / 60.0f);
            motion->Evaluate(t, plain);
            plain.UpdatePose(0.0f);
            for (size_t i = 0; i < dyn.size(); ++i) {
                const XMFLOAT4 q = localRot(dyn[i].bone);
                const float ang = angleBetween(q, restLocal[i]);
                Acc& a = chains[dyn[i].chain];
                if (std::isfinite(ang)) {
                    // per-frame local rotation change vs the previous frame
                    XMFLOAT4 prev = prevLocal[i].x || prevLocal[i].y || prevLocal[i].z || prevLocal[i].w ? prevLocal[i] : q;
                    const float step = angleBetween(q, prev);
                    a.speedSum += step * 60.0;  // rad/frame * 60 frames/s = rad/s, printed in deg
                    a.samples++;
                    a.maxRange = std::max(a.maxRange, (double)ang);
                    prevLocal[i] = q;
                    totalSpeed += step * 60.0;
                    totalSamples++;
                    totalRange = std::max(totalRange, (double)ang);
                } else {
                    ++nanFrames;
                }
                // travel: distance between the bone's world position with physics and without
                const XMFLOAT3 wp = inst.BoneWorldPosition(dyn[i].bone);
                const XMFLOAT3 wa = plain.BoneWorldPosition(dyn[i].bone);
                const float d = std::sqrt((wp.x - wa.x) * (wp.x - wa.x) + (wp.y - wa.y) * (wp.y - wa.y) +
                                          (wp.z - wa.z) * (wp.z - wa.z));
                if (std::isfinite(d)) {
                    a.travelSum += d;
                    a.travelSamples++;
                    totalTravel += d;
                }
            }
        }
        (void)nanFrames;  // non-finite samples are skipped and counted, but not currently reported

        // print per-chain lines
        for (auto& [name, a] : chains) {
            const double meanSpeedDeg = a.samples ? a.speedSum / a.samples * 180.0 / 3.14159265358979 : 0.0;
            const double rangeDeg = a.maxRange * 180.0 / 3.14159265358979;
            const double travel = a.travelSamples ? a.travelSum / a.travelSamples : 0.0;
            const char* mode = a.modeMask == 2 ? "1" : a.modeMask == 4 ? "2" : "mix";
            printf("SWING %s bones=%d mode=%s meanSpeed=%.2f range=%.2f travel=%.3f\n", name.c_str(), a.bones, mode,
                   meanSpeedDeg, rangeDeg, travel);
        }
        const double totalSpeedDeg = totalSamples ? totalSpeed / totalSamples * 180.0 / 3.14159265358979 : 0.0;
        const double totalRangeDeg = totalRange * 180.0 / 3.14159265358979;
        const double totalTravelAvg = totalSamples ? totalTravel / totalSamples : 0.0;
        printf("SWING_TOTAL meanSpeed=%.2f range=%.2f travel=%.3f\n", totalSpeedDeg, totalRangeDeg, totalTravelAvg);

        // mode histogram of dynamic bodies
        int modeCount[3] = {0, 0, 0};
        for (const Dyn& d : dyn) if (d.mode >= 0 && d.mode <= 2) ++modeCount[d.mode];
        printf("MODES mode0=%d mode1=%d mode2=%d\n", modeCount[0], modeCount[1], modeCount[2]);

        // angular-damping histogram (<0.5, <0.9, <0.99, <0.999, <1, ==1)
        int dampBuckets[6] = {0, 0, 0, 0, 0, 0};
        for (const PmxRigidBody& r : pmx->rigidBodies) {
            if (r.physicsMode == 0) continue;
            const float d = r.angularDamping;
            int b = d < 0.5f ? 0 : d < 0.9f ? 1 : d < 0.99f ? 2 : d < 0.999f ? 3 : d < 1.0f ? 4 : 5;
            ++dampBuckets[b];
        }
        printf("DAMP <0.5=%d <0.9=%d <0.99=%d <0.999=%d <1=%d ==1=%d\n", dampBuckets[0], dampBuckets[1],
               dampBuckets[2], dampBuckets[3], dampBuckets[4], dampBuckets[5]);
        return 0;
    }

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
