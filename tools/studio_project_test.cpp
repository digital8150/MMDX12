// Unit tests for the studio project file module (StudioProject.h). Imitates studio_pose_test.cpp:
// a tiny CHECK macro, prints PASS/FAIL per check and a summary, exit code 0 on success.
#include "studio/StudioProject.h"
#include <DirectXMath.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

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

static const std::filesystem::path kTemp = std::filesystem::temp_directory_path() / "mmdx_project_test";

static bool Vec3Eq(const XMFLOAT3& a, const XMFLOAT3& b, float tol) {
    return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol && std::abs(a.z - b.z) <= tol;
}

static bool QuatEq(const XMFLOAT4& a, const XMFLOAT4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// A non-linear VMD-style interpolation table (the same non-identity pattern on every channel).
static void SetTestInterp(uint8_t* interp, size_t block) {
    for (size_t i = 0; i < block; ++i) interp[i] = (uint8_t)(i * 7 + 3);
}

static void TestRoundTrip() {
    const std::filesystem::path projDir = kTemp / "proj";
    const std::filesystem::path projFile = projDir / L"\u30C6\u30B9\u30C8.mmdxproj";  // テスト.mmdxproj

    ProjectData data;
    // Key references kept at function scope for the post-load comparisons below.
    BoneKf b0, b30;
    CameraKf c0;
    const std::filesystem::path mikuPath = kTemp / "models" / "miku.pmx";  // the file need not exist
    const std::filesystem::path stagePath = kTemp / "models" / L"\u30B9\u30C6\u30FC\u30B8.pmx";  // ステージ.pmx

    // 1: character "初音ミク", bone / morph / IK tracks
    {
        ProjectModel m;
        m.name = "\xE5\x88\x9D\xE9\x9F\xB3\xE3\x83\x9F\xE3\x82\xAF";  // 初音ミク
        m.kind = ModelKind::Character;
        m.path = mikuPath;
        m.libraryId = "lib-1";
        m.place.translation = {2.5f, 0.0f, -3.0f};  // world placement of a character
        m.place.rotationDeg = {0.0f, 45.0f, 0.0f};
        m.place.scale = 1.25f;
        m.shader.pack = "test_pack";                                                   // shader pack choice
        m.shader.params["threshold"] = 0.5f;
        m.shader.textureFolder = "F:/tex/miku";                                        // per-character texture folder
        m.shader.SwitchPack("other_pack");                                             // remembered settings of a previous pack
        m.shader.params["gloss"] = 0.25f;
        m.shader.textureFolder = "F:/tex/other";
        m.shader.SwitchPack("test_pack");
        b0 = BoneKf{};
        b0.frame = 0;
        b0.t = {1, 2, 3};
        b0.r = {0.1f, 0.2f, 0.3f, 0.9f};
        SetTestInterp(b0.interp, 64);
        b30 = BoneKf{};
        b30.frame = 30;
        b30.t = {4, 5, 6};
        b30.r = {0.9f, 0.3f, 0.2f, 0.1f};
        SetTestInterp(b30.interp, 64);
        m.motion.bones["\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC"] = {b0, b30};  // センター
        m.motion.morphs["\xE3\x81\x82"] = {MorphKf{0, 0.0f}, MorphKf{30, 1.0f}};         // あ
        m.motion.ik["\xE5\x8F\xB3\xE8\x85\x95"] = {IkKf{0, true}, IkKf{30, false}};      // 右腕
        data.models.push_back(m);
    }
    // 2: stage, no keys, invisible, world placement
    {
        ProjectModel m;
        m.name = "\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xBC\xE3\x82\xB8";  // ステージ
        m.kind = ModelKind::Stage;
        m.path = stagePath;
        m.visible = false;
        m.place.translation = {0.0f, 0.0f, 5.0f};  // world placement of a stage
        m.place.rotationDeg = {0.0f, 180.0f, 0.0f};
        m.place.scale = 2.0f;
        data.models.push_back(m);
    }
    // 3: prop with attach and one bone key
    {
        ProjectModel m;
        m.name = "\xE3\x83\x9E\xE3\x82\xA4\xE3\x82\xAF";  // マイク
        m.kind = ModelKind::Prop;
        m.path = kTemp / "props" / "mic.pmx";
        m.attach.parent = 0;
        m.attach.bone = "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96";  // 右手首
        m.attach.translation = {1, 2, 3};
        m.attach.rotationDeg = {0, 90, 0};
        m.attach.scale = 1.5f;
        BoneKf b;
        b.frame = 10;
        b.t = {7, 8, 9};
        SetTestInterp(b.interp, 64);
        m.motion.bones["root"] = {b};
        data.models.push_back(m);
    }
    // camera: 2 camera keys, 1 light key, 1 shadow key
    {
        c0 = CameraKf{};
        c0.frame = 0;
        c0.distance = -30.0f;
        c0.target = {1, 2, 3};
        c0.rotation = {0.1f, 0.2f, 0.3f};
        SetTestInterp(c0.interp, 24);
        c0.fovDeg = 40;
        CameraKf c15;
        c15.frame = 15;
        c15.distance = -50.0f;
        c15.target = {4, 5, 6};
        c15.rotation = {0.4f, 0.1f, 0.6f};
        SetTestInterp(c15.interp, 24);
        c15.fovDeg = 50;
        data.camera.camera = {c0, c15};
        LightKf l;
        l.frame = 5;
        l.color = {0.7f, 0.8f, 0.9f};
        l.direction = {0.1f, -1.0f, 0.2f};
        data.camera.light = {l};
        ShadowKf s;
        s.frame = 8;
        s.mode = 2;
        s.distance = 0.01125f;
        data.camera.shadow = {s};
    }
    const std::filesystem::path audioBase = kTemp / "audio";
    data.audioPath = audioBase / L"\u66F2.wav";  // 曲.wav
    data.audioOffset = -0.5;
    data.audioOffset = -0.5;
    data.editor.frame = 120;
    data.editor.selectedModel = 2;
    data.editor.loop = true;
    data.editor.rangeStart = 10;
    data.editor.rangeEnd = 50;
    data.editor.pxPerFrame = 3.5f;
    data.editor.camTarget = {1, 2, 3};
    data.editor.camYaw = 0.5f;
    data.editor.camPitch = 0.2f;
    data.editor.camDistance = 30.0f;
    data.editor.camFovDeg = 40.0f;
    data.editor.useLightTrack = false;
    // the studio's lighting source + spot rig
    data.editor.lighting.source = LightSource::Custom;
    data.editor.lighting.presetIndex = 2;  // Concert
    data.editor.lighting.key.enabled = true;
    data.editor.lighting.key.direction = {0.2f, -0.9f, 0.4f};
    data.editor.lighting.key.color = {1.0f, 0.85f, 0.7f};
    data.editor.lighting.key.intensity = 1.2f;
    data.editor.lighting.key.rimStrength = 0.5f;
    {
        SpotLight s;
        s.name = "스팟 1";
        s.mode = 3;  // manual aim
        s.position = {10.0f, 40.0f, -12.0f};
        s.aim = {2.0f, 0.0f, 0.0f};
        s.color = {0.22f, 0.77f, 0.73f};
        s.intensity = 2.6f;
        s.coneOuter = 0.30f;
        s.enabled = true;
        s.swingPhase = 1.3f;
        s.keys = {SpotKf{0, {10.0f, 40.0f, -12.0f}, {2.0f, 0.0f, 0.0f}, 2.6f, 0.30f, {0.22f, 0.77f, 0.73f}},
                  SpotKf{30, {6.0f, 38.0f, -8.0f}, {-2.0f, 2.0f, 1.0f}, 3.2f, 0.24f, {0.95f, 0.35f, 0.62f}}};
        data.editor.lighting.spots.push_back(s);
        SpotLight s2;
        s2.name = "스팟 2";
        s2.mode = 1;  // follow the centre
        s2.position = {-8.0f, 40.0f, -12.0f};
        s2.aim = {-6.0f, 0.0f, 0.0f};
        s2.color = {0.95f, 0.35f, 0.62f};
        s2.intensity = 2.0f;
        s2.coneOuter = 0.24f;
        s2.enabled = false;  // disabled spot round-trips too
        data.editor.lighting.spots.push_back(s2);
    }
    data.editor.lighting.frontFill = true;

    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject round trip", "%s", err.c_str());

    // files written: character + prop VMDs, camera VMD; no VMD for the stage
    Check(std::filesystem::exists(projDir / L"\u30C6\u30B9\u30C8 - \u521D\u97F3\u30DF\u30AF.vmd"),
          "character VMD exists: \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 - \xE5\x88\x9D\xE9\x9F\xB3\xE3\x83\x9F\xE3\x82\xAF.vmd");  // テスト - 初音ミク.vmd
    Check(std::filesystem::exists(projDir / L"\u30C6\u30B9\u30C8 - \u30DE\u30A4\u30AF.vmd"),
          "prop VMD exists: \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 - \xE3\x83\x9E\xE3\x82\xA4\xE3\x82\xAF.vmd");  // テスト - マイク.vmd
    Check(std::filesystem::exists(projDir / L"\u30C6\u30B9\u30C8 - camera.vmd"),
          "camera VMD exists: \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 - camera.vmd");  // テスト - camera.vmd
    Check(!std::filesystem::exists(projDir / L"\u30C6\u30B9\u30C8 - \u30B9\u30C6\u30FC\u30B8.vmd"),
          "no VMD for the keyless stage");  // テスト - ステージ.vmd

    // the stored model path is relative to the project folder, pointing out of it: ../models/miku.pmx
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Check(text.find("\"../models/miku.pmx\"") != std::string::npos, "stored model path is \"../models/miku.pmx\"");
        Check(text.find("\"\\u521D\\u97F3\\u30DF\\u30AF\"") == std::string::npos,
              "JSON is UTF-8, not \\u escaped");
        Check(text.find("\"textureFolder\"") != std::string::npos, "stored shader textureFolder key");
    }

    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject round trip", "%s", err.c_str());
    Check(warnings.empty(), "LoadProject warnings empty");

    Check(loaded.models.size() == data.models.size(), "model count kept");
    if (loaded.models.size() == data.models.size()) {
        const ProjectModel& a = data.models[0];
        const ProjectModel& b = loaded.models[0];
        Check(b.name == a.name && b.kind == a.kind && b.libraryId == a.libraryId && b.visible == a.visible,
              "model 0 scalar fields kept");
        Check(b.path == std::filesystem::absolute(a.path).lexically_normal(), "model 0 path resolved absolute");
        Check(b.motion.bones.size() == 1 && b.motion.bones.count(a.motion.bones.begin()->first) == 1,
              "model 0 bone track kept");
        if (b.motion.bones.count("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC")) {
            const std::vector<BoneKf>& keys = b.motion.bones.at("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");
            Check(keys.size() == 2 && keys[0].frame == 0 && keys[1].frame == 30, "bone key frames kept");
            Check(Vec3Eq(keys[0].t, b0.t, 0) && Vec3Eq(keys[1].t, b30.t, 0), "bone key t kept");
            Check(std::memcmp(keys[0].interp, b0.interp, 64) == 0 && std::memcmp(keys[1].interp, b30.interp, 64) == 0,
                  "bone interp bytes kept");
            Check(keys[0].r.x == b0.r.x && keys[0].r.y == b0.r.y && keys[0].r.z == b0.r.z && keys[0].r.w == b0.r.w &&
                      QuatEq(keys[1].r, b30.r),
                  "bone quaternion components exact");
        }
        Check(b.motion.morphs.size() == 1 &&
                  b.motion.morphs.at("\xE3\x81\x82").size() == 2 &&
                  b.motion.morphs.at("\xE3\x81\x82")[1].weight == 1.0f,
              "morph track kept");
        Check(b.motion.ik.size() == 1 && b.motion.ik.at("\xE5\x8F\xB3\xE8\x85\x95").size() == 2 &&
                  b.motion.ik.at("\xE5\x8F\xB3\xE8\x85\x95")[1].enabled == false,
              "IK track kept");
        Check(b.shader == a.shader, "shader choice kept (pack, params, textureFolder)");

        Check(b.place == data.models[0].place, "character placement kept (t/r/s)");
        const ProjectModel& st = loaded.models[1];
        Check(st.name == data.models[1].name && st.kind == ModelKind::Stage && !st.visible &&
                  st.motion.Empty() && st.place == data.models[1].place,
              "stage kept: name, kind, visible=false, no keys, placement");

        const ProjectModel& p = loaded.models[2];
        Check(p.kind == ModelKind::Prop && p.attach == data.models[2].attach,
              "prop attach kept (parent/bone/t/r/s)");
        Check(p.motion.bones.size() == 1, "prop bone key kept");

        // camera from the "camera" VMD: only camera/light/shadow
        Check(loaded.camera.camera.size() == 2 && loaded.camera.light.size() == 1 && loaded.camera.shadow.size() == 1,
              "camera/light/shadow key counts kept");
        Check(loaded.camera.camera[0].frame == 0 && loaded.camera.camera[1].frame == 15 &&
                  loaded.camera.camera[0].distance == -30.0f && loaded.camera.camera[1].distance == -50.0f &&
                  loaded.camera.camera[0].fovDeg == 40 && loaded.camera.camera[1].fovDeg == 50,
              "camera key values kept");
        Check(std::memcmp(loaded.camera.camera[0].interp, c0.interp, 24) == 0, "camera interp bytes kept");
        Check(loaded.camera.light[0].color.x == 0.7f &&
                  std::abs(loaded.camera.light[0].direction.y - (-1.0f)) < 1e-6,
              "light key values kept");
        Check(loaded.camera.shadow[0].mode == 2 && loaded.camera.shadow[0].distance == 0.01125f,
              "shadow key values kept");
        Check(loaded.camera.modelName == "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E",
              "camera VMD model name is \xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E");  // カメラ・照明
    }

    Check(loaded.audioPath == std::filesystem::absolute(data.audioPath).lexically_normal(),
          "audio path resolved absolute");
    Check(loaded.audioOffset == -0.5, "audio offset kept");
    const ProjectEditor& ed = loaded.editor;
    Check(ed.frame == 120 && ed.selectedModel == 2 && ed.loop && ed.rangeStart == 10 && ed.rangeEnd == 50 &&
              ed.pxPerFrame == 3.5f && ed.camYaw == 0.5f && ed.camPitch == 0.2f && ed.camDistance == 30.0f &&
              ed.camFovDeg == 40.0f && !ed.useLightTrack && ed.useMotionCamera && ed.useShadowTrack &&
              ed.showCameraPath && ed.physics && Vec3Eq(ed.camTarget, {1, 2, 3}, 0),
          "editor state kept");
    // ---- the light rig (source, preset, key override, spots, front fill) ----
    Check(ed.lighting.source == LightSource::Custom && ed.lighting.presetIndex == 2 && ed.lighting.frontFill,
          "lighting source / preset / frontFill kept");
    Check(ed.lighting.key.enabled && Vec3Eq(ed.lighting.key.direction, {0.2f, -0.9f, 0.4f}, 0) &&
              Vec3Eq(ed.lighting.key.color, {1.0f, 0.85f, 0.7f}, 0) && ed.lighting.key.intensity == 1.2f &&
              ed.lighting.key.rimStrength == 0.5f,
          "key override kept");
    Check(ed.lighting.spots.size() == 2, "spot count kept", "size=%zu", ed.lighting.spots.size());
    if (ed.lighting.spots.size() == 2) {
        const SpotLight& a = data.editor.lighting.spots[0];
        const SpotLight& b = loaded.editor.lighting.spots[0];
        Check(b.name == a.name && b.mode == 3 && b.enabled && std::fabs(b.swingPhase - 1.3f) < 1e-6,
              "spot 0 name / mode / enabled / phase kept");
        Check(Vec3Eq(b.position, a.position, 0) && Vec3Eq(b.aim, a.aim, 0) && Vec3Eq(b.color, a.color, 0) &&
                  b.intensity == a.intensity && std::fabs(b.coneOuter - 0.30f) < 1e-6,
              "spot 0 values kept");
        Check(b.keys.size() == 2 && b.keys[0].frame == 0 && b.keys[1].frame == 30 &&
                  Vec3Eq(b.keys[1].position, a.keys[1].position, 0) && Vec3Eq(b.keys[1].aim, a.keys[1].aim, 0) &&
                  b.keys[1].intensity == 3.2f && std::fabs(b.keys[1].coneOuter - 0.24f) < 1e-6 &&
                  Vec3Eq(b.keys[1].color, a.keys[1].color, 0),
              "spot 0 keys kept");
        // keyed sample: halfway between the keys
        const SpotKf mid = SampleSpotKeys(b, 15);
        Check(std::fabs(mid.intensity - 2.9f) < 1e-4 && Vec3Eq(mid.position, {8.0f, 39.0f, -10.0f}, 1e-4),
              "spot keys interpolate linearly");
        const SpotLight& s2 = loaded.editor.lighting.spots[1];
        Check(s2.name == "스팟 2" && s2.mode == 1 && !s2.enabled && s2.keys.empty(),
              "spot 1 (disabled, keyless, mode 1) kept");
    }
    Check(data.editor.lighting == loaded.editor.lighting, "light rig round-trips exactly");
}

// A legacy v1 project without the "lighting" object: useLightTrack true + keys -> VmdTrack,
// useLightTrack true without a camera VMD light track -> Preset, false -> Preset.
static void TestLegacyLightMigration() {
    const std::filesystem::path projDir = kTemp / "legacy";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    std::string err;
    std::vector<std::string> warnings;

    // a minimal v1 project with an editor block; camera VMD referenced when it has light keys
    const auto writeV1 = [&](const char* editor) {
        std::ofstream f(projFile, std::ios::binary);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":1,\"models\":[],\"camera\":null,"
             "\"audio\":null,\"editor\":" << editor << "}";
    };

    // (a) useLightTrack: true but no camera VMD (no light keys) -> Preset (the brief's migration rule)
    {
        writeV1("{\"frame\":0,\"useLightTrack\":true}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (a): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lighting.source == LightSource::Preset && !loaded.editor.useLightTrack,
              "legacy true without light keys -> Preset");
    }
    // (b) useLightTrack: false -> Preset
    {
        writeV1("{\"frame\":0,\"useLightTrack\":false}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (b): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lighting.source == LightSource::Preset && !loaded.editor.useLightTrack,
              "legacy false -> Preset");
    }
    // (b) useLightTrack: true and a camera VMD that carries a light key -> VmdTrack
    {
        // a camera VMD with one non-default light key (SaveVmd -> the project references it)
        ProjectData data;
        LightKf l;
        l.frame = 0;
        l.color = {0.7f, 0.8f, 0.9f};
        l.direction = {0.1f, -1.0f, 0.2f};
        data.camera.light = {l};
        writeV1("{\"frame\":0,\"useLightTrack\":true}");
        // append the "camera" reference to the v1 JSON by rewriting it with the camera file present
        VmdMotion cam = data.camera.ToVmd();
        cam.modelName = "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E";
        SaveVmd(projDir / "scene - camera.vmd", cam);
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":1,\"models\":[],"
                 "\"camera\":\"scene - camera.vmd\",\"audio\":null,\"editor\":{\"frame\":0,\"useLightTrack\":true}}";
        }
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (b2): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lighting.source == LightSource::VmdTrack && loaded.editor.useLightTrack,
              "legacy true with light keys -> VmdTrack");
        Check(!loaded.camera.light.empty(), "legacy (b2): light track loaded");
    }
    // (c) an editor without any light field: the default VmdTrack stays
    {
        writeV1("{\"frame\":3}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (c): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lighting.source == LightSource::VmdTrack, "legacy plain project -> VmdTrack default");
    }
    // (d) a broken "lighting" source string keeps the default
    {
        writeV1("{\"frame\":0,\"lighting\":{\"source\":\"alien\",\"spots\":[]}}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (d): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lighting.source == LightSource::VmdTrack && loaded.editor.lighting.spots.empty(),
              "broken lighting source keeps the default");
    }
}

static void TestDuplicateNamesAndCleanup() {
    const std::filesystem::path projDir = kTemp / "dup";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    // Unrelated files that must survive every save.
    { std::ofstream keep(projDir / "keep.vmd"); }
    { std::ofstream other(projDir / L"scene-other.vmd"); }

    auto makeData = [](const char* secondName) {
        ProjectData data;
        for (int i = 0; i < 2; ++i) {
            ProjectModel m;
            m.name = i == 0 ? "A/B" : secondName;
            BoneKf b;
            b.frame = 0;
            b.t = {1, 0, 0};
            m.motion.bones["root"] = {b};
            data.models.push_back(m);
        }
        return data;
    };

    std::string err;
    ProjectData data = makeData("A/B");  // both sanitize to "A_B"
    Check(SaveProject(projFile, data, &err), "SaveProject duplicate names", "%s", err.c_str());
    Check(std::filesystem::exists(projDir / "scene - A_B.vmd"), "first A/B -> scene - A_B.vmd");
    Check(std::filesystem::exists(projDir / "scene - A_B (2).vmd"), "second A/B -> scene - A_B (2).vmd");

    // rename the second model to "C" and save again: the stale "(2)" file is cleaned up
    data = makeData("C");
    Check(SaveProject(projFile, data, &err), "SaveProject after rename", "%s", err.c_str());
    Check(std::filesystem::exists(projDir / "scene - A_B.vmd"), "scene - A_B.vmd still there");
    Check(std::filesystem::exists(projDir / "scene - C.vmd"), "renamed model -> scene - C.vmd");
    Check(!std::filesystem::exists(projDir / "scene - A_B (2).vmd"), "stale scene - A_B (2).vmd deleted");
    Check(std::filesystem::exists(projDir / "keep.vmd"), "unrelated keep.vmd survives");
    Check(std::filesystem::exists(projDir / L"scene-other.vmd"), "scene-other.vmd (no \" - \" separator) survives");
}

static void TestMissingAndBad() {
    const std::filesystem::path projDir = kTemp / "bad";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";

    // a project whose motion VMD is deleted after saving
    ProjectData data;
    ProjectModel m;
    m.name = "Miku";
    BoneKf b;
    m.motion.bones["root"] = {b};
    data.models.push_back(m);
    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject for missing-motion test", "%s", err.c_str());
    std::filesystem::remove(projDir / "scene - Miku.vmd");
    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with deleted motion -> true");
    Check(warnings.size() == 1, "exactly one warning", "size=%zu", warnings.size());
    if (!warnings.empty())
        Check(warnings[0].find("motion missing: ") == 0, "warning mentions the motion", "%s", warnings[0].c_str());
    Check(loaded.models.size() == 1 && loaded.models[0].motion.Empty(), "motion left empty");

    // not our format
    { std::ofstream f(projFile, std::ios::binary); f << "{\"format\":\"other\"}"; }
    Check(!LoadProject(projFile, loaded, &err, &warnings), "foreign format -> false");
    // newer version
    { std::ofstream f(projFile, std::ios::binary);
      f << "{\"format\":\"mmdx12-studio-project\",\"version\":99}"; }
    ProjectData loaded2;
    Check(!LoadProject(projFile, loaded2, &err, &warnings), "newer version -> false");
    Check(err.find("newer") != std::string::npos, "newer-version error mentions \"newer\"", "%s", err.c_str());
    // garbage
    { std::ofstream f(projFile, std::ios::binary); f << "this is { not json"; }
    Check(!LoadProject(projFile, loaded2, &err, &warnings), "garbage -> false");
    // missing file
    Check(!LoadProject(projDir / "nope.mmdxproj", loaded2, &err, &warnings), "missing file -> false");
    Check(err.find("cannot open") == 0, "missing-file error is \"cannot open\"", "%s", err.c_str());
}

static void TestUnknownKindRemap() {
    const std::filesystem::path projDir = kTemp / "remap";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    {
        std::ofstream f(projFile, std::ios::binary);
        // 4 entries: char (kept), x (unknown kind, skipped), prop (parent 0), prop2 (parent 1 = the skipped one)
        f << R"({
  "format": "mmdx12-studio-project",
  "version": 1,
  "models": [
    { "name": "char", "kind": "character", "path": "char.pmx" },
    { "name": "x", "kind": "alien", "path": "x.pmx" },
    { "name": "prop", "kind": "prop", "path": "p.pmx", "attach": { "parent": 0 } },
    { "name": "prop2", "kind": "prop", "path": "p2.pmx", "attach": { "parent": 1 } }
  ]
})";
    }
    ProjectData loaded;
    std::string err;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with unknown kind -> true", "%s", err.c_str());
    Check(loaded.models.size() == 3, "3 models loaded (unknown one skipped)", "size=%zu", loaded.models.size());
    Check(warnings.size() == 1, "one warning for the unknown kind", "size=%zu", warnings.size());
    if (loaded.models.size() == 3) {
        // after skipping the unknown kind: char -> 0, prop -> 1, prop2 -> 2
        Check(loaded.models[1].kind == ModelKind::Prop && loaded.models[1].attach.parent == 0,
              "prop parent 0 remapped to index 0");
        Check(loaded.models[2].kind == ModelKind::Prop && loaded.models[2].attach.parent == -1,
              "prop2 parent pointing at the skipped model becomes -1");
    }
}

static void TestAtomic() {
    const std::filesystem::path dir = kTemp / "atomic";
    std::filesystem::create_directories(dir);
    std::string err;
    const std::filesystem::path file = dir / "data.txt";
    Check(WriteFileAtomic(file, "one", &err), "WriteFileAtomic first write", "%s", err.c_str());
    Check(WriteFileAtomic(file, "two", &err), "WriteFileAtomic replaces an existing file", "%s", err.c_str());
    {
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Check(text == "two", "replaced file content", "%s", text.c_str());
    }
    Check(!std::filesystem::exists(dir / "data.txt.tmp"), "no .tmp left behind");
    Check(!std::filesystem::exists(file.native() + L".tmp"), "no .tmp left behind (wide check)");

    Check(SanitizeFileName("a:b") == "a_b", "SanitizeFileName a:b -> a_b");
    Check(SanitizeFileName("  .x. ") == "x", "SanitizeFileName \"  .x. \" -> x");
    Check(SanitizeFileName("") == "model", "SanitizeFileName \"\" -> model");
    Check(SanitizeFileName("\xE5\x88\x9D\xE9\x9F\xB3/\xE3\x83\x9F\xE3\x82\xAF") ==
              "\xE5\x88\x9D\xE9\x9F\xB3_\xE3\x83\x9F\xE3\x82\xAF",
          "SanitizeFileName \xE5\x88\x9D\xE9\x9F\xB3/\xE3\x83\x9F\xE3\x82\xAF -> \xE5\x88\x9D\xE9\x9F\xB3_\xE3\x83\x9F\xE3\x82\xAF");  // 初音/ミク -> 初音_ミク
}

static void TestPropOffsetMatrix() {
    // Expect: scale 2 * (1,0,0) = (2,0,0); XMMatrixRotationRollPitchYaw(0, 90deg, 0) is a row-vector
    // left-handed Y rotation with m[0][2] = -sin(90) = -1 (checked in DirectXMathMatrix.inl), so
    // (2,0,0) -> (0,0,-2); then + translation (1,2,3) = (1,2,1). The spec's expected (1,2,1) stands;
    // a right-handed rotor would give (0,0,+2) -> (1,2,5), which XMMatrixRotationRollPitchYaw is not.
    PropAttach a;
    a.translation = {1, 2, 3};
    a.rotationDeg = {0, 90, 0};
    a.scale = 2.0f;
    const XMMATRIX m = PropOffsetMatrix(a);
    const XMVECTOR p = XMVector3Transform(XMVectorSet(1.0f, 0.0f, 0.0f, 1.0f), m);
    XMFLOAT3 out;
    XMStoreFloat3(&out, p);
    Check(Vec3Eq(out, {1, 2, 1}, 1e-4f), "PropOffsetMatrix (1,0,0) -> (1,2,1)",
          "got (%g,%g,%g)", out.x, out.y, out.z);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash mid-test loses buffered PASS lines
    std::error_code ec;
    std::filesystem::remove_all(kTemp, ec);
    std::filesystem::create_directories(kTemp, ec);

    TestRoundTrip();
    TestLegacyLightMigration();
    TestDuplicateNamesAndCleanup();
    TestMissingAndBad();
    TestUnknownKindRemap();
    TestAtomic();
    TestPropOffsetMatrix();

    std::filesystem::remove_all(kTemp, ec);

    std::printf("studio_project_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
