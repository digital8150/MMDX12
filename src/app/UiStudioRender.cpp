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
#include "studio/FileDialog.h"

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

void Para(const std::string& text, ImU32 col, float sizePx = ui::size::Small, ui::Font font = ui::Font::Regular) {
    using namespace ui;
    if (text.empty()) return;
    PushFont(font, sizePx);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    PopFont();
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
    ResetStudioGi();   // a real studio render takes over the offline renderer
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
    offline_.alpha = options_.offlineAlpha == 1;   // transparent PNG still (MakeJobDesc sets the job's exportAlpha)
    offline_.output = offline_.fromCli              ? options_.offlineStill
                      : !mcpOfflineOutput_.empty() ? mcpOfflineOutput_  // MCP render_still
                                                   : OfflineOutputDir(false) / Utf8ToPath(StillName());
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
    // S3: the stage targets at the rendered frame (materials + the stage rig lights), before the models' UpdateMaterials
    StudioApplyStageTargets();
    d.RefreshCameraEvals();
    if (!StudioShownCameraEval()) d.useMotionCamera = false;  // the main camera's keys (or a possessed screen camera's)
    const uint64_t slot = ctx_.FrameNumber();
    for (int pass = 0; pass < 2; ++pass)  // props last: they follow a bone of a model posed in the first pass
        for (auto& mp : d.models)
            if (mp->IsProp() == (pass == 1)) StudioUpdateModel(*mp, slot, frame, physicsDt, reset);
}

void App::StudioRestoreAfterRender() {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    d.time = offline_.studioTime;
    d.useMotionCamera = offline_.studioMotionCamera && d.HasCameraKeys();
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

void App::StudioRenderVideo() {
    videoDialogOpen_ = true;
}

void App::StudioRenderStill() {
    StartStudioRender(false);
}

// The studio's "효과" panel (a tab next to the properties): real-time effects, colour LUT and the effect stack.
// Draws in the panel's content rect (screen coordinates), scrolling inside its own child.
void App::DrawStudioEffectsPanel(float x0, float y0, float x1, float y1) {
    using namespace ui;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
    const bool visible = ImGui::BeginChild("##studioeffects", ImVec2(x1 - x0, y1 - y0), ImGuiChildFlags_AlwaysUseWindowPadding,
                                           ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    if (visible) {
        bool changed = false;

        SectionLabel(Tr("실시간 효과"));
        changed |= Switch("##bloom", Tr("블룸"), &settings_.bloom);
        DrawBloomControls(ImGui::GetContentRegionAvail().x, &changed);
        changed |= Switch("##autolum", Tr("오토루미너스 발광"), &settings_.autoLuminous,
                          Tr("AutoLuminous 규격 재질의 자체 발광"));
        changed |= Switch("##dof", Tr("피사계 심도"), &settings_.dof, Tr("초점 밖을 흐림 · 뷰포트와 모든 렌더 (초점은 카메라의 초점 트랙)"));
        if (settings_.dof) {
            Gap(4.0f);
            changed |= SliderRow("##dofap", Tr("조리개"), &settings_.dofAperture, 0.2f, 3.0f, "%.2f");
            Gap(4.0f);
        }
        changed |= Switch("##vol", Tr("볼류메트릭 라이트"), &settings_.volumetric, Tr("빛줄기와 조명 산란"));
        if (settings_.volumetric) {
            Gap(4.0f);
            changed |= SliderRow("##vold", Tr("안개 밀도"), &settings_.volumetricDensity, 0.25f, 4.0f, "%.2f");
            // the haze's light shares (same settings as the play panel): shafts read when the spots scatter strongly
            // and the sun / sky barely do
            changed |= SliderRow("##volgain", Tr("빛줄기 강도"), &settings_.volumetricLightGain, 0.0f, 8.0f, "%.2f");
            changed |= SliderRow("##volamb", Tr("환경 산란"), &settings_.volumetricAmbientGain, 0.0f, 2.0f, "%.2f");
            changed |= SliderRow("##volrange", Tr("안개 범위"), &settings_.volumetricRange, 0.0f, 1000.0f,
                                 settings_.volumetricRange < 50.0f ? Tr("자동") : "%.0f");
            Tooltip(Tr("무대 원점에서 안개가 퍼지는 반지름이에요. 자동은 스테이지 값, 없으면 빛줄기 조명이 모두 들어가게 넓혀요."));
            changed |= Switch("##volfull", Tr("정밀 안개 계산"), &settings_.volumetricFull,
                              Tr("오프라인 GI에서 안개를 경로 추적으로 계산"));
            Gap(4.0f);
        }
        changed |= Switch("##ssao", Tr("앰비언트 오클루전"), &settings_.ssao);
        changed |= Switch("##ssr", Tr("화면 공간 반사"), &settings_.ssr);

        if (!luts_.empty()) {
            Gap(10.0f);
            SectionLabel(Tr("컬러 LUT"));
            const float fullW = ImGui::GetContentRegionAvail().x / Dpi();
            int lutCount = 1 + (int)luts_.size();
            for (int i = 0; i < lutCount; ++i) {
                ImGui::PushID(i);
                const ColorLutEntry* e = i > 0 ? &luts_[(size_t)i - 1] : nullptr;
                if (Chip("##lut", e ? Tr(e->displayName.c_str()) : Tr("없음"), icon::Image,
                         settings_.colorLut == (e ? e->id : std::string()), fullW)) {
                    settings_.colorLut = e ? e->id : std::string();
                    changed = true;
                }
                ImGui::PopID();
                Gap(4.0f);
            }
            if (!settings_.colorLut.empty()) {
                Gap(4.0f);
                changed |= SliderRow("##lutint", Tr("LUT 강도"), &settings_.lutIntensity, 0.0f, 1.0f, "%.2f");
            }
        }

        Gap(12.0f);
        const Palette& p = P();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float sepY = ImGui::GetCursorScreenPos().y;
        dl->AddLine(ImVec2(ImGui::GetWindowPos().x + Dp(14.0f), sepY),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - Dp(14.0f), sepY), p.line);
        Gap(8.0f);

        // 배경 AVI (背景AVI): a background video whose frame feeds screen.bmp materials.
        {
            StudioDoc& d = *studio_;
            SectionLabel(Tr("배경 AVI"));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
            ImGui::TextUnformatted(d.bgVideoPath.empty() ? Tr("없음") : PathToUtf8(d.bgVideoPath.filename()).c_str());
            ImGui::PopStyleColor();
            Gap(4.0f);
            if (Button("##bgvideochoose", Tr("파일 선택"), icon::FolderOpen, ButtonKind::Secondary, ImVec2(-1.0f, 0))) {
                const std::filesystem::path f = OpenFileDialog(
                    hwnd_, {{L"Video", L"*.avi;*.mp4;*.mov;*.wmv;*.mkv"}, {L"AVI", L"*.avi"}});
                if (!f.empty()) StudioSetBgVideo(f);
            }
            ImGui::BeginDisabled(d.bgVideoPath.empty());
            if (Button("##bgvideoclear", Tr("지우기"), icon::Trash, ButtonKind::Secondary, ImVec2(-1.0f, 0))) {
                StudioSetBgVideo({});
            }
            ImGui::EndDisabled();
            Gap(6.0f);
            float off = (float)d.bgVideoOffset;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
            ImGui::TextUnformatted(Tr("시작 오프셋 (초)"));
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-1.0f);
            PushFont(Font::Regular, size::Small);
            if (ImGui::DragFloat("##bgvideooffset", &off, 0.01f, -600.0f, 600.0f, "%+.2f")) {
                d.bgVideoOffset = off;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) ++d.projectVersion;
            PopFont();
            Tooltip(Tr("영상이 타임라인의 이 시간에 시작해요 (양수: 늦게, 음수: 앞부분을 건너뜀)"));
            Gap(4.0f);
            if (Switch("##bgvideoasbg", Tr("배경에도 표시"), &d.bgVideoAsBackground,
                       Tr("끄면 screen.bmp 재질에만 영상이 표시돼요"))) {
                ++d.projectVersion;
            }
            Gap(2.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(Tr("텍스처가 screen.bmp인 재질에 영상이 표시됩니다"));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        Gap(12.0f);
        {
            StudioDoc& d = *studio_;
            ShaderPackRegistry& reg = ShaderPacks();
            const std::string lang = PackLanguage();

            SectionLabel(Tr("화면 효과"));
            Para(Tr("위에서 아래로 화면 효과를 차례로 적용합니다. 영상·스틸 렌더에도 같은 스택이 쓰입니다."), p.ink3, size::Caption);
            Gap(6.0f);

            bool effChanged = false;
            size_t swapWith = SIZE_MAX, removeAt = SIZE_MAX, swapA = 0;
            for (size_t i = 0; i < d.effects.size(); ++i) {
                StudioEffect& eff = d.effects[i];
                const ShaderPack* pk = reg.Find(eff.entry.pack);
                const bool usable = pk && PackUsable(pk) && pk->type == PackType::Effect;
                const std::string name = pk ? pk->name.Get(lang) : eff.entry.pack;
                const char* where = !pk ? Tr("설치되어 있지 않음")
                                    : !usable ? Tr("사용할 수 없음 · 이펙트 탭에서 확인")
                                    : pk->stage == PackEffectStage::PreBloom ? Tr("블룸 앞 (HDR)") : Tr("톤맵 뒤");
                ImGui::PushID((int)i);
                bool on = eff.entry.enabled;
                if (Switch("##on", name.c_str(), &on, where)) {
                    std::vector<StudioEffect> before = d.effects;
                    eff.entry.enabled = on;
                    d.history.Push(std::make_unique<EffectsCommand>(d, on ? Tr("효과 켜기") : Tr("효과 끄기"), before, d.effects));
                    ++d.projectVersion;
                    d.rowsKey = ~0ull;
                    effChanged = true;
                }
                if (IconButton("##up", icon::CaretUp, Tr("위로"), false, 28.0f) && i > 0) {
                    swapA = i - 1;
                    swapWith = i;
                }
                ImGui::SameLine();
                if (IconButton("##down", icon::CaretDown, Tr("아래로"), false, 28.0f) && i + 1 < d.effects.size()) {
                    swapA = i;
                    swapWith = i + 1;
                }
                ImGui::SameLine();
                if (IconButton("##remove", icon::X, Tr("스택에서 제거"), false, 28.0f)) removeAt = i;

                if (usable && eff.entry.enabled && !pk->params.empty()) {
                    effChanged |= DrawKeyableParams(*pk, eff, eff.uid, RowKind::EffectParam, "##fx");
                }
                ImGui::PopID();
                Gap(8.0f);
            }

            if (removeAt != SIZE_MAX) {
                std::vector<StudioEffect> before = d.effects;
                d.effects.erase(d.effects.begin() + (long)removeAt);
                d.history.Push(std::make_unique<EffectsCommand>(d, Tr("효과 삭제"), before, d.effects));
                ++d.projectVersion;
                d.rowsKey = ~0ull;
                effChanged = true;
            } else if (swapWith != SIZE_MAX) {
                std::vector<StudioEffect> before = d.effects;
                std::swap(d.effects[swapA], d.effects[swapWith]);
                d.history.Push(std::make_unique<EffectsCommand>(d, Tr("효과 순서 변경"), before, d.effects));
                ++d.projectVersion;
                d.rowsKey = ~0ull;
                effChanged = true;
            }

            std::vector<const ShaderPack*> addable;
            for (const ShaderPack& pk : reg.Packs()) {
                if (pk.type != PackType::Effect || !PackUsable(&pk)) continue;
                bool in = false;
                for (const StudioEffect& e : d.effects) in |= e.entry.pack == pk.id;
                if (!in) addable.push_back(&pk);
            }
            if (!addable.empty()) {
                if (Button("##studiofxadd", Tr("효과 추가"), icon::Plus, ButtonKind::Secondary)) ImGui::OpenPopup("##studiofxaddpopup");
                ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
                if (ImGui::BeginPopup("##studiofxaddpopup")) {
                    for (const ShaderPack* pk : addable) {
                        const std::string rid = "##sfxadd_" + pk->id;
                        if (MenuItem(rid.c_str(), pk->name.Get(lang).c_str())) {
                            std::vector<StudioEffect> before = d.effects;
                            StudioEffect newEff;
                            newEff.uid = d.nextEffectUid++;
                            newEff.entry.pack = pk->id;
                            newEff.entry.enabled = true;
                            d.effects.push_back(std::move(newEff));
                            d.history.Push(std::make_unique<EffectsCommand>(d, Tr("효과 추가"), before, d.effects));
                            ++d.projectVersion;
                            d.rowsKey = ~0ull;
                            effChanged = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar();
            }
            if (d.effects.empty()) {
                Para(Tr("효과가 없습니다. 화면 효과를 추가하면 위에서 아래로 차례로 적용됩니다."), p.ink3, size::Caption);
                if (addable.empty()) {
                    Gap(6.0f);
                    Para(Tr("화면 효과는 라이브러리 화면의 이펙트 탭 > 온라인에서 받을 수 있습니다."), p.ink3, size::Caption);
                }
            }
            if (effChanged) {
                ++d.projectVersion;
                changed = true;   // the stack is read by ApplyRenderSettings: push it now, not with the next bloom edit
            }
        }

        // M6b: the project's own MME post stack (replaces the play-mode stack while the studio is open).
        Gap(12.0f);
        ImDrawList* mmeDl = ImGui::GetWindowDrawList();
        const float mmeSepY = ImGui::GetCursorScreenPos().y;
        mmeDl->AddLine(ImVec2(ImGui::GetWindowPos().x + Dp(14.0f), mmeSepY),
                       ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - Dp(14.0f), mmeSepY), p.line);
        Gap(8.0f);
        ImGui::BeginDisabled(!MmeGlobalEnabled());
        changed |= DrawStudioMmeStackEditor();
        ImGui::EndDisabled();

        if (changed) {
            ApplyRenderSettings();
            settings_.Save(settingsPath_);
        }
    }
    ImGui::EndChild();
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
    {{"Space"}, TrKey("재생 / 일시정지")},
    {{"←", "→"}, TrKey("이전 / 다음 프레임")},
    {{"Ctrl+←", "Ctrl+→"}, TrKey("이전 / 다음 키")},
    {{"Home", "End"}, TrKey("처음 / 끝으로")},
};
const HelpRow kEdit[] = {
    {{"I"}, TrKey("포즈 등록 (선택한 행에 키)")},
    {{"Ctrl+I"}, TrKey("모든 본 등록")},
    {{"Delete"}, TrKey("선택한 키 삭제")},
    {{"Ctrl+C", "Ctrl+X", "Ctrl+V"}, TrKey("복사 / 잘라내기 / 붙여넣기")},
    {{"Ctrl+Shift+V"}, TrKey("보간 곡선만 붙여넣기")},
    {{"Ctrl+A"}, TrKey("모든 키 선택")},
    {{"Ctrl+Z", "Ctrl+Y"}, TrKey("실행 취소 / 다시 실행")},
};
const HelpRow kPose[] = {
    {{"E"}, TrKey("회전 도구")},
    {{"W"}, TrKey("이동 도구")},
    {{"L"}, TrKey("로컬 ↔ 글로벌 축")},
    {{"Esc"}, TrKey("본 선택 해제")},
};
const HelpRow kProject[] = {
    {{"Ctrl+S"}, TrKey("프로젝트 저장")},
    {{"Ctrl+Shift+S"}, TrKey("다른 이름으로 저장")},
    {{"Ctrl+O"}, TrKey("프로젝트 열기")},
    {{"Ctrl+N"}, TrKey("새 프로젝트")},
    {{"?", "F1"}, TrKey("단축키 도움말")},
};
const HelpRow kViewport[] = {
    {{TrKey("왼쪽 드래그")}, TrKey("카메라 회전 · 기즈모 · 본 선택")},
    {{TrKey("Ctrl+클릭")}, TrKey("본 추가 선택")},
    {{TrKey("오른쪽 드래그"), TrKey("가운데 드래그")}, TrKey("카메라 이동")},
    {{TrKey("휠")}, TrKey("화각 (줌)")},
    {{TrKey("Ctrl+휠")}, TrKey("카메라 앞뒤 이동")},
    {{TrKey("오른쪽 버튼+휠")}, TrKey("비행 속도")},
};
const HelpRow kTimeline[] = {
    {{TrKey("휠")}, TrKey("위아래 스크롤")},
    {{TrKey("Ctrl+휠")}, TrKey("확대 / 축소")},
    {{TrKey("Shift+휠"), TrKey("가운데 드래그")}, TrKey("좌우 이동")},
    {{TrKey("Shift+눈금자 드래그")}, TrKey("프레임 범위 선택")},
    {{TrKey("우클릭")}, TrKey("키 메뉴 · 구간 시작 / 끝 / 해제")},
    {{TrKey("빈 곳 드래그")}, TrKey("키 박스 선택")},
    {{TrKey("더블클릭")}, TrKey("키 추가")},
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
        {{TrKey("재생과 이동"), kPlayback, (int)std::size(kPlayback)}, {TrKey("키 편집"), kEdit, (int)std::size(kEdit)}},
        {{TrKey("포즈"), kPose, (int)std::size(kPose)}, {TrKey("프로젝트"), kProject, (int)std::size(kProject)}},
        {{TrKey("마우스 · 뷰포트"), kViewport, (int)std::size(kViewport)}, {TrKey("마우스 · 타임라인"), kTimeline, (int)std::size(kTimeline)}},
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

// ---- shared keyable param widgets for studio effects / worlds ----

// "애니메이션 끄기" confirm modal (Premiere wording): pending while studioStopwatchOff_ is set. Confirming runs the
// callback (which clears all keys of the property); Esc cancels, Enter confirms, like DrawStudioLightPresetConfirm.
void App::DrawStudioStopwatchConfirm() {
    using namespace ui;
    if (!studioStopwatchOff_) return;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(420.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##stopwatchconfirm")) ImGui::OpenPopup("##stopwatchconfirm");
    if (ImGui::BeginPopupModal("##stopwatchconfirm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(372.0f), p.ink, Tr("애니메이션 끄기"));
        char body[256];
        std::snprintf(body, sizeof(body), "%s\n%s", Tr("키프레임을 모두 지웁니다. 계속하시겠습니까?"),
                      studioStopwatchLabel_.c_str());
        Text(dl, Font::Semibold, size::Body, ImVec2(c.x, c.y + Dp(28.0f)), p.ink, body);

        ImGui::Dummy(ImVec2(0, Dp(60.0f)));
        bool close = false;
        if (Button("##stopwatchcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close = true;
        }
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##stopwatchok", Tr("키프레임 지우기"), nullptr, ButtonKind::Danger, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            studioStopwatchOff_();
            ++d.projectVersion;
            close = true;
        }
        if (close) {
            studioStopwatchOff_ = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}


// The keyable typed params of a studio effect / world (docs/effect_ecosystem.md): one plain full-width slider / switch
// / colour field per pack param, under the pack's collapsible groups (the stopwatch and the key navigation live on the
// timeline rows: DrawKeyableParams keys a param when the value is edited and auto-key is on, as it always did).
// `uid` addresses the keyed tracks (EffectParamTrackName / WorldParamTrackName for `kind`). True = a static value or a
// key changed this frame.
bool App::DrawKeyableParams(const ShaderPack& pk, studio::StudioEffect& fx, uint32_t uid, studio::RowKind kind,
                            const char* idPrefix) {
    using namespace ui;
    const Palette& p = P();
    const std::string lang = PackLanguage();
    StudioDoc& d = *studio_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;
    const auto trackName = [&](const std::string& key) {
        return kind == RowKind::WorldParam ? WorldParamTrackName(uid, key) : EffectParamTrackName(uid, key);
    };

auto drawStudioParam = [&](const ShaderPackParam& sp, size_t k) {
    ImGui::PushID((int)k);
    const float availW = ImGui::GetContentRegionAvail().x;
    const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
    const std::string sid = std::string(idPrefix) + "_" + sp.key;

    switch (sp.type) {
    case ShaderPackParamType::Float: {
        float staticVal = fx.entry.params.count(sp.key) ? fx.entry.params[sp.key] : sp.def;
        EffectParamTrack* tr = nullptr;
        for (auto& t : fx.tracks) { if (t.param == sp.key) { tr = &t; break; } }
        const bool keyed = tr && FindKey(tr->keys, d.Frame()) != nullptr;
        const bool animated = tr && !tr->keys.empty();
        float curVal = tr ? EvaluateParamTrack(*tr, (float)d.Frame(), staticVal) : staticVal;
        float v = curVal;
        const char* fmt = sp.step > 0.0f ? (sp.step >= 1.0f ? "%.0f" : "%.2f") : "%.2f";
        if (SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, fmt)) {
            if (sp.step > 0.0f) v = sp.min + std::round((v - sp.min) / sp.step) * sp.step;
            if (animated || d.autoKey) {
                if (!tr) { fx.tracks.push_back({sp.key, {}}); tr = &fx.tracks.back(); }
                UpsertParamKey(*tr, EffectParamKey{d.Frame(), v, false});
                studioKeyChanged_ = true;
                d.rowsKey = ~0ull;
            } else {
                fx.entry.params[sp.key] = v;
                ++d.projectVersion;
            }
            changed = true;
        }
        if (ImGui::IsItemActivated()) {
            studioKeyBefore_ = {CaptureTrack(d, -1, kind, trackName(sp.key))};
            studioKeyEdit_ = true;
            studioKeyChanged_ = false;
        }
        if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
        break;
    }
    case ShaderPackParamType::Int: {
        float staticVal = fx.entry.params.count(sp.key) ? fx.entry.params[sp.key] : sp.def;
        EffectParamTrack* tr = nullptr;
        for (auto& t : fx.tracks) { if (t.param == sp.key) { tr = &t; break; } }
        const bool animated = tr && !tr->keys.empty();
        float curVal = tr ? EvaluateParamTrack(*tr, (float)d.Frame(), staticVal) : staticVal;
        float v = curVal;
        if (SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, "%.0f")) {
            v = std::round(v);
            if (animated || d.autoKey) {
                if (!tr) { fx.tracks.push_back({sp.key, {}}); tr = &fx.tracks.back(); }
                UpsertParamKey(*tr, EffectParamKey{d.Frame(), v, true});
                studioKeyChanged_ = true;
                d.rowsKey = ~0ull;
            } else {
                fx.entry.params[sp.key] = v;
                ++d.projectVersion;
            }
            changed = true;
        }
        if (ImGui::IsItemActivated()) {
            studioKeyBefore_ = {CaptureTrack(d, -1, kind, trackName(sp.key))};
            studioKeyEdit_ = true;
            studioKeyChanged_ = false;
        }
        if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
        break;
    }
    case ShaderPackParamType::Bool: {
        float staticVal = fx.entry.params.count(sp.key) ? fx.entry.params[sp.key] : (sp.defBool ? 1.0f : 0.0f);
        EffectParamTrack* tr = nullptr;
        for (auto& t : fx.tracks) { if (t.param == sp.key) { tr = &t; break; } }
        const bool animated = tr && !tr->keys.empty();
        float curVal = tr ? EvaluateParamTrack(*tr, (float)d.Frame(), staticVal) : staticVal;
        bool b = (curVal > 0.5f);
        if (Switch(sid.c_str(), label.c_str(), &b)) {
            float v = b ? 1.0f : 0.0f;
            if (animated || d.autoKey) {
                if (!tr) { fx.tracks.push_back({sp.key, {}}); tr = &fx.tracks.back(); }
                UpsertParamKey(*tr, EffectParamKey{d.Frame(), v, true});
                studioKeyChanged_ = true;
                d.rowsKey = ~0ull;
            } else {
                fx.entry.params[sp.key] = v;
                ++d.projectVersion;
            }
            changed = true;
        }
        break;
    }
    case ShaderPackParamType::Color: {
        int numComp = sp.alpha ? 4 : 3;
        EffectParamTrack* trComp[4] = {nullptr, nullptr, nullptr, nullptr};
        bool keyedComp[4] = {false, false, false, false};
        float curValComp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int c = 0; c < numComp; ++c) {
            std::string tc = sp.key + "#" + std::to_string(c);
            float defVal = sp.defVec[c];
            float staticVal = fx.entry.params.count(tc) ? fx.entry.params[tc] : defVal;
            for (auto& t : fx.tracks) { if (t.param == tc) { trComp[c] = &t; break; } }
            keyedComp[c] = trComp[c] && FindKey(trComp[c]->keys, d.Frame()) != nullptr;
            curValComp[c] = trComp[c] ? EvaluateParamTrack(*trComp[c], (float)d.Frame(), staticVal) : staticVal;
        }
        const bool rgbAnimated = (trComp[0] && !trComp[0]->keys.empty()) || (trComp[1] && !trComp[1]->keys.empty()) ||
                                 (trComp[2] && !trComp[2]->keys.empty());
        const float itemW = availW;
        const ImVec2 cpos = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(cpos.x, cpos.y + Dp(4.0f)), cpos.x + itemW * 0.4f, p.ink2, label.c_str());
        ImGui::SetCursorScreenPos(ImVec2(cpos.x + itemW * 0.4f, cpos.y));
        float rgb[3] = {curValComp[0], curValComp[1], curValComp[2]};
        if (ColorField(sid.c_str(), rgb)) {
            for (int c = 0; c < 3; ++c) {
                std::string tc = sp.key + "#" + std::to_string(c);
                if (rgbAnimated || d.autoKey || keyedComp[c]) {
                    if (!trComp[c]) { fx.tracks.push_back({tc, {}}); trComp[c] = &fx.tracks.back(); }
                    UpsertParamKey(*trComp[c], EffectParamKey{d.Frame(), rgb[c], false});
                } else {
                    fx.entry.params[tc] = rgb[c];
                }
            }
            if (rgbAnimated || d.autoKey) { studioKeyChanged_ = true; d.rowsKey = ~0ull; }
            else { ++d.projectVersion; }
            changed = true;
        }
        if (sp.alpha) {
            std::string alphaLabel = label + " " + Tr("불투명도");
            ImGui::SetNextItemWidth(availW);
            float a = curValComp[3];
            if (SliderRow((sid + "_a").c_str(), alphaLabel.c_str(), &a, 0.0f, 1.0f, "%.2f")) {
                std::string tc = sp.key + "#3";
                if (rgbAnimated || d.autoKey || keyedComp[3]) {
                    if (!trComp[3]) { fx.tracks.push_back({tc, {}}); trComp[3] = &fx.tracks.back(); }
                    UpsertParamKey(*trComp[3], EffectParamKey{d.Frame(), a, false});
                    studioKeyChanged_ = true;
                    d.rowsKey = ~0ull;
                } else {
                    fx.entry.params[tc] = a;
                    ++d.projectVersion;
                }
                changed = true;
            }
            if (ImGui::IsItemActivated()) {
                studioKeyBefore_ = {CaptureTrack(d, -1, kind, trackName(sp.key + "#3"))};
                studioKeyEdit_ = true;
                studioKeyChanged_ = false;
            }
            if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
        }
        break;
    }
    case ShaderPackParamType::Vec3: {
        static const char* kAxes[3] = {" X", " Y", " Z"};
        for (int c = 0; c < 3; ++c) {
            std::string tc = sp.key + "#" + std::to_string(c);
            float defVal = sp.defVec[c];
            float staticVal = fx.entry.params.count(tc) ? fx.entry.params[tc] : defVal;
            EffectParamTrack* tr = nullptr;
            for (auto& t : fx.tracks) { if (t.param == tc) { tr = &t; break; } }
            const bool animated = tr && !tr->keys.empty();
            float curVal = tr ? EvaluateParamTrack(*tr, (float)d.Frame(), staticVal) : staticVal;
            std::string clabel = label + kAxes[c];
            std::string csid = sid + "_" + std::to_string(c);
            float v = curVal;
            const char* fmt = sp.step > 0.0f ? (sp.step >= 1.0f ? "%.0f" : "%.2f") : "%.2f";
            if (SliderRow(csid.c_str(), clabel.c_str(), &v, sp.min, sp.max, fmt)) {
                if (sp.step > 0.0f) v = sp.min + std::round((v - sp.min) / sp.step) * sp.step;
                if (animated || d.autoKey) {
                    if (!tr) { fx.tracks.push_back({tc, {}}); tr = &fx.tracks.back(); }
                    UpsertParamKey(*tr, EffectParamKey{d.Frame(), v, false});
                    studioKeyChanged_ = true;
                    d.rowsKey = ~0ull;
                } else {
                    fx.entry.params[tc] = v;
                    ++d.projectVersion;
                }
                changed = true;
            }
            if (ImGui::IsItemActivated()) {
                studioKeyBefore_ = {CaptureTrack(d, -1, kind, trackName(tc))};
                studioKeyEdit_ = true;
                studioKeyChanged_ = false;
            }
            if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
        }
        break;
    }
    }
    ImGui::PopID();
};

if (pk.groups.empty()) {
    for (size_t k = 0; k < pk.params.size(); ++k) {
        drawStudioParam(pk.params[k], k);
    }
} else {
    std::vector<std::pair<const ShaderPackParam*, size_t>> ungrouped;
    std::map<std::string, std::vector<std::pair<const ShaderPackParam*, size_t>>> grouped;
    for (size_t k = 0; k < pk.params.size(); ++k) {
        const ShaderPackParam& sp = pk.params[k];
        if (sp.group.empty()) ungrouped.push_back({&sp, k});
        else grouped[sp.group].push_back({&sp, k});
    }
    for (const auto& [sp, k] : ungrouped) {
        drawStudioParam(*sp, k);
    }
    for (const auto& g : pk.groups) {
        const auto it = grouped.find(g.id);
        if (it == grouped.end() || it->second.empty()) continue;
        const char* groupPrefix = kind == RowKind::EffectParam ? "##studiofx" : idPrefix;
        const bool open = DrawParamGroupHeader(pk, g, groupPrefix);
        if (open) {
            ImGui::Indent(Dp(14.0f));
            for (const auto& [sp, k] : it->second) {
                drawStudioParam(*sp, k);
            }
            ImGui::Unindent(Dp(14.0f));
        }
    }
}
    return changed;
}

} // namespace mmdx

