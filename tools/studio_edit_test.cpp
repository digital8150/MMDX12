// Console tests for the Studio keyframe editing core (StudioMotion.h, StudioDoc.h, CommandStack.h).
#include "studio/CommandStack.h"
#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace mmdx::studio;

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

static std::string Frames(const std::vector<MorphKf>& keys) {
    std::string s;
    for (const MorphKf& k : keys) {
        if (!s.empty()) s += ',';
        s += std::to_string(k.frame);
    }
    return s;
}
static bool SameFrames(const std::vector<MorphKf>& keys, std::initializer_list<int> want) {
    if (keys.size() != want.size()) return false;
    size_t i = 0;
    for (int w : want)
        if (keys[i++].frame != w) return false;
    return true;
}
static const MorphKf* KeyAt(const std::vector<MorphKf>& keys, int frame) { return FindKey(keys, frame); }

// 1. FindKey / EraseKey
static void TestFindKey() {
    std::vector<MorphKf> keys = {{0, 0}, {5, 5}, {9, 9}};
    bool ok = true;
    const char* detail = "";
    if (!FindKey(keys, 5) || FindKey(keys, 5)->frame != 5) { ok = false; detail = "FindKey(5) failed"; }
    if (FindKey(keys, 4)) { ok = false; detail = "FindKey(4) should be nullptr"; }
    if (FindKey(keys, 10)) { ok = false; detail = "FindKey(10) should be nullptr"; }
    const std::vector<MorphKf>& ckeys = keys;
    if (!FindKey(ckeys, 5)) { ok = false; detail = "const FindKey(5) failed"; }
    if (!EraseKey(keys, 5)) ok = false, detail = "EraseKey(5) returned false";
    if (!SameFrames(keys, {0, 9})) ok = false, detail = "erase result wrong";
    Check(ok, "FindKey/EraseKey", "%s", detail);
}

// 2-6 helper: build morph keys with weight == frame.
static std::vector<MorphKf> Morphs(std::initializer_list<int> frames) {
    std::vector<MorphKf> keys;
    for (int f : frames) keys.push_back({f, (float)f});
    return keys;
}

// 2. MoveKeyFrames replaces an unmoved key on the target frame.
static void TestMoveReplace() {
    auto keys = Morphs({0, 10, 20, 30});
    std::set<int> sel = {10, 20};
    MoveKeyFrames(keys, sel, 10);
    bool ok = SameFrames(keys, {0, 20, 30});
    const MorphKf* k20 = ok ? KeyAt(keys, 20) : nullptr;
    const MorphKf* k30 = ok ? KeyAt(keys, 30) : nullptr;
    if (!ok) Check(false, "MoveKeyFrames replace", "frames %s", Frames(keys).c_str());
    else if (!k20 || k20->weight != 10.0f) Check(false, "MoveKeyFrames replace", "frame 20 weight wrong");
    else if (!k30 || k30->weight != 20.0f) Check(false, "MoveKeyFrames replace", "frame 30 weight wrong");
    else Check(true, "MoveKeyFrames replace");
}

// 3. MoveKeyFrames clamp collision: the later key wins.
static void TestMoveClamp() {
    auto keys = Morphs({2, 4, 8});
    std::set<int> sel = {2, 4};
    MoveKeyFrames(keys, sel, -4);
    bool ok = SameFrames(keys, {0, 8});
    const MorphKf* k0 = ok ? KeyAt(keys, 0) : nullptr;
    if (!ok) Check(false, "MoveKeyFrames clamp", "frames %s", Frames(keys).c_str());
    else if (!k0 || k0->weight != 4.0f) Check(false, "MoveKeyFrames clamp", "frame 0 weight wrong");
    else Check(true, "MoveKeyFrames clamp");
}

// 4. MoveKeyFrames negative without collision.
static void TestMoveNegative() {
    auto keys = Morphs({10, 20, 30});
    std::set<int> sel = {20, 30};
    MoveKeyFrames(keys, sel, -5);
    bool ok = SameFrames(keys, {10, 15, 25});
    if (!ok) Check(false, "MoveKeyFrames negative", "frames %s", Frames(keys).c_str());
    else Check(true, "MoveKeyFrames negative");
}

// 5. InsertFrameSpan
static void TestInsertSpan() {
    auto keys = Morphs({0, 10, 20});
    bool moved = InsertFrameSpan(keys, 10, 5);
    bool ok = moved && SameFrames(keys, {0, 15, 25});
    if (!ok) { Check(false, "InsertFrameSpan", "frames %s", Frames(keys).c_str()); return; }
    // On the untouched {0,10,20} no key is >= 25: unchanged, returns false.
    auto fresh = Morphs({0, 10, 20});
    moved = InsertFrameSpan(fresh, 25, 3);
    if (moved || !SameFrames(fresh, {0, 10, 20})) Check(false, "InsertFrameSpan", "at 25 returned true");
    else Check(true, "InsertFrameSpan");
}

// 6. DeleteFrameSpan
static void TestDeleteSpan() {
    auto keys = Morphs({0, 10, 12, 20});
    bool changed = DeleteFrameSpan(keys, 10, 5);
    bool ok = changed && SameFrames(keys, {0, 15});
    if (!ok) Check(false, "DeleteFrameSpan", "frames %s", Frames(keys).c_str());
    else Check(true, "DeleteFrameSpan");
}

// 7. MotionData::InsertFrames / DeleteFrames over every track.
static void TestMotionDataSpans() {
    MotionData d;
    d.bones["center"] = {{0, {}}, {30, {}}};
    d.morphs["smile"] = {{40, 1}};
    d.ik["leg"] = {{0, true}};
    d.camera = {{0, {}}, {60, {}}};
    d.shadow = {{50, 1, 0}};

    bool ok = true;
    const char* detail = "";
    if (!d.DeleteFrames(35, 10)) { ok = false; detail = "DeleteFrames(35,10) returned false"; }
    if (ok && d.morphs.count("smile") != 0) { ok = false; detail = "empty morph track not removed"; }
    if (ok && (d.shadow.size() != 1 || d.shadow[0].frame != 40)) {
        ok = false; detail = "shadow key frame wrong";  // 50 -> 40
    }
    if (ok && (d.camera.size() != 2 || d.camera[0].frame != 0 || d.camera[1].frame != 50)) {
        ok = false; detail = "camera frames wrong";
    }
    if (ok && (d.bones["center"].size() != 2 || d.bones["center"][0].frame != 0 || d.bones["center"][1].frame != 30)) {
        ok = false; detail = "bone frames wrong";
    }
    if (!ok) { Check(false, "MotionData spans", "%s", detail); return; }

    if (!d.InsertFrames(0, 2)) { ok = false; detail = "InsertFrames(0,2) returned false"; }
    if (ok && (d.bones["center"].size() != 2 || d.bones["center"][0].frame != 2 || d.bones["center"][1].frame != 32)) {
        ok = false; detail = "bone insert frames wrong";
    }
    if (ok && (d.ik["leg"].size() != 1 || d.ik["leg"][0].frame != 2)) { ok = false; detail = "IK insert frames wrong"; }
    if (!ok) Check(false, "MotionData spans", "%s", detail);
    else Check(true, "MotionData spans");
}

// 8. CommandStack byte budget.
namespace {
struct CounterCommand : Command {
    int* counter;
    size_t bytes;
    CounterCommand(int* c, size_t b) : counter(c), bytes(b) {}
    void Do() override { ++*counter; }
    void Undo() override { --*counter; }
    std::string Name() const override { return "counter"; }
    size_t Bytes() const override { return bytes; }
};
} // namespace

static void TestCommandStackBudget() {
    bool ok = true;
    const char* detail = "";
    int counter = 0;
    CommandStack stack;
    const size_t mb100 = 100ull << 20;
    const size_t budget = 256ull << 20;  // CommandStack::kByteBudget (private)
    for (int i = 0; i < 5; ++i) stack.Push(std::make_unique<CounterCommand>(&counter, mb100));
    if (stack.Count() > 2) { ok = false; detail = "Count too large after 5x100MB pushes"; }
    else if (stack.Bytes() > budget) { ok = false; detail = "bytes over budget"; }
    else if (counter != 5) { ok = false; detail = "counter != 5"; }
    if (!ok) { Check(false, "CommandStack budget", "%s", detail); counter = 0; }

    stack.Undo();
    stack.Undo();
    if (counter != 3) { ok = false; detail = "counter != 3 after two undos"; }
    else if (stack.CanUndo()) { ok = false; detail = "CanUndo() true after undoing everything kept"; }
    if (!ok) { Check(false, "CommandStack budget", "%s", detail); counter = 0; }
    if (ok) Check(true, "CommandStack budget");

    // One oversized command on an empty stack is never trimmed.
    int counter2 = 0;
    CommandStack stack2;
    stack2.Push(std::make_unique<CounterCommand>(&counter2, 300ull << 20));
    bool ok2 = stack2.Count() == 1 && counter2 == 1;
    // Pushing after an Undo discards the redo branch; bytes drop accordingly.
    stack2.Undo();
    const size_t bytesAfterUndo = stack2.Bytes();
    stack2.Push(std::make_unique<CounterCommand>(&counter2, mb100));
    ok2 = ok2 && stack2.Count() == 1 && stack2.Bytes() == mb100 && bytesAfterUndo == 300ull << 20;
    Check(ok2, "CommandStack trim/redo", "count %zu bytes %zu", stack2.Count(), stack2.Bytes());
}

// Light/shadow helpers for the new tests.
static std::vector<LightKf> Lights(std::initializer_list<int> frames) {
    std::vector<LightKf> keys;
    for (int f : frames) keys.push_back(LightKf{f, {}, {}});
    return keys;
}
static std::vector<ShadowKf> Shadows(std::initializer_list<int> frames) {
    std::vector<ShadowKf> keys;
    for (int f : frames) keys.push_back(ShadowKf{f, 1, 0.01f});
    return keys;
}
static std::string ShadowModes(const std::vector<ShadowKf>& keys) {
    std::string s;
    for (const ShadowKf& k : keys) {
        if (!s.empty()) s += ',';
        s += std::to_string(k.mode);
    }
    return s;
}

// 10. SampleLight: linear colour/direction interpolation, clamped past the ends.
static void TestSampleLight() {
    std::vector<LightKf> keys = {{{10, {0.2f, 0.4f, 0.6f}, {0, -1, 0}}, {20, {0.4f, 0.4f, 0.2f}, {1, -1, 1}}}};
    bool ok = true;
    const char* detail = "";
    LightKf k = SampleLight(keys, 0.0f);
    if (k.frame != 0 || k.color.x != 0.2f || k.color.y != 0.4f || k.color.z != 0.6f || k.direction.x != 0.0f ||
        k.direction.y != -1.0f || k.direction.z != 0.0f) {
        ok = false; detail = "before the first key should be the first key";
    }
    k = SampleLight(keys, 15.0f);
    if (ok && (std::fabs(k.color.x - 0.3f) > 1e-5f || std::fabs(k.color.y - 0.4f) > 1e-5f ||
               std::fabs(k.color.z - 0.4f) > 1e-5f || std::fabs(k.direction.x - 0.5f) > 1e-5f ||
               std::fabs(k.direction.y + 1.0f) > 1e-5f || std::fabs(k.direction.z - 0.5f) > 1e-5f)) {
        ok = false; detail = "midpoint interpolation wrong";
    }
    k = SampleLight(keys, 12.5f);
    if (ok && std::fabs(k.color.x - 0.25f) > 1e-5f) { ok = false; detail = "frame 12.5 color.x wrong"; }
    k = SampleLight(keys, 25.0f);
    if (ok && (k.color.x != 0.4f || k.color.z != 0.2f || k.direction.x != 1.0f)) {
        ok = false; detail = "after the last key should be the last key";
    }
    std::vector<LightKf> single = {{{7, {0.1f, 0.2f, 0.3f}, {0, -1, 0}}}};
    k = SampleLight(single, 99.0f);
    if (ok && (k.frame != 99 || k.color.x != 0.1f)) { ok = false; detail = "single key wrong"; }
    k = SampleLight(keys, 15.7f);
    if (ok && k.frame != 15) { ok = false; detail = "returned frame should be (int)floor(15.7)"; }
    if (!ok) Check(false, "SampleLight", "%s", detail);
    else Check(true, "SampleLight");
}

// 11. SampleShadow: the last key at or before the frame holds until the next.
static void TestSampleShadow() {
    std::vector<ShadowKf> keys = {{{10, 1, 0.01f}, {20, 0, 0.05f}}};
    bool ok = true;
    const char* detail = "";
    ShadowKf k = SampleShadow(keys, 5.0f);
    if (k.mode != 1) { ok = false; detail = "before the first key should be the first key"; }
    k = SampleShadow(keys, 10.0f);
    if (ok && k.mode != 1) { ok = false; detail = "at the key should be that key"; }
    k = SampleShadow(keys, 19.9f);
    if (ok && (k.mode != 1 || std::fabs(k.distance - 0.01f) > 1e-6f)) {
        ok = false; detail = "19.9 should hold key 10 (mode 1, d 0.01)";
    }
    k = SampleShadow(keys, 20.0f);
    if (ok && k.mode != 0) { ok = false; detail = "at 20 should be mode 0"; }
    k = SampleShadow(keys, 100.0f);
    if (ok && k.mode != 0) { ok = false; detail = "after the last key should be the last key"; }
    // MMD's self-shadow distance UI value <-> the VMD value.
    if (ok && std::fabs(ShadowUiFromVmd(0.01125f) - 8875.0f) >= 0.5f) {
        ok = false; detail = "ShadowUiFromVmd(0.01125) should be 8875";
    }
    if (ok) {
        const float ui = 2500.0f;
        if (std::fabs(ShadowVmdFromUi(ShadowUiFromVmd(ui)) - ui) > 1e-2f) { ok = false; detail = "UI value round trip"; }
        const float d = 0.05f;
        if (std::fabs(ShadowUiFromVmd(ShadowVmdFromUi(d)) - d) > 1e-2f) { ok = false; detail = "VMD value round trip"; }
    }
    if (!ok) Check(false, "SampleShadow", "%s", detail);
    else Check(true, "SampleShadow");
}

// 12. InsertFrames / DeleteFrames / EndFrame / Empty over the light and shadow tracks.
static void TestLightShadowSpans() {
    MotionData d;
    d.light = Lights({0, 10, 20});
    d.shadow = Shadows({0, 10, 20});

    bool ok = d.InsertFrames(5, 3);
    const char* detail = "";
    if (!ok) { ok = false; detail = "InsertFrames(5,3) returned false"; }
    if (ok && (d.light.size() != 3 || d.light[0].frame != 0 || d.light[1].frame != 13 || d.light[2].frame != 23))
        ok = false, detail = "light insert frames wrong";
    if (ok && (d.shadow.size() != 3 || d.shadow[0].frame != 0 || d.shadow[1].frame != 13 || d.shadow[2].frame != 23))
        ok = false, detail = "shadow insert frames wrong";
    if (!ok) { Check(false, "LightShadow spans insert", "%s", detail); return; }

    ok = d.DeleteFrames(13, 1);
    if (!ok) { ok = false; detail = "DeleteFrames(13,1) returned false"; }
    if (ok && (d.light.size() != 2 || d.light[0].frame != 0 || d.light[1].frame != 22))
        ok = false, detail = "light delete frames wrong";
    if (ok && (d.shadow.size() != 2 || d.shadow[0].frame != 0 || d.shadow[1].frame != 22))
        ok = false, detail = "shadow delete frames wrong";
    if (ok && d.EndFrame() != 22) ok = false, detail = "EndFrame should be 22";
    if (!ok) { Check(false, "LightShadow spans delete", "%s", detail); return; }

    MotionData onlyShadow;
    onlyShadow.shadow = Shadows({5});
    if (onlyShadow.Empty()) Check(false, "LightShadow spans empty", "Empty() true with only a shadow key");
    else Check(true, "LightShadow spans");
}

// 13. VMD round trip of the light and shadow tracks (out of order keys, values preserved).
static void TestVmdRoundTripLightShadow() {
    mmdx::VmdMotion vmd;
    vmd.shadowKeys = {{30, 2, 0.02f}, {0, 1, 0.01f}, {15, 0, 0.05f}};
    vmd.lightKeys = {{30, {0.4f, 0.4f, 0.2f}, {1, -1, 1}}, {0, {0.2f, 0.4f, 0.6f}, {0, -1, 0}}};
    MotionData d = MotionData::FromVmd(vmd);

    bool ok = d.shadow.size() == 3 && d.shadow[0].frame == 0 && d.shadow[1].frame == 15 && d.shadow[2].frame == 30;
    const char* detail = "";
    if (!ok) { detail = "FromVmd shadow keys not sorted"; }
    if (ok && (d.shadow[0].mode != 1 || std::fabs(d.shadow[0].distance - 0.01f) > 1e-6f || d.shadow[1].mode != 0 ||
               std::fabs(d.shadow[1].distance - 0.05f) > 1e-6f || d.shadow[2].mode != 2 ||
               std::fabs(d.shadow[2].distance - 0.02f) > 1e-6f)) {
        ok = false; detail = "FromVmd shadow key values wrong";
    }
    if (ok && (d.light.size() != 2 || d.light[0].frame != 0 || d.light[1].frame != 30)) {
        ok = false; detail = "FromVmd light keys not sorted";
    }
    if (!ok) { Check(false, "VMD round trip light/shadow", "%s", detail); return; }

    mmdx::VmdMotion out = d.ToVmd();
    ok = out.shadowKeys.size() == 3;
    if (!ok) { detail = "ToVmd shadowKeys count wrong"; }
    if (ok && (out.shadowKeys[0].frame != 0 || out.shadowKeys[0].mode != 1 ||
               std::fabs(out.shadowKeys[0].distance - 0.01f) > 1e-6f || out.shadowKeys[1].frame != 15 ||
               out.shadowKeys[1].mode != 0 || std::fabs(out.shadowKeys[1].distance - 0.05f) > 1e-6f ||
               out.shadowKeys[2].frame != 30 || out.shadowKeys[2].mode != 2 ||
               std::fabs(out.shadowKeys[2].distance - 0.02f) > 1e-6f)) {
        ok = false; detail = "ToVmd shadow key values wrong";
    }
    if (ok && out.maxFrame < 30) ok = false, detail = "maxFrame should be >= 30";
    if (ok && (out.lightKeys.size() != 2 || out.lightKeys[0].frame != 0 || out.lightKeys[1].frame != 30)) {
        ok = false; detail = "ToVmd light keys wrong";
    }
    if (ok) {  // colour/direction bit-exact
        const mmdx::VmdLightKey& a = out.lightKeys[0];
        const mmdx::VmdLightKey& b = vmd.lightKeys[1];
        if (std::memcmp(&a.color, &b.color, sizeof(DirectX::XMFLOAT3)) != 0 ||
            std::memcmp(&a.direction, &b.direction, sizeof(DirectX::XMFLOAT3)) != 0) {
            ok = false; detail = "ToVmd light values not bit-exact";
        }
    }
    if (!ok) Check(false, "VMD round trip light/shadow", "%s", detail);
    else Check(true, "VMD round trip light/shadow");
}

// 14. TrackEditCommand undo/redo of the light and shadow tracks (whole-track snapshots).
static void TestLightShadowUndo() {
    StudioDoc doc;
    UpsertKey(doc.camera.light, LightKf{0, {0.2f, 0.4f, 0.6f}, {0, -1, 0}});
    UpsertKey(doc.camera.light, LightKf{10, {0.4f, 0.4f, 0.2f}, {1, -1, 1}});
    UpsertKey(doc.camera.shadow, ShadowKf{5, 1, 0.01125f});
    const uint64_t versionBefore = doc.cameraVersion;

    std::vector<TrackState> before;
    before.push_back(CaptureTrack(doc, -1, RowKind::Light, ""));
    before.push_back(CaptureTrack(doc, -1, RowKind::Shadow, ""));

    // modify: erase a light key, change the shadow mode
    EraseKey(doc.camera.light, 0);
    doc.camera.shadow[0].mode = 2;

    std::vector<TrackState> after;
    after.push_back(CaptureTrack(doc, -1, RowKind::Light, ""));
    after.push_back(CaptureTrack(doc, -1, RowKind::Shadow, ""));

    doc.history.Push(std::make_unique<TrackEditCommand>(doc, "edit", before, after));

    bool ok = doc.camera.light.size() == 1 && doc.camera.shadow[0].mode == 2;
    const char* detail = "";
    if (!ok) detail = "Push did not apply the modified state";
    doc.history.Undo();
    if (ok && (doc.camera.light.size() != 2 || doc.camera.light[0].frame != 0 || doc.camera.light[1].frame != 10 ||
               doc.camera.shadow[0].mode != 1 || std::fabs(doc.camera.shadow[0].distance - 0.01125f) > 1e-6f)) {
        ok = false; detail = "Undo did not restore the original keys exactly";
    }
    if (ok && doc.cameraVersion <= versionBefore) ok = false, detail = "cameraVersion not bumped by restore";
    const uint64_t versionAfterUndo = doc.cameraVersion;
    doc.history.Redo();
    if (ok && (doc.camera.light.size() != 1 || doc.camera.shadow[0].mode != 2)) {
        ok = false; detail = "Redo did not re-apply the modified state";
    }
    if (ok && doc.cameraVersion <= versionAfterUndo) ok = false, detail = "cameraVersion not bumped by redo";
    if (!ok) Check(false, "LightShadow undo", "%s", detail);
    else Check(true, "LightShadow undo");
}

// 9. Performance: 100k keys, move + erase.
static void TestPerformance() {
    const int kTracks = 40, kKeys = 2500;
    std::vector<std::vector<BoneKf>> tracks(kTracks);
    std::vector<std::set<int>> selection(kTracks);
    for (int t = 0; t < kTracks; ++t) {
        tracks[t].reserve(kKeys);
        for (int i = 0; i < kKeys; ++i) {
            const int f = i * 2;
            tracks[t].push_back(BoneKf{f, {}, {0, 0, 0, 1}, {}});
            selection[t].insert(f);
        }
    }
    bool ok = true;
    const char* detail = "";
    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < kTracks; ++t) MoveKeyFrames(tracks[t], selection[t], 3);
    auto t1 = std::chrono::steady_clock::now();
    // The move shifted every frame by +3: re-select every key so the erase covers them all, and verify the move.
    for (int t = 0; t < kTracks && ok; ++t) {
        if (tracks[t].size() != (size_t)kKeys || tracks[t].front().frame != 3) { ok = false; detail = "post-move track wrong"; }
        for (const BoneKf& k : tracks[t]) selection[t].insert(k.frame);
    }
    for (int t = 0; t < kTracks; ++t) EraseKeyFrames(tracks[t], selection[t]);
    auto t2 = std::chrono::steady_clock::now();
    const auto moveMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto eraseMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    std::printf("perf: move %lld ms, erase %lld ms (100k keys)\n", (long long)moveMs, (long long)eraseMs);
    if (!ok) { Check(false, "Performance", "%s", detail); return; }
    for (int t = 0; t < kTracks; ++t)
        if (!tracks[t].empty()) { ok = false; detail = "post-erase track not empty"; }
    if (moveMs >= 500 || eraseMs >= 500) { ok = false; detail = "phase over 500 ms"; }
    if (!ok) Check(false, "Performance", "%s", detail);
    else Check(true, "Performance");
}

int main() {
    TestFindKey();
    TestMoveReplace();
    TestMoveClamp();
    TestMoveNegative();
    TestInsertSpan();
    TestDeleteSpan();
    TestMotionDataSpans();
    TestCommandStackBudget();
    TestSampleLight();
    TestSampleShadow();
    TestLightShadowSpans();
    TestVmdRoundTripLightShadow();
    TestLightShadowUndo();
    TestPerformance();
    std::printf("studio_edit_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
