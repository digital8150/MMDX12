#include "studio/StudioPose.h"
#include <DirectXMath.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
#include <set>

using namespace mmdx;
using namespace mmdx::studio;
using namespace DirectX;

static int g_passed = 0, g_failed = 0;

static void Check(bool ok, const char* name, const char* fmt = "", ...) {
    if (ok) {
        ++g_passed;
        std::printf("PASS %s\n", name);
        return;
    }
    ++g_failed;
    std::printf("FAIL %s: ", name);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static void TestMirrorBoneName() {
    Check(MirrorBoneName("\xE5\xB7\xA6\xE8\x85\x95") == "\xE5\x8F\xB3\xE8\x85\x95", "MirrorBoneName: \xE5\xB7\xA6\xE8\x85\x95 -> \xE5\x8F\xB3\xE8\x85\x95"); // 左腕 -> 右腕
    Check(MirrorBoneName("\xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB") == "\xE5\xB7\xA6\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB", "MirrorBoneName: \xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB -> \xE5\xB7\xA6\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB"); // 右足ＩＫ -> 左足ＩＫ
    Check(MirrorBoneName("\xE5\xB7\xA6\xE5\x8F\xB3") == "\xE5\x8F\xB3\xE5\xB7\xA6", "MirrorBoneName: \xE5\xB7\xA6\xE5\x8F\xB3 -> \xE5\x8F\xB3\xE5\xB7\xA6"); // 左右 -> 右左
    Check(MirrorBoneName("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC") == "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", "MirrorBoneName: \xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC unchanged"); // センター
    Check(MirrorBoneName("Arm_L") == "Arm_R", "MirrorBoneName: Arm_L -> Arm_R");
    Check(MirrorBoneName("hand.R") == "hand.L", "MirrorBoneName: hand.R -> hand.L");
    Check(MirrorBoneName("LeftFoot") == "RightFoot", "MirrorBoneName: LeftFoot -> RightFoot");
    Check(MirrorBoneName("mixamorig:RightHand") == "mixamorig:LeftHand", "MirrorBoneName: mixamorig:RightHand -> mixamorig:LeftHand");
    Check(MirrorBoneName("Hips") == "Hips", "MirrorBoneName: Hips unchanged");
    Check(MirrorBoneName("_L") == "_L", "MirrorBoneName: _L unchanged");
}

static void TestMirrorPoseDouble() {
    PoseBone p{{1,2,3}, {0,0,0,0}};
    XMVECTOR q = XMVector4Normalize(XMVectorSet(0.1f, 0.2f, 0.3f, 0.9f));
    XMStoreFloat4(&p.r, q);
    PoseBone m = MirrorPose(MirrorPose(p));
    Check(std::abs(m.t.x - p.t.x) < 1e-6 && std::abs(m.t.y - p.t.y) < 1e-6 && std::abs(m.t.z - p.t.z) < 1e-6 &&
          std::abs(m.r.x - p.r.x) < 1e-6 && std::abs(m.r.y - p.r.y) < 1e-6 && std::abs(m.r.z - p.r.z) < 1e-6 && std::abs(m.r.w - p.r.w) < 1e-6,
          "MirrorPose twice returns the original");
}

static void TestMirrorPoseReflection() {
    XMVECTOR v = XMVectorSet(0.3f, -0.7f, 0.5f, 0.0f);
    XMVECTOR q = XMVector4Normalize(XMVectorSet(0.2f, -0.4f, 0.1f, 0.8f));
    PoseBone pb{{}, {}};
    XMStoreFloat4(&pb.r, q);
    PoseBone mp = MirrorPose(pb);
    XMVECTOR mq = XMLoadFloat4(&mp.r);
    
    auto M = [](XMVECTOR a) {
        XMFLOAT4 f; XMStoreFloat4(&f, a);
        return XMVectorSet(-f.x, f.y, f.z, f.w);
    };
    
    XMVECTOR M_v = M(v);
    XMVECTOR rot_Mv = XMVector3Rotate(M_v, mq);
    XMVECTOR rot_v = XMVector3Rotate(v, q);
    XMVECTOR M_rot_v = M(rot_v);
    
    XMFLOAT4 a, b;
    XMStoreFloat4(&a, rot_Mv);
    XMStoreFloat4(&b, M_rot_v);
    
    bool ok = std::abs(a.x - b.x) < 1e-5 && std::abs(a.y - b.y) < 1e-5 && std::abs(a.z - b.z) < 1e-5;
    Check(ok, "MirrorPose is a true reflection");
}

static void TestMirrorPoseInto() {
    PmxModel model;
    model.bones.resize(4);
    model.bones[0].name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";
    model.bones[0].flags = PmxBone_Rotatable | PmxBone_Movable;
    model.bones[1].name = "\xE5\xB7\xA6\xE8\x85\x95";
    model.bones[1].flags = PmxBone_Rotatable;
    model.bones[2].name = "\xE5\x8F\xB3\xE8\x85\x95";
    model.bones[2].flags = PmxBone_Rotatable;
    model.bones[3].name = "\xE9\xA0\xAD";
    model.bones[3].flags = PmxBone_Rotatable;
    
    std::vector<PoseBone> current(4, PoseBone{{0,0,0},{0,0,0,1}});
    XMVECTOR r1 = XMQuaternionRotationRollPitchYaw(0.1f, 0.4f, 0.3f);
    XMStoreFloat4(&current[1].r, r1);
    current[0].t = {2, 0, 1};
    
    {
        PoseLayer out;
        int written = MirrorPoseInto(model, current, nullptr, out);
        Check(written == 3, "Whole-model MirrorPoseInto written == 3");
        PoseBone m1 = MirrorPose(current[1]);
        Check(out.bones[2].r.x == m1.r.x && out.bones[2].r.y == m1.r.y && out.bones[2].r.z == m1.r.z && out.bones[2].r.w == m1.r.w, "Whole-model MirrorPoseInto out.bones[2].r == MirrorPose(current[1]).r");
        Check(out.bones[1].r.x == 0 && out.bones[1].r.y == 0 && out.bones[1].r.z == 0 && out.bones[1].r.w == 1, "Whole-model MirrorPoseInto out.bones[1].r == identity");
        Check(out.bones[0].t.x == -2 && out.bones[0].t.y == 0 && out.bones[0].t.z == 1, "Whole-model MirrorPoseInto out.bones[0].t == (-2, 0, 1)");
    }
    
    {
        PoseLayer out;
        std::set<int> onlyBones = {1};
        int written = MirrorPoseInto(model, current, &onlyBones, out);
        Check(written == 1, "onlyBones = {1} written == 1");
        Check(out.bones.size() == 1 && out.bones.count(2), "onlyBones = {1} only out.bones[2] set");
    }
    
    {
        model.bones[0].flags = PmxBone_Rotatable;
        PoseLayer out;
        std::set<int> onlyBones = {0};
        int written = MirrorPoseInto(model, current, &onlyBones, out);
        Check(written == 0, "Bone 0 not Movable MirrorPoseInto writes nothing");
        model.bones[0].flags = PmxBone_Rotatable | PmxBone_Movable; // restore
    }
    
    {
        PoseLayer out;
        out.bones[3] = {{9,9,9},{0,0,0,1}};
        std::set<int> onlyBones = {1};
        MirrorPoseInto(model, current, &onlyBones, out);
        Check(out.bones.count(3) && out.bones[3].t.x == 9, "Existing unrelated entries in out are kept");
    }
}

static void TestVpd() {
    PmxModel model;
    model.name = "TestModel";
    model.bones.resize(4);
    model.bones[0].name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";
    model.bones[0].flags = PmxBone_Rotatable | PmxBone_Movable;
    model.bones[1].name = "\xE5\xB7\xA6\xE8\x85\x95";
    model.bones[1].flags = PmxBone_Rotatable;
    model.bones[2].name = "\xE5\x8F\xB3\xE8\x85\x95";
    model.bones[2].flags = PmxBone_Rotatable;
    model.bones[3].name = "\xE9\xA0\xAD";
    model.bones[3].flags = PmxBone_Rotatable;
    
    model.morphs.resize(2);
    model.morphs[0].name = "a";
    model.morphs[1].name = "b";
    
    std::vector<PoseBone> current(4, PoseBone{{0,0,0},{0,0,0,1}});
    XMVECTOR r1 = XMQuaternionRotationRollPitchYaw(0.1f, 0.4f, 0.3f);
    XMStoreFloat4(&current[1].r, r1);
    current[0].t = {2, 0, 1};
    
    std::vector<float> morphWeights = {0.0f, 0.5f};
    
    VpdPose vpdAll = MakeVpdPose(model, current, morphWeights, nullptr);
    Check(vpdAll.bones.size() == 2 && vpdAll.bones[0].name == model.bones[0].name && vpdAll.bones[1].name == model.bones[1].name, "MakeVpdPose whole model bones non-identity listed");
    Check(vpdAll.morphs.size() == 1 && vpdAll.morphs[0].name == "b" && vpdAll.morphs[0].weight == 0.5f, "MakeVpdPose whole model morphs listed");
    
    std::set<int> ob = {3};
    VpdPose vpdPart = MakeVpdPose(model, current, morphWeights, &ob);
    Check(vpdPart.bones.size() == 1 && vpdPart.bones[0].name == model.bones[3].name, "MakeVpdPose onlyBones exactly bone 3 listed");
    Check(vpdPart.morphs.empty(), "MakeVpdPose onlyBones no morphs");
    
    auto tempPath = std::filesystem::temp_directory_path() / "studio_pose_test.vpd";
    std::string err;
    bool saved = SaveVpd(tempPath, vpdAll, &err);
    Check(saved, "SaveVpd");
    
    VpdPose loaded;
    bool loadedOk = LoadVpd(tempPath, loaded, &err);
    Check(loadedOk, "LoadVpd");
    
    PoseLayer out;
    int applied = ApplyVpdPose(model, loaded, nullptr, out, nullptr);
    Check(applied == 3, "ApplyVpdPose count == 3"); // 2 bones + 1 morph
    Check(out.bones.count(0) && std::abs(out.bones[0].t.x - 2) < 1e-5, "ApplyVpdPose bone 0 value ok");
    Check(out.bones.count(1) && std::abs(out.bones[1].r.x - current[1].r.x) < 1e-5, "ApplyVpdPose bone 1 value ok");
    Check(out.morphs.count(1) && out.morphs[1] == 0.5f, "ApplyVpdPose morph b == 0.5");
    
    std::filesystem::remove(tempPath);
}

static void TestApplyMissing() {
    PmxModel model;
    model.bones.resize(4);
    model.bones[0].name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";
    model.bones[1].name = "\xE5\xB7\xA6\xE8\x85\x95";
    model.bones[2].name = "\xE5\x8F\xB3\xE8\x85\x95";
    model.bones[3].name = "\xE9\xA0\xAD";
    
    VpdPose vpd;
    vpd.bones.push_back({"nonexistent", {0,0,0}, {0,0,0,1}});
    vpd.morphs.push_back({"zzz", 1.0f});
    vpd.bones.push_back({"\xE5\x8F\xB3\xE8\x85\x95", {0,0,0}, {0,0,0,1}});
    
    PoseLayer out;
    std::vector<std::string> missing;
    int count = ApplyVpdPose(model, vpd, nullptr, out, &missing);
    Check(count == 1, "ApplyVpdPose missing count excludes them");
    Check(missing.size() == 2 && missing[0] == "nonexistent" && missing[1] == "zzz", "ApplyVpdPose missing reported");
    
    missing.clear();
    out.bones.clear();
    std::set<int> ob = {1};
    count = ApplyVpdPose(model, vpd, &ob, out, &missing);
    Check(count == 0, "ApplyVpdPose onlyBones skips RightArm");
    Check(missing.size() == 1 && missing[0] == "nonexistent", "ApplyVpdPose onlyBones morphs ignored");
}

static void TestSamePoseLayer() {
    PoseLayer a, b;
    a.frame = 10; b.frame = 10;
    a.bones[1] = {{1,2,3}, {0,0,0,1}};
    b.bones[1] = {{1,2,3}, {0,0,0,1}};
    a.morphs[2] = 0.5f;
    b.morphs[2] = 0.5f;
    
    Check(SamePoseLayer(a, b), "SamePoseLayer equal layers true");
    b.frame = 11;
    Check(!SamePoseLayer(a, b), "SamePoseLayer changing frame false");
    b.frame = 10;
    b.bones[1].t.x = 2;
    Check(!SamePoseLayer(a, b), "SamePoseLayer changing one value false");
    b.bones[1].t.x = 1;
    b.morphs[3] = 0.1f;
    Check(!SamePoseLayer(a, b), "SamePoseLayer adding a morph false");
}

int main() {
    TestMirrorBoneName();
    TestMirrorPoseDouble();
    TestMirrorPoseReflection();
    TestMirrorPoseInto();
    TestVpd();
    TestApplyMissing();
    TestSamePoseLayer();
    
    std::printf("studio_pose_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
