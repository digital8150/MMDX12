// Console tests for the Studio viewport pose-editing overlay (Gizmo.h): projection, bone overlay
// helpers, bone picking, and translate/rotate gizmo hit testing + drag solving.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "studio/Gizmo.h"
#include "anim/ModelInstance.h"
#include "asset/PmxModel.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace DirectX;
using namespace mmdx;
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

namespace {

constexpr float kPi = 3.14159265358979f;

XMVECTOR Load(const XMFLOAT3& v) { return XMLoadFloat3(&v); }
XMFLOAT3 Store(XMVECTOR v) { XMFLOAT3 r; XMStoreFloat3(&r, v); return r; }

} // namespace

static XMFLOAT4X4 TestView(const XMFLOAT3& eye, const XMFLOAT3& target) {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixLookAtLH(Load(eye), Load(target), XMVectorSet(0, 1, 0, 0)));
    return m;
}

static constexpr float kFovY = 30.0f * 3.14159265358979f / 180.0f;

int main() {
    // ---- 1. Project: target -> viewport centre; behind-the-eye point fails.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        ImVec2 out;
        const bool ok = vp.Project({0, 10, 0}, out) && std::fabs(out.x - 500.0f) < 0.01f &&
                        std::fabs(out.y - 350.0f) < 0.01f;
        ImVec2 behind;
        const bool behindFails = !vp.Project({0, 10, -60}, behind);
        Check(ok && behindFails, "Project centre / behind eye", "centre (%.3f, %.3f)", out.x, out.y);
    }

    // ---- 2. PixelWorldSize at the target depth (z = 50).
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const float expect = 2.0f * 50.0f * std::tan(kFovY * 0.5f) / 600.0f;
        const float got = vp.PixelWorldSize({0, 10, 0});
        Check(std::fabs(got - expect) < 1e-4f, "PixelWorldSize", "got %.6f want %.6f", got, expect);
    }

    static constexpr float kArrow = 80.0f;  // GizmoStyle default arrowLength (px)

    // ---- 3. Translate hit test: AxisX, AxisY, None far away, PlaneXY.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const GizmoFrame f;  // centre (0,10,0)
        const GizmoStyle s;
        ImVec2 cp;
        if (!vp.Project(f.center, cp)) {
            Check(false, "Translate hit test", "centre failed to project");
            return 1;
        }
        const float arrowPx = kArrow;  // at the target depth 1 px == 1 px
        const GizmoPart hitX = GizmoHitTest(vp, f, GizmoMode::Translate, s, {cp.x + arrowPx * 0.6f, cp.y});
        const GizmoPart hitY = GizmoHitTest(vp, f, GizmoMode::Translate, s, {cp.x, cp.y - arrowPx * 0.6f});
        const GizmoPart hitFar = GizmoHitTest(vp, f, GizmoMode::Translate, s, {cp.x + 300.0f, cp.y + 300.0f});
        const float planeMid = 16.0f + 18.0f * 0.5f;  // planeOffset + planeSize / 2 (px, at target depth)
        const GizmoPart hitPlane = GizmoHitTest(vp, f, GizmoMode::Translate, s,
                                                {cp.x + planeMid, cp.y - planeMid});
        const bool ok = hitX == GizmoPart::AxisX && hitY == GizmoPart::AxisY && hitFar == GizmoPart::None &&
                        hitPlane == GizmoPart::PlaneXY;
        Check(ok, "Translate hit test",
              "axis %d, y %d, far %d, plane %d", (int)hitX, (int)hitY, (int)hitFar, (int)hitPlane);
    }

    // ---- 4. Drag AxisX by +40 px: x scales with PixelWorldSize, y/z unchanged.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const GizmoFrame f;
        const GizmoStyle s;
        ImVec2 cp;
        vp.Project(f.center, cp);
        const GizmoDrag d = BeginGizmoDrag(vp, f, GizmoMode::Translate, s, GizmoPart::AxisX, cp);
        const XMFLOAT3 t = GizmoDragTranslation(d, {cp.x + 40.0f, cp.y});
        const float pixelWorld = vp.PixelWorldSize(f.center);
        const float expectX = 40.0f * pixelWorld;
        const bool ok = t.x > 0.0f && std::fabs(t.y) < 1e-4f && std::fabs(t.z) < 1e-4f &&
                        std::fabs(t.x - expectX) < 0.02f * expectX;
        Check(ok, "Drag AxisX", "t (%.4f, %.4f, %.4f) want x %.4f", t.x, t.y, t.z, expectX);
    }

    // ---- 5. Drag PlaneXY by (+30, -20) px: x > 0, y > 0, z unchanged.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const GizmoFrame f;
        const GizmoStyle s;
        ImVec2 cp;
        vp.Project(f.center, cp);
        const GizmoDrag d = BeginGizmoDrag(vp, f, GizmoMode::Translate, s, GizmoPart::PlaneXY, cp);
        const XMFLOAT3 t = GizmoDragTranslation(d, {cp.x + 30.0f, cp.y - 20.0f});
        const bool ok = t.x > 0.0f && t.y > 0.0f && std::fabs(t.z) < 1e-4f;
        Check(ok, "Drag PlaneXY", "t (%.4f, %.4f, %.4f)", t.x, t.y, t.z);
    }

    // ---- 6. The Z arrow is degenerate (camera looks along +Z): AxisZ is never hit.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 600.0f, 600.0f);
        const GizmoFrame f;
        const GizmoStyle s;
        ImVec2 cp;
        vp.Project(f.center, cp);
        bool hit = false;
        for (float dx = -100.0f; dx <= 100.0f; dx += 4.0f)
            for (float dy = -100.0f; dy <= 100.0f; dy += 4.0f)
                if (GizmoHitTest(vp, f, GizmoMode::Translate, s, {cp.x + dx, cp.y + dy}) == GizmoPart::AxisZ)
                    hit = true;
        Check(!hit, "Z arrow degenerate (never AxisZ)");
    }

    // ---- 7. Rotate: RingZ faces the camera; positive angle moves the grab point with the mouse.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const GizmoFrame f;
        const GizmoStyle s;
        ImVec2 cp;
        vp.Project(f.center, cp);
        const ImVec2 grab{cp.x + s.ringRadius, cp.y};  // rightmost point of RingZ
        const GizmoPart hit = GizmoHitTest(vp, f, GizmoMode::Rotate, s, grab);
        if (hit != GizmoPart::RingZ) {
            Check(false, "Rotate RingZ drag sign", "hit test gave %d", (int)hit);
            return 1;
        }
        const GizmoDrag d = BeginGizmoDrag(vp, f, GizmoMode::Rotate, s, GizmoPart::RingZ, grab);
        bool ok = true;
        const char* detail = "";
        for (const float dy : {-30.0f, 30.0f}) {
            const float angle = GizmoDragAngle(d, {grab.x, grab.y + dy}, nullptr);
            XMFLOAT3 axis;
            const float angle2 = GizmoDragAngle(d, {grab.x, grab.y + dy}, &axis);
            // Apply the rotation to the grab vector and compare screen heights.
            const XMVECTOR r = XMVectorSet(s.ringRadius, 0, 0, 0);
            const XMVECTOR rotated = XMVector3Rotate(r, XMQuaternionRotationAxis(Load(axis), angle));
            const float rotatedY = XMVectorGetY(rotated);
            if (!((dy < 0 && rotatedY > 0.0f && angle > 0.0f) || (dy > 0 && rotatedY < 0.0f && angle < 0.0f))) {
                ok = false;
                detail = "angle sign wrong";
            }
            if (std::fabs(angle - angle2) > 1e-6f) {
                ok = false;
                detail = "axis overload differs";
            }
        }
        Check(ok, "Rotate RingZ drag sign", "%s", detail);
    }

    // ---- 8. RingY with a top-down camera (edge-on): the same follow-the-mouse property.
    {
        const ViewProj vp = MakeViewProj(TestView({0, 60, 0.001f}, {0, 0, 0}), {0, 60, 0.001f}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const GizmoFrame f;  // centre (0,0,0)
        const GizmoStyle s;
        ImVec2 cp;
        if (!vp.Project(f.center, cp)) {
            Check(false, "Rotate RingY top-down", "centre failed to project");
            return 1;
        }
        // Tangential grab point: to the right of the ring centre on screen.
        const ImVec2 grab{cp.x + s.ringRadius, cp.y};
        const GizmoPart hit = GizmoHitTest(vp, f, GizmoMode::Rotate, s, grab);
        if (hit != GizmoPart::RingY) {
            Check(false, "Rotate RingY top-down", "hit test gave %d", (int)hit);
            return 1;
        }
        const GizmoDrag d = BeginGizmoDrag(vp, f, GizmoMode::Rotate, s, GizmoPart::RingY, grab);
        bool ok = true;
        char detail[128] = "";
        const float scale = vp.PixelWorldSize(f.center);
        // The grabbed ring sample: nearest sample to the mouse in screen space (as BeginGizmoDrag picks).
        // With this camera the screen x axis is world -X, so the grab point is on world -X, not +X.
        const auto ringPoint = [&](float angle) {
            // RingY: U = axes Z, V = axes X (orthonormal frame), C + R * (cos a * U + sin a * V).
            const float ca = std::cos(angle) * s.ringRadius * scale;
            const float sa = std::sin(angle) * s.ringRadius * scale;
            return XMFLOAT3{f.center.x + sa, f.center.y, f.center.z + ca};
        };
        float bestD = 1e30f;
        XMFLOAT3 grabWorld = f.center;
        for (int k = 0; k < 64; ++k) {
            const XMFLOAT3 p = ringPoint(2.0f * kPi / 64.0f * k);
            ImVec2 q;
            if (!vp.Project(p, q)) continue;
            const float dist = std::hypot(grab.x - q.x, grab.y - q.y);
            if (dist < bestD) {
                bestD = dist;
                grabWorld = p;
            }
        }
        for (const float dy : {-30.0f, 30.0f}) {
            const float angle = GizmoDragAngle(d, {grab.x, grab.y + dy}, nullptr);
            XMFLOAT3 axis;
            GizmoDragAngle(d, {grab.x, grab.y + dy}, &axis);
            // Apply the rotation to the grab vector (P - C) and compare screen heights.
            const XMVECTOR r = XMVectorSubtract(XMLoadFloat3(&grabWorld), XMLoadFloat3(&f.center));
            const XMVECTOR rotated = XMVector3Rotate(r, XMQuaternionRotationAxis(Load(axis), angle));
            const XMFLOAT3 wr = Store(XMVectorAdd(rotated, XMLoadFloat3(&f.center)));
            ImVec2 pGrab, pRot;
            if (!vp.Project(grabWorld, pGrab) || !vp.Project(wr, pRot)) {
                ok = false;
                std::snprintf(detail, sizeof(detail), "projection failed on ring point");
                break;
            }
            // The rotated point must move up for the up-drag and down for the down-drag.
            const bool follows = dy < 0 ? pRot.y < pGrab.y : pRot.y > pGrab.y;
            if (!follows) {
                ok = false;
                std::snprintf(detail, sizeof(detail), "dy %.0f: angle %.4f, rot y %.1f -> %.1f",
                              dy, angle, pGrab.y, pRot.y);
                break;
            }
        }
        Check(ok, "Rotate RingY top-down", "%s", detail);
    }

    // ---- 9. Bone overlay helpers with a tiny synthetic model.
    {
        auto model = std::make_shared<PmxModel>();
        model->bones.resize(3);
        model->bones[0].position = {0, 0, 0};
        model->bones[0].flags = PmxBone_Rotatable | PmxBone_Movable | PmxBone_Visible;
        model->bones[0].parentIndex = -1;
        model->bones[0].tailBoneIndex = 1;  // TailIsBone
        model->bones[1].position = {0, 5, 0};
        model->bones[1].flags = PmxBone_Rotatable | PmxBone_Visible;
        model->bones[1].parentIndex = 0;
        model->bones[1].tailOffset = {0, 3, 0};
        model->bones[2].position = {0, 8, 0};
        model->bones[2].flags = 0;  // hidden
        model->bones[2].parentIndex = 1;
        model->bones[0].flags |= PmxBone_TailIsBone;

        ModelInstance inst(model);
        bool ok = true;
        const char* detail = "";

        const bool s0 = BoneShownInOverlay(*model, 0);
        const bool s1 = BoneShownInOverlay(*model, 1);
        const bool s2 = BoneShownInOverlay(*model, 2);
        if (!(s0 && s1 && !s2)) { ok = false; detail = "shown flags wrong"; }

        const XMFLOAT3 j1 = BoneJointWorld(*model, inst, 1);
        if (std::fabs(j1.x) > 1e-4f || std::fabs(j1.y - 5.0f) > 1e-4f || std::fabs(j1.z) > 1e-4f) {
            ok = false; detail = "BoneJointWorld(1) wrong";
        }

        XMFLOAT3 tail0, tail1, tailNone;
        const bool has0 = BoneTailWorld(*model, inst, 0, tail0);
        const bool has1 = BoneTailWorld(*model, inst, 1, tail1);
        if (!has0 || std::fabs(tail0.y - 5.0f) > 1e-4f) { ok = false; detail = "BoneTailWorld(0) wrong"; }
        if (ok && (!has1 || std::fabs(tail1.y - 8.0f) > 1e-4f)) { ok = false; detail = "BoneTailWorld(1) wrong"; }
        if (ok && BoneTailWorld(*model, inst, 2, tailNone)) { ok = false; detail = "hidden bone tail should fail"; }

        // Picking: from the camera above, the bones project onto the viewport.
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const BoneOverlayStyle style;

        const XMFLOAT3 w0 = BoneJointWorld(*model, inst, 0);
        const XMFLOAT3 w1 = BoneJointWorld(*model, inst, 1);
        const XMFLOAT3 w2 = BoneJointWorld(*model, inst, 2);
        ImVec2 p0, p1, p2;
        if (!vp.Project(w0, p0) || !vp.Project(w1, p1) || !vp.Project(w2, p2)) {
            ok = false; detail = "bone projection failed";
        }
        if (ok) {
            const int pick1 = PickBone(vp, *model, inst, p1, style);
            const int pickFar = PickBone(vp, *model, inst, {p0.x - 500.0f, p0.y - 500.0f}, style);
            if (pick1 != 1) { ok = false; detail = "PickBone at joint 1 picked the wrong bone"; }
            else if (pickFar != -1) { ok = false; detail = "PickBone far away picked a bone"; }
            else {
                // Hidden bone 2 sits 3 units above bone 1; at its exact projection only the nearest
                // shown joint within pickRadius wins. Compute the pixel distance and assert accordingly.
                const float dPx = std::hypot(p2.x - p1.x, p2.y - p1.y);
                const int pick2 = PickBone(vp, *model, inst, p2, style);
                const int expect = dPx > (int)style.pickRadius ? -1 : 1;
                if (pick2 != expect) {
                    ok = false;
                    detail = dPx > style.pickRadius ? "hidden bone picked at its projection"
                                                    : "nearest shown bone not picked";
                }
            }
        }
        if (ok) Check(true, "Bone overlay helpers");
        else Check(false, "Bone overlay helpers", "%s", detail);
    }

    // ---- 10. Draw smoke: bones + gizmo gain vertices on the foreground draw list.
    {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini in the working directory
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1000.0f, 700.0f);
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        ImGui::NewFrame();
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 100.0f, 50.0f, 800.0f, 600.0f);
        const GizmoFrame f;
        const GizmoStyle s;
        auto model = std::make_shared<PmxModel>();
        model->bones.resize(2);
        model->bones[0].position = {0, 0, 0};
        model->bones[0].flags = PmxBone_Rotatable | PmxBone_Movable | PmxBone_Visible;
        model->bones[0].tailOffset = {0, 5, 0};
        model->bones[1].position = {0, 5, 0};
        model->bones[1].flags = PmxBone_Rotatable | PmxBone_Visible;
        model->bones[1].parentIndex = 0;
        ModelInstance inst(model);
        const std::set<int> selected;
        DrawBoneOverlay(dl, vp, *model, inst, selected, -1, -1, BoneOverlayStyle());
        DrawGizmo(dl, vp, f, GizmoMode::Rotate, s, GizmoPart::None, GizmoPart::None);
        DrawGizmo(dl, vp, f, GizmoMode::Translate, s, GizmoPart::None, GizmoPart::None);

        const bool ok = dl->VtxBuffer.Size > 0;
        ImGui::EndFrame();
        ImGui::DestroyContext();
        Check(ok, "Draw smoke (vertex count)");
    }

    // ---- 11. ProjectSegment
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 0.0f, 0.0f, 800.0f, 600.0f);
        ImVec2 pa, pb;
        
        // both in front
        bool ok = ProjectSegment(vp, {0, 10, 0}, {5, 10, 0}, pa, pb);
        ImVec2 ea, eb;
        vp.Project({0, 10, 0}, ea);
        vp.Project({5, 10, 0}, eb);
        Check(ok && std::fabs(pa.x - ea.x) < 1e-4f && std::fabs(pb.x - eb.x) < 1e-4f, "ProjectSegment front");
        
        // a in front (0,10,0) and b behind (0,10,-60)
        ok = ProjectSegment(vp, {0, 10, 0}, {0, 10, -60}, pa, pb);
        Check(ok && std::fabs(pa.x - ea.x) < 1e-4f && std::fabs(pb.x - 400.0f) < 1.0f && std::isfinite(pb.x) && std::isfinite(pb.y), "ProjectSegment clipped");

        // both behind
        ok = ProjectSegment(vp, {0, 10, -60}, {0, 10, -70}, pa, pb);
        Check(!ok, "ProjectSegment behind");
    }

    // ---- 12. PickCameraKey
    {
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 0.0f, 0.0f, 800.0f, 600.0f);
        XMFLOAT3 keys[2] = {{0, 10, 0}, {5, 10, 0}};
        ImVec2 ea;
        vp.Project(keys[0], ea);
        ImVec2 mouse = {ea.x + 2.0f, ea.y + 1.0f};
        int pick1 = PickCameraKey(vp, keys, 2, mouse, 5.0f);
        int pickFar = PickCameraKey(vp, keys, 2, {ea.x + 100.0f, ea.y}, 5.0f);
        Check(pick1 == 0 && pickFar == -1, "PickCameraKey");
    }

    // ---- 13. DrawCameraPath smoke
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // no imgui.ini in the working directory
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ViewProj vp = MakeViewProj(TestView({0, 10, -50}, {0, 10, 0}), {0, 10, -50}, kFovY,
                                         0.5f, 3000.0f, 0.0f, 0.0f, 800.0f, 600.0f);
        
        std::vector<CameraPathPoint> path;
        for (int i = 0; i < 100; ++i) {
            float a = i * 2.0f * kPi / 100.0f;
            CameraPathPoint pt;
            pt.eye = {30.0f * std::cos(a), 10.0f, 30.0f * std::sin(a)};
            if (i == 50) pt.eye.x += 100.0f; // cut
            pt.target = {0, 10, 0};
            pt.fovY = 0.5f;
            XMStoreFloat4x4(&pt.view, XMMatrixLookAtLH(Load(pt.eye), Load(pt.target), XMVectorSet(0, 1, 0, 0)));
            path.push_back(pt);
        }
        XMFLOAT3 keys[5];
        for (int i = 0; i < 5; ++i) keys[i] = path[i * 20].eye;
        
        std::set<int> selected = {1};
        CameraPathStyle style;
        
        DrawCameraPath(dl, vp, path.data(), 100, keys, 5, selected, &path[0], 1.5f, 10.0f, 90.0f, style);
        bool ok = dl->VtxBuffer.Size > 0;
        
        int prevSize = dl->VtxBuffer.Size;
        DrawCameraPath(dl, vp, nullptr, 0, nullptr, 0, selected, nullptr, 1.5f, 10.0f, 90.0f, style);
        bool ok2 = dl->VtxBuffer.Size == prevSize;
        
        Check(ok && ok2, "DrawCameraPath smoke test");
        
        ImGui::EndFrame();
        ImGui::DestroyContext();
    }

    std::printf("studio_gizmo_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
