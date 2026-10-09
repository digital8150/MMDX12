// Unit tests for the studio scene lights: SceneLight.h (key sampling, presets), the light rows of StudioDoc.h (track
// snapshots, LightsCommand), BuildSceneLighting (app/Lighting.h) and the camera VMD light-track filter of the loader.
// Imitates studio_project_test.cpp: a tiny CHECK, PASS/FAIL per check and a summary, exit code 0 on success.
#include "app/Lighting.h"
#include "asset/AssetLibrary.h"
#include "render/Renderer.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"
#include <DirectXMath.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace mmdx;
using namespace mmdx::studio;
using DirectX::XMFLOAT3;

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

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static bool Near3(const XMFLOAT3& a, const XMFLOAT3& b, float tol) {
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
}
static bool Same3(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// The first field in which two LightParams differ ("" when they match within `tol`).
static std::string DiffParams(const LightParams& a, const LightParams& b, float tol) {
    if (!Near3(a.direction, b.direction, tol)) return "direction";
    if (!Near3(a.color, b.color, tol)) return "color";
    if (!Near(a.sunIntensity, b.sunIntensity, tol)) return "sunIntensity";
    if (!Near3(a.skyZenith, b.skyZenith, tol)) return "skyZenith";
    if (!Near3(a.skyHorizon, b.skyHorizon, tol)) return "skyHorizon";
    if (!Near3(a.groundColor, b.groundColor, tol)) return "groundColor";
    if (!Near(a.hemiStrength, b.hemiStrength, tol)) return "hemiStrength";
    if (!Near(a.rimStrength, b.rimStrength, tol)) return "rimStrength";
    if (!Near3(a.rimColor, b.rimColor, tol)) return "rimColor";
    if (a.sunShadow != b.sunShadow) return "sunShadow";
    if (!Near(a.sunShadowSoftness, b.sunShadowSoftness, tol)) return "sunShadowSoftness";
    if (!Near(a.sunShadowDensity, b.sunShadowDensity, tol)) return "sunShadowDensity";
    if (!Near3(a.sunShadowColor, b.sunShadowColor, tol)) return "sunShadowColor";
    if (a.punctual.size() != b.punctual.size()) return "punctual.size";
    for (size_t i = 0; i < a.punctual.size(); ++i) {
        const PunctualLight &p = a.punctual[i], &q = b.punctual[i];
        const std::string at = "punctual[" + std::to_string(i) + "].";
        if (!Near3(p.position, q.position, tol)) return at + "position";
        if (!Near3(p.direction, q.direction, tol)) return at + "direction";
        if (!Near(p.range, q.range, tol)) return at + "range";
        if (!Near3(p.color, q.color, tol)) return at + "color";
        if (!Near(p.intensity, q.intensity, tol)) return at + "intensity";
        if (!Near(p.spotCosOuter, q.spotCosOuter, tol)) return at + "spotCosOuter";
        if (!Near(p.spotCosInner, q.spotCosInner, tol)) return at + "spotCosInner";
        if (p.shadow != q.shadow) return at + "shadow";
        if (!Near(p.shadowSoftness, q.shadowSoftness, tol)) return at + "shadowSoftness";
        if (!Near(p.shadowDensity, q.shadowDensity, tol)) return at + "shadowDensity";
        if (!Near3(p.shadowColor, q.shadowColor, tol)) return at + "shadowColor";
        if (p.falloff != q.falloff) return at + "falloff";
        if (p.affectDiffuse != q.affectDiffuse) return at + "affectDiffuse";
        if (p.affectSpecular != q.affectSpecular) return at + "affectSpecular";
    }
    return "";
}

static const SceneLight* FindKind(const std::vector<SceneLight>& lights, LightKind kind) {
    for (const SceneLight& l : lights)
        if (l.kind == kind) return &l;
    return nullptr;
}

static SceneLight MakeSpot(float x, float y, float z) {
    SceneLight s;
    s.uid = 1;
    s.name = "spot";
    s.kind = LightKind::Spot;
    s.v.position = {x, y, z};
    s.v.aim = {0, 0, 0};
    s.v.color = {1, 1, 1};
    s.v.intensity = 2.0f;
    return s;
}

// ---- (a) presets: PresetLights through BuildSceneLighting equals BuildLighting ----
static void TestPresetRegression() {
    const XMFLOAT3 focus{1.5f, 12.0f, -3.0f};
    LightAnchors anchors;
    anchors.focus = focus;
    anchors.head = {focus.x, focus.y + 8.0f, focus.z};
    for (int p = 0; p < kLightingPresetCount; ++p) {
        for (const double t : {0.0, 3.7, 12.5}) {
            LightParams ref;
            BuildLighting((LightingPreset)p, t, focus, ref);
            uint32_t next = 1;
            const std::vector<SceneLight> lights = PresetLights(p, focus, next);
            LightParams got;
            BuildSceneLighting(lights, {}, t, anchors, got);
            const std::string diff = DiffParams(ref, got, 1e-4f);
            char name[96];
            std::snprintf(name, sizeof(name), "preset %d at %.1f s equals BuildLighting", p, t);
            Check(diff.empty(), name, "differs in %s", diff.c_str());
        }
    }
    // the comparison is not vacuous: the concert beams sway with the song time
    uint32_t next = 1;
    const std::vector<SceneLight> concert = PresetLights(2, focus, next);
    LightParams a, b;
    BuildSceneLighting(concert, {}, 0.0, anchors, a);
    BuildSceneLighting(concert, {}, 3.7, anchors, b);
    Check(a.punctual.size() == 7 && b.punctual.size() == 7 && !Near3(a.punctual[0].direction, b.punctual[0].direction, 0.5f),
          "concert: six spots + the fill, the beams move with the time");
}

// ---- PresetLights: order, names, uids, the defaults of the common properties ----
static void TestPresetStructure() {
    const XMFLOAT3 focus{1.5f, 12.0f, -3.0f};
    for (int p = 0; p < kLightingPresetCount; ++p) {
        uint32_t next = 5;
        const std::vector<SceneLight> lights = PresetLights(p, focus, next);
        const size_t want = p == 2 ? 9 : 2;
        char name[96];
        std::snprintf(name, sizeof(name), "preset %d: %zu lights, sun then ambient, uids from the counter", p, want);
        bool ok = lights.size() == want && next == 5 + (uint32_t)want && lights[0].kind == LightKind::Sun &&
                  lights[1].kind == LightKind::Ambient && lights[0].name == "메인 조명" && lights[1].name == "환경광";
        for (size_t i = 0; ok && i < lights.size(); ++i) ok = lights[i].uid == 5 + (uint32_t)i && lights[i].enabled;
        Check(ok, name, "size=%zu next=%u", lights.size(), next);
    }
    uint32_t next = 1;
    const std::vector<SceneLight> clamped = PresetLights(99, focus, next);
    uint32_t next2 = 1;
    Check(clamped.size() == PresetLights(3, focus, next2).size() && clamped[0].v.color.x == PresetLights(3, focus, next2)[0].v.color.x,
          "an out-of-range preset index clamps (to Night)");

    next = 1;
    const std::vector<SceneLight> concert = PresetLights(2, focus, next);
    bool names = true, kinds = true;
    for (int i = 0; i < 6; ++i) {
        names = names && concert[2 + i].name == "스팟 " + std::to_string(i + 1);
        kinds = kinds && concert[2 + i].kind == LightKind::Spot && concert[2 + i].aimMode == AimMode::Sway;
    }
    Check(names && kinds && concert[8].kind == LightKind::Point && concert[8].name == "채움광",
          "concert: spots 1..6 (sway) then the fill light");
    Check(Near3(concert[2].v.position, {focus.x - 14.0f, 58.0f, focus.z - 18.0f}, 1e-5f) &&
              Near3(concert[3].v.position, {focus.x + 14.0f, 58.0f, focus.z - 18.0f}, 1e-5f) &&
              Near3(concert[4].v.position, {focus.x - 23.0f, 58.0f, focus.z - 12.0f}, 1e-5f) &&
              Near3(concert[2].v.aim, {focus.x + 3.5f, 0.0f, focus.z}, 1e-5f) && Near(concert[3].swayPhase, 1.3f, 1e-6f) &&
              concert[2].v.intensity == 2.6f && concert[2].v.range == 140.0f && concert[2].v.coneOuter == 0.24f &&
              concert[2].v.coneInner == 0.15f,
          "concert spot positions, aims, phases and cone");
    Check(Near3(concert[8].v.position, {focus.x, focus.y + 22.0f, focus.z - 40.0f}, 1e-5f) && concert[8].v.intensity == 0.55f &&
              concert[8].v.range == 120.0f,
          "concert fill light position, intensity, range");

    // the defaults of the common properties: every light Hard (the ray paths always shadowed the fill: no look change), falloff None
    bool shadows = true, falloff = true, common = true;
    for (int p = 0; p < kLightingPresetCount; ++p) {
        next = 1;
        for (const SceneLight& l : PresetLights(p, focus, next)) {
            shadows = shadows && l.shadow == ShadowType::Hard;
            falloff = falloff && l.falloff == FalloffType::None;
            common = common && l.shadowSoftness == 0.5f && l.shadowDensity == 1.0f && Same3(l.shadowColor, {0, 0, 0}) &&
                     l.affectDiffuse && l.affectSpecular && l.viewportVisible;
        }
    }
    Check(shadows, "preset lights: every light Hard");
    Check(falloff, "preset lights: falloff None");
    Check(common, "preset lights: softness 0.5, density 1, black shadow colour, affect diffuse / specular, visible");
    Check(concert[0].vmdLink && concert[0].shadow == ShadowType::Hard && concert[1].shadow == ShadowType::Hard,
          "preset sun is linked to the VMD; the ambient light keeps the struct default shadow type");
    const SceneLight def;
    Check(def.shadow == ShadowType::Hard && def.shadowSoftness == 0.5f && def.shadowDensity == 1.0f &&
              Same3(def.shadowColor, {0, 0, 0}) && def.falloff == FalloffType::None && def.affectDiffuse &&
              def.affectSpecular && def.viewportVisible,
          "SceneLight struct defaults of the common properties");
}

// ---- (b) no sun ----
static void TestNoSun() {
    LightAnchors anchors;
    uint32_t next = 1;
    for (int disabled = 0; disabled < 2; ++disabled) {
        std::vector<SceneLight> lights = PresetLights(0, {0, 10, 0}, next);
        if (disabled) lights[0].enabled = false;
        else lights.erase(lights.begin());
        LightParams out;
        BuildSceneLighting(lights, {}, 0.0, anchors, out);
        const char* name = disabled ? "a disabled sun counts as no sun: black key light, sunIntensity 1, no rim"
                                    : "no sun: black key light, sunIntensity 1, no rim";
        Check(Same3(out.color, {0, 0, 0}) && out.sunIntensity == 1.0f && out.rimStrength == 0.0f &&
              out.sunShadow == LightShadowType::NoCast && out.sunShadowDensity == 0.0f, name);
        const SceneLight* amb = FindKind(lights, LightKind::Ambient);
        const bool skyLit = (out.skyZenith.x + out.skyZenith.y + out.skyZenith.z) > 0.0f &&
                            (out.skyHorizon.x + out.skyHorizon.y + out.skyHorizon.z) > 0.0f &&
                            (out.groundColor.x + out.groundColor.y + out.groundColor.z) > 0.0f;
        Check(skyLit && amb && Same3(out.skyZenith, amb->skyZenith) && out.hemiStrength == amb->v.intensity,
              "no sun: the environment stays lit (sky colours not black, ambient light applied)");
    }
    LightParams none;
    BuildSceneLighting({}, {}, 0.0, anchors, none);
    const LightParams def;
    Check(Same3(none.color, {0, 0, 0}) && none.sunIntensity == 1.0f && none.hemiStrength == 0.0f && none.punctual.empty() &&
              Same3(none.skyZenith, def.skyZenith),
          "no lights at all: black key light, no hemisphere, the default sky colours, nothing punctual");
}

// ---- (c) no ambient ----
static void TestNoAmbient() {
    LightAnchors anchors;
    const LightParams def;
    uint32_t next = 1;
    for (int disabled = 0; disabled < 2; ++disabled) {
        std::vector<SceneLight> lights = PresetLights(1, {0, 10, 0}, next);  // Sunset: a coloured sky
        if (disabled) lights[1].enabled = false;
        else lights.erase(lights.begin() + 1);
        LightParams out;
        BuildSceneLighting(lights, {}, 0.0, anchors, out);
        const char* name = disabled ? "a disabled ambient light counts as none: hemiStrength 0, default sky colours"
                                    : "no ambient: hemiStrength 0, default sky colours";
        Check(out.hemiStrength == 0.0f && Same3(out.skyZenith, def.skyZenith) && Same3(out.skyHorizon, def.skyHorizon) &&
                  Same3(out.groundColor, def.groundColor),
              name);
        Check(out.sunIntensity == 1.0f && Near3(out.color, {0.68f, 0.56f, 0.46f}, 1e-6f), "no ambient: the sun is unaffected");
    }
}

// ---- (d) the camera VMD light track and the sun's vmdLink ----
static void TestVmdLink() {
    LightAnchors anchors;
    uint32_t next = 1;
    std::vector<SceneLight> lights = PresetLights(0, {0, 10, 0}, next);
    lights[0].v.intensity = 0.7f;
    lights[0].rimStrength = 0.9f;
    lights[0].rimColor = {0.1f, 0.2f, 0.3f};
    const std::vector<LightKf> cam = {LightKf{0, {0.9f, 0.1f, 0.2f}, {0.3f, -1.0f, 0.1f}},
                                      LightKf{60, {0.1f, 0.9f, 0.2f}, {-0.3f, -1.0f, 0.1f}}};
    LightParams own, linked;
    BuildSceneLighting(lights, {}, 1.0, anchors, own);
    BuildSceneLighting(lights, cam, 1.0, anchors, linked);
    // frame 30: halfway between the two keys
    Check(Near3(linked.color, {0.5f, 0.5f, 0.2f}, 1e-5f) && Near3(linked.direction, {0.0f, -1.0f, 0.1f}, 1e-5f),
          "vmdLink: the camera VMD light track sets the sun's colour and direction");
    Check(linked.sunIntensity == 0.7f && linked.rimStrength == 0.9f && Same3(linked.rimColor, {0.1f, 0.2f, 0.3f}) &&
              Same3(linked.skyZenith, own.skyZenith) && linked.hemiStrength == own.hemiStrength,
          "vmdLink: intensity, rim and the environment are unchanged");
    lights[0].vmdLink = false;
    LightParams unlinked;
    BuildSceneLighting(lights, cam, 1.0, anchors, unlinked);
    Check(DiffParams(own, unlinked, 0.0f).empty(), "vmdLink off: the light track is ignored");

    lights[0].vmdLink = true;
    const std::vector<LightKf> broken = {LightKf{0, {0.4f, 0.4f, 0.4f}, {0.0f, 0.0f, 0.0f}}};
    LightParams b;
    BuildSceneLighting(lights, broken, 1.0, anchors, b);
    Check(Near3(b.color, {0.4f, 0.4f, 0.4f}, 1e-6f) && Same3(b.direction, own.direction),
          "vmdLink: a zero direction in the track keeps the sun's direction");

    // the sun's own keys apply under the link too: the track overrides colour and direction only
    lights[0].keys = {LightKey{0, lights[0].v}};
    lights[0].keys[0].v.intensity = 1.9f;
    LightParams keyed;
    BuildSceneLighting(lights, cam, 1.0, anchors, keyed);
    Check(keyed.sunIntensity == 1.9f && Near3(keyed.color, linked.color, 1e-6f), "vmdLink: the sun's keyed intensity still applies");

    // no sun object: the track has nothing to drive
    lights.erase(lights.begin());
    LightParams nosun;
    BuildSceneLighting(lights, cam, 1.0, anchors, nosun);
    Check(Same3(nosun.color, {0, 0, 0}), "no sun: the camera VMD light track drives nothing");
}

// ---- (e) key sampling ----
static void TestSampling() {
    SceneLight spot = MakeSpot(9, 9, 9);
    spot.v.direction = {1, 1, 1};
    LightValues v0, v1;
    v0.position = {0, 40, -10};
    v0.aim = {0, 0, 0};
    v0.direction = {-1, -1, 0};
    v0.color = {1, 0, 0};
    v0.intensity = 2.0f;
    v0.range = 100.0f;
    v0.coneOuter = 0.2f;
    v0.coneInner = 0.1f;
    v1.position = {10, 60, 10};
    v1.aim = {4, 0, 2};
    v1.direction = {1, -1, 2};
    v1.color = {0, 0, 1};
    v1.intensity = 4.0f;
    v1.range = 200.0f;
    v1.coneOuter = 0.4f;
    v1.coneInner = 0.3f;

    Check(SampleLightValues(spot, 15) == spot.v && SampleLightValues(spot, -3) == spot.v, "no keys: the base values at any frame");
    spot.keys = {LightKey{0, v0}, LightKey{30, v1}};
    Check(SampleLightValues(spot, -5) == v0 && SampleLightValues(spot, 0) == v0, "before / at the first key: that key");
    Check(SampleLightValues(spot, 30) == v1 && SampleLightValues(spot, 100) == v1, "at / after the last key: that key");
    const LightValues mid = SampleLightValues(spot, 15);
    Check(Near3(mid.position, {5, 50, 0}, 1e-5f) && Near3(mid.aim, {2, 0, 1}, 1e-5f) && Near3(mid.direction, {0, -1, 1}, 1e-5f) &&
              Near3(mid.color, {0.5f, 0, 0.5f}, 1e-5f) && Near(mid.intensity, 3.0f, 1e-5f) && Near(mid.range, 150.0f, 1e-4f) &&
              Near(mid.coneOuter, 0.3f, 1e-6f) && Near(mid.coneInner, 0.2f, 1e-6f),
          "between two keys: linear on every field (halfway)");
    const LightValues third = SampleLightValues(spot, 10);
    Check(Near(third.intensity, 2.0f + 2.0f / 3.0f, 1e-5f) && Near3(third.position, {10.0f / 3.0f, 40.0f + 20.0f / 3.0f, -10.0f + 20.0f / 3.0f}, 1e-4f),
          "between two keys: one third of the way");

    // more keys: the segment is found by frame, an exact key frame returns the key
    LightValues v2 = v1;
    v2.intensity = 10.0f;
    spot.keys = {LightKey{0, v0}, LightKey{10, v1}, LightKey{100, v2}};
    Check(Near(SampleLightValues(spot, 55).intensity, 7.0f, 1e-5f) && SampleLightValues(spot, 10) == v1 &&
              Near(SampleLightValues(spot, 5).intensity, 3.0f, 1e-5f),
          "three keys: the right segment, an exact key frame returns the key");

    SceneLight amb;
    amb.kind = LightKind::Ambient;
    amb.v.intensity = 0.3f;
    amb.keys = {LightKey{0, v0}, LightKey{10, v1}};
    Check(SampleLightValues(amb, 5) == amb.v, "ambient: the keys are ignored");

    // the common properties are base-only: keys do not carry them
    spot.shadow = ShadowType::Soft;
    spot.falloff = FalloffType::Linear;
    spot.affectDiffuse = false;
    Check(SampleLightValues(spot, 10) == v1, "SampleLightValues ignores the common properties");
}

// ---- (f) at most kMaxPunctualLights ----
static void TestLightLimit() {
    LightAnchors anchors;
    std::vector<SceneLight> lights;
    for (uint32_t i = 0; i < 17; ++i) {
        SceneLight p;
        p.uid = i + 1;
        p.kind = LightKind::Point;
        p.v.position = {(float)i, 10, 0};
        lights.push_back(p);
    }
    LightParams out;
    BuildSceneLighting(lights, {}, 0.0, anchors, out);
    bool order = out.punctual.size() == Renderer::kMaxPunctualLights;
    for (size_t i = 0; order && i < out.punctual.size(); ++i) order = out.punctual[i].position.x == (float)i;
    Check(Renderer::kMaxPunctualLights == 16 && out.punctual.size() == 16 && order,
          "17 enabled point lights give 16 PunctualLights, the first 16 in list order", "size=%zu", out.punctual.size());

    // disabled lights do not count; spots and points share the limit; a point light has no cone
    lights[0].enabled = false;
    lights[1].kind = LightKind::Spot;
    LightParams out2;
    BuildSceneLighting(lights, {}, 0.0, anchors, out2);
    Check(out2.punctual.size() == 16 && out2.punctual[0].position.x == 1.0f && out2.punctual[0].spotCosOuter > -1.0f &&
              out2.punctual[1].spotCosOuter <= -1.0f,
          "disabled lights are skipped; a spot has a cone, a point light none");

    // a second sun / ambient light is not used; neither is a sun among the punctual lights
    uint32_t next = 1;
    std::vector<SceneLight> two = PresetLights(0, {0, 10, 0}, next);
    SceneLight sun2 = two[0];
    sun2.uid = 50;
    sun2.v.color = {0.1f, 0.1f, 0.1f};
    two.push_back(sun2);
    LightParams out3;
    BuildSceneLighting(two, {}, 0.0, anchors, out3);
    Check(Near3(out3.color, {0.6f, 0.6f, 0.6f}, 1e-6f) && out3.punctual.empty(), "the first enabled sun is used, suns are not punctual");
}

// ---- (g) spot aim ----
static void TestSpotAim() {
    LightAnchors anchors;
    anchors.focus = {5, 10, 1};
    anchors.head = {5, 16, 1};
    anchors.characters.push_back({7, {-20, 9, 3}, {-20, 15, 3}});
    anchors.characters.push_back({9, {30, 8, 0}, {30, 14, 0}});
    const auto dirOf = [&](const SceneLight& s, double t = 0.0) {
        LightParams out;
        BuildSceneLighting({s}, {}, t, anchors, out);
        return out.punctual.empty() ? XMFLOAT3{} : out.punctual[0].direction;
    };

    SceneLight s = MakeSpot(0, 50, 0);
    s.v.aim = {2, 0, 3};
    s.aimMode = AimMode::Manual;
    Check(Near3(dirOf(s), {2, -50, 3}, 1e-5f), "Manual: aim - position");

    s.aimMode = AimMode::Target;
    s.targetUid = 0;
    s.targetPart = TargetPart::Centre;
    Check(Near3(dirOf(s), {5, -40, 1}, 1e-5f), "Target, uid 0, centre: the performer's centre");
    s.targetPart = TargetPart::Head;
    Check(Near3(dirOf(s), {5, -34, 1}, 1e-5f), "Target, uid 0, head: the performer's head");
    s.targetUid = 7;
    s.targetPart = TargetPart::Centre;
    Check(Near3(dirOf(s), {-20, -41, 3}, 1e-5f), "Target, a known uid, centre: that character's centre");
    s.targetUid = 9;
    s.targetPart = TargetPart::Head;
    Check(Near3(dirOf(s), {30, -36, 0}, 1e-5f), "Target, a known uid, head: that character's head");
    s.targetUid = 99;
    Check(Near3(dirOf(s), {5, -34, 1}, 1e-5f), "Target, an unknown uid: falls back to the performer's head");
    s.targetPart = TargetPart::Centre;
    Check(Near3(dirOf(s), {5, -40, 1}, 1e-5f), "Target, an unknown uid: falls back to the performer's centre");

    // Sway: the aim point plus the concert sway offsets (re-derived here from the formula)
    s.aimMode = AimMode::Sway;
    s.swayPhase = 1.3f;
    s.v.aim = {2, 4, 3};
    for (const double t : {0.0, 2.5}) {
        const float dx = (float)std::sin(t * 0.9 + 1.3f) * 16.0f * 0.8f;
        const float dz = (float)std::cos(t * 0.7 + 1.3f * 0.6) * 10.0f;
        char name[96];
        std::snprintf(name, sizeof(name), "Sway at %.1f s: aim + (sin * 12.8, 0, cos * 10)", t);
        Check(Near3(dirOf(s, t), {2 + dx, 4 - 50, 3 + dz}, 1e-4f), name);
    }

    // degenerate and cone clamps
    s.aimMode = AimMode::Manual;
    s.v.aim = s.v.position;
    Check(Same3(dirOf(s), {0, -1, 0}), "aim at the light's own position: straight down");
    s.v.aim = {0, 0, 0};
    s.v.coneOuter = 0.4f;
    s.v.coneInner = 0.25f;
    LightParams out;
    BuildSceneLighting({s}, {}, 0.0, anchors, out);
    Check(Near(out.punctual[0].spotCosOuter, std::cos(0.4f), 1e-6f) && Near(out.punctual[0].spotCosInner, std::cos(0.25f), 1e-6f) &&
              out.punctual[0].range == 140.0f && out.punctual[0].intensity == 2.0f,
          "cone angles become cosines; range and intensity pass through");
    s.v.coneOuter = 0.001f;
    s.v.coneInner = 0.5f;
    BuildSceneLighting({s}, {}, 0.0, anchors, out);
    Check(Near(out.punctual[0].spotCosOuter, std::cos(0.02f), 1e-6f) && Near(out.punctual[0].spotCosInner, std::cos(0.01f), 1e-6f),
          "a tiny cone clamps to 0.02 rad (outer) and 0.01 rad (inner)");
    s.v.coneOuter = 3.0f;
    s.v.coneInner = 2.0f;
    BuildSceneLighting({s}, {}, 0.0, anchors, out);
    Check(Near(out.punctual[0].spotCosOuter, std::cos(1.5f), 1e-6f), "a huge cone clamps to 1.5 rad");
}

// ---- the common properties do not change what is rendered ----
static void TestRenderIndependence() {
    const XMFLOAT3 focus{1.5f, 12.0f, -3.0f};
    LightAnchors anchors;
    anchors.focus = focus;
    uint32_t next = 1;
    std::vector<SceneLight> lights = PresetLights(2, focus, next);
    lights[0].keys = {LightKey{0, lights[0].v}, LightKey{60, lights[0].v}};
    lights[0].keys[1].v.intensity = 2.0f;
    const std::vector<LightKf> cam = {LightKf{0, {0.9f, 0.1f, 0.2f}, {0.3f, -1.0f, 0.1f}}};
    std::vector<SceneLight> changed = lights;
    for (SceneLight& l : changed) {
        l.viewportVisible = false;
    }
    bool equal = true;
    std::string where;
    for (const double t : {0.0, 1.0, 3.7}) {
        LightParams a, b;
        BuildSceneLighting(lights, cam, t, anchors, a);
        BuildSceneLighting(changed, cam, t, anchors, b);
        const std::string diff = DiffParams(a, b, 0.0f);
        if (!diff.empty()) { equal = false; where = diff; }
    }
    Check(equal, "BuildSceneLighting does not depend on viewportVisible", "differs in %s", where.c_str());
    Check(SampleLightValues(lights[0], 30) == SampleLightValues(changed[0], 30),
          "SampleLightValues does not depend on the common properties");
}

// ---- whole-frame times must sample that frame ----
static void TestFrameTimes() {
    LightAnchors anchors;
    SceneLight sun;
    sun.uid = 1;
    sun.kind = LightKind::Sun;
    for (int f = 0; f < 400; ++f) {
        LightKey k;
        k.frame = f;
        k.v.intensity = (float)f;
        sun.keys.push_back(k);
    }
    const std::vector<SceneLight> lights = {sun};
    int bad = 0, first = -1;
    for (int f = 0; f < 400; ++f) {
        LightParams out;
        BuildSceneLighting(lights, {}, f / (double)kMmdFps, anchors, out);  // the time the studio plays frame f at
        if (out.sunIntensity != (float)f) {
            if (first < 0) first = f;
            ++bad;
        }
    }
    Check(bad == 0, "the time of frame N (N / 30) samples the keys of frame N", "%d frames off, first %d", bad, first);
}

// ---- track snapshots and LightsCommand on a StudioDoc ----
static void TestDocLights() {
    Check((int)RowKind::SceneLight == 7 && RowKindOf(MakeRowId(RowKind::SceneLight, 0, 42)) == RowKind::SceneLight &&
              RowIndexOf(MakeRowId(RowKind::SceneLight, 0, 42)) == 42 && !IsCameraKind(RowKind::SceneLight) &&
              LightUidOfTrack(LightTrackName(1234)) == 1234,
          "SceneLight row ids and track names carry the uid");

    StudioDoc doc;
    doc.lights = PresetLights(2, {0, 10, 0}, doc.nextLightUid);
    const uint32_t spotUid = doc.lights[2].uid;
    Check(doc.nextLightUid == 10 && doc.FindLight(spotUid) == &doc.lights[2] && !doc.FindLight(0) && !doc.FindLight(99) &&
              std::as_const(doc).FindLight(spotUid) == &doc.lights[2],
          "StudioDoc::FindLight by uid");

    const TrackState before = CaptureTrack(doc, -1, RowKind::SceneLight, LightTrackName(spotUid));
    Check(before.uid == spotUid && before.existed && before.lightKeys.empty() && before.kind == RowKind::SceneLight,
          "CaptureTrack: a scene light track (uid, existed, no keys)");
    LightKey k0, k1;
    k0.frame = 0;
    k0.v = doc.lights[2].v;
    k1.frame = 30;
    k1.v = doc.lights[2].v;
    k1.v.intensity = 5.0f;
    doc.FindLight(spotUid)->keys = {k0, k1};
    const TrackState after = CaptureTrack(doc, -1, RowKind::SceneLight, LightTrackName(spotUid));
    Check(after.lightKeys.size() == 2 && after.Bytes() > before.Bytes(), "CaptureTrack: the keys and their bytes");

    doc.rowsKey = 5;
    RestoreTrack(doc, before);
    Check(doc.FindLight(spotUid)->keys.empty() && doc.rowsKey == ~0ull, "RestoreTrack: an emptied track keeps the light");
    RestoreTrack(doc, after);
    Check(doc.FindLight(spotUid)->keys == after.lightKeys && doc.lights.size() == 9, "RestoreTrack: the keys come back");

    TrackState missing = after;
    missing.uid = 4242;
    doc.rowsKey = 5;
    const std::vector<SceneLight> snapshot = doc.lights;
    RestoreTrack(doc, missing);
    const TrackState gone = CaptureTrack(doc, -1, RowKind::SceneLight, LightTrackName(4242));
    Check(doc.lights == snapshot && doc.rowsKey == 5 && !gone.existed && gone.lightKeys.empty(),
          "restoring a track of a missing light does nothing; capturing one reports it missing");

    // undo / redo of a key edit through the command stack
    RestoreTrack(doc, before);
    doc.history.Push(std::make_unique<TrackEditCommand>(doc, "keys", std::vector<TrackState>{before}, std::vector<TrackState>{after}));
    Check(doc.FindLight(spotUid)->keys.size() == 2, "TrackEditCommand applies the edit");
    doc.history.Undo();
    Check(doc.FindLight(spotUid)->keys.empty(), "undo: the keys are gone");
    doc.history.Redo();
    Check(doc.FindLight(spotUid)->keys.size() == 2, "redo: the keys are back");

    // LightsCommand replaces the whole list; undo gives the old list back (and rebuilds the rows)
    const std::vector<SceneLight> old = doc.lights;
    std::vector<SceneLight> preset = PresetLights(0, {0, 10, 0}, doc.nextLightUid);
    doc.rowsKey = 5;
    const size_t bytesBefore = doc.history.Bytes();
    doc.history.Push(std::make_unique<LightsCommand>(doc, "preset", old, preset));
    Check(doc.lights == preset && doc.rowsKey == ~0ull && doc.history.Bytes() > bytesBefore && doc.history.UndoName() == "preset",
          "LightsCommand: Do sets the new list and invalidates the rows");
    doc.rowsKey = 5;
    doc.history.Undo();
    Check(doc.lights == old && doc.rowsKey == ~0ull, "LightsCommand: Undo restores the old list and invalidates the rows");
    doc.history.Redo();
    Check(doc.lights == preset, "LightsCommand: Redo");
    Check(doc.selectedLightUid == 0 && doc.selectedModel == -1, "a new document selects no light");
}

// ---- D6: a camera VMD light track that only repeats MMD's defaults is dropped when a song is loaded ----
static void TestSongCameraLightTrack() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mmdx_light_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto writeCamera = [&](const char* file, LightKf light) {
        MotionData m;
        CameraKf c;
        c.frame = 0;
        FillLinearCameraInterp(c.interp);
        m.camera = {c};
        light.frame = 0;
        m.light = {light};
        VmdMotion vmd = m.ToVmd();
        vmd.modelName = "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E";  // カメラ・照明
        std::string err;
        const std::filesystem::path path = dir / file;
        if (!SaveVmd(path, vmd, &err)) std::printf("SaveVmd failed: %s\n", err.c_str());
        return path;
    };
    const LightKf mmdDefault{0, {154.0f / 255.0f, 154.0f / 255.0f, 154.0f / 255.0f}, {-0.5f, -1.0f, 0.5f}};
    const LightKf custom{0, {0.9f, 0.5f, 0.2f}, {0.2f, -1.0f, 0.3f}};

    SongAsset song;
    song.cameraVmd = writeCamera("default.vmd", mmdDefault);
    MotionData dance, camera;
    std::string err;
    bool ok = LoadStudioSong(song, dance, camera, &err);
    Check(ok && camera.camera.size() == 1 && camera.light.empty(), "song camera VMD: the MMD default light track is dropped",
          "ok=%d cam=%zu light=%zu %s", (int)ok, camera.camera.size(), camera.light.size(), err.c_str());
    song.cameraVmd = writeCamera("custom.vmd", custom);
    ok = LoadStudioSong(song, dance, camera, &err);
    Check(ok && camera.camera.size() == 1 && camera.light.size() == 1 && Near3(camera.light[0].color, custom.color, 1e-6f),
          "song camera VMD: a light track with real values is kept");
    std::filesystem::remove_all(dir, ec);
}

// ---- D7: per-light renderer properties (shadow, softness, density, color, falloff, affectDiffuse, affectSpecular) ----
static void TestPerLightProperties() {
    LightAnchors anchors;
    SceneLight sun;
    sun.uid = 1;
    sun.name = "sun";
    sun.kind = LightKind::Sun;
    sun.v.direction = {0.1f, -0.9f, 0.2f};
    sun.v.color = {0.8f, 0.8f, 0.8f};
    sun.shadow = ShadowType::Soft;
    sun.shadowSoftness = 0.75f;
    sun.shadowDensity = 0.6f;
    sun.shadowColor = {0.2f, 0.1f, 0.05f};

    SceneLight spot = MakeSpot(10, 20, 30);
    spot.shadow = ShadowType::Soft;
    spot.shadowSoftness = 0.85f;
    spot.shadowDensity = 0.4f;
    spot.shadowColor = {0.1f, 0.2f, 0.3f};
    spot.falloff = FalloffType::InverseSquare;
    spot.affectDiffuse = false;
    spot.affectSpecular = true;

    SceneLight point;
    point.uid = 3;
    point.name = "point";
    point.kind = LightKind::Point;
    point.v.position = {5, 15, 25};
    point.shadow = ShadowType::NoCast;
    point.shadowSoftness = 0.0f;
    point.shadowDensity = 0.0f;
    point.shadowColor = {0, 0, 0};
    point.falloff = FalloffType::Linear;
    point.affectDiffuse = true;
    point.affectSpecular = false;

    std::vector<SceneLight> lights = {sun, spot, point};
    LightParams out;
    BuildSceneLighting(lights, {}, 0.0, anchors, out);

    Check(out.sunShadow == LightShadowType::Soft && Near(out.sunShadowSoftness, 0.75f, 1e-5f) &&
          Near(out.sunShadowDensity, 0.6f, 1e-5f) && Near3(out.sunShadowColor, {0.2f, 0.1f, 0.05f}, 1e-5f),
          "BuildSceneLighting carries sun shadow properties");

    Check(out.punctual.size() == 2, "BuildSceneLighting produces 2 punctual lights");
    if (out.punctual.size() == 2) {
        const PunctualLight& pSpot = out.punctual[0];
        Check(pSpot.shadow == LightShadowType::Soft && Near(pSpot.shadowSoftness, 0.85f, 1e-5f) &&
              Near(pSpot.shadowDensity, 0.4f, 1e-5f) && Near3(pSpot.shadowColor, {0.1f, 0.2f, 0.3f}, 1e-5f) &&
              pSpot.falloff == LightFalloffType::InverseSquare && !pSpot.affectDiffuse && pSpot.affectSpecular,
              "BuildSceneLighting carries spot properties (soft shadow, inverse-square falloff, spec only)");

        const PunctualLight& pPoint = out.punctual[1];
        Check(pPoint.shadow == LightShadowType::NoCast && pPoint.falloff == LightFalloffType::Linear &&
              pPoint.affectDiffuse && !pPoint.affectSpecular,
              "BuildSceneLighting carries point properties (NoCast, linear falloff, diffuse only)");

        Check(!pSpot.castPointShadow, "BuildSceneLighting does not set castPointShadow for a spot light");
        Check(!pPoint.castPointShadow, "BuildSceneLighting does not set castPointShadow for a NoCast point light");
    }

    SceneLight pointHard = point;
    pointHard.uid = 4;
    pointHard.shadow = ShadowType::Hard;
    SceneLight pointSoft = point;
    pointSoft.uid = 5;
    pointSoft.shadow = ShadowType::Soft;
    std::vector<SceneLight> testLights = {pointHard, pointSoft};
    LightParams outHardSoft;
    BuildSceneLighting(testLights, {}, 0.0, anchors, outHardSoft);
    Check(outHardSoft.punctual.size() == 2 && outHardSoft.punctual[0].castPointShadow && outHardSoft.punctual[1].castPointShadow,
          "BuildSceneLighting sets castPointShadow for Hard and Soft point lights");

    bool buildLightingNeverSets = true;
    for (int p = 0; p < kLightingPresetCount; ++p) {
        LightParams lp;
        BuildLighting((LightingPreset)p, 0.0, {0, 0, 0}, lp);
        for (const PunctualLight& pl : lp.punctual) {
            if (pl.castPointShadow) buildLightingNeverSets = false;
        }
    }
    Check(buildLightingNeverSets, "BuildLighting never sets castPointShadow");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash mid-test loses buffered PASS lines
    TestPresetRegression();
    TestPresetStructure();
    TestNoSun();
    TestNoAmbient();
    TestVmdLink();
    TestSampling();
    TestLightLimit();
    TestSpotAim();
    TestRenderIndependence();
    TestFrameTimes();
    TestDocLights();
    TestSongCameraLightTrack();
    TestPerLightProperties();
    std::printf("studio_light_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
