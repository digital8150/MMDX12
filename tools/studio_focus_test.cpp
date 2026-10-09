// Studio DoF focus: the focus track evaluation (EvaluateFocus), the automatic focus (FocusScore / AutoFocus) and the
// project round trip of the focus keys.
#include "studio/StudioFocus.h"
#include "studio/StudioProject.h"
#include <DirectXMath.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
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

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static FocusKf Key(int frame, FocusMode mode, float distance = 40.0f, uint32_t target = 0, int transition = 0,
                   float aperture = 1.0f) {
    FocusKf k;
    k.frame = frame;
    k.mode = mode;
    k.distance = distance;
    k.target = target;
    k.transition = transition;
    k.aperture = aperture;
    return k;
}

// targets: uid 1 at z 20, uid 2 at z 80, uid 3 behind the camera
static bool TargetZ(uint32_t uid, FocusBone, float& z) {
    if (uid == 1) { z = 20.0f; return true; }
    if (uid == 2) { z = 80.0f; return true; }
    return false;
}

static void TestEvaluate() {
    std::vector<FocusKf> keys;
    FocusResult r = EvaluateFocus(keys, 10.0f, TargetZ, 33.0f);
    Check(Near(r.distance, 33.0f) && Near(r.aperture, 1.0f), "no keys: auto focus, aperture 1");

    keys = {Key(0, FocusMode::Target, 40, 1), Key(30, FocusMode::Target, 40, 2), Key(60, FocusMode::Auto)};
    Check(Near(EvaluateFocus(keys, -5.0f, TargetZ, 33).distance, 20.0f), "before the first key: the first segment");
    Check(Near(EvaluateFocus(keys, 29.9f, TargetZ, 33).distance, 20.0f), "target segment 1");
    Check(Near(EvaluateFocus(keys, 30.0f, TargetZ, 33).distance, 80.0f), "target segment 2 (cut)");
    Check(Near(EvaluateFocus(keys, 70.0f, TargetZ, 33).distance, 33.0f), "auto segment");
    keys[1].target = 3;
    Check(Near(EvaluateFocus(keys, 40.0f, TargetZ, 33).distance, 33.0f), "missing target -> auto");

    // manual -> manual: linear distance; transition ignored there
    keys = {Key(0, FocusMode::Manual, 10), Key(10, FocusMode::Manual, 30, 0, 5), Key(20, FocusMode::Target, 0, 2, 10)};
    Check(Near(EvaluateFocus(keys, 5.0f, TargetZ, 33).distance, 20.0f), "manual -> manual linear");
    Check(Near(EvaluateFocus(keys, 10.0f, TargetZ, 33).distance, 30.0f), "manual key value");
    Check(Near(EvaluateFocus(keys, 12.0f, TargetZ, 33).distance, 30.0f), "manual -> target: held before the target key");
    // transition from manual 30 to target 80 over 10 frames, smoothstep in 1/z
    const float mid = EvaluateFocus(keys, 25.0f, TargetZ, 33).distance;
    const float expect = 1.0f / (0.5f * (1.0f / 30.0f + 1.0f / 80.0f));
    Check(Near(mid, expect, 0.01f), "rack midpoint is the 1/z midpoint", "%.3f vs %.3f", mid, expect);
    Check(Near(EvaluateFocus(keys, 20.0f, TargetZ, 33).distance, 30.0f), "rack starts at the previous focus");
    Check(Near(EvaluateFocus(keys, 30.0f, TargetZ, 33).distance, 80.0f), "rack ends at the new focus");
    const float a = EvaluateFocus(keys, 21.0f, TargetZ, 33).distance, b = EvaluateFocus(keys, 24.0f, TargetZ, 33).distance;
    Check(a > 30.0f && b > a && mid > b, "rack is monotonic");

    // aperture: linear between keys, held outside
    keys = {Key(10, FocusMode::Auto, 40, 0, 0, 1.0f), Key(20, FocusMode::Auto, 40, 0, 0, 0.0f)};
    Check(Near(EvaluateFocus(keys, 0.0f, TargetZ, 33).aperture, 1.0f), "aperture before the first key");
    Check(Near(EvaluateFocus(keys, 15.0f, TargetZ, 33).aperture, 0.5f), "aperture linear");
    Check(Near(EvaluateFocus(keys, 99.0f, TargetZ, 33).aperture, 0.0f), "aperture after the last key");
}

static FocusView MakeView(XMFLOAT3 eye, XMFLOAT3 at) {
    FocusView v;
    XMStoreFloat4x4(&v.view, XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&at), XMVectorSet(0, 1, 0, 0)));
    v.eye = eye;
    v.tanHalfFovY = std::tan(XMConvertToRadians(30.0f) * 0.5f);
    return v;
}

static FocusSubject Subject(uint32_t uid, float x, float z) {
    return {uid, {x, 16.0f, z}, {x, 12.0f, z}, {x, 8.0f, z}};
}

static void TestAutoFocus() {
    const FocusView v = MakeView({0, 12, -40}, {0, 12, 0});
    // a near character slightly right vs a far one on the left edge: the near one is the subject
    const FocusSubject nearS = Subject(1, 3.0f, -10.0f), farS = Subject(2, -9.0f, 20.0f);
    Check(FocusScore(nearS, v) > FocusScore(farS, v), "near, central character scores higher",
          "%.3f vs %.3f", FocusScore(nearS, v), FocusScore(farS, v));
    Check(FocusScore(Subject(3, 0, -60.0f), v) == 0.0f, "behind the camera scores 0");
    Check(FocusScore(Subject(4, 200.0f, 0.0f), v) == 0.0f, "out of frame scores 0");

    AutoFocus af;
    const std::vector<FocusSubject> both{farS, nearS};
    float z = af.Update(0.0, both, v);
    Check(af.Subject() == 1 && Near(z, FocusViewZ(nearS.head, v), 0.01f), "auto picks the near subject, snapped");
    // the near one walks out of frame: the focus racks to the far one over kRackSeconds
    const FocusSubject gone = Subject(1, 300.0f, -10.0f);
    const std::vector<FocusSubject> after{farS, gone};
    const float z0 = z;
    z = af.Update(1.0 / 60.0, after, v);
    Check(af.Subject() == 2 && Near(z, z0, 0.01f), "subject change: the rack starts at the shown focus", "%.2f", z);
    z = af.Update(6.0 / 60.0, after, v);
    Check(z > z0 && z < FocusViewZ(farS.head, v), "subject change racks (not a snap)", "%.2f", z);
    for (int i = 2; i <= 40; ++i) z = af.Update(i / 60.0, after, v);
    Check(Near(z, FocusViewZ(farS.head, v), 0.01f), "rack settles on the new subject");
    // same time again: nothing changes
    Check(Near(af.Update(40 / 60.0, after, v), z), "re-evaluating the same time is stable");
    // hysteresis: a slightly better candidate does not steal the focus
    AutoFocus h;
    const FocusSubject a = Subject(1, 0.5f, 0.0f), b = Subject(2, -0.5f, -1.0f);
    h.Update(0.0, {a}, v);
    h.Update(1 / 60.0, {a, b}, v);
    Check(h.Subject() == 1, "hysteresis keeps the current subject");
    // time jump: snap
    AutoFocus s;
    s.Update(0.0, {nearS}, v);
    z = s.Update(5.0, {farS}, v);
    Check(Near(z, FocusViewZ(farS.head, v), 0.01f), "a time jump snaps");
    Check(AutoFocus{}.Update(0.0, {}, v) == 0.0f, "no subjects: 0");
}

static void TestProjectRoundTrip() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mmdx_focus_test";
    std::filesystem::remove_all(dir);
    ProjectData pd;
    pd.camera.focus = {Key(0, FocusMode::Auto), Key(30, FocusMode::Target, 40, 2, 12, 0.5f), Key(90, FocusMode::Manual, 27.5f)};
    pd.camera.focus[1].bone = FocusBone::UpperBody;
    std::string err;
    const bool saved = SaveProject(dir / "p.mmdxproj", pd, &err);
    Check(saved, "save project with focus keys", "%s", err.c_str());
    ProjectData back;
    std::vector<std::string> warnings;
    const bool loaded = LoadProject(dir / "p.mmdxproj", back, &err, &warnings);
    Check(loaded, "load project", "%s", err.c_str());
    Check(back.camera.focus.size() == 3 && back.camera.focus == pd.camera.focus, "focus keys round trip");
    Check(!std::filesystem::exists(dir / "p - camera.vmd"), "focus keys alone write no camera VMD");
    std::filesystem::remove_all(dir);
}

int main() {
    TestEvaluate();
    TestAutoFocus();
    TestProjectRoundTrip();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
