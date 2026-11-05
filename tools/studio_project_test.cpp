// Unit tests for the studio project file module (StudioProject.h). Imitates studio_pose_test.cpp:
// a tiny CHECK macro, prints PASS/FAIL per check and a summary, exit code 0 on success.
#include "studio/StudioProject.h"
#include "anim/Motion.h"
#include "core/TextUtil.h"
#include <DirectXMath.h>
#include <json.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

static bool Near(float a, float b, float tol = 1e-4f) { return std::abs(a - b) <= tol; }

static bool QuatEq(const XMFLOAT4& a, const XMFLOAT4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// A non-linear VMD-style interpolation table (the same non-identity pattern on every channel).
static void SetTestInterp(uint8_t* interp, size_t block) {
    for (size_t i = 0; i < block; ++i) interp[i] = (uint8_t)(i * 7 + 3);
}

// ---- scene lights ----
static bool SameVec3(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// The first member in which two sets of keyable values differ ("" when they are equal), compared one by one.
static std::string DiffValues(const LightValues& a, const LightValues& b) {
    if (!SameVec3(a.position, b.position)) return "position";
    if (!SameVec3(a.aim, b.aim)) return "aim";
    if (!SameVec3(a.direction, b.direction)) return "direction";
    if (!SameVec3(a.color, b.color)) return "color";
    if (a.intensity != b.intensity) return "intensity";
    if (a.range != b.range) return "range";
    if (a.coneOuter != b.coneOuter) return "coneOuter";
    if (a.coneInner != b.coneInner) return "coneInner";
    if (a.size.x != b.size.x || a.size.y != b.size.y) return "size";
    if (!SameVec3(a.extent, b.extent)) return "extent";
    if (a.volumetric != b.volumetric) return "volumetric";
    return "";
}

// The first member in which two lights differ ("" when they are equal), compared one by one (not through operator==).
static std::string DiffLight(const SceneLight& a, const SceneLight& b) {
    if (a.uid != b.uid) return "uid";
    if (a.name != b.name) return "name";
    if (a.kind != b.kind) return "kind";
    if (a.enabled != b.enabled) return "enabled";
    if (const std::string d = DiffValues(a.v, b.v); !d.empty()) return "v." + d;
    if (a.keys.size() != b.keys.size()) return "keys.size";
    for (size_t i = 0; i < a.keys.size(); ++i) {
        const std::string at = "keys[" + std::to_string(i) + "].";
        if (a.keys[i].frame != b.keys[i].frame) return at + "frame";
        if (const std::string d = DiffValues(a.keys[i].v, b.keys[i].v); !d.empty()) return at + d;
    }
    if (a.vmdLink != b.vmdLink) return "vmdLink";
    if (a.rimStrength != b.rimStrength) return "rimStrength";
    if (!SameVec3(a.rimColor, b.rimColor)) return "rimColor";
    if (a.aimMode != b.aimMode) return "aimMode";
    if (a.targetUid != b.targetUid) return "targetUid";
    if (a.targetPart != b.targetPart) return "targetPart";
    if (a.swayPhase != b.swayPhase) return "swayPhase";
    if (!SameVec3(a.skyZenith, b.skyZenith)) return "skyZenith";
    if (!SameVec3(a.skyHorizon, b.skyHorizon)) return "skyHorizon";
    if (!SameVec3(a.groundColor, b.groundColor)) return "groundColor";
    if (a.shadow != b.shadow) return "shadow";
    if (a.shadowSoftness != b.shadowSoftness) return "shadowSoftness";
    if (a.shadowDensity != b.shadowDensity) return "shadowDensity";
    if (!SameVec3(a.shadowColor, b.shadowColor)) return "shadowColor";
    if (a.falloff != b.falloff) return "falloff";
    if (a.affectDiffuse != b.affectDiffuse) return "affectDiffuse";
    if (a.affectSpecular != b.affectSpecular) return "affectSpecular";
    if (a.viewportVisible != b.viewportVisible) return "viewportVisible";
    if (a.volumetricG != b.volumetricG) return "volumetricG";
    if (a.fogShape != b.fogShape) return "fogShape";
    if (a.fogEdge != b.fogEdge) return "fogEdge";
    if (a.gobo != b.gobo) return "gobo";
    if (a.goboRotation != b.goboRotation) return "goboRotation";
    if (a.goboScale != b.goboScale) return "goboScale";
    if (a.goboSpin != b.goboSpin) return "goboSpin";
    if (a.goboFocus != b.goboFocus) return "goboFocus";
    // S9b-3: the sky light's image
    if (a.skyMap != b.skyMap) return "skyMap";
    if (a.skyMapEncoding != b.skyMapEncoding) return "skyMapEncoding";
    if (a.skyMapMirror != b.skyMapMirror) return "skyMapMirror";
    // S9b-1: the Physical Sky light's values
    if (a.v.skyTimeOfDay != b.v.skyTimeOfDay) return "v.skyTimeOfDay";
    if (a.v.skyNorthDeg != b.v.skyNorthDeg) return "v.skyNorthDeg";
    if (a.v.skyTurbidity != b.v.skyTurbidity) return "v.skyTurbidity";
    if (a.v.skyHaze != b.v.skyHaze) return "v.skyHaze";
    if (!SameVec3(a.v.skyGround, b.v.skyGround)) return "v.skyGround";
    if (a.v.skyIntensity != b.v.skyIntensity) return "v.skyIntensity";
    if (a.v.skySunIntensity != b.v.skySunIntensity) return "v.skySunIntensity";
    if (a.v.skyMaxElevationDeg != b.v.skyMaxElevationDeg) return "v.skyMaxElevationDeg";
    if (a.v.skyMapRotationDeg != b.v.skyMapRotationDeg) return "v.skyMapRotationDeg";
    if (a.v.skyMapIntensity != b.v.skyMapIntensity) return "v.skyMapIntensity";
    // S9b-4: the cloud layer on the physical sky
    if (a.v.cloudCoverage != b.v.cloudCoverage) return "v.cloudCoverage";
    if (a.v.cloudOpacity != b.v.cloudOpacity) return "v.cloudOpacity";
    if (a.v.cloudSoftness != b.v.cloudSoftness) return "v.cloudSoftness";
    if (a.v.cloudHeightKm != b.v.cloudHeightKm) return "v.cloudHeightKm";
    if (a.v.cloudScaleKm != b.v.cloudScaleKm) return "v.cloudScaleKm";
    if (a.v.cloudWindDeg != b.v.cloudWindDeg) return "v.cloudWindDeg";
    if (a.v.cloudWindSpeed != b.v.cloudWindSpeed) return "v.cloudWindSpeed";
    return "";
}

static std::string DiffLights(const std::vector<SceneLight>& a, const std::vector<SceneLight>& b) {
    if (a.size() != b.size()) return "count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    for (size_t i = 0; i < a.size(); ++i)
        if (const std::string d = DiffLight(a[i], b[i]); !d.empty()) return "light " + std::to_string(i) + " " + d;
    return "";
}

// Lights of every kind with a non-default value in every member (floats that are not exact in binary, on purpose).
static std::vector<SceneLight> MakeTestLights() {
    std::vector<SceneLight> out;
    {   // 1: the sun, not linked to the VMD, soft shadow, two keys
        SceneLight l;
        l.uid = 1;
        l.name = "\xEB\xA9\x94\xEC\x9D\xB8 \xEC\xA1\xB0\xEB\xAA\x85";  // 메인 조명
        l.kind = LightKind::Sun;
        l.v.direction = {0.25f, -0.875f, 0.1f};
        l.v.color = {0.6f, 0.55f, 0.3f};
        l.v.intensity = 0.85f;
        l.vmdLink = false;
        l.rimStrength = 0.6f;
        l.rimColor = {0.9f, 0.8f, 0.7f};
        l.shadow = ShadowType::Soft;
        l.shadowSoftness = 0.8f;
        l.shadowDensity = 0.7f;
        l.shadowColor = {0.1f, 0.2f, 0.3f};
        l.affectDiffuse = false;
        l.viewportVisible = false;
        LightKey k0, k1;
        k0.frame = 0;
        k0.v = l.v;
        k1.frame = 45;
        k1.v = l.v;
        k1.v.direction = {-0.5f, -1.0f, 1.0f / 3.0f};
        k1.v.color = {0.1f, 0.7f, 0.9f};
        k1.v.intensity = 1.4f;
        l.keys = {k0, k1};
        out.push_back(l);
    }
    {   // 2: a second sun, linked to the VMD, disabled, no keys
        SceneLight l;
        l.uid = 2;
        l.name = "\xEB\xB3\xB4\xEC\xA1\xB0 \xEC\xA1\xB0\xEB\xAA\x85";  // 보조 조명
        l.kind = LightKind::Sun;
        l.enabled = false;
        l.vmdLink = true;
        out.push_back(l);
    }
    {   // 3: the ambient light, its environment colours
        SceneLight l;
        l.uid = 3;
        l.name = "\xED\x99\x98\xEA\xB2\xBD\xEA\xB4\x91";  // 환경광
        l.kind = LightKind::Ambient;
        l.v.intensity = 0.3f;
        l.skyZenith = {0.05f, 0.07f, 0.16f};
        l.skyHorizon = {0.16f, 0.21f, 0.36f};
        l.groundColor = {0.08f, 0.09f, 0.12f};
        out.push_back(l);
    }
    {   // 4: a point light without keys, inverse-square falloff, no shadow
        SceneLight l;
        l.uid = 4;
        l.name = "\xEC\xA0\x90\xEA\xB4\x91\xEC\x9B\x90 1";  // 점광원 1
        l.kind = LightKind::Point;
        l.v.position = {1.5f, 30.25f, -7.1f};
        l.v.color = {1.0f, 0.9f, 0.8f};
        l.v.intensity = 1.5f;
        l.v.range = 90.0f;
        l.falloff = FalloffType::InverseSquare;
        l.shadow = ShadowType::NoCast;
        l.affectSpecular = false;
        out.push_back(l);
    }
    {   // 5: a spot aimed by hand, two keys
        SceneLight l;
        l.uid = 5;
        l.name = "\xEC\x8A\xA4\xED\x8C\x9F 1";  // 스팟 1
        l.kind = LightKind::Spot;
        l.aimMode = AimMode::Manual;
        l.v.position = {10.0f, 40.0f, -12.0f};
        l.v.aim = {2.0f, 0.1f, 0.3f};
        l.v.color = {0.22f, 0.77f, 0.73f};
        l.v.intensity = 2.6f;
        l.v.range = 120.0f;
        l.v.coneOuter = 0.30f;
        l.v.coneInner = 0.18f;
        LightKey k0, k1;
        k0.frame = 0;
        k0.v = l.v;
        k1.frame = 30;
        k1.v = l.v;
        k1.v.position = {6.0f, 38.0f, -8.0f};
        k1.v.aim = {-2.0f, 2.0f, 1.0f};
        k1.v.color = {0.95f, 0.35f, 0.62f};
        k1.v.intensity = 3.2f;
        k1.v.range = 140.0f;
        k1.v.coneOuter = 0.24f;
        k1.v.coneInner = 0.15f;
        l.keys = {k0, k1};
        out.push_back(l);
    }
    {   // 6: a spot that follows a character's head, linear falloff, two keys
        SceneLight l;
        l.uid = 6;
        l.name = "\xEC\x8A\xA4\xED\x8C\x9F 2";  // 스팟 2
        l.kind = LightKind::Spot;
        l.aimMode = AimMode::Target;
        l.targetUid = 3;
        l.targetPart = TargetPart::Head;
        l.v.position = {-8.0f, 40.0f, -12.0f};
        l.falloff = FalloffType::Linear;
        LightKey k0, k1;
        k0.frame = 10;
        k0.v = l.v;
        k1.frame = 20;
        k1.v = l.v;
        k1.v.position = {-4.0f, 44.0f, -10.0f};
        k1.v.color = {0.3f, 0.6f, 0.9f};
        k1.v.coneOuter = 0.35f;
        l.keys = {k0, k1};
        out.push_back(l);
    }
    {   // 7: a swaying spot (a Japanese name), three keys
        SceneLight l;
        l.uid = 7;
        l.name = "\xE3\x82\xB9\xE3\x83\x9D\xE3\x83\x83\xE3\x83\x88 3";  // スポット 3
        l.kind = LightKind::Spot;
        l.aimMode = AimMode::Sway;
        l.swayPhase = 2.6f;
        l.v.position = {-23.0f, 58.0f, -12.0f};
        l.v.aim = {3.5f, 0.0f, 0.0f};
        l.viewportVisible = false;
        LightKey k;
        for (int f : {0, 15, 90}) {
            k.frame = f;
            k.v = l.v;
            k.v.intensity = 1.0f + (float)f / 7.0f;
            l.keys.push_back(k);
        }
        out.push_back(l);
    }
    {   // 8: a disabled spot
        SceneLight l;
        l.uid = 8;
        l.name = "\xEC\x8A\xA4\xED\x8C\x9F 4";  // 스팟 4
        l.kind = LightKind::Spot;
        l.enabled = false;
        l.aimMode = AimMode::Target;
        // S7b: a gobo (a path with a non-default value in every field)
        l.gobo = "gobos/ring.png";
        l.goboRotation = 22.5f;
        l.goboScale = 1.25f;
        l.goboSpin = -40.0f;
        l.goboFocus = 0.45f;
        out.push_back(l);
    }
    return out;
}

// Effects of the project stack: 2 entries, one disabled, linear + stepped tracks, a texture folder.
static std::vector<StudioEffect> MakeTestEffects() {
    std::vector<StudioEffect> out;
    {
        StudioEffect e;
        e.uid = 1;
        e.entry.pack = "hoyo_toon_v2";
        e.entry.enabled = true;
        e.entry.params["intensity"] = 0.75f;
        e.entry.params["threshold"] = 0.25f;
        e.entry.textureFolder = "F:/tex/effect";
        EffectParamTrack t;
        t.param = "intensity";
        t.keys = {{0, 0.0f, false}, {30, 1.0f, false}, {60, 0.5f, false}};
        e.tracks.push_back(t);
        EffectParamTrack s;
        s.param = "threshold";
        s.keys = {{10, 0.2f, true}, {40, 0.8f, false}};
        e.tracks.push_back(s);
        out.push_back(std::move(e));
    }
    {
        StudioEffect e;
        e.uid = 2;
        e.entry.pack = "vignette";
        e.entry.enabled = false;
        EffectParamTrack t;
        t.param = "amount";
        t.keys = {{0, 0.1f, false}, {15, 0.9f, false}};
        e.tracks.push_back(t);
        out.push_back(std::move(e));
    }
    return out;
}

static void TestEffects() {
    // EvaluateParamTrack: empty, before / between / after, stepped
    {
        const EffectParamTrack empty;
        Check(EvaluateParamTrack(empty, 12.0f, 0.3f) == 0.3f, "empty track returns the fallback");
    }
    {
        EffectParamTrack t;
        t.keys = {{10, 0.0f, false}, {20, 1.0f, false}};
        Check(EvaluateParamTrack(t, 0.0f, 9.0f) == 0.0f, "before the first key returns the first value");
        Check(Near(EvaluateParamTrack(t, 15.0f, 9.0f), 0.5f), "between linear keys interpolates");
        Check(EvaluateParamTrack(t, 25.0f, 9.0f) == 1.0f, "after the last key returns the last value");
    }
    {
        EffectParamTrack t;
        t.keys = {{10, 0.0f, true}, {20, 1.0f, false}};
        Check(EvaluateParamTrack(t, 15.0f, 9.0f) == 0.0f, "a stepped key holds until the next key");
    }
    // EvaluateEffect: tracked params replace the static values, fallback is the static value
    {
        std::vector<StudioEffect> effects = MakeTestEffects();
        const EffectStackEntry e = EvaluateEffect(effects[0], 15.0f);
        Check(Near(e.params.at("intensity"), 0.5f) && Near(e.params.at("threshold"), 0.2f) &&
                  e.pack == "hoyo_toon_v2" && e.textureFolder == "F:/tex/effect",
              "EvaluateEffect applies track values at the frame");
    }
    // Upsert / erase
    {
        EffectParamTrack t;
        UpsertParamKey(t, {10, 1.0f, false});
        UpsertParamKey(t, {0, 0.0f, false});
        UpsertParamKey(t, {5, 0.5f, true});
        UpsertParamKey(t, {10, 2.0f, true});
        Check(t.keys.size() == 3 && t.keys[0].frame == 0 && t.keys[1].frame == 5 && t.keys[2].frame == 10 &&
                  t.keys[2].value == 2.0f && t.keys[2].stepped,
              "UpsertParamKey sorts and replaces the key at the same frame");
        EraseParamKeys(t, {5});
        Check(t.keys.size() == 2 && t.keys[0].frame == 0 && t.keys[1].frame == 10, "EraseParamKeys removes the frames");
    }
    // project round trip
    const std::filesystem::path projDir = kTemp / "effects";
    const std::filesystem::path projFile = projDir / "fx.mmdxproj";
    ProjectData data;
    data.effects = MakeTestEffects();
    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with effects", "%s", err.c_str());
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        Check(!j.is_discarded() && j.value("version", 0) == 6, "effect project format is version 6");
        Check(j.contains("effects") && j["effects"].is_array() && j["effects"].size() == 2 &&
                  j["effects"][0]["pack"] == "hoyo_toon_v2" && j["effects"][1]["enabled"] == false &&
                  j["effects"][0]["textureFolder"] == "F:/tex/effect" && j["effects"][0]["tracks"].size() == 2 &&
                  j["effects"][0]["tracks"][1]["keys"].size() == 2 && j["effects"][0]["tracks"][1]["keys"][0].size() == 3 &&
                  j["effects"][0]["tracks"][1]["keys"][0][2] == 1,
              "effects are written as pack / enabled / params / textureFolder / tracks");
    }
    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with effects", "%s", err.c_str());
    Check(warnings.empty(), "effect project loads without warnings");
    Check(loaded.effects.size() == data.effects.size(), "effect count kept");
    if (loaded.effects.size() == data.effects.size()) {
        Check(loaded.effects[0] == data.effects[0] && loaded.effects[1] == data.effects[1],
              "effects round-trip exactly (ignoring uid)");
        Check(loaded.effects[1].entry.enabled == false && loaded.effects[0].tracks[1].keys[0].stepped,
              "disabled entry and stepped key kept");
    }
}

static void TestWorlds() {
    const std::filesystem::path projDir = kTemp / "worlds";
    const std::filesystem::path projFile = projDir / "worlds.mmdxproj";
    ProjectData data;
    StudioWorld w;
    w.name = "snow field";
    w.fx.entry.pack = "snow";
    w.fx.entry.enabled = false;
    w.fx.entry.params["density"] = 0.7f;
    w.fx.entry.textureFolder = "F:/tex/world";
    EffectParamTrack track;
    track.param = "density";
    track.keys = {{0, 0.2f, false}, {20, 1.0f, true}};
    w.fx.tracks.push_back(track);
    w.place.translation = {2.0f, 0.0f, -3.5f};
    w.place.rotationDeg = {10.0f, 20.0f, 30.0f};
    w.place.scale = 1.5f;
    data.worlds.push_back(w);
    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with worlds", "%s", err.c_str());
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        Check(!j.is_discarded() && j.value("version", 0) == 6, "world project format is version 6");
        Check(j.contains("worlds") && j["worlds"].is_array() && j["worlds"].size() == 1 &&
                  j["worlds"][0]["name"] == "snow field" && j["worlds"][0]["pack"] == "snow" &&
                  j["worlds"][0]["enabled"] == false && j["worlds"][0]["tracks"].size() == 1 &&
                  j["worlds"][0]["tracks"][0]["keys"].size() == 2 &&
                  j["worlds"][0]["tracks"][0]["keys"][1][2] == 1 &&
                  j["worlds"][0]["place"]["t"][2] == -3.5f && j["worlds"][0]["place"]["s"] == 1.5f,
              "worlds are written as name / pack / enabled / params / tracks / place");
    }
    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with worlds", "%s", err.c_str());
    Check(warnings.empty(), "world project loads without warnings");
    Check(loaded.worlds.size() == data.worlds.size(), "world count kept");
    if (loaded.worlds.size() == data.worlds.size()) {
        Check(loaded.worlds[0] == data.worlds[0], "worlds round-trip exactly (ignoring uid)");
        Check(loaded.worlds[0].fx.entry.enabled == false && loaded.worlds[0].fx.tracks[0].keys[1].stepped,
              "disabled world and stepped key kept");
    }
    // absent key = no worlds
    {
        const std::filesystem::path legacy = kTemp / "worlds" / "legacy.mmdxproj";
        std::filesystem::create_directories(legacy.parent_path());
        std::ofstream f(legacy);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":3,\"models\":[],\"camera\":null,\"audio\":null}";
        f.close();
        ProjectData l2;
        warnings.clear();
        Check(LoadProject(legacy, l2, &err, &warnings), "LoadProject without worlds", "%s", err.c_str());
        Check(l2.worlds.empty(), "absent worlds key = no worlds");
    }
}

static void TestStages() {
    const std::filesystem::path projDir = kTemp / "stages";
    const std::filesystem::path projFile = projDir / "stages.mmdxproj";
    ProjectData data;
    // S3: one stage entity with a variant, a base value, a keyed colour component and a cue the project replaced
    data.stages.resize(1);
    StudioStage& st = data.stages[0];
    st.uid = 3;
    st.stageId = "stages:cyclorama";
    st.variant = "dusk";
    st.base["keyIntensity"] = StageValue::Number(1.25f);
    st.base["coveColor"] = StageValue::Color(XMFLOAT4{0.1f, 0.2f, 0.3f, 1.0f});
    EffectParamTrack track;
    track.param = "coveColor#2";
    track.keys = {{0, 0.25f, false}, {30, 0.75f, true}};
    st.tracks.push_back(track);
    st.cuesOff.insert("keyIntensity");
    // S5: a screen override (source / file / camera / brightness), saved in the stage entry
    StageScreenOverride so;
    so.source = std::string("video");
    so.file = mmdx::PathToUtf8(kTemp / "stages" / "led.mp4");
    so.camera = std::string("close_cam");
    so.brightness = 2.5f;
    st.screens["main"] = so;
    // a part model referring to it (the reference is a uid, and is remapped only when the id is repeated / missing)
    ProjectModel pm;
    pm.name = "cove";
    pm.kind = ModelKind::Stage;
    pm.path = kTemp / "stages" / "cove.pmx";
    pm.stageUid = 3;
    data.models.push_back(pm);
    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with stages", "%s", err.c_str());
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        Check(!j.is_discarded() && j.value("version", 0) == 6, "stage project format is version 6");
        Check(j.contains("stages") && j["stages"].is_array() && j["stages"].size() == 1 &&
                  j["stages"][0]["uid"] == 3 && j["stages"][0]["stageId"] == "stages:cyclorama" &&
                  j["stages"][0]["variant"] == "dusk" && j["stages"][0]["base"].is_object() &&
                  j["stages"][0]["base"]["keyIntensity"] == 1.25f &&
                  j["stages"][0]["base"]["coveColor"].size() == 4 &&
                  j["stages"][0]["tracks"].size() == 1 && j["stages"][0]["tracks"][0]["param"] == "coveColor#2" &&
                  j["stages"][0]["tracks"][0]["keys"].size() == 2 &&
                  j["stages"][0]["tracks"][0]["keys"][1][2] == 1 &&
                  j["stages"][0]["cuesOff"].size() == 1 && j["stages"][0]["cuesOff"][0] == "keyIntensity",
              "stages are written as uid / stageId / path / variant / base / tracks / cuesOff");
        Check(j["stages"][0]["screens"].is_object() && j["stages"][0]["screens"].size() == 1 &&
                  j["stages"][0]["screens"]["main"]["source"] == "video" &&
                  j["stages"][0]["screens"]["main"]["file"] == "led.mp4" &&
                  j["stages"][0]["screens"]["main"]["camera"] == "close_cam" &&
                  j["stages"][0]["screens"]["main"]["brightness"] == 2.5f,
              "a screen override is written as source / file / camera / brightness");
        Check(j["models"][0]["stageUid"] == 3, "a part model writes its stage uid");
    }
    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with stages", "%s", err.c_str());
    Check(loaded.stages.size() == data.stages.size(), "stage count kept");
    if (loaded.stages.size() == data.stages.size()) {
        Check(loaded.stages[0] == data.stages[0], "stages round-trip exactly (ignoring uid)");
        Check(loaded.stages[0].uid == 3, "a saved stage uid survives (the parts refer to it)");
        Check(loaded.stages[0].screens.count("main") == 1 &&
                  loaded.stages[0].screens.at("main") == data.stages[0].screens.at("main") &&
                  mmdx::PathToUtf8(Utf8ToPath(*loaded.stages[0].screens.at("main").file)) ==
                      mmdx::PathToUtf8(*data.stages[0].screens.at("main").file),
              "a screen override round-trips (its file resolves against the project folder)");
        Check(loaded.stages[0].base.at("coveColor").type == StageValue::Type::Color &&
                  Near(loaded.stages[0].base.at("coveColor").x, 0.1f) &&
                  loaded.stages[0].tracks[0].keys[1].stepped,
              "a colour base value and a stepped key are kept");
    }
    Check(loaded.models.size() == 1 && loaded.models[0].stageUid == 3, "a part model keeps its stage uid");
    // a repeated uid gets a fresh one and the references follow it
    {
        const std::filesystem::path dup = kTemp / "stages" / "dup.mmdxproj";
        std::ofstream f(dup);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":4,\"models\":[{\"name\":\"a\",\"kind\":\"stage\","
             "\"path\":\"a.pmx\",\"stageUid\":5},{\"name\":\"b\",\"kind\":\"stage\",\"path\":\"b.pmx\",\"stageUid\":5}],"
             "\"camera\":null,\"audio\":null,\"stages\":[{\"uid\":5,\"stageId\":\"x\",\"path\":\"s/stage.json\"},"
             "{\"uid\":5,\"stageId\":\"y\",\"path\":\"t/stage.json\"}]}";
        f.close();
        ProjectData d2;
        warnings.clear();
        Check(LoadProject(dup, d2, &err, &warnings), "LoadProject with a repeated stage uid", "%s", err.c_str());
        Check(d2.stages.size() == 2 && d2.stages[0].uid != d2.stages[1].uid, "a repeated stage uid becomes unique");
        Check(d2.models.size() == 2 && d2.models[1].stageUid == d2.stages[1].uid, "the part of the second stage follows it");
    }
    // a version 3 file: no stages, the part's "stageVariant" is read (converted to an entity by the studio loader)
    {
        const std::filesystem::path legacy = kTemp / "stages" / "legacy.mmdxproj";
        std::ofstream f(legacy);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":3,\"models\":[{\"name\":\"a\",\"kind\":\"stage\","
             "\"path\":\"a.pmx\",\"stageVariant\":\"dusk\"}],\"camera\":null,\"audio\":null}";
        f.close();
        ProjectData l2;
        warnings.clear();
        Check(LoadProject(legacy, l2, &err, &warnings), "LoadProject (version 3) with stages", "%s", err.c_str());
        Check(l2.stages.empty() && l2.models.size() == 1 && l2.models[0].stageVariant == "dusk",
              "a version 3 part keeps its stageVariant for the loader to convert");
    }
    // a reference to a stage the file does not declare becomes 0
    {
        const std::filesystem::path orphan = kTemp / "stages" / "orphan.mmdxproj";
        std::ofstream f(orphan);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":4,\"models\":[{\"name\":\"a\",\"kind\":\"stage\","
             "\"path\":\"a.pmx\",\"stageUid\":9}],\"camera\":null,\"audio\":null,\"stages\":[]}";
        f.close();
        ProjectData l3;
        warnings.clear();
        Check(LoadProject(orphan, l3, &err, &warnings), "LoadProject with an orphan stage reference", "%s", err.c_str());
        Check(!l3.models.empty() && l3.models[0].stageUid == 0, "a reference to no declared stage becomes 0");
    }
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
        m.physicsOff = true;                                                          // P2.2: physics baked / off
        m.shader.pack = "test_pack";                                                   // shader pack choice
        m.shader.params["threshold"] = 0.5f;
        m.shader.textureFolder = "F:/tex/miku";                                        // per-character texture folder
        m.shader.SwitchPack("other_pack");                                             // remembered settings of a previous pack
        m.shader.params["gloss"] = 0.25f;
        m.shader.textureFolder = "F:/tex/other";
        m.shader.SwitchPack("test_pack");
        m.shader.materials["\xE9\xAB\xAA"] = (int)PackClass::Hair;                   // 髪: per-material class
        m.shader.materials["body|01"] = kMaterialPackOff;                              // ... and a material with the pack off
        m.material.ray = true;                                                        // P5: Ray-MMD material source
        m.material.emd = (kTemp / "models" / "miku.emd").lexically_normal().string();  // ... with an explicit .emd
        m.mme = MmeChoice{MmeChoice::File, kTemp / "models" / "miku.fx"};              // M6a: an MME object effect
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
        m.libraryId = "stage-1";                                                      // P5: a stage's material choice key
        m.material.ray = true;                                                        // ... saved for every kind
        m.material.emd = (kTemp / "models" / "stage.emd").lexically_normal().string();
        m.mme = MmeChoice{MmeChoice::Auto, {}};                                       // M6a: stages default to Auto
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
        // accessory keys: world at 0, parented to model index 0 (parentUid 1) at 30
        AccessoryKf a0;
        a0.frame = 0;
        a0.translation = {0, 1, 2};
        a0.rotationDeg = {0, 45, 0};
        a0.scale = 2.0f;
        a0.opacity = 1.0f;
        a0.visible = true;
        a0.parentUid = 0;
        AccessoryKf a30;
        a30.frame = 30;
        a30.translation = {3, 4, 5};
        a30.rotationDeg = {10, 20, 30};
        a30.scale = 0.5f;
        a30.opacity = 0.3f;
        a30.visible = false;
        a30.parentUid = 1;  // 1 + model index 0
        a30.bone = "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96";  // 右手首
        m.motion.accessory = {a0, a30};
        data.models.push_back(m);
    }
    // 3b: M5c MME effect object (an accessory `.x` + its same-stem `.fx`), written as kind "effect"
    {
        ProjectModel m;
        m.name = "AirDust";
        m.kind = ModelKind::Effect;
        m.path = kTemp / "effects" / "AirDust.x";
        m.fx = kTemp / "effects" / "AirDust.fx";
        AccessoryKf k;
        k.frame = 0;
        k.scale = 1.0f;
        k.opacity = 1.0f;
        k.visible = true;
        m.motion.accessory = {k};
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
    data.bgVideoPath = kTemp / "video" / "bg.avi";   // background AVI (背景AVI)
    data.bgVideoOffset = -1.25;
    data.bgVideoAsBackground = true;
    data.editor.frame = 120;
    data.editor.selectedModel = 2;
    data.editor.aspectW = 9;
    data.editor.aspectH = 16;
    data.editor.loop = true;
    data.editor.rangeStart = 10;
    data.editor.rangeEnd = 50;
    data.editor.pxPerFrame = 3.5f;
    data.editor.camTarget = {1, 2, 3};
    data.editor.camYaw = 0.5f;
    data.editor.camPitch = 0.2f;
    data.editor.camDistance = 30.0f;
    data.editor.camFovDeg = 40.0f;
    data.editor.lights = MakeTestLights();  // every kind of scene light
    // M6b: the project's own MME post stack
    data.mmePost = {{kTemp / "fx" / "o_Monochrome.fx", true}, {kTemp / "fx" / "o_Vignette.fx", false}};

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
        Check(text.find("\"material\"") != std::string::npos && text.find("miku.emd") != std::string::npos,
              "stored material choice (ray + relative .emd path)");
        Check(text.find("stage.emd") != std::string::npos, "stored stage material choice (every kind)");
        Check(text.find("\"mme\"") != std::string::npos && text.find("miku.fx") != std::string::npos,
              "stored MME choice (file mode + relative .fx path)");
        Check(text.find("\"mmeEffects\"") != std::string::npos && text.find("o_Monochrome.fx") != std::string::npos,
              "stored project MME post stack (relative .fx paths)");
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
        Check(b.physicsOff == a.physicsOff && b.physicsOff, "physics-off flag kept (baked character)");
        Check(!loaded.models[1].physicsOff, "physics on by default for a model without the flag");
        Check(b.shader == a.shader, "shader choice kept (pack, params, textureFolder)");
        Check(b.material == a.material, "material choice kept (ray + explicit .emd)");
        Check(b.mme == a.mme && b.mme.mode == MmeChoice::File, "MME choice kept (file mode + path)");

        Check(b.place == data.models[0].place, "character placement kept (t/r/s)");
        const ProjectModel& st = loaded.models[1];
        Check(st.name == data.models[1].name && st.kind == ModelKind::Stage && !st.visible &&
                  st.motion.Empty() && st.place == data.models[1].place,
              "stage kept: name, kind, visible=false, no keys, placement");
        Check(st.libraryId == data.models[1].libraryId, "stage library id kept");
        Check(st.material == data.models[1].material, "stage material choice kept (ray + explicit .emd)");
        Check(st.mme.mode == MmeChoice::Auto && st.mme == data.models[1].mme, "stage MME choice kept (auto)");

        const ProjectModel& p = loaded.models[2];
        Check(p.kind == ModelKind::Prop && p.attach == data.models[2].attach,
              "prop attach kept (parent/bone/t/r/s)");
        Check(p.motion.bones.size() == 1, "prop bone key kept");
        Check(p.motion.accessory.size() == 2 && p.motion.accessory[0] == data.models[2].motion.accessory[0] &&
                  p.motion.accessory[1] == data.models[2].motion.accessory[1],
              "prop accessory keys kept (placement, scale, opacity, visible, parent, bone)");

        // M5c: the effect object (kind "effect") keeps its `.x` mesh + its same-stem `.fx` and its accessory keys
        const ProjectModel& efo = loaded.models[3];
        Check(efo.kind == ModelKind::Effect, "effect object kind kept");
        Check(efo.path == std::filesystem::absolute(data.models[3].path).lexically_normal(), "effect object mesh path kept");
        Check(efo.fx == std::filesystem::absolute(data.models[3].fx).lexically_normal(), "effect object fx path kept");
        Check(efo.motion.accessory.size() == 1, "effect object accessory key kept");

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
    // Background AVI (背景AVI)
    Check(loaded.bgVideoPath == std::filesystem::absolute(data.bgVideoPath).lexically_normal(),
          "background video path resolved absolute");
    Check(loaded.bgVideoOffset == -1.25, "background video offset kept");
    Check(loaded.bgVideoAsBackground, "background video asBackground kept");
    // M6b: the project MME post stack
    Check(loaded.mmePost.size() == data.mmePost.size(), "MME post stack entry count kept");
    if (loaded.mmePost.size() == data.mmePost.size()) {
        Check(loaded.mmePost[0].path == std::filesystem::absolute(data.mmePost[0].path).lexically_normal() &&
                  loaded.mmePost[0].enabled,
              "MME post entry 0 kept (absolute path + enabled)");
        Check(loaded.mmePost[1].path == std::filesystem::absolute(data.mmePost[1].path).lexically_normal() &&
                  !loaded.mmePost[1].enabled,
              "MME post entry 1 kept (absolute path + disabled)");
    }
    const ProjectEditor& ed = loaded.editor;
    Check(ed.frame == 120 && ed.selectedModel == 2 && ed.loop && ed.rangeStart == 10 && ed.rangeEnd == 50 &&
              ed.pxPerFrame == 3.5f && ed.camYaw == 0.5f && ed.camPitch == 0.2f && ed.camDistance == 30.0f &&
              ed.camFovDeg == 40.0f && ed.useMotionCamera && ed.useShadowTrack &&
              ed.showCameraPath && ed.physics && Vec3Eq(ed.camTarget, {1, 2, 3}, 0) &&
              ed.aspectW == 9 && ed.aspectH == 16,
          "editor state kept (including non-default aspect 9:16)");
    // ---- the scene lights (version 2) ----
    Check(ed.lights.size() == data.editor.lights.size(), "scene light count kept", "size=%zu", ed.lights.size());
    {
        const std::string diff = DiffLights(data.editor.lights, ed.lights);
        Check(diff.empty(), "scene lights round-trip field by field", "differs: %s", diff.c_str());
        Check(data.editor.lights == ed.lights, "scene lights round-trip exactly (operator==)");
    }
    if (ed.lights.size() == 8) {
        const SceneLight& sun = ed.lights[0];
        Check(sun.kind == LightKind::Sun && !sun.vmdLink && sun.shadow == ShadowType::Soft && sun.shadowSoftness == 0.8f &&
                  sun.shadowDensity == 0.7f && SameVec3(sun.shadowColor, {0.1f, 0.2f, 0.3f}) && !sun.affectDiffuse &&
                  sun.affectSpecular && !sun.viewportVisible && sun.falloff == FalloffType::None,
              "sun: link off, soft shadow, shadow colour, affect and visibility flags kept");
        Check(ed.lights[1].kind == LightKind::Sun && ed.lights[1].vmdLink && !ed.lights[1].enabled && ed.lights[1].keys.empty(),
              "second sun: linked, disabled, no keys");
        Check(ed.lights[3].kind == LightKind::Point && ed.lights[3].falloff == FalloffType::InverseSquare &&
                  ed.lights[3].shadow == ShadowType::NoCast && !ed.lights[3].affectSpecular && ed.lights[3].keys.empty(),
              "point: inverse-square falloff, no shadow, no specular, no keys");
        Check(ed.lights[4].aimMode == AimMode::Manual && ed.lights[5].aimMode == AimMode::Target &&
                  ed.lights[5].targetUid == 3 && ed.lights[5].targetPart == TargetPart::Head &&
                  ed.lights[6].aimMode == AimMode::Sway && ed.lights[6].swayPhase == 2.6f && !ed.lights[7].enabled,
              "spots: the three aim modes, target, part, phase and the disabled one kept");
        // keyed sample: halfway between the first spot's keys
        const LightValues mid = SampleLightValues(ed.lights[4], 15);
        Check(std::fabs(mid.intensity - 2.9f) < 1e-4 && Vec3Eq(mid.position, {8.0f, 39.0f, -10.0f}, 1e-4f) &&
                  std::fabs(mid.coneOuter - 0.27f) < 1e-5,
              "spot keys interpolate linearly");
    }
}

// The project file of the scene lights: version 2, the "lights" array and its schema, no "lighting" / "useLightTrack".
static void TestSceneLightFile() {
    const std::filesystem::path projDir = kTemp / "lightsfile";
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    ProjectData data;
    data.editor.lights = MakeTestLights();
    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with scene lights", "%s", err.c_str());
    std::string text;
    {   // closed again before the next save replaces the file
        std::ifstream in(projFile, std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    Check(kProjectFormatVersion == 6 && !j.is_discarded() && j.value("version", 0) == 6, "project format version is 6");
    const nlohmann::json& e = j["editor"];
    Check(e.contains("lights") && e["lights"].is_array() && e["lights"].size() == 8 && !e.contains("lighting") &&
              !e.contains("useLightTrack"),
          "the editor writes \"lights\" and neither \"lighting\" nor \"useLightTrack\"");
    const nlohmann::json& s = e["lights"][0];
    Check(s["uid"] == 1 && s["kind"] == "sun" && s["name"] == "\xEB\xA9\x94\xEC\x9D\xB8 \xEC\xA1\xB0\xEB\xAA\x85" && s["enabled"] == true,
          "light object: uid, kind, name, enabled");
    Check(s["values"].contains("position") && s["values"].contains("aim") && s["values"].contains("direction") &&
              s["values"].contains("color") && s["values"].contains("intensity") && s["values"].contains("range") &&
              s["values"].contains("coneOuter") && s["values"].contains("coneInner") && s["values"]["direction"].size() == 3,
          "light object: every value is written");
    Check(s["sun"]["vmdLink"] == false && s["sun"].contains("rimStrength") && s["sun"]["rimColor"].size() == 3,
          "light object: the \"sun\" block");
    Check(s["spot"]["aim"] == "manual" && s["spot"]["target"] == 0 && s["spot"]["part"] == "centre" && s["spot"].contains("swayPhase"),
          "light object: the \"spot\" block");
    Check(s["sky"]["zenith"].size() == 3 && s["sky"]["horizon"].size() == 3 && s["sky"]["ground"].size() == 3,
          "light object: the \"sky\" block");
    Check(s["shadow"]["type"] == "soft" && s["shadow"].contains("softness") && s["shadow"].contains("density") &&
              s["shadow"]["color"].size() == 3 && s["falloff"] == "none" && s["affectDiffuse"] == false &&
              s["affectSpecular"] == true && s["viewportVisible"] == false,
          "light object: shadow, falloff, affect and visibility flags");
    Check(s["keys"].size() == 2 && s["keys"][1]["frame"] == 45 && s["keys"][1]["values"].contains("coneInner"),
          "light object: keys with a frame and every value");
    const nlohmann::json& pt = e["lights"][3];
    const nlohmann::json& sp = e["lights"][6];
    Check(pt["kind"] == "point" && pt["falloff"] == "inverse_square" && pt["shadow"]["type"] == "nocast" && pt["keys"].empty() &&
              e["lights"][2]["kind"] == "ambient" && sp["kind"] == "spot" && sp["spot"]["aim"] == "sway" &&
              e["lights"][5]["spot"]["aim"] == "target" && e["lights"][5]["spot"]["part"] == "head" &&
              e["lights"][5]["spot"]["target"] == 3 && e["lights"][5]["falloff"] == "linear",
          "light objects: kind, aim, part and enum names");

    // no lights: an empty array is written and read back as empty
    ProjectData none;
    Check(SaveProject(projFile, none, &err), "SaveProject without scene lights", "%s", err.c_str());
    ProjectData loadedNone;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loadedNone, &err, &warnings) && loadedNone.editor.lights.empty() && warnings.empty(),
          "no lights round-trip as no lights");
}

// Reading version 2 files: defaults for missing keys, unknown kinds, unsorted / repeated key frames, bad values.
static void TestSceneLightReading() {
    const std::filesystem::path projDir = kTemp / "lightsread";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    const auto load = [&](const std::string& editorBody, ProjectData& out, std::vector<std::string>& warnings) {
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{"
              << editorBody << "}}";
        }
        std::string err;
        warnings.clear();
        return LoadProject(projFile, out, &err, &warnings);
    };
    std::vector<std::string> warnings;

    {   // "lights" absent: none, not the defaults
        ProjectData d;
        Check(load("\"frame\":3", d, warnings) && d.editor.lights.empty(), "version 2 without \"lights\": no lights");
    }
    {   // a minimal light: every member keeps its default; a missing uid gets one
        ProjectData d;
        const bool ok = load("\"lights\":[{\"kind\":\"spot\"},{\"kind\":\"ambient\",\"uid\":9,\"shadow\":{\"type\":\"soft\"}}]", d, warnings);
        Check(ok && d.editor.lights.size() == 2 && warnings.empty(), "minimal lights load");
        if (d.editor.lights.size() == 2) {
            SceneLight want;
            want.kind = LightKind::Spot;
            want.uid = d.editor.lights[0].uid;
            Check(d.editor.lights[0].uid != 0 && DiffLight(want, d.editor.lights[0]).empty(),
                  "missing keys keep the SceneLight defaults (also the common properties)", "differs: %s",
                  DiffLight(want, d.editor.lights[0]).c_str());
            const SceneLight& a = d.editor.lights[1];
            Check(a.uid == 9 && a.shadow == ShadowType::Soft && a.shadowSoftness == 0.5f && a.shadowDensity == 1.0f &&
                      SameVec3(a.shadowColor, {0, 0, 0}) && a.falloff == FalloffType::None && a.affectDiffuse &&
                      a.affectSpecular && a.viewportVisible,
                  "a partial \"shadow\" block keeps the other defaults");
        }
    }
    {   // an unknown kind is skipped with a warning
        ProjectData d;
        const bool ok = load("\"lights\":[{\"kind\":\"laser\",\"uid\":1},{\"kind\":\"point\",\"uid\":2},3]", d, warnings);
        Check(ok && d.editor.lights.size() == 1 && d.editor.lights[0].uid == 2 && warnings.size() == 1 &&
                  warnings[0].find("unknown light kind") == 0,
              "an unknown light kind is skipped with a warning", "lights=%zu warnings=%zu", d.editor.lights.size(), warnings.size());
    }
    {   // keys: unsorted input comes out sorted, a repeated frame keeps the last one, a key without values holds the base values
        ProjectData d;
        const bool ok = load("\"lights\":[{\"kind\":\"point\",\"uid\":1,\"values\":{\"intensity\":2.5},\"keys\":["
                             "{\"frame\":30,\"values\":{\"intensity\":3}},{\"frame\":0,\"values\":{\"intensity\":1}},"
                             "{\"frame\":30,\"values\":{\"intensity\":5}},{\"frame\":60}]}]", d, warnings);
        Check(ok && d.editor.lights.size() == 1, "keyed light loads");
        if (!d.editor.lights.empty()) {
            const std::vector<LightKey>& k = d.editor.lights[0].keys;
            Check(k.size() == 3 && k[0].frame == 0 && k[1].frame == 30 && k[2].frame == 60 && k[0].v.intensity == 1.0f &&
                      k[1].v.intensity == 5.0f && k[2].v.intensity == 2.5f,
                  "keys are sorted, a repeated frame keeps the last, a key without values holds the base values");
        }
    }
    {   // wrong types and unknown names keep the defaults
        ProjectData d;
        const bool ok = load("\"lights\":[{\"kind\":\"spot\",\"uid\":1,\"enabled\":\"yes\",\"name\":5,\"falloff\":\"cubic\","
                             "\"values\":{\"intensity\":\"high\",\"position\":[1,2]},\"spot\":{\"aim\":\"orbit\",\"part\":\"feet\"},"
                             "\"shadow\":{\"type\":\"sharp\",\"softness\":\"very\"},\"viewportVisible\":1}]", d, warnings);
        SceneLight want;
        want.kind = LightKind::Spot;
        want.uid = 1;
        Check(ok && d.editor.lights.size() == 1 && DiffLight(want, d.editor.lights[0]).empty(),
              "wrong types and unknown enum names keep the defaults", "differs: %s",
              d.editor.lights.empty() ? "(no light)" : DiffLight(want, d.editor.lights[0]).c_str());
    }
    {   // uids identify the lights: a repeated or zero uid gets a fresh one
        ProjectData d;
        const bool ok = load("\"lights\":[{\"kind\":\"point\",\"uid\":4},{\"kind\":\"point\",\"uid\":4},{\"kind\":\"point\",\"uid\":0},"
                             "{\"kind\":\"point\",\"uid\":2}]", d, warnings);
        bool unique = ok && d.editor.lights.size() == 4;
        for (size_t i = 0; unique && i < d.editor.lights.size(); ++i) {
            unique = d.editor.lights[i].uid != 0;
            for (size_t k = 0; unique && k < i; ++k) unique = d.editor.lights[k].uid != d.editor.lights[i].uid;
        }
        Check(unique && d.editor.lights[0].uid == 4 && d.editor.lights[3].uid == 2, "repeated and zero uids are renumbered, the others kept");
    }
}

// Area light project round trip: size + keys survive, missing size loads the default {20, 20}.
static void TestAreaLightProject() {
    const std::filesystem::path projDir = kTemp / "arealight";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "area.mmdxproj";

    ProjectData data;
    SceneLight al;
    al.uid = 1;
    al.name = "\xEB\xA9\xB4\xEA\xB4\x91\xEC\x9B\x90 1";  // 면광원 1
    al.kind = LightKind::Area;
    al.v.position = {5.0f, 15.0f, -10.0f};
    al.v.aim = {0.0f, 10.0f, 0.0f};
    al.v.size = {30.0f, 15.0f};
    al.v.color = {0.8f, 0.7f, 0.9f};
    al.v.intensity = 2.5f;
    al.v.range = 50.0f;
    al.shadow = ShadowType::Soft;
    al.shadowSoftness = 0.6f;
    al.falloff = FalloffType::InverseSquare;

    LightKey k0, k1;
    k0.frame = 0;
    k0.v = al.v;
    k1.frame = 30;
    k1.v = al.v;
    k1.v.size = {45.0f, 25.0f};
    al.keys = {k0, k1};
    data.editor.lights = {al};

    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with area light", "%s", err.c_str());

    // Verify raw JSON contents
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        Check(!j.is_discarded() && j.value("version", 0) == 6, "area light project format is version 6");
        const nlohmann::json& l = j["editor"]["lights"][0];
        Check(l["kind"] == "area", "area light JSON kind is \"area\"");
        Check(l["values"].contains("size") && l["values"]["size"].is_array() && l["values"]["size"].size() == 2 &&
              Near(l["values"]["size"][0].get<float>(), 30.0f, 1e-4f) && Near(l["values"]["size"][1].get<float>(), 15.0f, 1e-4f),
              "area light base values serialize size [30, 15]");
        Check(l["keys"].size() == 2 && l["keys"][1]["values"].contains("size") &&
              Near(l["keys"][1]["values"]["size"][0].get<float>(), 45.0f, 1e-4f) &&
              Near(l["keys"][1]["values"]["size"][1].get<float>(), 25.0f, 1e-4f),
              "area light key values serialize size [45, 25]");
    }

    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings) && warnings.empty(), "LoadProject with area light", "%s", err.c_str());
    Check(loaded.editor.lights.size() == 1, "1 area light loaded");
    if (loaded.editor.lights.size() == 1) {
        const SceneLight& loadedAl = loaded.editor.lights[0];
        const std::string diff = DiffLight(al, loadedAl);
        Check(diff.empty(), "area light round-trip field by field", "differs: %s", diff.c_str());
        Check(al == loadedAl, "area light round-trip exactly (operator==)");
        Check(loadedAl.v.size.x == 30.0f && loadedAl.v.size.y == 15.0f, "loaded area light has correct base size");
        Check(loadedAl.keys.size() == 2 && loadedAl.keys[1].v.size.x == 45.0f && loadedAl.keys[1].v.size.y == 25.0f,
              "loaded area light has correct key size");
    }

    // Missing "size" loads the default {20.0f, 20.0f}
    {
        const std::filesystem::path noSizeFile = projDir / "nosize.mmdxproj";
        {
            std::ofstream f(noSizeFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{"
                 "\"lights\":[{\"kind\":\"area\",\"uid\":10,\"values\":{\"position\":[1,2,3]}}]}}";
        }
        ProjectData d;
        warnings.clear();
        Check(LoadProject(noSizeFile, d, &err, &warnings) && d.editor.lights.size() == 1, "area light without size loads");
        if (d.editor.lights.size() == 1) {
            Check(d.editor.lights[0].v.size.x == 20.0f && d.editor.lights[0].v.size.y == 20.0f,
                  "area light without size gets default size {20, 20}");
        }
    }
}

// S9: a FogVolume light round trip (extent, density and tint in values; the per-light anisotropy, the shape and the
// edge band), and the stage fog volumes that feed such lights.
static void TestFogVolumeProject() {
    const std::filesystem::path projDir = kTemp / "fogvolume";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "fog.mmdxproj";

    ProjectData data;
    SceneLight fv;
    fv.uid = 2;
    fv.name = "\xEC\x95\x88\xEA\xB0\x9C \xEB\xB0\x95\xEC\x8A\xA4";  // 안개 박스
    fv.kind = LightKind::FogVolume;
    fv.v.position = {0.0f, 22.0f, -4.0f};
    fv.v.extent = {35.0f, 18.5f, 28.0f};
    fv.v.intensity = 2.25f;    // density multiplier
    fv.v.color = {0.7f, 0.8f, 0.9f};
    fv.volumetricG = 0.85f;
    fv.fogShape = FogShape::Sphere;
    fv.fogEdge = 0.6f;
    LightKey k0, k1;
    k0.frame = 0;
    k0.v = fv.v;
    k1.frame = 60;
    k1.v = fv.v;
    k1.v.intensity = 4.0f;
    k1.v.extent = {50.0f, 20.0f, 40.0f};
    fv.keys = {k0, k1};
    data.editor.lights = {fv};

    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with a fog volume", "%s", err.c_str());
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        const nlohmann::json& l = j["editor"]["lights"][0];
        Check(l["kind"] == "fog_volume" && l["values"].contains("extent") && l["values"]["extent"].size() == 3 &&
                  !l["values"].contains("volumetric"),
              "fog volume: kind and extent are written; a default volumetric (1) is omitted");
        Check(l.contains("volumetricG") && l["volumetricG"].get<float>() == 0.85f && l["fogShape"] == "sphere" &&
                  Near(l["fogEdge"].get<float>(), 0.6f, 1e-4f),
              "fog volume: anisotropy, shape and edge are written");
        Check(l["keys"].size() == 2 && l["keys"][1]["values"]["extent"][0].get<float>() == 50.0f,
              "fog volume: key values carry the extent");
    }

    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings) && warnings.empty(), "LoadProject with a fog volume", "%s", err.c_str());
    Check(loaded.editor.lights.size() == 1, "1 fog volume loaded");
    if (loaded.editor.lights.size() == 1) {
        const SceneLight& got = loaded.editor.lights[0];
        const std::string diff = DiffLight(fv, got);
        Check(diff.empty(), "fog volume round-trip field by field", "differs: %s", diff.c_str());
        Check(fv == got, "fog volume round-trip exactly (operator==)");
    }

    // A fog volume light without the new keys loads with the defaults (an old file).
    {
        const std::filesystem::path oldFile = projDir / "old.mmdxproj";
        {
            std::ofstream f(oldFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,"
                 "\"editor\":{\"lights\":[{\"kind\":\"fog_volume\",\"uid\":3,\"values\":{\"position\":[1,2,3]}}]}}";
        }
        ProjectData d;
        warnings.clear();
        Check(LoadProject(oldFile, d, &err, &warnings) && d.editor.lights.size() == 1, "fog volume without the new keys loads");
        if (d.editor.lights.size() == 1) {
            const SceneLight& got = d.editor.lights[0];
            Check(got.volumetricG == -1.0f && got.fogShape == FogShape::Box && got.v.extent.x == 20.0f &&
                      got.v.extent.y == 10.0f && got.v.volumetric == 1.0f,
                  "fog volume defaults: anisotropy -1, box, extent {20,10,20}, volumetric 1");
        }
    }
}

// S9b-1: a Sky light project round trip (its values in "values", omitted at their defaults, and a keyed time of day).
static void TestSkyLightProject() {
    const std::filesystem::path projDir = kTemp / "skylight";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "sky.mmdxproj";

    ProjectData data;
    SceneLight sky;
    sky.uid = 9;
    sky.name = "\xED\x95\x98\xEB\x8A\x98";  // 하늘
    sky.kind = LightKind::Sky;
    sky.v.skyTimeOfDay = 6.75f;
    sky.v.skyNorthDeg = -32.0f;
    sky.v.skyTurbidity = 4.25f;
    sky.v.skyHaze = 0.45f;
    sky.v.skyGround = {0.3f, 0.29f, 0.27f};
    sky.v.skyIntensity = 1.4f;
    sky.v.skySunIntensity = 0.85f;
    sky.v.skyMaxElevationDeg = 52.0f;
    // S9b-3: the sky's image (its rotation / scale are keyable values, its file / encoding / mirror base values)
    sky.skyMap = "sky/test_sky.hdr";
    sky.skyMapEncoding = "rgbt";
    sky.skyMapMirror = true;
    sky.v.skyMapRotationDeg = -72.5f;
    sky.v.skyMapIntensity = 1.75f;
    // S9b-4: the cloud layer (keyable with the rest of the sky values)
    sky.v.cloudCoverage = 0.45f;
    sky.v.cloudOpacity = 0.8f;
    sky.v.cloudSoftness = 0.35f;
    sky.v.cloudHeightKm = 3.25f;
    sky.v.cloudScaleKm = 2.5f;
    sky.v.cloudWindDeg = 90.0f;
    sky.v.cloudWindSpeed = 25.0f;
    LightKey k0, k1;
    k0.frame = 0;
    k0.v = sky.v;
    k1.frame = 300;
    k1.v = sky.v;
    k1.v.skyTimeOfDay = 18.25f;
    sky.keys = {k0, k1};
    data.editor.lights = {sky};

    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with a sky light", "%s", err.c_str());
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        const nlohmann::json& l = j["editor"]["lights"][0];
        Check(l["kind"] == "sky" && l["values"]["skyTimeOfDay"] == 6.75f && l["values"]["skyGround"].size() == 3 &&
                  l["values"]["skySunIntensity"] == 0.85f,
              "sky light: kind and values are written");
        Check(l["skyMap"] == "sky/test_sky.hdr" && l["skyMapEncoding"] == "rgbt" && l["skyMapMirror"] == true &&
                  l["values"]["skyMapRotationDeg"] == -72.5f && l["values"]["skyMapIntensity"] == 1.75f,
              "sky light: the image and its values are written");
        Check(l["values"]["cloudCoverage"] == 0.45f && l["values"]["cloudSoftness"] == 0.35f &&
                  l["values"]["cloudHeightKm"] == 3.25f && l["values"]["cloudWindDeg"] == 90.0f &&
                  l["values"]["cloudWindSpeed"] == 25.0f,
              "sky light: the cloud layer's values are written");
        Check(l["keys"].size() == 2 && l["keys"][1]["values"]["skyTimeOfDay"] == 18.25f,
              "sky light: key values carry the time of day");
    }

    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings) && warnings.empty(), "LoadProject with a sky light", "%s",
          err.c_str());
    Check(loaded.editor.lights.size() == 1, "1 sky light loaded");
    if (loaded.editor.lights.size() == 1) {
        const std::string diff = DiffLight(sky, loaded.editor.lights[0]);
        Check(diff.empty(), "sky light round-trip field by field", "differs: %s", diff.c_str());
        Check(sky == loaded.editor.lights[0], "sky light round-trip exactly (operator==)");
    }

    // A sky light without the new keys loads with the defaults, and a file of one writes no sky value keys at all.
    {
        const std::filesystem::path oldFile = projDir / "old.mmdxproj";
        {
            std::ofstream f(oldFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,"
                 "\"editor\":{\"lights\":[{\"kind\":\"sky\",\"uid\":4}]}}";
        }
        ProjectData d;
        warnings.clear();
        Check(LoadProject(oldFile, d, &err, &warnings) && d.editor.lights.size() == 1, "sky without the new keys loads");
        if (d.editor.lights.size() == 1) {
            const SceneLight& got = d.editor.lights[0];
            Check(got.v.skyTimeOfDay == 12.0f && got.v.skyNorthDeg == 0.0f && got.v.skyTurbidity == 3.0f &&
                      got.v.skyHaze == 0.2f && got.v.skyGround.x == 0.35f && got.v.skyIntensity == 1.0f &&
                      got.v.skySunIntensity == 1.0f && got.v.skyMaxElevationDeg == 60.0f,
                  "sky defaults: noon, north 0, turbidity 3, haze 0.2, grey ground, intensity 1, elevation 60");
            Check(got.skyMap.empty() && got.skyMapEncoding == "auto" && !got.skyMapMirror &&
                      got.v.skyMapRotationDeg == 0.0f && got.v.skyMapIntensity == 1.0f,
                  "sky image defaults: no map, auto encoding, no mirror, rotation 0, intensity 1");
            Check(got.v.cloudCoverage == 0.0f && got.v.cloudOpacity == 1.0f && got.v.cloudSoftness == 0.25f &&
                      got.v.cloudHeightKm == 2.0f && got.v.cloudScaleKm == 1.5f && got.v.cloudWindDeg == 0.0f &&
                      got.v.cloudWindSpeed == 10.0f,
                  "cloud defaults: no clouds, opaque, softness 0.25, 2 km, 1.5 km, north wind 10 m/s");
        }
        ProjectData plain;
        SceneLight def;
        def.uid = 5;
        def.name = "sky";
        def.kind = LightKind::Sky;
        plain.editor.lights = {def};
        const std::filesystem::path plainFile = projDir / "plain.mmdxproj";
        Check(SaveProject(plainFile, plain, &err), "SaveProject with a default sky light", "%s", err.c_str());
        std::ifstream in(plainFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        const nlohmann::json& vals = j["editor"]["lights"][0]["values"];
        Check(!vals.contains("skyTimeOfDay") && !vals.contains("skyTurbidity") && !vals.contains("skyIntensity") &&
                  !vals.contains("skyMapIntensity") && !vals.contains("cloudCoverage") && !vals.contains("cloudWindSpeed"),
              "a default sky light writes no sky value keys (an older file's bytes are kept)");
        Check(!j["editor"]["lights"][0].contains("skyMap"),
              "a sky light with no image writes no skyMap key");
    }
}

// Version 1 projects: the old lighting source + key override + spot rig become scene lights.
static void TestLegacyConversion() {
    const std::filesystem::path projDir = kTemp / "legacy";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    std::string err;
    std::vector<std::string> warnings;
    const auto writeV1 = [&](const std::string& editor) {
        std::ofstream f(projFile, std::ios::binary);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":1,\"models\":[],\"camera\":null,"
             "\"audio\":null,\"editor\":" << editor << "}";
    };
    const auto load = [&](const std::string& editor, ProjectData& out) {
        writeV1(editor);
        return LoadProject(projFile, out, &err, &warnings);
    };
    const auto preset = [](int index) {
        uint32_t next = 1;
        return PresetLights(index, XMFLOAT3{0.0f, 10.0f, 0.0f}, next);
    };
    const char* key = "\"key\":{\"direction\":[0.25,-0.5,0.75],\"color\":[1.0,0.5,0.25],\"intensity\":1.5,"
                      "\"rimStrength\":0.5,\"rimColor\":[0.5,0.25,0.125]}";

    {   // source "vmd": the preset's lights, the sun linked; the key override, spots and fill did not apply
        ProjectData d;
        const std::string body = std::string("{\"lighting\":{\"source\":\"vmd\",\"preset\":2,") + key +
                                 ",\"spots\":[{\"name\":\"x\",\"mode\":\"manual\"}],\"frontFill\":true}}";
        Check(load(body, d), "legacy vmd: LoadProject", "%s", err.c_str());
        const std::vector<SceneLight> want = preset(2);
        Check(DiffLights(want, d.editor.lights).empty() && d.editor.lights[0].vmdLink,
              "legacy source vmd: the Concert preset's lights, the sun linked to the VMD", "differs: %s",
              DiffLights(want, d.editor.lights).c_str());
    }
    {   // source "preset" without an override: the preset's lights, the sun unlinked
        ProjectData d;
        Check(load("{\"lighting\":{\"source\":\"preset\",\"preset\":1}}", d), "legacy preset: LoadProject", "%s", err.c_str());
        std::vector<SceneLight> want = preset(1);
        want[0].vmdLink = false;
        Check(DiffLights(want, d.editor.lights).empty(), "legacy source preset: the Sunset lights, the sun unlinked",
              "differs: %s", DiffLights(want, d.editor.lights).c_str());
    }
    {   // source "preset" with the key override: it replaces the sun's light and rim
        ProjectData d;
        const std::string body = std::string("{\"lighting\":{\"source\":\"preset\",\"preset\":3,") + key + "}}";
        Check(load(body, d), "legacy preset + key: LoadProject", "%s", err.c_str());
        std::vector<SceneLight> want = preset(3);
        want[0].vmdLink = false;
        want[0].v.direction = {0.25f, -0.5f, 0.75f};
        want[0].v.color = {1.0f, 0.5f, 0.25f};
        want[0].v.intensity = 1.5f;
        want[0].rimStrength = 0.5f;
        want[0].rimColor = {0.5f, 0.25f, 0.125f};
        Check(DiffLights(want, d.editor.lights).empty(), "legacy source preset with a key override: the sun takes it, unlinked",
              "differs: %s", DiffLights(want, d.editor.lights).c_str());
    }
    {   // source "custom": the sun and environment of the preset, the rig's spots in order, the front fill
        ProjectData d;
        const std::string body = std::string("{\"lighting\":{\"source\":\"custom\",\"preset\":2,") + key + ",\"frontFill\":true,\"spots\":["
            "{\"name\":\"A\",\"mode\":\"auto\",\"position\":[10,40,-12],\"aim\":[2,0,0],\"color\":[0.25,0.5,0.75],\"intensity\":2.5,"
            "\"cone\":0.5,\"enabled\":true,\"phase\":1.5,\"keys\":["
            "{\"frame\":30,\"position\":[6,38,-8],\"aim\":[-2,2,1],\"color\":[0.75,0.5,0.25],\"intensity\":3.5,\"cone\":0.25},"
            "{\"frame\":0,\"position\":[10,40,-12],\"aim\":[2,0,0],\"color\":[0.25,0.5,0.75],\"intensity\":2.5,\"cone\":0.5}]},"
            "{\"name\":\"B\",\"mode\":\"center\",\"position\":[-8,40,-12],\"aim\":[-6,0,0],\"enabled\":false,\"phase\":0.5},"
            "{\"name\":\"C\",\"mode\":\"head\",\"position\":[1,2,3],\"intensity\":1.25},"
            "{\"name\":\"D\",\"mode\":\"manual\",\"position\":[4,5,6],\"aim\":[7,8,9],\"cone\":0.125},"
            "{\"name\":\"D\",\"mode\":\"auto\"},{\"mode\":\"auto\"}]}}";
        Check(load(body, d) && warnings.empty(), "legacy custom: LoadProject", "%s", err.c_str());
        const std::vector<SceneLight>& l = d.editor.lights;
        Check(l.size() == 7, "legacy custom: sun, ambient, four spots (nameless and repeated ones dropped), the fill",
              "size=%zu", l.size());
        if (l.size() == 7) {
            bool uids = true;
            for (size_t i = 0; i < l.size(); ++i) uids = uids && l[i].uid == i + 1;
            Check(uids, "legacy custom: uids 1..N in list order");
            const std::vector<SceneLight> p = preset(2);
            Check(l[0].kind == LightKind::Sun && !l[0].vmdLink && SameVec3(l[0].v.direction, {0.25f, -0.5f, 0.75f}) &&
                      SameVec3(l[0].v.color, {1.0f, 0.5f, 0.25f}) && l[0].v.intensity == 1.5f && l[0].rimStrength == 0.5f &&
                      SameVec3(l[0].rimColor, {0.5f, 0.25f, 0.125f}),
                  "legacy custom: the sun with the key override, unlinked");
            Check(l[1].kind == LightKind::Ambient && SameVec3(l[1].skyZenith, p[1].skyZenith) &&
                      SameVec3(l[1].skyHorizon, p[1].skyHorizon) && SameVec3(l[1].groundColor, p[1].groundColor) &&
                      l[1].v.intensity == p[1].v.intensity,
                  "legacy custom: the preset's environment is kept (its own spots and fill are not)");
            // A: auto swing
            const SceneLight& a = l[2];
            Check(a.kind == LightKind::Spot && a.name == "A" && a.enabled && a.aimMode == AimMode::Sway && a.swayPhase == 1.5f &&
                      SameVec3(a.v.position, {10, 40, -12}) && SameVec3(a.v.aim, {2, 0, 0}) && SameVec3(a.v.color, {0.25f, 0.5f, 0.75f}) &&
                      a.v.intensity == 2.5f && a.v.coneOuter == 0.5f && a.v.coneInner == std::min(0.5f * 0.6f, 0.5f) &&
                      a.v.range == 140.0f,
                  "legacy spot auto: Sway about the old aim, phase, values, inner cone 0.6 * outer, range 140");
            Check(a.keys.size() == 2 && a.keys[0].frame == 0 && a.keys[1].frame == 30 &&
                      SameVec3(a.keys[1].v.position, {6, 38, -8}) && SameVec3(a.keys[1].v.aim, {-2, 2, 1}) &&
                      SameVec3(a.keys[1].v.color, {0.75f, 0.5f, 0.25f}) && a.keys[1].v.intensity == 3.5f &&
                      a.keys[1].v.coneOuter == 0.25f && a.keys[1].v.coneInner == std::min(0.25f * 0.6f, 0.25f) &&
                      a.keys[1].v.range == 140.0f,
                  "legacy spot keys: sorted, values and inner cone converted");
            // B: follow the centre, disabled
            Check(l[3].name == "B" && !l[3].enabled && l[3].aimMode == AimMode::Target && l[3].targetUid == 0 &&
                      l[3].targetPart == TargetPart::Centre && SameVec3(l[3].v.aim, {-6, 0, 0}) && l[3].swayPhase == 0.5f,
                  "legacy spot center: Target (the performer), centre, disabled kept");
            // C: follow the head, old defaults for missing values
            Check(l[4].name == "C" && l[4].aimMode == AimMode::Target && l[4].targetPart == TargetPart::Head &&
                      SameVec3(l[4].v.position, {1, 2, 3}) && l[4].v.intensity == 1.25f && l[4].v.coneOuter == 0.24f &&
                      SameVec3(l[4].v.color, {1.0f, 0.98f, 0.92f}) && l[4].enabled,
                  "legacy spot head: Target, head; the old defaults (colour, cone) fill what the file lacks");
            // D: manual
            Check(l[5].name == "D" && l[5].aimMode == AimMode::Manual && SameVec3(l[5].v.aim, {7, 8, 9}) &&
                      l[5].v.coneOuter == 0.125f && l[5].v.coneInner == std::min(0.125f * 0.6f, 0.125f),
                  "legacy spot manual: Manual with the old aim");
            bool defaults = true;
            for (int i = 2; i < 6; ++i)
                defaults = defaults && l[i].shadow == ShadowType::Hard && l[i].shadowSoftness == 0.5f && l[i].shadowDensity == 1.0f &&
                           SameVec3(l[i].shadowColor, {0, 0, 0}) && l[i].falloff == FalloffType::None && l[i].affectDiffuse &&
                           l[i].affectSpecular && l[i].viewportVisible;
            Check(defaults, "legacy spots keep the defaults of the common properties");
            // the front fill
            Check(l[6].kind == LightKind::Point && l[6].name == "\xEC\xB1\x84\xEC\x9B\x80\xEA\xB4\x91" && l[6].enabled &&  // 채움광
                      SameVec3(l[6].v.position, {0, 32, -40}) && SameVec3(l[6].v.color, {0.917f, 0.83f, 0.72f}) &&
                      l[6].v.intensity == 0.55f && l[6].v.range == 120.0f && l[6].falloff == FalloffType::None,
                  "legacy front fill: a point light at (0, 32, -40), warm colour, intensity 0.55, range 120");
        }
    }
    {   // custom without spots and without the fill: only the preset's sun and environment remain
        ProjectData d;
        Check(load("{\"lighting\":{\"source\":\"custom\",\"preset\":2,\"frontFill\":false}}", d), "legacy custom empty: LoadProject",
              "%s", err.c_str());
        const std::vector<SceneLight> p = preset(2);
        Check(d.editor.lights.size() == 2 && d.editor.lights[0].kind == LightKind::Sun && !d.editor.lights[0].vmdLink &&
                  d.editor.lights[1].kind == LightKind::Ambient && d.editor.lights[0].uid == 1 && d.editor.lights[1].uid == 2 &&
                  SameVec3(d.editor.lights[0].v.color, p[0].v.color),
              "legacy custom without spots: the Concert preset's own spots and fill are gone");
    }
    {   // a broken source name keeps the old default: the VMD track; the spots did not apply
        ProjectData d;
        Check(load("{\"lighting\":{\"source\":\"alien\",\"spots\":[]}}", d), "legacy broken source: LoadProject", "%s", err.c_str());
        Check(DiffLights(preset(0), d.editor.lights).empty(), "legacy broken source: the Studio preset, the sun linked");
    }
    {   // no lighting information at all: the old default, the VMD track over the Studio preset
        ProjectData d;
        Check(load("{\"frame\":3}", d), "legacy plain editor: LoadProject", "%s", err.c_str());
        Check(DiffLights(preset(0), d.editor.lights).empty(), "legacy plain project: the Studio preset, the sun linked");
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":1,\"models\":[]}";
        }
        ProjectData d2;
        Check(LoadProject(projFile, d2, &err, &warnings) && DiffLights(preset(0), d2.editor.lights).empty(),
              "legacy project without an editor object: the Studio preset, the sun linked");
    }
}

// A legacy v1 project without the "lighting" object: useLightTrack true + camera light keys -> the sun linked to the
// VMD; useLightTrack true without a camera VMD light track -> the preset (unlinked); false -> the preset (unlinked).
static void TestLegacyLightMigration() {
    const std::filesystem::path projDir = kTemp / "legacy2";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "scene.mmdxproj";
    std::string err;
    std::vector<std::string> warnings;
    uint32_t next = 1;
    const std::vector<SceneLight> studioPreset = PresetLights(0, XMFLOAT3{0.0f, 10.0f, 0.0f}, next);

    // a minimal v1 project with an editor block; the camera VMD is referenced when it has light keys
    const auto writeV1 = [&](const char* editor) {
        std::ofstream f(projFile, std::ios::binary);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":1,\"models\":[],\"camera\":null,"
             "\"audio\":null,\"editor\":" << editor << "}";
    };

    // (a) useLightTrack: true but no camera VMD (no light keys) -> the preset, unlinked
    {
        writeV1("{\"frame\":0,\"useLightTrack\":true}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (a): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lights.size() == studioPreset.size() && !loaded.editor.lights[0].vmdLink &&
                  loaded.editor.lights[0].v == studioPreset[0].v,
              "legacy true without light keys -> the preset, the sun not linked");
    }
    // (b) useLightTrack: false -> the preset, unlinked
    {
        writeV1("{\"frame\":0,\"useLightTrack\":false}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (b): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lights.size() == studioPreset.size() && !loaded.editor.lights[0].vmdLink,
              "legacy false -> the preset, the sun not linked");
    }
    // (b2) useLightTrack: true and a camera VMD that carries a light key -> the sun linked to the VMD
    {
        ProjectData data;
        LightKf l;
        l.frame = 0;
        l.color = {0.7f, 0.8f, 0.9f};
        l.direction = {0.1f, -1.0f, 0.2f};
        data.camera.light = {l};
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
        Check(loaded.editor.lights.size() == studioPreset.size() && loaded.editor.lights[0].vmdLink,
              "legacy true with light keys -> the sun linked to the VMD");
        Check(!loaded.camera.light.empty(), "legacy (b2): light track loaded");
    }
    // (c) an editor without any light field: the old default (the VMD track) -> linked
    {
        writeV1("{\"frame\":3}");
        ProjectData loaded;
        Check(LoadProject(projFile, loaded, &err, &warnings), "legacy (c): LoadProject", "%s", err.c_str());
        Check(loaded.editor.lights.size() == studioPreset.size() && loaded.editor.lights[0].vmdLink,
              "legacy plain project -> the sun linked (the old default source)");
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
    { "name": "prop2", "kind": "prop", "path": "p2.pmx", "attach": { "parent": 1 } },
    { "name": "eff", "kind": "effect", "path": "e.x", "fx": "e.fx" }
  ]
})";
    }
    ProjectData loaded;
    std::string err;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with unknown kind -> true", "%s", err.c_str());
    Check(loaded.models.size() == 4, "4 models loaded (unknown one skipped)", "size=%zu", loaded.models.size());
    Check(warnings.size() == 1, "one warning for the unknown kind", "size=%zu", warnings.size());
    if (loaded.models.size() == 4) {
        // after skipping the unknown kind: char -> 0, prop -> 1, prop2 -> 2, eff -> 3
        Check(loaded.models[1].kind == ModelKind::Prop && loaded.models[1].attach.parent == 0,
              "prop parent 0 remapped to index 0");
        Check(loaded.models[2].kind == ModelKind::Prop && loaded.models[2].attach.parent == -1,
              "prop2 parent pointing at the skipped model becomes -1");
        Check(loaded.models[3].kind == ModelKind::Effect && !loaded.models[3].fx.empty(),
              "effect object kind + fx read");
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

static void TestCameraEntities() {
    const std::filesystem::path projDir = kTemp / "cameras";
    const std::filesystem::path projFile = projDir / "cameras.mmdxproj";
    ProjectData data;
    // the main camera (uid 0) keeps the light / shadow tracks
    {
        CameraKf k;
        k.frame = 0;
        k.target = {0.0f, 10.0f, 0.0f};
        k.distance = -45.0f;
        k.fovDeg = 17.7258f;
        data.camera.camera = {k};
        LightKf l;
        l.frame = 0;
        l.color = {0.6f, 0.6f, 0.6f};
        data.camera.light = {l};
    }
    // S4: two extra cameras, one from a stage's stage.json, and a switch track
    {
        StudioCamera a;
        a.uid = 1;
        a.name = "wide";
        a.visible = true;
        CameraKf k;
        k.frame = 0;
        k.target = {0.0f, 12.0f, 0.0f};
        k.distance = -70.0f;
        k.fovDeg = 17.7258f;
        a.track.camera = {k};
        data.cameras.push_back(a);

        StudioCamera b;
        b.uid = 2;
        b.name = "close";
        b.stageUid = 4;
        b.visible = false;
        CameraKf k2;
        k2.frame = 30;
        k2.target = {0.0f, 9.0f, 0.0f};
        k2.distance = -18.0f;
        b.track.camera = {k2};
        data.cameras.push_back(b);

    }
    std::string err;
    Check(SaveProject(projFile, data, &err), "SaveProject with cameras", "%s", err.c_str());
    {
        std::ifstream in(projFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        Check(!j.is_discarded() && j.value("version", 0) == 6, "camera project format is version 6");
        Check(j.contains("cameras") && j["cameras"].is_array() && j["cameras"].size() == 2 &&
                  j["cameras"][0]["uid"] == 1 && j["cameras"][0]["name"] == "wide" &&
                  j["cameras"][0]["visible"] == true && j["cameras"][0]["motion"].is_string() &&
                  j["cameras"][1]["uid"] == 2 && j["cameras"][1]["stageUid"] == 4 &&
                  j["cameras"][1]["visible"] == false,
              "cameras are written as uid / name / visible / stageUid / motion");
        Check(j["camera"].is_string(), "the main camera keeps its own VMD");
        Check(j.contains("fovExact") && j["fovExact"].size() == 1 && j["fovExact"][0][0] == 0,
              "main camera fractional FOV is saved");
        Check(j["cameras"][0].contains("fovExact") && !j["cameras"][1].contains("fovExact"),
              "extra camera fractional FOV is saved and integer FOV is omitted");
    }
    ProjectData loaded;
    std::vector<std::string> warnings;
    Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with cameras", "%s", err.c_str());
    Check(loaded.cameras.size() == data.cameras.size(), "extra camera count kept");
    if (loaded.cameras.size() == data.cameras.size()) {
        Check(loaded.cameras[0] == data.cameras[0], "an extra camera round-trips exactly (ignoring uid)");
        Check(loaded.cameras[1] == data.cameras[1], "a stage camera round-trips exactly (ignoring uid)");
        Check(loaded.cameras[0].uid == 1 && loaded.cameras[1].uid == 2, "extra camera uids survive");
        Check(loaded.cameras[1].stageUid == 4 && !loaded.cameras[1].visible, "stageUid / visible survive");
    }
    Check(loaded.camera.camera.size() == 1 && loaded.camera.light.size() == 1, "the main camera keeps its tracks");

    if (!loaded.camera.camera.empty())
        Check(std::fabs(loaded.camera.camera[0].fovDeg - 17.7258f) < 1e-4f, "main camera fractional FOV round-trips");
    if (!loaded.cameras.empty() && !loaded.cameras[0].track.camera.empty())
        Check(std::fabs(loaded.cameras[0].track.camera[0].fovDeg - 17.7258f) < 1e-4f, "extra camera fractional FOV round-trips");

    // a version 4 file has the main camera only
    {
        const std::filesystem::path legacy = kTemp / "cameras" / "legacy.mmdxproj";
        std::ofstream f(legacy);
        f << "{\"format\":\"mmdx12-studio-project\",\"version\":4,\"models\":[],\"camera\":null,\"audio\":null}";
        f.close();
        ProjectData l2;
        warnings.clear();
        Check(LoadProject(legacy, l2, &err, &warnings), "LoadProject (version 4) with cameras", "%s", err.c_str());
        Check(l2.cameras.empty(), "a version 4 file has no extra cameras");
    }
    // a camera that shares a name with another one still gets its own VMD (unique file names)
    {
        ProjectData dup = data;
        dup.cameras[1].name = dup.cameras[0].name;
        const std::filesystem::path dupFile = kTemp / "cameras" / "dup.mmdxproj";
        Check(SaveProject(dupFile, dup, &err), "SaveProject with two same-named cameras", "%s", err.c_str());
        ProjectData d2;
        warnings.clear();
        Check(LoadProject(dupFile, d2, &err, &warnings), "LoadProject with two same-named cameras", "%s", err.c_str());
        Check(d2.cameras.size() == 2 && !d2.cameras[0].track.camera.empty() && !d2.cameras[1].track.camera.empty(),
              "same-named cameras do not share one VMD");
    }
    // saving the same project twice must not delete the extra cameras' own VMDs (the stale-VMD cleanup knows them)
    {
        const std::filesystem::path twice = kTemp / "cameras" / "twice.mmdxproj";
        Check(SaveProject(twice, data, &err), "SaveProject a first time (cameras)", "%s", err.c_str());
        Check(SaveProject(twice, data, &err), "SaveProject a second time (cameras)", "%s", err.c_str());
        ProjectData d3;
        warnings.clear();
        Check(LoadProject(twice, d3, &err, &warnings), "LoadProject after two saves", "%s", err.c_str());
        Check(d3.cameras.size() == 2 && !d3.cameras[0].track.camera.empty() && !d3.cameras[1].track.camera.empty(),
              "a second save keeps the extra cameras' VMDs");
        Check(warnings.empty(), "a second save writes no missing-camera warning");
    }
}

static void TestAspectProject() {
    const std::filesystem::path projDir = kTemp / "aspect";
    std::filesystem::create_directories(projDir);
    const std::filesystem::path projFile = projDir / "test.mmdxproj";
    std::string err;
    std::vector<std::string> warnings;

    // 1. Test saving and loading each preset
    for (int i = 0; i < kAspectPresetCount; ++i) {
        ProjectData d;
        d.editor.aspectW = kAspectPresets[i].w;
        d.editor.aspectH = kAspectPresets[i].h;
        Check(SaveProject(projFile, d, &err), "SaveProject aspect preset", "%s", kAspectPresets[i].label);

        ProjectData loaded;
        warnings.clear();
        Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject aspect preset", "%s", kAspectPresets[i].label);
        Check(loaded.editor.aspectW == kAspectPresets[i].w && loaded.editor.aspectH == kAspectPresets[i].h,
              "aspect ratio round-trip matches preset", "%s", kAspectPresets[i].label);
    }

    // 2. Missing aspect in editor defaults to 16:9
    {
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{}}";
        }
        ProjectData loaded;
        warnings.clear();
        Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject without aspect field");
        Check(loaded.editor.aspectW == 16 && loaded.editor.aspectH == 9, "missing aspect defaults to 16:9");
    }

    // 3. Unknown aspect values fall back to 16:9
    {
        // Unknown w/h in object
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{\"aspect\":{\"w\":7,\"h\":3}}}";
        }
        ProjectData loaded;
        warnings.clear();
        Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with unknown aspect object");
        Check(loaded.editor.aspectW == 16 && loaded.editor.aspectH == 9, "unknown aspect object {7,3} falls back to 16:9");

        // Unknown label string
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{\"aspect\":\"unknown\"}}";
        }
        ProjectData loaded2;
        warnings.clear();
        Check(LoadProject(projFile, loaded2, &err, &warnings), "LoadProject with unknown aspect string");
        Check(loaded2.editor.aspectW == 16 && loaded2.editor.aspectH == 9, "unknown aspect string falls back to 16:9");
    }

    // 4. String preset name loads correctly
    {
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{\"aspect\":\"9:16\"}}";
        }
        ProjectData loaded;
        warnings.clear();
        Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with string aspect \"9:16\"");
        Check(loaded.editor.aspectW == 9 && loaded.editor.aspectH == 16, "string aspect \"9:16\" loads as 9:16");
    }

    // 5. Array [w, h] loads correctly
    {
        {
            std::ofstream f(projFile, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":2,\"models\":[],\"camera\":null,\"audio\":null,\"editor\":{\"aspect\":[4,5]}}";
        }
        ProjectData loaded;
        warnings.clear();
        Check(LoadProject(projFile, loaded, &err, &warnings), "LoadProject with array aspect [4,5]");
        Check(loaded.editor.aspectW == 4 && loaded.editor.aspectH == 5, "array aspect [4,5] loads as 4:5");
    }
}

// The project's gravity and wind ("physicsWorld"): round trip, the defaults of a file without it, the bounds.
static void TestPhysicsWorld() {
    const std::filesystem::path file = kTemp / "physics.mmdxproj";
    std::string err;
    ProjectData p;
    PhysicsEnv& e = p.physicsEnv;
    e.gravity = 3.5f;
    e.gravityDir = {0.5f, -1.0f, 0.25f};
    e.windDir = {-1.0f, 0.0f, 0.5f};
    e.windStrength = 0.4f;
    e.windGust = 0.2f;
    e.windGustFreq = 5.0f;
    e.windJitter = 0.3f;
    {
        ProjectData loaded;
        Check(SaveProject(file, p, &err) && LoadProject(file, loaded, &err, nullptr), "physics world round trip",
              "%s", err.c_str());
        Check(loaded.physicsEnv == e, "gravity and wind persist");
    }
    // older files have none: MMD's default gravity and no wind, as they were simulated
    {
        {
            std::ofstream f(file, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":6,\"models\":[],\"camera\":null,\"audio\":null}";
        }
        ProjectData loaded;
        Check(LoadProject(file, loaded, &err, nullptr) && loaded.physicsEnv == PhysicsEnv{} &&
                  loaded.physicsEnv.DefaultGravity() && !loaded.physicsEnv.HasWind(),
              "no physicsWorld: default gravity, no wind");
    }
    // partial and out-of-range values: bounded, a direction keeps its heading, missing fields keep their defaults
    {
        {
            std::ofstream f(file, std::ios::binary);
            f << "{\"format\":\"mmdx12-studio-project\",\"version\":6,\"models\":[],\"camera\":null,\"audio\":null,"
                 "\"physicsWorld\":{\"gravity\":{\"acceleration\":-5,\"direction\":[0,4,2]},"
                 "\"wind\":{\"strength\":50,\"jitter\":3}}}";
        }
        ProjectData loaded;
        const PhysicsEnv& b = loaded.physicsEnv;
        Check(LoadProject(file, loaded, &err, nullptr) && b.gravity == 0.0f && Vec3Eq(b.gravityDir, {0, 1, 0.5f}, 1e-6f) &&
                  b.windStrength == 10.0f && b.windJitter == 1.0f && b.windGustFreq == 2.0f && b.windGust == 0.0f &&
                  Vec3Eq(b.windDir, {0, 0, 1}, 0.0f),
              "physics world bounded, missing fields default", "gravity %g dir (%g,%g,%g) wind %g jitter %g", b.gravity,
              b.gravityDir.x, b.gravityDir.y, b.gravityDir.z, b.windStrength, b.windJitter);
    }
    PhysicsEnv bad;
    bad.gravity = std::nanf("");
    bad.gravityDir.y = std::nanf("");
    bad.windGustFreq = 0.0f;
    bad.Sanitize();
    Check(bad.gravity == PhysicsEnv::kDefaultGravity && Vec3Eq(bad.gravityDir, {0, -1, 0}, 0.0f) &&
              bad.windGustFreq == 0.05f,
          "physics world invalid values fall back");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash mid-test loses buffered PASS lines
    std::error_code ec;
    std::filesystem::remove_all(kTemp, ec);
    std::filesystem::create_directories(kTemp, ec);

    TestRoundTrip();
    TestEffects();
    TestWorlds();
    TestStages();
    TestCameraEntities();
    TestAspectProject();
    TestSceneLightFile();
    TestSceneLightReading();
    TestAreaLightProject();
    TestFogVolumeProject();
    TestSkyLightProject();
    TestLegacyConversion();
    TestLegacyLightMigration();
    TestDuplicateNamesAndCleanup();
    TestMissingAndBad();
    TestUnknownKindRemap();
    TestAtomic();
    TestPropOffsetMatrix();
    TestPhysicsWorld();

    std::filesystem::remove_all(kTemp, ec);

    std::printf("studio_project_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
