// Auto-update UI: the select screen's notice (new version available / download progress /
// failure / not-writable) and the App-side wiring of updater::Controller. The controller runs
// its workers; everything here only polls and draws (UiKit, so it matches DESIGN.md).
#include "app/App.h"
#include "app/Components.h"
#include "app/Icons.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"

#include <shellapi.h>

#include <cmath>
#include <cstdio>

#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"

namespace mmdx {

using namespace ui;

namespace {

// One "fetch this optional component (Components.h)" action: a download button, or the progress
// strip while running, or the result line. Returns true when a fetch was started this frame.
bool ComponentAction(updater::ComponentFetcher& fetcher, const components::Def& def, const char* label,
                     float width, bool restartNote) {
    using Phase = updater::ComponentFetcher::Phase;
    const updater::ComponentFetcher::State s = fetcher.Get();
    const bool active = s.id == def.id;
    const Palette& p = P();
    bool started = false;
    ImGui::PushID(def.id);
    if (active && s.phase == Phase::Running) {
        char buf[192];
        if (s.totalMb > 0.05)
            std::snprintf(buf, sizeof(buf), "%s  ·  %.0f / %.0f MB", label, s.doneMb, s.totalMb);
        else
            std::snprintf(buf, sizeof(buf), "%s", label);
        PushFont(Font::Regular, size::Small);
        ImGui::TextUnformatted(buf);
        PopFont();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ProgressBar(dl, a, ImVec2(a.x + width, a.y + Dp(8.0f)), s.fraction);
        ImGui::Dummy(ImVec2(width, Dp(14.0f)));
    } else if (active && s.phase == Phase::Done) {
        PushFont(Font::Semibold, size::Small);
        ImGui::TextUnformatted(label);
        PopFont();
        PushFont(Font::Regular, size::Caption);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        ImGui::TextUnformatted(restartNote ? Tr("받았어요 · 재시작 후 적용돼요") : Tr("받았어요 · 사용할 수 있어요"));
        ImGui::PopStyleColor();
        PopFont();
    } else {
        const bool failed = active && s.phase == Phase::Failed;
        if (Button("##compfetch", label, icon::DownloadSimple, ButtonKind::Secondary,
                   ImVec2(width / Dpi(), 32.0f))) {
            fetcher.Start(def);
            started = true;
        }
        if (failed) {
            PushFont(Font::Regular, size::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.warn));
            ImGui::TextWrapped("%s", s.error.c_str());
            ImGui::PopStyleColor();
            PopFont();
        }
    }
    ImGui::PopID();
    return started;
}

} // namespace

// ---------------------------------------------------------------------------
// Optional components (Components.h)
// ---------------------------------------------------------------------------

void App::UpdateComponentUi() {
    component_.Get();  // drains a finished fetch (its state feeds the rows below)
}

// Fetches one optional component with no window and exits (tools/components_e2e.py). Called from
// App::Run before the updater is configured; the process must not leave the lobby otherwise.
int App::CliComponentE2e() {
    const components::Def* def = components::Find(options_.componentId);
    if (!def) {
        LOG_ERROR("COMPONENT ok=0 error='unknown component %s'", options_.componentId.c_str());
        return 1;
    }
    const std::string feed = !options_.updateFeed.empty() ? options_.updateFeed : updater::kDefaultFeedUrl;
    component_.Configure(feed, MMDX12_VERSION);
    LOG_INFO("COMPONENT start id=%s feed=%s", def->id, feed.c_str());
    component_.Start(*def);
    for (int i = 0; i < 9000; ++i) {  // up to ~3 min; the caller's timeout is the real guard
        const auto phase = component_.Get().phase;
        if (phase != updater::ComponentFetcher::Phase::Running) break;
        Sleep(20);
    }
    const updater::ComponentFetcher::State& s = component_.Get();
    if (s.phase == updater::ComponentFetcher::Phase::Done) {
        LOG_INFO("COMPONENT ok=1 id=%s", def->id);
        return 0;
    }
    LOG_ERROR("COMPONENT ok=0 id=%s error='%s'", def->id, s.error.c_str());
    return 1;
}

// The upscaler picker (UiSelect, "업스케일러"): offer the runtime DLL when it is the only reason the
// upscaler is unavailable (a matching GPU with the file missing), never when the GPU is unsupported.
void App::DrawUpscalerComponentRow(UpscalerKind kind, float width) {
    const components::Def* def = components::ForUpscaler((int)kind);
    // Present: hidden, except right after this session fetched it (the "restart to apply" line).
    if (!def || (components::Present(*def) && component_.Get().id != def->id)) return;
    if (kind == UpscalerKind::DLSS && ctx_.Caps().vendorId != 0x10DE) return;  // a missing DLL is not the cause
    Gap(6.0f);
    char label[128];
    std::snprintf(label, sizeof(label), Tr("%s 다운로드"), Tr(def->label));
    ComponentAction(component_, *def, label, width, true);
}

// The lobby's settings panel (UiSelect, fonts): offer the Noto CJK fonts in a Japanese / Chinese UI
// where they are missing (without them the UI falls back to system fonts). Applies after a restart.
void App::DrawFontComponentRow(float width) {
    const Language lang = ActiveLanguage();
    if (lang != Language::Japanese && lang != Language::Chinese) return;
    if (components::Present(components::kCjk) && component_.Get().id != components::kCjk.id) return;
    Gap(14.0f);
    SectionLabel(Tr("글꼴"));
    ComponentAction(component_, components::kCjk, Tr("고품질 CJK 글꼴 받기"), width, true);
}

// ---------------------------------------------------------------------------
// The notice (select screen)
// ---------------------------------------------------------------------------

// Width ui::Button gives itself for `label` (+ optional icon) with size.x == 0 (same helper as
// UiOffline.cpp's; kept local so UiHelpers.h stays free of UiKit includes).
float ButtonWidth(const char* label, bool withIcon) {
    return Dp(32.0f) + (withIcon ? Dp(18.0f + 8.0f) : 0.0f) + TextSize(Font::Semibold, size::Body, label).x;
}

bool App::HeadlessRun() const {
    // Scripted/headless runs must not be interrupted (or slowed) by a network check, and they
    // must not leave prompts behind for the next test.
    return options_.quitAfterFrames > 0 || !options_.uiScript.empty() || !options_.benchmarkCategory.empty() ||
           options_.offlineProbe || !options_.offlineStill.empty() || !options_.offlineVideo.empty() ||
           !options_.startScreen.empty();
}

void App::InitUpdater() {
    std::string feed = !options_.updateFeed.empty() ? options_.updateFeed
                       : !settings_.updateFeedUrl.empty() ? settings_.updateFeedUrl
                                                          : updater::kDefaultFeedUrl;
    component_.Configure(feed, MMDX12_VERSION);
    update_.Configure(std::move(feed), MMDX12_VERSION);
}

void App::StartUpdateCheck() {
    updateCheckQueued_ = true;  // one attempt per run
    if (HeadlessRun()) return;  // never block or race scripted/headless runs
    if (updater::kNightlyBuild && !settings_.nightlyNoticeShown) {  // once per install; the 정보 popup keeps the text
        settings_.nightlyNoticeShown = true;
        settings_.Save(settingsPath_);
        toast_ = {Tr("나이틀리 빌드입니다"), Tr("자동으로 만든 테스트 빌드예요. 여기서 저장한 프로젝트는 정식 버전에서 열리지 않을 수 있어요."),
                  {}, false, timeSeconds_ + 10.0};
    }
    update_.StartCheck();
}

void App::UpdateUpdate() {
    // The check starts once the window is up (the start is not slowed: it is a worker thread).
    if (!updateCheckQueued_ && screen_ == Screen::Select) StartUpdateCheck();
    const updater::Controller::State& s = update_.Get();
    // Once the staging succeeded the app must exit: the applier (already spawned) waits for this
    // process to end, swaps the files and restarts the new version.
    if (s.phase == updater::Controller::Phase::Staged) {
        if (updateInstallingFromCli_) LOG_INFO("UPDATEINSTALL staged=1 - exiting for the restart");
        else LOG_INFO("update: staged - exiting for the restart");
        running_ = false;
    }
    if (updateInstallingFromCli_ && s.phase == updater::Controller::Phase::Failed) {
        LOG_ERROR("UPDATEINSTALL staged=0 error='%s'", s.error.c_str());
        running_ = false;
    }
}

// "Update" on the notice: relaunch args for the restarted app. A CLI trigger updates without
// the preselection panes; a normal one reopens the select screen.
void App::StartUpdateInstall() {
    std::string relaunch;
    if (!options_.updateFeed.empty()) relaunch += "--update-feed \"" + options_.updateFeed + "\"";
    update_.StartUpdate(relaunch);
}

void App::UpdateCheckCommand() {
    // Synchronous check that also drives the notice (scripted runs skip the automatic check, so
    // this is how a script brings the notice up for a capture / the full flow).
    update_.CheckNow();
    const updater::Controller::State& s = update_.Get();
    const char* phase = s.phase == updater::Controller::Phase::Available    ? "available"
                        : s.phase == updater::Controller::Phase::UpToDate   ? "uptodate"
                        : s.phase == updater::Controller::Phase::NotWritable ? "notwritable"
                                                                            : "error";
    LOG_INFO("UPDATECHECK current=%s phase=%s version=%s error='%s'", MMDX12_VERSION, phase,
             s.version.c_str(), s.error.c_str());
}

void App::UpdateInstallCommand() {
    // For scripts: stage the update for the feed and exit; the applier restarts the app.
    const updater::Feed f = updater::FetchFeed(
        !options_.updateFeed.empty() ? options_.updateFeed : updater::kDefaultFeedUrl);
    if (!f.ok || !updater::IsNewer(f.version, MMDX12_VERSION)) {
        LOG_ERROR("UPDATEINSTALL ok=0 error='%s' current=%s feed=%s", f.ok ? "not newer" : f.error.c_str(),
                  MMDX12_VERSION, f.version.c_str());
        running_ = false;
        return;
    }
    // The restarted app must find the feed again (the applier relaunches with these args).
    std::string relaunch;
    if (!options_.updateFeed.empty()) {
        relaunch = "--update-feed \"" + options_.updateFeed + "\"";
        // An automated E2E run: the restarted version quits on its own after a few frames
        // (--screen select makes --frames count from the first frame; see AppRenderFrame).
        if (options_.updateRelaunchFrames > 0)
            relaunch += " --screen select --frames " + std::to_string(options_.updateRelaunchFrames);
    }
    update_.Configure(!options_.updateFeed.empty() ? options_.updateFeed : updater::kDefaultFeedUrl,
                      MMDX12_VERSION);
    update_.StartFeedUpdate(f, relaunch);
    LOG_INFO("UPDATEINSTALL ok=1 version=%s (staging in the background)", f.version.c_str());
    updateInstallingFromCli_ = true;
}

// ---------------------------------------------------------------------------
// The notice (select screen)
// ---------------------------------------------------------------------------

void App::DrawUpdateNotice() {
    const updater::Controller::State& s = update_.Get();
    if (!update_.NoticeVisible()) {
        // While installing from a ui-script the progress is still logged, not drawn.
        return;
    }

    // A download in progress has its own dialog (below); the idle notice is a slim footer card.
    const bool busy = s.phase == updater::Controller::Phase::Downloading;
    if (busy) {
        DrawUpdateProgressDialog();
        return;
    }

    if (s.phase != updater::Controller::Phase::Available &&
        s.phase != updater::Controller::Phase::Failed &&
        s.phase != updater::Controller::Phase::NotWritable)
        return;

    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float gap = Dp(12.0f);
    const Palette& p = P();

    const bool failure = s.phase == updater::Controller::Phase::Failed;
    const bool nowrite = s.phase == updater::Controller::Phase::NotWritable;
    const ImU32 tint = failure || nowrite ? p.warn : p.accent;
    char titleBuf[256];
    if (failure) {
        std::snprintf(titleBuf, sizeof(titleBuf), "%s", Tr("업데이트 확인에 실패했습니다"));
    } else if (nowrite) {
        std::snprintf(titleBuf, sizeof(titleBuf), "%s", Tr("새 버전이 있지만 설치 폴더에 쓸 수 없습니다"));
    } else {
        std::snprintf(titleBuf, sizeof(titleBuf), "%s %s", Tr("새 버전"), s.version.c_str());
    }
    const char* title = titleBuf;

    // Buttons: Update + Later (available), Open release page (failure/not-writable).
    float buttonsW = 0;
    const float btnH = Dp(34.0f);
    float updateW = 0, laterW = 0, pageW = 0;
    if (!failure && !nowrite) {
        updateW = ButtonWidth(Tr("업데이트"), true);
        laterW = ButtonWidth(Tr("나중에"), false);
        buttonsW = updateW + laterW + gap;
    } else {
        pageW = ButtonWidth(Tr("릴리스 페이지 열기"), true);
        buttonsW = pageW;
    }

    // The feed's notes are a few short lines ("• ..."): one row each, left of the buttons.
    std::vector<std::string> noteLines;
    for (size_t start = 0; start < s.note.size() && noteLines.size() < 4;) {
        size_t end = s.note.find('\n', start);
        if (end == std::string::npos) end = s.note.size();
        if (end > start) noteLines.push_back(s.note.substr(start, end - start));
        start = end + 1;
    }
    const float lineH = Dp(19.0f);
    const float textH = Dp(22.0f) + lineH * float(noteLines.size());
    const float w = std::min(ds.x - Dp(56.0f), Dp(560.0f));
    const float h = Dp(28.0f) + std::max(textH, btnH);
    const ImVec2 a(ds.x * 0.5f - w * 0.5f, ds.y - h - Dp(20.0f)), b(a.x + w, a.y + h);

    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##updatenotice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Panel(dl, a, b, Dp(14.0f), 1.2f);
    dl->AddLine(ImVec2(a.x, a.y + Dp(2.0f)), ImVec2(a.x + Dp(2.0f), b.y - Dp(2.0f)), tint, Dp(3.0f));

    const float tx = a.x + Dp(20.0f);
    float y = a.y + Dp(14.0f);
    Text(dl, Font::Semibold, size::Body, ImVec2(tx, y), p.ink, title);
    y += Dp(22.0f);
    for (const std::string& line : noteLines) {
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(tx, y), b.x - Dp(32.0f) - buttonsW, p.ink2, line.c_str());
        y += lineH;
    }
    y = b.y - Dp(14.0f) - btnH;

    if (!failure && !nowrite) {
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(16.0f) - buttonsW, y));
        if (Button("##updignore", Tr("나중에"), nullptr, ButtonKind::Ghost, ImVec2(laterW / Dpi(), 34.0f)))
            update_.HideNotice();
        ImGui::SameLine(0, Dp(gap));
        if (Button("##updinstall", Tr("업데이트"), icon::DownloadSimple, ButtonKind::Primary,
                   ImVec2(updateW / Dpi(), 34.0f)))
            StartUpdateInstall();
    } else {
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(16.0f) - pageW, y));
        if (Button("##updpage", Tr("릴리스 페이지 열기"), icon::Globe, ButtonKind::Secondary,
                   ImVec2(pageW / Dpi(), 34.0f)))
            update_.OpenReleasePage();
    }
    ImGui::End();
}

// The download/verify/extract progress dialog ("Update" was clicked; cancel-able).
void App::DrawUpdateProgressDialog() {
    const updater::Controller::State& s = update_.Get();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const Palette& p = P();
    const float w = std::min(ds.x - Dp(56.0f), Dp(460.0f));
    const float h = Dp(130.0f);
    const ImVec2 a(ds.x * 0.5f - w * 0.5f, ds.y - h - Dp(20.0f)), b(a.x + w, a.y + h);

    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##updateprogress", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Panel(dl, a, b, Dp(14.0f), 1.2f);

    const float tx = a.x + Dp(20.0f);
    char buf[128];
    std::snprintf(buf, sizeof(buf), Tr("MMDX12 %s(으)로 업데이트하는 중..."), s.version.c_str());
    Text(dl, Font::Semibold, size::Body, ImVec2(tx, a.y + Dp(16.0f)), p.ink, buf);

    const char* stage = s.stage == updater::Controller::Stage::Scan     ? Tr("설치된 파일 확인 중")
                        : s.stage == updater::Controller::Stage::Verify ? Tr("확인하는 중")
                        : s.stage == updater::Controller::Stage::Extract ? Tr("압축을 푸는 중")
                                                                         : Tr("다운로드하는 중");
    char detail[160];
    if (s.stage == updater::Controller::Stage::Download && s.delta && s.deltaFiles > 0) {
        std::snprintf(detail, sizeof(detail), Tr("변경 파일 %d개 · %.1f MB / 전체 %.0f MB"), s.deltaFiles,
                      s.doneMb, s.fullMb > 0 ? s.fullMb : s.totalMb);
    } else if (s.totalMb > 0.5 && s.stage == updater::Controller::Stage::Download) {
        std::snprintf(detail, sizeof(detail), "%s  ·  %.0f / %.0f MB", stage, s.doneMb, s.totalMb);
    } else {
        std::snprintf(detail, sizeof(detail), "%s", stage);
    }
    Text(dl, Font::Regular, size::Small, ImVec2(tx, a.y + Dp(40.0f)), p.ink2, detail);

    const float barY = a.y + Dp(66.0f);
    ProgressBar(dl, ImVec2(tx, barY), ImVec2(b.x - Dp(16.0f), barY + Dp(8.0f)), s.fraction);

    const float cancelW = ButtonWidth(Tr("취소"), true);
    ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(16.0f) - cancelW, b.y - Dp(14.0f) - Dp(30.0f)));
    if (Button("##updcancel", Tr("취소"), icon::X, ButtonKind::Ghost, ImVec2(cancelW / Dpi(), 30.0f)))
        update_.CancelUpdate();
    ImGui::End();
}

}  // namespace mmdx
