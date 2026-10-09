// Studio camera VMD / self-shadow: the inspector panel of the camera target (view mode, key from view, the VMD light
// track, the shadow track switch, editable key values), the camera path overlay in the viewport, and how the shadow
// track drives the renderer (the light track drives the sun: BuildSceneLighting). Independent of any model being loaded.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

using namespace studio;

namespace {
constexpr float kRenderAspect = 16.0f / 9.0f;  // every video size of the render dialog is 16:9
constexpr float kOrthoFovDeg = 3.0f;  // "perspective off" is drawn as a long lens from far away (see StudioCamera)

uint64_t RowOf(RowKind k) { return MakeRowId(k, 0, 0); }

// Last key at or before `frame` (the first before it): MMD switches perspective per key, it does not interpolate it.
bool PerspectiveAt(const std::vector<CameraKf>& keys, float frame) {
    if (keys.empty()) return true;
    auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](float f, const CameraKf& k) { return f < (float)k.frame; });
    return it == keys.begin() ? keys.front().perspective : std::prev(it)->perspective;
}
} // namespace

// ---------------------------------------------------------------------------
// Camera / light evaluation
// ---------------------------------------------------------------------------

bool App::StudioCameraPerspective(float frame) const {
    return PerspectiveAt(studio_->camera.camera, frame);
}

void App::StudioOrthoCamera(CameraPose& pose, CameraParams& camera) const {
    // MMD's "perspective off" is an orthographic view whose frame size at the target is the perspective one. The
    // renderer is perspective-only (SSAO/SSR/TAA reconstruct from a perspective depth), so the view is approximated by a
    // 3 degree lens moved back until the frame at the target has the same size: parallel lines stay parallel to within
    // a fraction of a pixel.
    // distance 0 (the target is the eye) has no frame size: use MMD's default distance
    const float halfH = (std::fabs(pose.distance) < 1.0f ? 45.0f : std::fabs(pose.distance)) * std::tan(DirectX::XMConvertToRadians(pose.fovDeg) * 0.5f);
    const float dist = halfH / std::tan(DirectX::XMConvertToRadians(kOrthoFovDeg) * 0.5f);
    pose.distance = pose.distance > 0.0f ? dist : -dist;
    pose.fovDeg = kOrthoFovDeg;
    CameraMotion::ToView(pose, &camera.view, &camera.eye);
    camera.fovYRadians = DirectX::XMConvertToRadians(kOrthoFovDeg);
    camera.nearZ = std::max(0.5f, dist - 400.0f);
    camera.farZ = dist + 3000.0f;
}

void App::ApplyLightShadowTracks(const std::vector<LightKf>& light, const std::vector<ShadowKf>& shadow, float frame,
                                FrameView& view) {
    if (!light.empty()) {
        const LightKf k = SampleLight(light, frame);
        // a zero direction (broken file) keeps the preset's
        if (std::fabs(k.direction.x) + std::fabs(k.direction.y) + std::fabs(k.direction.z) > 1e-4f) view.light.direction = k.direction;
        view.light.color = k.color;
    }
    if (!shadow.empty()) {
        const ShadowKf s = SampleShadow(shadow, frame);
        view.shadowsOff = s.mode == 0;
        view.shadowDistance = ShadowRangeFromVmd(s.distance);
    }
}

void App::StudioApplyLightTracks(FrameView& view) const {
    const StudioDoc& d = *studio_;
    // the camera VMD's light track drives the sun inside BuildSceneLighting; only the self-shadow track is applied here
    static const std::vector<LightKf> kNoLight;
    static const std::vector<ShadowKf> kNoShadow;
    ApplyLightShadowTracks(kNoLight, d.useShadowTrack ? d.camera.shadow : kNoShadow, (float)(d.time * kMmdFps), view);
}

LightKf App::StudioCurrentLight() const {
    return StudioSunLight((float)(studio_->time * kMmdFps));
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void App::StudioKeyCameraFromView() {
    StudioDoc& d = *studio_;
    // StudioInsertKeys keys the free camera when it is the view, else the motion camera's value at the frame
    StudioInsertKeys({RowOf(RowKind::Camera)}, d.Frame());
    if (!d.useMotionCamera) {
        // the key now is what the free camera shows: viewing through the motion camera keeps the view where it is
        d.useMotionCamera = true;
    }
    d.selectedModel = -1;
}

// One undo step per field drag / typed value: the track is captured when a field becomes active and pushed when it
// is released (or when the edited key disappears).
void App::StudioBeginKeyEdit(RowKind kind) {
    if (studioKeyEdit_) return;
    studioKeyBefore_ = {CaptureTrack(*studio_, -1, kind, "")};
    studioKeyEdit_ = true;
    studioKeyChanged_ = false;
}

void App::StudioEndKeyEdit() {
    if (!studioKeyEdit_) return;
    // a click that changed nothing (e.g. the first click of a double click into text input) is no undo step
    if (studioKeyChanged_) StudioPushTrackEdit(Tr("키 값 편집"), studioKeyBefore_);
    studioKeyBefore_.clear();
    studioKeyEdit_ = false;
    studioKeyLive_ = false;
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

namespace {
// Shaded ball preview of a directional light as seen from the viewing camera (like MMD's light panel).
void LightBall(ImDrawList* dl, ImVec2 center, float radius, const DirectX::XMFLOAT4X4& view, const LightKf& k, ImU32 rim) {
    using namespace DirectX;
    XMVECTOR L = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&k.direction), XMLoadFloat4x4(&view)));
    if (XMVector3Equal(L, XMVectorZero())) L = XMVectorSet(0, -1, 0, 0);
    XMFLOAT3 l;
    XMStoreFloat3(&l, XMVectorNegate(L));  // toward the light
    constexpr int kCells = 26;
    const float cell = radius * 2.0f / kCells;
    const auto toByte = [](float v) { return (int)std::lround(255.0f * std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f)); };
    for (int j = 0; j < kCells; ++j)
        for (int i = 0; i < kCells; ++i) {
            const float u = ((i + 0.5f) / kCells) * 2.0f - 1.0f, v = ((j + 0.5f) / kCells) * 2.0f - 1.0f;
            const float r2 = u * u + v * v;
            if (r2 > 1.0f) continue;
            // view space: x right, y up, the visible hemisphere faces the camera (-z)
            const float nx = u, ny = -v, nz = -std::sqrt(1.0f - r2);
            const float lambert = std::max(0.0f, nx * l.x + ny * l.y + nz * l.z);
            const float amb = 0.10f;
            const ImU32 col = IM_COL32(toByte(amb + lambert * k.color.x * 1.4f), toByte(amb + lambert * k.color.y * 1.4f),
                                       toByte(amb + lambert * k.color.z * 1.4f), 255);
            const ImVec2 a(center.x - radius + i * cell, center.y - radius + j * cell);
            dl->AddRectFilled(a, ImVec2(a.x + cell + 0.6f, a.y + cell + 0.6f), col);
        }
    dl->AddCircle(center, radius, rim, 48, 1.0f);
}
} // namespace

void App::DrawStudioCameraPanel(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const auto separator = [&](float gap) {
        ImGui::Dummy(ImVec2(w, Dp(gap)));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        cdl->AddLine(c, ImVec2(c.x + w, c.y), p.line);
        ImGui::Dummy(ImVec2(w, Dp(8.0f)));
    };
    // a full-width widget with a square icon button at the right end of the row
    const float btn = 34.0f, bw = Dp(btn);
    char buf[160];
    // A camera key is being edited: its fields and curves follow, so the light / shadow sections shrink to their
    // switches (their values and key buttons come back when no camera key is selected).
    bool compact = false;
    {
        RowKind kind;
        std::string name;
        for (const KeyId& k : d.selection)
            if (StudioTrackOfRow(k.first, kind, name)) { compact = kind == RowKind::Camera; break; }
    }

    // --- view: motion / free camera, key from the view, path overlay
    separator(2.0f);
    {
        const char* modes[] = {Tr("카메라 모션"), Tr("자유 카메라")};
        const char* modeIcons[] = {icon::VideoCamera, icon::Crosshair};
        int mode = d.useMotionCamera && d.cameraEval ? 0 : 1;
        ImGui::BeginDisabled(!d.cameraEval);
        if (Segmented("##viewmode", modes, 2, &mode, w / Dpi(), 32.0f, modeIcons)) {
            d.useMotionCamera = mode == 0;
            if (mode == 1) StudioPossess(false);  // a free view is not the possessed camera
        }
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        if (Button("##camkey", Tr("현재 시점을 키로 등록"), icon::Plus, ButtonKind::Secondary, ImVec2((w - 2.0f * bw) / Dpi() - 12.0f, btn)))
            StudioKeyCameraFromView();
        Tooltip(d.useMotionCamera && d.cameraEval ? Tr("카메라 모션의 현재 값으로 키를 만들어요.")
                                                  : Tr("자유 카메라로 보고 있는 시점으로 키를 만들어요."));
        ImGui::SameLine(0, Dp(6.0f));
        if (IconButton("##campossess", icon::Crosshair, d.possessCamera ? Tr("카메라 빙의 해제  (Esc)") : Tr("카메라 빙의: 뷰포트 조작이 이 카메라를 움직여요"),
                       d.possessCamera, btn))
            StudioPossess(!d.possessCamera);
        ImGui::SameLine(0, Dp(6.0f));
        if (IconButton("##campath", d.showCameraPath ? icon::Eye : icon::EyeSlash, d.showCameraPath ? Tr("카메라 경로 숨기기 (자유 카메라로 볼 때 표시)")
                                                                 : Tr("카메라 경로 표시 (자유 카메라로 볼 때)"),
                       d.showCameraPath, btn))
            d.showCameraPath = !d.showCameraPath;
        // render frame guides (shown when looking through the motion camera)
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        const float gw = (w / Dpi() - 12.0f) / 3.0f;
        if (Button("##fmask", Tr("16:9 프레임"), nullptr, d.frameMask ? ButtonKind::Primary : ButtonKind::Secondary, ImVec2(gw, 30.0f)))
            d.frameMask = !d.frameMask;
        Tooltip(Tr("카메라 모션으로 볼 때 렌더 영상과 같은 16:9 영역만 보여줘요."));
        ImGui::SameLine(0, Dp(6.0f));
        if (Button("##fthirds", Tr("3분할"), nullptr, d.showThirds ? ButtonKind::Primary : ButtonKind::Secondary, ImVec2(gw, 30.0f)))
            d.showThirds = !d.showThirds;
        ImGui::SameLine(0, Dp(6.0f));
        if (Button("##fsafe", Tr("세이프"), nullptr, d.showSafeFrames ? ButtonKind::Primary : ButtonKind::Secondary, ImVec2(gw, 30.0f)))
            d.showSafeFrames = !d.showSafeFrames;
        Tooltip(Tr("액션 세이프 93 % · 타이틀 세이프 90 %"));
    }

    // --- the camera at the playhead: always editable (auto-key keys the edit), key state like an AE/Blender property
    separator(8.0f);
    {
        const bool keyed = FindKey(d.camera.camera, d.Frame()) != nullptr;
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, Tr("카메라 값"));
        // key state at the playhead: filled = key here, hollow = interpolated between keys
        const ImVec2 dc(c.x + w - Dp(10.0f), c.y + Dp(8.0f));
        const float dr = Dp(5.0f);
        const ImU32 kc = keyed ? p.accent : p.ink3;
        const ImVec2 pts[4] = {{dc.x, dc.y - dr}, {dc.x + dr, dc.y}, {dc.x, dc.y + dr}, {dc.x - dr, dc.y}};
        if (keyed) cdl->AddConvexPolyFilled(pts, 4, kc);
        else cdl->AddPolyline(pts, 4, kc, ImDrawFlags_Closed, 1.5f);
        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(w, Dp(22.0f)));
        Switch("##autokey", Tr("자동 키"), &d.autoKey, Tr("값을 고치면 재생 헤드에 키가 생겨요"));
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        DrawStudioCameraKeyFields(w, RowKind::Camera, d.Frame(), true);  // disabled without a key when auto-key is off
    }

    // --- the camera VMD's light track: the sun's effective light (the ball), its numbers, the track's key count and the
    // key button. The track drives the main light's colour and direction while the light is linked to the VMD.
    separator(compact ? 4.0f : 8.0f);
    if (compact) {
        // a camera key is being edited above: only the self-shadow switch stays
        Switch("##shadowtrack", Tr("셀프 섀도 트랙"), &d.useShadowTrack);
        separator(4.0f);
        return;
    }
    {
        const ImVec2 t0 = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, t0, p.ink2, Tr("조명 트랙"));
        std::snprintf(buf, sizeof(buf), Tr("키 %d개"), (int)d.camera.light.size());
        const ImVec2 ks = TextSize(Font::Regular, size::Caption, buf);
        Text(cdl, Font::Regular, size::Caption, ImVec2(t0.x + w - ks.x - Dp(4.0f), t0.y + Dp(2.0f)), p.ink3, buf);
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));
        Tooltip(Tr("카메라 VMD의 조명 키예요. 메인 조명이 VMD와 연동돼 있으면 그 색과 방향을 따라요."));
        const LightKf cur = StudioCurrentLight();
        const float r = Dp(20.0f);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        LightBall(cdl, ImVec2(c.x + r, c.y + r), r, studioVp_.view, cur, p.line);
        const float tx = c.x + r * 2.0f + Dp(12.0f);
        std::snprintf(buf, sizeof(buf), "%d, %d, %d", (int)std::lround(cur.color.x * 256.0f),
                      (int)std::lround(cur.color.y * 256.0f), (int)std::lround(cur.color.z * 256.0f));
        Text(cdl, Font::Regular, size::Caption, ImVec2(tx, c.y + Dp(2.0f)), p.ink3, Tr("색"));
        Text(cdl, Font::Regular, size::Small, ImVec2(tx + Dp(34.0f), c.y + Dp(1.0f)), p.ink2, buf);
        std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", cur.direction.x, cur.direction.y, cur.direction.z);
        Text(cdl, Font::Regular, size::Caption, ImVec2(tx, c.y + Dp(22.0f)), p.ink3, Tr("방향"));
        Text(cdl, Font::Regular, size::Small, ImVec2(tx + Dp(34.0f), c.y + Dp(21.0f)), p.ink2, buf);
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - bw, c.y + r - bw * 0.5f));
        if (IconButton("##lightkey", icon::Plus, Tr("현재 조명을 키로 등록"), false, btn))
            StudioInsertKeys({RowOf(RowKind::Light)}, d.Frame());
        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(w, r * 2.0f + Dp(4.0f)));
        ImGui::Dummy(ImVec2(w, Dp(2.0f)));
    }

    // --- self-shadow track
    separator(8.0f);
    {
        Switch("##shadowtrack", Tr("셀프 섀도 트랙"), &d.useShadowTrack);
        Tooltip(d.camera.shadow.empty() ? Tr("섀도 키가 없으면 렌더 설정을 따라요.") : Tr("끄면 렌더 설정을 따라요."));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        if (d.camera.shadow.empty()) {
            std::snprintf(buf, sizeof(buf), "%s", Tr("키 없음 · 렌더 설정"));
        } else {
            const ShadowKf s = SampleShadow(d.camera.shadow, (float)(d.time * kMmdFps));
            if (s.mode == 0) std::snprintf(buf, sizeof(buf), "%s", Tr("그림자 끔"));
            else std::snprintf(buf, sizeof(buf), Tr("모드 %d · 거리 %.0f"), (int)s.mode, ShadowUiFromVmd(s.distance));
        }
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + (bw - Dp(16.0f)) * 0.5f),
             d.useShadowTrack && !d.camera.shadow.empty() ? p.ink2 : p.ink3, buf);
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - bw, c.y));
        if (IconButton("##shadowkey", icon::Plus, Tr("셀프 섀도 키 등록"), false, btn))
            StudioInsertKeys({RowOf(RowKind::Shadow)}, d.Frame());
        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(w, bw));
    }
    separator(10.0f);
    ImGui::Dummy(ImVec2(w, Dp(2.0f)));
}

bool App::DrawStudioCameraKeyFields(float w, RowKind kind, int frame, bool live) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    bool changed = false, ended = false;
    // label + full-width widget row
    const auto row = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(64.0f));
        PushFont(Font::Regular, size::Small);
        // camera keys have four rows above their curves: slimmer fields keep the curve editor closer to view
        if (kind == RowKind::Camera)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, Dp(4.0f)));
        changed |= widget();
        if (kind == RowKind::Camera) ImGui::PopStyleVar();
        PopFont();
        if (ImGui::IsItemActivated()) {
            StudioBeginKeyEdit(kind);
            studioKeyLive_ = live;
        }
        ended |= ImGui::IsItemDeactivated();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    };

    if (kind == RowKind::Camera) {
        CameraKf* k = FindKey(d.camera.camera, frame);
        // live: the fields of the playhead. They always show the camera in effect (a key or the interpolated / free
        // view); editing one keys it here when auto-key is on (or the key already exists), like Adobe / Blender.
        CameraKf cur;
        if (live) cur = StudioViewedCamera(frame);
        else if (k) cur = *k;
        else { StudioEndKeyEdit(); return false; }
        const bool editable = !live || k || d.autoKey;
        ImGui::BeginDisabled(!editable);
        float target[3] = {cur.target.x, cur.target.y, cur.target.z};
        float rot[3] = {DirectX::XMConvertToDegrees(cur.rotation.x), DirectX::XMConvertToDegrees(cur.rotation.y),
                        DirectX::XMConvertToDegrees(cur.rotation.z)};
        float dist = cur.distance;
        int fov = (int)cur.fovDeg;
        row(Tr("중심"), [&] { return ImGui::DragFloat3("##camtarget", target, 0.05f, 0.0f, 0.0f, "%.2f"); });
        row(Tr("회전"), [&] { return ImGui::DragFloat3("##camrot", rot, 0.25f, 0.0f, 0.0f, "%.1f°"); });
        row(Tr("거리"), [&] { return ImGui::DragFloat("##camdist", &dist, 0.1f, -5000.0f, 5000.0f, "%.2f"); });
        row(Tr("시야각"), [&] { return ImGui::DragInt("##camfov", &fov, 0.25f, 1, 125, "%d°"); });
        bool persp = cur.perspective;
        {
            const bool before = persp;
            Switch("##campersp", Tr("원근"), &persp, Tr("끄면 정사영 (MMD 퍼스 OFF)"));
            if (persp != before) {
                StudioBeginKeyEdit(kind);
                changed = ended = true;
            }
        }
        ImGui::EndDisabled();
        if (changed && editable) {
            cur.target = {target[0], target[1], target[2]};
            cur.rotation = {DirectX::XMConvertToRadians(rot[0]), DirectX::XMConvertToRadians(rot[1]), DirectX::XMConvertToRadians(rot[2])};
            cur.distance = dist;
            cur.fovDeg = (uint32_t)std::clamp(fov, 1, 125);
            cur.perspective = persp;
            if (k) {
                // keep the key's interpolation curves: only the values change
                k->target = cur.target;
                k->rotation = cur.rotation;
                k->distance = cur.distance;
                k->fovDeg = cur.fovDeg;
                k->perspective = cur.perspective;
            } else {
                cur.frame = frame;
                UpsertKey(d.camera.camera, cur);  // auto-key: the edit creates the key under the playhead
            }
            if (live) d.useMotionCamera = true;  // show the effect of the edit (a free view becomes the keyed camera)
            ++d.cameraVersion;
            d.rowsKey = ~0ull;
            studioKeyChanged_ = true;
        }
        if (ended) StudioEndKeyEdit();
        return false;  // the interpolation curves follow
    }

    if (kind == RowKind::Light) {
        LightKf* k = FindKey(d.camera.light, frame);
        if (!k) { StudioEndKeyEdit(); return true; }
        // MMD shows the colour as 0..255 and stores value / 256
        int rgb[3] = {(int)std::lround(k->color.x * 256.0f), (int)std::lround(k->color.y * 256.0f), (int)std::lround(k->color.z * 256.0f)};
        float dir[3] = {k->direction.x, k->direction.y, k->direction.z};
        row(Tr("색"), [&] { return ImGui::DragInt3("##lightrgb", rgb, 0.5f, 0, 255); });
        row(Tr("방향"), [&] { return ImGui::DragFloat3("##lightdir", dir, 0.01f, -1.0f, 1.0f, "%.2f"); });
        {
            // colour swatch + picker
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
            float col[3] = {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f};
            if (ImGui::ColorEdit3("##lightpick", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                for (int i = 0; i < 3; ++i) rgb[i] = (int)std::lround(col[i] * 255.0f);
                changed = true;
            }
            if (ImGui::IsItemActivated()) StudioBeginKeyEdit(kind);
            ended |= ImGui::IsItemDeactivated();
            ImGui::SameLine(0, Dp(10.0f));
            if (ui::Button("##lightdefault", Tr("MMD 기본값"), nullptr, ButtonKind::Ghost, ImVec2(0.0f, 26.0f))) {
                StudioBeginKeyEdit(kind);
                rgb[0] = rgb[1] = rgb[2] = 154;
                dir[0] = -0.5f; dir[1] = -1.0f; dir[2] = 0.5f;
                changed = ended = true;
            }
            ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        }
        if (changed) {
            k->color = {std::clamp(rgb[0], 0, 255) / 256.0f, std::clamp(rgb[1], 0, 255) / 256.0f, std::clamp(rgb[2], 0, 255) / 256.0f};
            k->direction = {dir[0], dir[1], dir[2]};
            studioKeyChanged_ = true;
        }
        if (ended) StudioEndKeyEdit();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Caption, c, p.ink3, Tr("조명 키는 선형으로 보간돼요."));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
        return true;
    }

    // self-shadow
    ShadowKf* k = FindKey(d.camera.shadow, frame);
    if (!k) { StudioEndKeyEdit(); return true; }
    {
        const char* modes[] = {Tr("끔"), Tr("모드 1"), Tr("모드 2")};
        int mode = std::clamp((int)k->mode, 0, 2);
        if (Segmented("##shadowmode", modes, 3, &mode, w / Dpi(), 32.0f)) {
            StudioBeginKeyEdit(kind);
            k->mode = (uint8_t)mode;
            studioKeyChanged_ = true;
            StudioEndKeyEdit();
        }
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    }
    float ui = ShadowUiFromVmd(k->distance);
    row(Tr("거리"), [&] { return ImGui::DragFloat("##shadowdist", &ui, 5.0f, 0.0f, 9999.0f, "%.0f"); });
    if (changed) {
        k->distance = ShadowVmdFromUi(std::clamp(std::round(ui), 0.0f, 9999.0f));
        studioKeyChanged_ = true;
    }
    if (ended) StudioEndKeyEdit();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    Text(cdl, Font::Regular, size::Caption, c, p.ink3, Tr("섀도 키는 보간하지 않아요 (다음 키까지 유지)."));
    ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    return true;
}

// ---------------------------------------------------------------------------
// Viewport: camera path
// ---------------------------------------------------------------------------

void App::StudioUpdateCameraPath() {
    StudioDoc& d = *studio_;
    if (studioCamPathVersion_ == d.cameraEvalVersion && !studioCamPath_.empty()) return;
    studioCamPath_.clear();
    studioCamKeys_.clear();
    studioCamPathVersion_ = d.cameraEvalVersion;  // the evaluator the samples come from
    if (!d.cameraEval || d.camera.camera.empty()) return;
    const int first = d.camera.camera.front().frame, last = d.camera.camera.back().frame;
    const int stride = std::max(1, (last - first) / 20000 + 1);  // bounded cost on very long cameras
    studioCamPathStride_ = stride;
    const auto sample = [&](float f) {
        const CameraPose pose = d.cameraEval->Evaluate(f);
        CameraPathPoint pt;
        CameraMotion::ToView(pose, &pt.view, &pt.eye);
        pt.target = pose.target;
        pt.fovY = DirectX::XMConvertToRadians(pose.fovDeg);
        return pt;
    };
    for (int f = first; f <= last; f += stride) studioCamPath_.push_back(sample((float)f));
    for (const CameraKf& k : d.camera.camera) studioCamKeys_.push_back(sample((float)k.frame).eye);
}


// ---------------------------------------------------------------------------
// Possession (C4D style) and camera handles
// ---------------------------------------------------------------------------

App::FreeCamera App::StudioViewedFree(int frame) const {
    return FreeFromKey(StudioViewedCamera(frame));
}

App::FreeCamera App::FreeFromKey(const CameraKf& k) {
    FreeCamera c;
    c.target = k.target;
    c.yaw = k.rotation.y;
    c.pitch = std::clamp(-k.rotation.x, -1.45f, 1.45f);
    c.distance = std::clamp(std::fabs(k.distance), 0.1f, 600.0f);
    c.fovDeg = (float)k.fovDeg;
    return c;
}

void App::StudioPossess(bool on) {
    StudioDoc& d = *studio_;
    if (on == d.possessCamera) return;
    if (studioKeyEdit_ && studioKeyLive_) StudioEndKeyEdit();
    d.possessCamera = on;
    if (on) {
        d.selectedModel = -1;  // the camera row is the possessed object
        if (d.cameraEval) d.useMotionCamera = true;  // look through the camera that is being edited
        studioCamHandle_ = 0;
    }
}

void App::StudioWriteCamera(const FreeCamera& cam, const CameraKf* base, bool takeView) {
    StudioDoc& d = *studio_;
    const int frame = d.Frame();
    CameraKf* key = FindKey(d.camera.camera, frame);
    if (!key && !d.autoKey) {
        toast_ = {Tr("이 프레임에 카메라 키가 없어요"), Tr("자동 키를 켜거나 키를 먼저 등록하세요."), {}, false, timeSeconds_ + 3.0};
        return;
    }
    if (!studioKeyEdit_) {
        StudioBeginKeyEdit(RowKind::Camera);
        studioKeyLive_ = true;
    }
    CameraKf k = key ? *key : base ? *base : StudioViewedCamera(frame);
    k.target = cam.target;
    k.rotation.x = -cam.pitch;  // rotation = (-pitch, yaw, roll): the roll of the key stays
    k.rotation.y = cam.yaw;
    k.distance = -cam.distance;
    k.fovDeg = (uint32_t)std::clamp((int)std::lround(cam.fovDeg), 1, 125);
    if (key) {
        key->target = k.target;
        key->rotation = k.rotation;
        key->distance = k.distance;
        key->fovDeg = k.fovDeg;  // the interpolation curves stay
    } else {
        k.frame = frame;
        UpsertKey(d.camera.camera, k);
    }
    if (takeView) d.useMotionCamera = true;  // possession looks through the edited camera; handles keep the free view
    ++d.cameraVersion;
    d.rowsKey = ~0ull;
    studioKeyChanged_ = true;
    studioNavWrote_ = true;
}

void App::StudioNavEditTick() {
    if (!studioKeyEdit_ || !studioKeyLive_ || !studio_->possessCamera) {
        studioNavWrote_ = false;
        studioNavIdle_ = 0;
        return;
    }
    if (studioNavWrote_) {
        studioNavIdle_ = 0;
    } else if (++studioNavIdle_ >= 18 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
               !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        StudioEndKeyEdit();  // one undo step per gesture (a burst of drags, fly keys or wheel notches)
        studioNavIdle_ = 0;
    }
    studioNavWrote_ = false;
}


void App::StudioRenderRect(float x0, float y0, float x1, float y1, float out[4]) const {
    const StudioDoc& d = *studio_;
    out[0] = x0; out[1] = y0; out[2] = x1 - x0; out[3] = y1 - y0;
    if (d.viewLayout != 0 || !d.frameMask || !d.useMotionCamera || !d.cameraEval) return;
    const float area[4] = {x0, y0, x1 - x0, y1 - y0};
    FitInArea(area, 16.0f, 9.0f, out);
    for (int i = 0; i < 4; ++i) out[i] = std::floor(out[i]);  // whole pixels: the 3D image and the mask agree
}

void App::DrawStudioFrameMask(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    float r[4];
    StudioRenderRect(x0, y0, x1, y1, r);
    const bool locked = d.viewLayout == 0 && d.frameMask && d.useMotionCamera && d.cameraEval;
    if (!locked) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bar = IM_COL32(14, 17, 22, 255);
    const float fx0 = r[0], fy0 = r[1], fx1 = r[0] + r[2], fy1 = r[1] + r[3];
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, fy0), bar);   // above
    dl->AddRectFilled(ImVec2(x0, fy1), ImVec2(x1, y1), bar);   // below
    dl->AddRectFilled(ImVec2(x0, fy0), ImVec2(fx0, fy1), bar); // left
    dl->AddRectFilled(ImVec2(fx1, fy0), ImVec2(x1, fy1), bar); // right
    dl->AddRect(ImVec2(fx0, fy0), ImVec2(fx1, fy1), IM_COL32(255, 255, 255, 90), 0.0f, 0, 1.0f);
    const ImU32 guide = IM_COL32(255, 255, 255, 110);
    if (d.showThirds) {
        for (int i = 1; i <= 2; ++i) {
            const float gx = fx0 + (fx1 - fx0) * i / 3.0f, gy = fy0 + (fy1 - fy0) * i / 3.0f;
            dl->AddLine(ImVec2(gx, fy0), ImVec2(gx, fy1), guide, 1.0f);
            dl->AddLine(ImVec2(fx0, gy), ImVec2(fx1, gy), guide, 1.0f);
        }
    }
    if (d.showSafeFrames) {
        // action safe 93 %, title safe 90 % (centred rectangles of the frame)
        for (const float s : {0.93f, 0.90f}) {
            const float mx = (fx1 - fx0) * (1.0f - s) * 0.5f, my = (fy1 - fy0) * (1.0f - s) * 0.5f;
            dl->AddRect(ImVec2(fx0 + mx, fy0 + my), ImVec2(fx1 - mx, fy1 - my), guide, 0.0f, 0, 1.0f);
        }
    }
    Text(dl, Font::Regular, size::Caption, ImVec2(fx0 + Dp(8.0f), fy1 - Dp(20.0f)), IM_COL32(255, 255, 255, 150), "16:9");
}

void App::DrawStudioCameraPath(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    if (!d.showCameraPath || (d.useMotionCamera && d.cameraEval) || !d.cameraEval) return;
    StudioUpdateCameraPath();
    if (studioCamPath_.empty()) return;
    const float frame = (float)(d.time * kMmdFps);
    // Only the stretch around the current frame (or the frame range) is drawn: a whole dance camera keyed on every
    // frame would cover the view.
    int lo, hi;
    StudioCameraPathWindow(lo, hi);
    const std::vector<CameraKf>& keys = d.camera.camera;
    const int first = keys.front().frame, stride = studioCamPathStride_;
    const int p0 = std::clamp((lo - first) / stride, 0, (int)studioCamPath_.size() - 1);
    const int p1 = std::clamp((hi - first) / stride + 1, 0, (int)studioCamPath_.size() - 1);
    const auto byFrame = [](const CameraKf& k, int f) { return k.frame < f; };
    const int k0 = (int)(std::lower_bound(keys.begin(), keys.end(), lo, byFrame) - keys.begin());
    const int k1 = (int)(std::lower_bound(keys.begin(), keys.end(), hi + 1, byFrame) - keys.begin());
    std::set<int> selected;
    if (d.selectedModel < 0 && d.selectedLightUid == 0) {
        const uint64_t row = RowOf(RowKind::Camera);
        for (int i = k0; i < k1; ++i)
            if (d.selection.count({row, keys[(size_t)i].frame})) selected.insert(i - k0);
    }
    const CameraPose pose = d.cameraEval->Evaluate(frame);
    CameraPathPoint cur;
    CameraMotion::ToView(pose, &cur.view, &cur.eye);
    cur.target = pose.target;
    cur.fovY = DirectX::XMConvertToRadians(pose.fovDeg);
    CameraPathStyle style;
    style.lineWidth = Dp(2.0f);
    style.keyRadius = Dp(4.0f);
    style.currentRadius = Dp(5.5f);
    // The frustum is as deep as a fraction of the eye -> target distance: it scales with the scene, so the pyramid
    // shows the real lens (a fixed screen-size icon could not). The render aspect is 16:9 (every video size).
    const float toTarget = std::sqrt((cur.eye.x - cur.target.x) * (cur.eye.x - cur.target.x) +
                                     (cur.eye.y - cur.target.y) * (cur.eye.y - cur.target.y) +
                                     (cur.eye.z - cur.target.z) * (cur.eye.z - cur.target.z));
    // MMD cameras often sit almost on their target (distance ~ 0), so the depth also follows how far the viewer is.
    const float toViewer = std::sqrt((cur.eye.x - studioVp_.eye.x) * (cur.eye.x - studioVp_.eye.x) +
                                     (cur.eye.y - studioVp_.eye.y) * (cur.eye.y - studioVp_.eye.y) +
                                     (cur.eye.z - studioVp_.eye.z) * (cur.eye.z - studioVp_.eye.z));
    const float frustum = std::clamp(std::max(toTarget * 0.45f, toViewer * 0.14f), 2.0f, std::max(2.0f, toViewer * 0.5f));
    // Camera cuts are jumps far longer than the neighbouring per-frame moves (MMD cuts are keys one frame apart, but
    // cameras keyed on every frame look the same): 8x the median step of the drawn stretch, at least 3 units.
    std::vector<float> steps;
    steps.reserve((size_t)(p1 - p0));
    for (int i = p0; i < p1; ++i) {
        const DirectX::XMFLOAT3 &a = studioCamPath_[(size_t)i].eye, &b = studioCamPath_[(size_t)i + 1].eye;
        steps.push_back(std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)));
    }
    float cut = 3.0f;
    if (!steps.empty()) {
        std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
        cut = std::max(cut, steps[steps.size() / 2] * 8.0f);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    DrawCameraPath(dl, studioVp_, studioCamPath_.data() + p0, p1 - p0 + 1, studioCamKeys_.data() + k0, k1 - k0, selected, &cur, kRenderAspect, frustum, cut, style);
    dl->PopClipRect();
}

void App::StudioCameraPathWindow(int& lo, int& hi) const {
    const StudioDoc& d = *studio_;
    if (d.HasRange()) {
        lo = d.view.rangeStart;
        hi = d.view.rangeEnd;
    } else {
        lo = d.Frame() - 90;  // 3 s either side
        hi = d.Frame() + 90;
    }
}

bool App::StudioPickCameraKey(ImVec2 mouse) {
    StudioDoc& d = *studio_;
    if (d.selectedModel >= 0 || d.selectedLightUid != 0 || !d.showCameraPath || (d.useMotionCamera && d.cameraEval) || !d.cameraEval) return false;
    StudioUpdateCameraPath();
    int lo, hi;
    StudioCameraPathWindow(lo, hi);
    const auto byFrame = [](const CameraKf& k, int f) { return k.frame < f; };
    const auto& keys = d.camera.camera;
    const int k0 = (int)(std::lower_bound(keys.begin(), keys.end(), lo, byFrame) - keys.begin());
    const int k1 = (int)(std::lower_bound(keys.begin(), keys.end(), hi + 1, byFrame) - keys.begin());
    if (k1 <= k0 || k1 > (int)studioCamKeys_.size()) return false;
    int i = PickCameraKey(studioVp_, studioCamKeys_.data() + k0, k1 - k0, mouse, ui::Dp(7.0f));
    if (i < 0) return false;
    i += k0;
    const int frame = d.camera.camera[(size_t)i].frame;
    const uint64_t row = RowOf(RowKind::Camera);
    if (!ImGui::GetIO().KeyCtrl) { d.selection.clear(); d.selectedRows.clear(); }
    d.selection.insert({row, frame});
    d.rowsKey = ~0ull;
    StudioSetPlaying(false);
    StudioSeek(frame / (double)kMmdFps);
    return true;
}

} // namespace mmdx
