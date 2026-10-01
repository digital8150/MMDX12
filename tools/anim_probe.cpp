// anim_probe <model.pmx> <motion.vmd> [camera.vmd]
// Binds a motion, evaluates a few frames and checks IK convergence (effector vs IK goal distance).
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
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
    if (!LoadPmx(argv[1], *pmx, &err)) { printf("pmx: %s\n", err.c_str()); return 1; }
    VmdMotion vmd;
    if (!LoadVmd(argv[2], vmd, &err)) { printf("vmd: %s\n", err.c_str()); return 1; }
    auto motion = BoundMotion::Bind(*pmx, {&vmd});
    printf("model '%s' bones=%zu morphs=%zu | motion keys=%zu bound tracks bones=%d morphs=%d end=%.0f\n",
           pmx->name.c_str(), pmx->bones.size(), pmx->morphs.size(), vmd.boneKeys.size(),
           motion->BoundBoneCount(), motion->BoundMorphCount(), motion->EndFrame());

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
