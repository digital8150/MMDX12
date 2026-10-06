// Studio: rendering the project (video through the render dialog, GI still of the current frame) and the
// shortcut help overlay.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

using namespace studio;

namespace {
const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター

std::string StillName() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "MMDX12_studio_%04d%02d%02d_%02d%02d%02d.png", lt.tm_year + 1900, lt.tm_mon + 1,
                  lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}
} // namespace

// ---------------------------------------------------------------------------
// Render jobs
// ---------------------------------------------------------------------------

void App::StudioRenderRange(double& a, double& b) const {
    const StudioDoc& d = *studio_;
    const double end = d.EndFrame() / (double)kMmdFps;
    if (options_.offlineRange[0] >= 0 || options_.offlineRange[1] >= 0) {
        a = options_.offlineRange[0] >= 0 ? options_.offlineRange[0] : 0.0;
        b = options_.offlineRange[1] >= 0 ? options_.offlineRange[1] : end;
    } else if (d.HasRange() && !studioRenderWhole_) {
        // the range plays through its last frame (like loop playback)
        a = d.view.rangeStart / (double)kMmdFps;
        b = (d.view.rangeEnd + 1) / (double)kMmdFps;
    } else {
        a = 0.0;
        b = end;
    }
    b = std::max(a, b);
}

void App::StartStudioRender(bool video) {
    if (!studio_ || offline_.mode != OfflineMode::None || !studioJobs_.empty()) return;
    StudioDoc& d = *studio_;
    if (video) {
        double a = 0, b = 0;
        StudioRenderRange(a, b);
        LOG_INFO("studio render: video %.3f..%.3f s", a, b);
        StartOfflineVideo(a, b);
        return;
    }
    if (!renderer_.OfflineSupported()) {
        toast_ = {Tr("고품질 렌더를 시작할 수 없습니다"), Tr("이 그래픽 카드에서는 오프라인 GI 렌더를 사용할 수 없습니다"), {}, true,
                  timeSeconds_ + 8.0};
        return;
    }
    // the still shows the current frame through the view the viewport shows (motion or free camera)
    StudioSetPlaying(false);
    offline_ = OfflineJob{};
    offline_.mode = OfflineMode::Still;
    offline_.studio = true;
    offline_.studioTime = d.time;
    offline_.studioMotionCamera = d.useMotionCamera;
    offline_.fromCli = !options_.offlineStill.empty();
    offline_.output = offline_.fromCli ? options_.offlineStill : OfflineOutputDir(false) / Utf8ToPath(StillName());
    offline_.startSeconds = d.time;
    offline_.frameCount = 1;
    offline_.startWall = timeSeconds_;
    offline_.beginPending = true;
    RenderSettings rs = renderer_.Settings();
    rs.viewportX = rs.viewportY = rs.viewportW = rs.viewportH = 0;
    rs.shading = ViewShading::Lit;  // the studio shading mode is an editing view only
    renderer_.SetSettings(rs);
    screen_ = Screen::Offline;
    LOG_INFO("studio render: still at frame %d -> %s", d.Frame(), PathToUtf8(offline_.output).c_str());
}

void App::StudioPoseForRender(float frame) {
    StudioDoc& d = *studio_;
    d.time = frame / (double)kMmdFps;
    // physics follows the video frames: one step per frame, reset at the first frame and on jumps
    float physicsDt = 0.0f;
    bool reset = d.physicsFrame < 0.0f;
    if (!reset) {
        const float df = frame - d.physicsFrame;
        reset = df < 0.0f || df > 0.25f * kMmdFps;
        if (!reset) physicsDt = df / kMmdFps;
    }
    d.physicsFrame = frame;
    if (d.cameraEvalVersion != d.cameraVersion) {
        d.cameraEval = d.camera.camera.empty() ? nullptr : CameraMotion::Create(d.camera.ToVmd());
        d.cameraEvalVersion = d.cameraVersion;
        if (!d.cameraEval) d.useMotionCamera = false;
    }
    const uint64_t slot = ctx_.FrameNumber();
    for (int pass = 0; pass < 2; ++pass)  // props last: they follow a bone of a model posed in the first pass
        for (auto& mp : d.models)
            if (mp->IsProp() == (pass == 1)) StudioUpdateModel(*mp, slot, frame, physicsDt, reset);
}

void App::StudioRestoreAfterRender() {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    d.time = offline_.studioTime;
    d.useMotionCamera = offline_.studioMotionCamera && d.cameraEval != nullptr;
    d.physicsFrame = -1.0f;   // the editor's next frame starts the simulation over
    if (d.hasAudio) StudioSeekAudio();
}

DirectX::XMFLOAT3 App::StudioPerformerCenter() const {
    if (!studio_) return {};
    for (const auto& m : studio_->models) {
        if (m->kind != ModelKind::Character || !m->visible) continue;
        const int c = m->pmx->FindBone(kCenterBone);
        return c >= 0 ? m->inst->BoneWorldPosition(c) : DirectX::XMFLOAT3{};
    }
    return {};
}

void App::DrawStudioRenderMenu() {
    using namespace ui;
    ImGui::SetNextWindowSize(ImVec2(Dp(320.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##studiorender")) {
        const StudioDoc& d = *studio_;
        bool video = false, still = false;
        double a = 0, b = 0;
        StudioRenderRange(a, b);
        const std::string range = d.HasRange() && !studioRenderWhole_
                                      ? std::string(Tr("타임라인 범위")) + " " + std::to_string(d.view.rangeStart) + "–" +
                                            std::to_string(d.view.rangeEnd)
                                      : std::string(Tr("프로젝트 전체")) + " 0–" + std::to_string(d.EndFrame());
        if (MenuItem("##rvideo", Tr("영상 렌더…"), icon::FilmStrip, range.c_str())) video = true;
        Tooltip(Tr("타임라인 범위(없으면 전체)를 카메라 모션으로, 음원과 함께 MP4로 렌더링합니다"));
        ImGui::BeginDisabled(!renderer_.OfflineSupported());
        if (MenuItem("##rstill", Tr("고품질 스틸"), icon::Image, (std::string(Tr("현재 프레임")) + " " +
                                                                      std::to_string(d.Frame())).c_str()))
            still = true;
        ImGui::EndDisabled();
        Tooltip(renderer_.OfflineSupported() ? Tr("뷰포트에 보이는 장면을 오프라인 GI로 4K 사진으로 렌더링합니다")
                                             : Tr("이 그래픽 카드에서는 오프라인 GI 렌더를 사용할 수 없습니다"));
        if (video || still) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (video) {
            videoDialogOpen_ = true;
        }
        if (still) StartStudioRender(false);
    }
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Shortcut help
// ---------------------------------------------------------------------------

namespace {
struct HelpRow {
    const char* keys[3];
    const char* desc;
};
struct HelpGroup {
    const char* title;
    const HelpRow* rows;
    int count;
};

const HelpRow kPlayback[] = {
    {{"Space"}, "재생 / 일시정지"},
    {{"←", "→"}, "이전 / 다음 프레임"},
    {{"Ctrl+←", "Ctrl+→"}, "이전 / 다음 키"},
    {{"Home", "End"}, "처음 / 끝으로"},
};
const HelpRow kEdit[] = {
    {{"I"}, "포즈 등록 (선택한 행에 키)"},
    {{"Ctrl+I"}, "모든 본 등록"},
    {{"Delete"}, "선택한 키 삭제"},
    {{"Ctrl+C", "Ctrl+X", "Ctrl+V"}, "복사 / 잘라내기 / 붙여넣기"},
    {{"Ctrl+Shift+V"}, "보간 곡선만 붙여넣기"},
    {{"Ctrl+A"}, "모든 키 선택"},
    {{"Ctrl+Z", "Ctrl+Y"}, "실행 취소 / 다시 실행"},
};
const HelpRow kPose[] = {
    {{"E"}, "회전 도구"},
    {{"W"}, "이동 도구"},
    {{"L"}, "로컬 ↔ 글로벌 축"},
    {{"Esc"}, "본 선택 해제"},
};
const HelpRow kProject[] = {
    {{"Ctrl+S"}, "프로젝트 저장"},
    {{"Ctrl+Shift+S"}, "다른 이름으로 저장"},
    {{"Ctrl+O"}, "프로젝트 열기"},
    {{"Ctrl+N"}, "새 프로젝트"},
    {{"?", "F1"}, "단축키 도움말"},
};
const HelpRow kViewport[] = {
    {{"왼쪽 드래그"}, "카메라 회전 · 기즈모 · 본 선택"},
    {{"Ctrl+클릭"}, "본 추가 선택"},
    {{"오른쪽 드래그", "가운데 드래그"}, "카메라 이동"},
    {{"휠"}, "확대 / 축소"},
};
const HelpRow kTimeline[] = {
    {{"휠"}, "위아래 스크롤"},
    {{"Ctrl+휠"}, "확대 / 축소"},
    {{"Shift+휠", "가운데 드래그"}, "좌우 이동"},
    {{"Shift+눈금자 드래그"}, "프레임 범위 선택"},
    {{"눈금자 우클릭"}, "범위 해제"},
    {{"빈 곳 드래그"}, "키 박스 선택"},
    {{"더블클릭"}, "키 추가"},
};

float ChipsWidth(const HelpRow& r) {
    using namespace ui;
    float w = 0;
    for (const char* k : r.keys) {
        if (!k) break;
        if (w > 0) w += Dp(6.0f);
        w += TextSize(Font::Semibold, size::Caption, Tr(k)).x + Dp(14.0f);
    }
    return w;
}
} // namespace

void App::DrawStudioHelp() {
    using namespace ui;
    if (!studioHelpOpen_) return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const Palette& p = P();
    static const HelpGroup kColumns[3][2] = {
        {{"재생과 이동", kPlayback, (int)std::size(kPlayback)}, {"키 편집", kEdit, (int)std::size(kEdit)}},
        {{"포즈", kPose, (int)std::size(kPose)}, {"프로젝트", kProject, (int)std::size(kProject)}},
        {{"마우스 · 뷰포트", kViewport, (int)std::size(kViewport)}, {"마우스 · 타임라인", kTimeline, (int)std::size(kTimeline)}},
    };

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ds);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 24, 32, 120));
    ImGui::Begin("##studiohelp", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // three columns: each column is as wide as its widest key chips + description
    const float rowH = Dp(30.0f), headH = Dp(34.0f), groupGap = Dp(14.0f), colGap = Dp(28.0f), pad = Dp(28.0f);
    float keyW[3] = {}, descW[3] = {}, colH[3] = {};
    for (int c = 0; c < 3; ++c) {
        for (const HelpGroup& g : kColumns[c]) {
            for (int i = 0; i < g.count; ++i) {
                keyW[c] = std::max(keyW[c], ChipsWidth(g.rows[i]));
                descW[c] = std::max(descW[c], TextSize(Font::Regular, size::Small, Tr(g.rows[i].desc)).x);
            }
            colH[c] += headH + rowH * g.count;
        }
        colH[c] += groupGap;
    }
    float contentW = colGap * 2.0f, contentH = 0;
    for (int c = 0; c < 3; ++c) {
        contentW += keyW[c] + Dp(12.0f) + descW[c];
        contentH = std::max(contentH, colH[c]);
    }
    const float titleH = Dp(64.0f);
    const float w = std::min(ds.x - Dp(32.0f), contentW + pad * 2.0f);
    const float h = std::min(ds.y - Dp(32.0f), titleH + contentH + pad);
    const ImVec2 a((ds.x - w) * 0.5f, (ds.y - h) * 0.5f), b(a.x + w, a.y + h);
    Panel(dl, a, b, Dp(20.0f));
    Icon(dl, icon::Keyboard, 20.0f, ImVec2(a.x + pad + Dp(10.0f), a.y + Dp(34.0f)), p.accent);
    Text(dl, Font::Bold, size::Heading, ImVec2(a.x + pad + Dp(30.0f), a.y + Dp(20.0f)), p.ink, Tr("스튜디오 단축키"));
    ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(16.0f + 36.0f), a.y + Dp(16.0f)));
    const bool closeBtn = IconButton("##helpclose", icon::X, Tr("닫기  (Esc)"));

    // the columns scroll when the window is too small for them
    const ImVec2 c0(a.x + pad, a.y + titleH);
    ImGui::SetCursorScreenPos(c0);
    ImGui::BeginChild("##helpcols", ImVec2(b.x - pad - c0.x, b.y - Dp(12.0f) - c0.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float x = o.x;
    for (int c = 0; c < 3; ++c) {
        float y = o.y;
        for (const HelpGroup& g : kColumns[c]) {
            Text(cdl, Font::Semibold, size::Caption, ImVec2(x, y + Dp(8.0f)), p.ink3, Tr(g.title));
            y += headH;
            for (int i = 0; i < g.count; ++i) {
                const HelpRow& r = g.rows[i];
                float kx = x;
                for (const char* k : r.keys) {
                    if (!k) break;
                    const char* kt = Tr(k);
                    const ImVec2 ts = TextSize(Font::Semibold, size::Caption, kt);
                    const ImVec2 ka(kx, y + (rowH - Dp(22.0f)) * 0.5f), kb(kx + ts.x + Dp(14.0f), ka.y + Dp(22.0f));
                    cdl->AddRectFilled(ka, kb, p.sunken, Dp(6.0f));
                    cdl->AddRect(ka, kb, p.line, Dp(6.0f));
                    Text(cdl, Font::Semibold, size::Caption, ImVec2(ka.x + Dp(7.0f), ka.y + (Dp(22.0f) - ts.y) * 0.5f), p.ink, kt);
                    kx = kb.x + Dp(6.0f);
                }
                const char* desc = Tr(r.desc);
                const ImVec2 ds2 = TextSize(Font::Regular, size::Small, desc);
                Text(cdl, Font::Regular, size::Small, ImVec2(x + keyW[c] + Dp(12.0f), y + (rowH - ds2.y) * 0.5f), p.ink2, desc);
                y += rowH;
            }
            y += groupGap;
        }
        x += keyW[c] + Dp(12.0f) + descW[c] + colGap;
    }
    ImGui::Dummy(ImVec2(contentW, contentH));
    ImGui::EndChild();
    // a click outside the panel closes it like Esc
    const bool outside = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseHoveringRect(a, b);
    ImGui::End();
    if (closeBtn || outside || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) studioHelpOpen_ = false;
}

} // namespace mmdx
