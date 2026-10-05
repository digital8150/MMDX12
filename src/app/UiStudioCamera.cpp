// Studio camera / light / self-shadow: the inspector panel of the camera target (view mode, key from view, light and
// shadow track switches, editable key values), the camera path overlay in the viewport, and how the light and shadow
// tracks drive the renderer. Independent of any model being loaded.
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
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

using namespace studio;

namespace {
constexpr float kOrthoFovDeg = 3.0f;  // "perspective off" is drawn as a long lens from far away (see StudioCamera)

uint64_t RowOf(RowKind k) { return MakeRowId(k, 0, 0); }

// MMD's self-shadow distance (VMD value) -> the cascade range in MMD units. MMD's default 8875 maps to the renderer's
// default range (160); a lower UI value covers more of the scene, as in MMD.
float ShadowRangeFromVmd(float vmd) {
    const float ui = std::clamp(ShadowUiFromVmd(vmd), 0.0f, 9999.0f);
    return std::clamp((10000.0f - ui) * (160.0f / 1125.0f), 20.0f, 2000.0f);
}

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

void App::StudioApplyLightTracks(FrameView& view) const {
    const StudioDoc& d = *studio_;
    const float frame = (float)(d.time * kMmdFps);
    if (d.useLightTrack && !d.camera.light.empty()) {
        const LightKf k = SampleLight(d.camera.light, frame);
        // a zero direction (broken file) keeps the preset's
        if (std::fabs(k.direction.x) + std::fabs(k.direction.y) + std::fabs(k.direction.z) > 1e-4f) view.light.direction = k.direction;
        view.light.color = k.color;
    }
    if (d.useShadowTrack && !d.camera.shadow.empty()) {
        const ShadowKf s = SampleShadow(d.camera.shadow, frame);
        view.shadowsOff = s.mode == 0;
        view.shadowDistance = ShadowRangeFromVmd(s.distance);
    }
}

LightKf App::StudioCurrentLight() const {
    const StudioDoc& d = *studio_;
    const float frame = (float)(d.time * kMmdFps);
    if (d.useLightTrack && !d.camera.light.empty()) return SampleLight(d.camera.light, frame);
    return StudioPresetLightKey(d.Frame());
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

    // --- view: motion / free camera, key from the view, path overlay
    separator(2.0f);
    {
        const char* modes[] = {Tr("카메라 모션"), Tr("자유 카메라")};
        const char* modeIcons[] = {icon::VideoCamera, icon::Crosshair};
        int mode = d.useMotionCamera && d.cameraEval ? 0 : 1;
        ImGui::BeginDisabled(!d.cameraEval);
        if (Segmented("##viewmode", modes, 2, &mode, w / Dpi(), 32.0f, modeIcons)) d.useMotionCamera = mode == 0;
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        if (Button("##camkey", Tr("현재 시점을 키로 등록"), icon::Plus, ButtonKind::Secondary, ImVec2((w - bw) / Dpi() - 6.0f, btn)))
            StudioKeyCameraFromView();
        Tooltip(d.useMotionCamera && d.cameraEval ? Tr("카메라 모션의 현재 값으로 키를 만들어요.")
                                                  : Tr("자유 카메라로 보고 있는 시점으로 키를 만들어요."));
        ImGui::SameLine(0, Dp(6.0f));
        if (IconButton("##campath", d.showCameraPath ? icon::Eye : icon::EyeSlash, d.showCameraPath ? Tr("카메라 경로 숨기기 (자유 카메라로 볼 때 표시)")
                                                                 : Tr("카메라 경로 표시 (자유 카메라로 볼 때)"),
                       d.showCameraPath, btn))
            d.showCameraPath = !d.showCameraPath;
    }

    // --- light track: switch, preview ball + values, key button
    separator(8.0f);
    {
        Switch("##lighttrack", Tr("조명 트랙"), &d.useLightTrack);
        Tooltip(d.camera.light.empty() ? Tr("조명 키가 없으면 조명 프리셋을 써요.") : Tr("끄면 조명 프리셋을 써요."));
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

bool App::DrawStudioCameraKeyFields(float w, RowKind kind, int frame) {
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
        changed |= widget();
        PopFont();
        if (ImGui::IsItemActivated()) StudioBeginKeyEdit(kind);
        ended |= ImGui::IsItemDeactivated();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    };

    if (kind == RowKind::Camera) {
        CameraKf* k = FindKey(d.camera.camera, frame);
        if (!k) { StudioEndKeyEdit(); return false; }
        float target[3] = {k->target.x, k->target.y, k->target.z};
        float rot[3] = {DirectX::XMConvertToDegrees(k->rotation.x), DirectX::XMConvertToDegrees(k->rotation.y),
                        DirectX::XMConvertToDegrees(k->rotation.z)};
        float dist = k->distance;
        int fov = (int)k->fovDeg;
        row(Tr("중심"), [&] { return ImGui::DragFloat3("##camtarget", target, 0.05f, 0.0f, 0.0f, "%.2f"); });
        row(Tr("회전"), [&] { return ImGui::DragFloat3("##camrot", rot, 0.25f, 0.0f, 0.0f, "%.1f°"); });
        row(Tr("거리"), [&] { return ImGui::DragFloat("##camdist", &dist, 0.1f, -5000.0f, 5000.0f, "%.2f"); });
        row(Tr("시야각"), [&] { return ImGui::DragInt("##camfov", &fov, 0.25f, 1, 125, "%d°"); });
        bool persp = k->perspective;
        {
            const bool before = persp;
            Switch("##campersp", Tr("원근"), &persp, Tr("끄면 정사영 (MMD 퍼스 OFF)"));
            if (persp != before) {
                StudioBeginKeyEdit(kind);
                changed = ended = true;
            }
        }
        if (changed) {
            k->target = {target[0], target[1], target[2]};
            k->rotation = {DirectX::XMConvertToRadians(rot[0]), DirectX::XMConvertToRadians(rot[1]), DirectX::XMConvertToRadians(rot[2])};
            k->distance = dist;
            k->fovDeg = (uint32_t)std::clamp(fov, 1, 125);
            k->perspective = persp;
            ++d.cameraVersion;
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
    if (d.selectedModel < 0) {
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
    const float frustum = studioVp_.PixelWorldSize(cur.eye) * Dp(56.0f);
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
    DrawCameraPath(dl, studioVp_, studioCamPath_.data() + p0, p1 - p0 + 1, studioCamKeys_.data() + k0, k1 - k0, selected, &cur, (x1 - x0) / std::max(1.0f, y1 - y0), frustum, cut, style);
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
    if (d.selectedModel >= 0 || !d.showCameraPath || (d.useMotionCamera && d.cameraEval) || !d.cameraEval) return false;
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
