// Self test of the spot shadow budget (render/SpotShadowPick): the reach test, the score and the hysteresis.
#include "render/SpotShadowPick.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace mmdx;
using DirectX::XMFLOAT3;

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

PunctualLight Spot(XMFLOAT3 pos, XMFLOAT3 aim, float range, float coneOuter, float intensity = 2.0f,
                   XMFLOAT3 color = {1, 1, 1}) {
    PunctualLight p;
    p.position = pos;
    p.direction = {aim.x - pos.x, aim.y - pos.y, aim.z - pos.z};
    p.range = range;
    p.spotCosOuter = std::cos(coneOuter);
    p.spotCosInner = std::cos(coneOuter * 0.6f);
    p.intensity = intensity;
    p.color = color;
    return p;
}

CameraParams FrontCamera() {
    CameraParams c;
    c.eye = {0, 12, -40};
    DirectX::XMStoreFloat4x4(&c.view, DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0, 12, -40, 1),
                                                                DirectX::XMVectorSet(0, 10, 0, 1),
                                                                DirectX::XMVectorSet(0, 1, 0, 0)));
    return c;
}

void TestReach() {
    std::printf("reach\n");
    const PunctualLight s = Spot({0, 50, 0}, {0, 0, 0}, 100, 0.3f);
    Check(LightReachesSphere(s, {0, 10, 0}, 1), "on the axis");
    Check(!LightReachesSphere(s, {60, 10, 0}, 1), "far off the axis");
    Check(LightReachesSphere(s, {60, 10, 0}, 60), "large sphere touches the cone");
    Check(!LightReachesSphere(s, {0, 200, 0}, 1), "behind the apex, out of range");
    Check(!LightReachesSphere(s, {0, 80, 0}, 5), "behind the apex, in range");
    Check(LightReachesSphere(s, {0, 52, 0}, 5), "apex inside the sphere");
    Check(!LightReachesSphere(s, {0, -60, 0}, 5), "past the range on the axis");
    Check(LightReachesSphere(s, {0, -60, 0}, 15), "the range cuts into the sphere");
    PunctualLight point;
    point.position = {0, 0, 0};
    point.range = 50;
    Check(LightReachesSphere(point, {40, 0, 0}, 1), "point: in range");
    Check(!LightReachesSphere(point, {60, 0, 0}, 1), "point: out of range");
}

// The geometry that made the problem visible: a stage rig at the origin, street lamps along a far avenue that used to
// fill the slices, and a beam pointing at the sky.
void TestStageVsAvenue() {
    std::printf("stage rig vs avenue lamps\n");
    std::vector<PunctualLight> lights;
    lights.push_back(Spot({-28, 45, -45}, {-6, 12, 0}, 160, 0.28f, 2.2f));
    lights.push_back(Spot({28, 45, -45}, {6, 12, 0}, 160, 0.28f, 2.2f));
    lights.push_back(Spot({0, 68, 0}, {0, 0, 0}, 150, 0.35f, 1.8f));
    lights.push_back(Spot({-32, 38, 28}, {-8, 12, 0}, 140, 0.25f, 2.5f, {0.2f, 0.8f, 1.0f}));
    for (float z : {-400.f, -240.f, -80.f, 80.f, 240.f, 400.f, 560.f, 720.f, 880.f, 1040.f, 1500.f, 1750.f, 2000.f, 2250.f}) {
        lights.push_back(Spot({-202, 36, z}, {-185, -6, z}, 120, 0.45f, 1.4f));
        lights.push_back(Spot({202, 36, z}, {185, -6, z}, 120, 0.45f, 1.4f));
    }
    lights.push_back(Spot({0, 1610, 1350}, {0, 2200, 1350}, 800, 0.15f, 3.5f));

    const CameraParams cam = FrontCamera();
    const XMFLOAT3 performer{0, 10, 0};
    std::vector<SpotCaster> casters{{performer, 12.0f, CasterViewWeight(cam, 16.0f / 9.0f, performer, 12.0f)}};
    std::vector<float> score;
    for (const PunctualLight& l : lights) score.push_back(SpotShadowScore(l, casters, false));
    int reached = 0;
    bool rigOnly = true;
    for (size_t i = 0; i < score.size(); ++i) {
        if (score[i] > 0.0f) ++reached;
        if (i >= 4 && score[i] > 0.0f) rigOnly = false;
    }
    Check(reached == 4 && rigOnly, "only the stage rig scores");
    const std::vector<int> pick = PickSpotSlices(score, {}, 16);
    int picked = 0;
    for (int s : pick) picked += s >= 0 ? 1 : 0;
    Check(picked == 4, "four slices, the other twelve stay empty");

    // A performer on the avenue: the lamp above them joins.
    const XMFLOAT3 walker{-185, 10, 80};
    std::vector<SpotCaster> both = casters;
    both.push_back({walker, 12.0f, CasterViewWeight(cam, 16.0f / 9.0f, walker, 12.0f)});
    score.clear();
    for (const PunctualLight& l : lights) score.push_back(SpotShadowScore(l, both, false));
    Check(score[4 + 3 * 2] > 0.0f, "a lamp above a performer scores");

    casters.clear();
    score.clear();
    for (const PunctualLight& l : lights) score.push_back(SpotShadowScore(l, casters, false));
    int any = 0;
    for (int s : PickSpotSlices(score, {}, 16)) any += s >= 0 ? 1 : 0;
    Check(any == 0, "no caster: no slice");
}

void TestScoreRanks() {
    std::printf("score\n");
    const CameraParams cam = FrontCamera();
    const XMFLOAT3 c{0, 10, 0};
    std::vector<SpotCaster> casters{{c, 12.0f, 1.0f}};
    const PunctualLight near = Spot({0, 40, -20}, c, 100, 0.3f);
    const PunctualLight far = Spot({0, 80, -60}, c, 200, 0.3f);
    Check(SpotShadowScore(near, casters, false) > SpotShadowScore(far, casters, false), "nearer spot ranks higher");
    PunctualLight off = near;
    off.intensity = 0.0f;
    Check(SpotShadowScore(off, casters, false) == 0.0f, "intensity 0");
    off = near;
    off.shadow = LightShadowType::NoCast;
    Check(SpotShadowScore(off, casters, false) == 0.0f, "nocast");
    off = near;
    off.shadowDensity = 0.0f;
    Check(SpotShadowScore(off, casters, false) == 0.0f, "density 0");
    off = near;
    off.affectDiffuse = off.affectSpecular = false;
    Check(SpotShadowScore(off, casters, false) == 0.0f, "no surface term, haze off");
    Check(SpotShadowScore(off, casters, true) > 0.0f, "no surface term, haze on: the beam reads the map");
    const PunctualLight blue = Spot({0, 40, -20}, c, 100, 0.3f, 2.0f, {0, 0, 1});
    Check(SpotShadowScore(blue, casters, false) > 0.4f * SpotShadowScore(near, casters, false),
          "a saturated spot is not ranked out by its luminance");
    const float behind = CasterViewWeight(cam, 16.0f / 9.0f, {0, 10, -200}, 5.0f);
    const float front = CasterViewWeight(cam, 16.0f / 9.0f, {0, 10, 0}, 5.0f);
    const float close = CasterViewWeight(cam, 16.0f / 9.0f, {0, 12, -30}, 5.0f);
    Check(behind < 0.5f && front > 1.0f && close > front, "view weight: behind < on screen < close");
}

void TestHysteresis() {
    std::printf("hysteresis\n");
    std::vector<float> score(20);
    for (size_t i = 0; i < score.size(); ++i) score[i] = 20.0f - (float)i;
    std::vector<int> a = PickSpotSlices(score, {}, 16);
    int n = 0;
    for (int s : a) n += s >= 0 ? 1 : 0;
    Check(n == 16 && a[15] >= 0 && a[16] < 0, "the 16 best win");

    std::vector<float> s2 = score;
    s2[16] = s2[15] * 1.2f;
    std::vector<int> b = PickSpotSlices(s2, a, 16);
    Check(b == a, "a newcomer 1.2x stronger changes nothing");

    s2[16] = s2[15] * 2.0f;
    b = PickSpotSlices(s2, a, 16);
    Check(b[16] == a[15] && b[15] < 0, "a newcomer 2x stronger takes the weakest pick's slice");
    bool others = true;
    for (size_t i = 0; i < 15; ++i) others = others && b[i] == a[i];
    Check(others, "the other slices stay");

    std::vector<float> s3 = score;
    s3[3] = 0.0f;
    b = PickSpotSlices(s3, a, 16);
    Check(b[3] < 0 && b[16] == a[3], "a light out of reach frees its slice at once");

    std::vector<float> shorter(10, 1.0f);
    b = PickSpotSlices(shorter, a, 16);
    bool ok = true;
    for (size_t i = 0; i < 10; ++i) ok = ok && b[i] == (int)i;
    Check(ok, "a changed light list ignores the old choice");
}

}  // namespace

int main() {
    TestReach();
    TestStageVsAvenue();
    TestScoreRanks();
    TestHysteresis();
    if (g_failures) {
        std::printf("spot_shadow_pick_test: %d FAILED\n", g_failures);
        return 1;
    }
    std::printf("spot_shadow_pick_test: all checks passed\n");
    return 0;
}
