// bake_test [model.pmx] [motion.vmd] [frames]
// Numeric check for the physics bake (P2.2): simulate a character's physics live over N frames, bake the
// same range into bone keys, re-evaluate the merged motion with physics off and report the max bone
// position error (target < 0.05 units). Once with MMD's default gravity and once with a project's own gravity
// and wind (BakeOptions::env), which must also move the simulated bones.
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "studio/PhysicsBake.h"
#include "studio/StudioMotion.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mmdx;
using namespace mmdx::studio;

namespace {

float Distance(const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& q) {
    const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// One live simulation + bake + baked replay under `env`. `live` receives every driven bone's live world position per
// frame. Returns the max baked-vs-live position error, or a negative value on a failure (printed).
float BakeError(const std::shared_ptr<PmxModel>& pmx, const BoundMotion& motion, const VmdMotion& vmd,
                const std::vector<int>& driven, int end, const PhysicsEnv& env, const char* label,
                std::vector<std::vector<DirectX::XMFLOAT3>>& live) {
    const int frames = end + 1;
    // 1. Live: simulate with physics on and record every driven bone's world position (the studio's order:
    //    enable, gravity / wind, reset).
    live.assign(frames, {});
    {
        ModelInstance inst(pmx);
        inst.EnablePhysics(true);
        inst.SetGravity(env.gravity, env.gravityDir);
        inst.SetWind(env.windDir, env.windStrength, env.windGust, env.windGustFreq, env.windJitter);
        inst.ResetPhysics();
        for (int f = 0; f <= end; ++f) {
            motion.Evaluate((float)f, inst);
            inst.UpdatePose(1.0f / kMmdFps);
            live[f].resize(driven.size());
            for (size_t b = 0; b < driven.size(); ++b) live[f][b] = inst.BoneWorldPosition(driven[b]);
        }
    }

    // 2. Bake the same range (physics on, its own instance + world, the same environment).
    BakeOptions o;
    o.start = 0; o.end = end; o.step = 1;
    o.env = env;
    BakeResult baked;
    const DirectX::XMFLOAT4X4 root{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!BakePhysics(pmx, motion, root, o, baked, nullptr, nullptr)) { std::printf("%s: bake cancelled\n", label); return -1; }
    std::printf("%s: baked %d bone tracks\n", label, (int)baked.bones.size());
    if (baked.bones.empty()) { std::printf("%s: bake produced no keys\n", label); return -1; }

    // 3. Merge: base motion + baked keys (replace the driven bones' keys inside [0, end]), physics off.
    MotionData data = MotionData::FromVmd(vmd);
    data.CanonicalizeNames(*pmx);
    for (const auto& [name, keys] : baked.bones) {
        auto& track = data.bones[name];
        track.erase(std::remove_if(track.begin(), track.end(),
                                   [end](const BoneKf& k) { return k.frame >= 0 && k.frame <= end; }),
                    track.end());
        for (const BoneKf& k : keys) UpsertKey(track, k);
    }
    VmdMotion merged = data.ToVmd();
    auto mergedMotion = BoundMotion::Bind(*pmx, {&merged});

    ModelInstance inst(pmx);
    inst.EnablePhysics(false);  // baked keys replace physics
    float worst = 0.0f;
    int worstFrame = -1, worstBone = -1;
    for (int f = 0; f <= end; ++f) {
        mergedMotion->Evaluate((float)f, inst);
        inst.UpdatePose(0.0f);
        for (size_t b = 0; b < driven.size(); ++b) {
            const float d = Distance(inst.BoneWorldPosition(driven[b]), live[f][b]);
            if (d > worst) { worst = d; worstFrame = f; worstBone = driven[b]; }
        }
    }
    std::printf("%s: max bone position error: %.5f units (frame %d bone %d)\n", label, worst, worstFrame, worstBone);
    return worst;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::filesystem::path modelPath = argc >= 2 ? argv[1]
        : L"library\\model\\Sekai Model Pack v4\\LeoNeed\\Ichika\\01_9001.pmx";
    std::filesystem::path vmdPath = argc >= 3 ? argv[2] : L"library\\motion\\Evergreen\\ch0208_so2009.vmd";
    const int frames = argc >= 4 ? _wtoi(argv[3]) : 60;
    const int end = frames - 1;

    auto pmx = std::make_shared<PmxModel>();
    std::string err;
    if (!LoadModelFile(modelPath, ModelRole::Character, *pmx, &err)) { std::printf("pmx: %s\n", err.c_str()); return 1; }
    VmdMotion vmd;
    if (!LoadVmd(vmdPath, vmd, &err)) { std::printf("vmd: %s\n", err.c_str()); return 1; }
    auto motion = BoundMotion::Bind(*pmx, {&vmd});

    const std::vector<int> driven = PhysicsDrivenBones(*pmx);
    std::printf("model '%s' bones=%zu | %d physics-driven | frames 0..%d\n", pmx->name.c_str(), pmx->bones.size(),
                (int)driven.size(), end);
    if (driven.empty()) { std::printf("no physics-driven bones: nothing to bake\n"); return 1; }

    std::vector<std::vector<DirectX::XMFLOAT3>> liveDefault, liveEnv;
    const float defaultError = BakeError(pmx, *motion, vmd, driven, end, PhysicsEnv{}, "default gravity", liveDefault);

    // A project's own physics environment: weak, tilted gravity and a gusty wind from the front.
    PhysicsEnv env;
    env.gravity = 4.0f; env.gravityDir = {0.6f, -1.0f, 0.0f};
    env.windDir = {0, 0, -1}; env.windStrength = 0.5f; env.windGust = 0.3f; env.windGustFreq = 1.5f; env.windJitter = 0.2f;
    const float envError = BakeError(pmx, *motion, vmd, driven, end, env, "project gravity + wind", liveEnv);

    // the environment must actually reach the simulation (else both passes would agree trivially)
    float moved = 0.0f;
    for (int f = 0; f <= end; ++f)
        for (size_t b = 0; b < driven.size(); ++b) moved = std::max(moved, Distance(liveDefault[f][b], liveEnv[f][b]));
    std::printf("gravity / wind moved the simulated bones by up to %.3f units (target > 0.5)\n", moved);

    const bool ok = defaultError >= 0.0f && defaultError < 0.05f && envError >= 0.0f && envError < 0.05f && moved > 0.5f;
    std::printf("%s (target < 0.05)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
