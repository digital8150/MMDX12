#include "app/App.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Log.h"
#include "core/TextUtil.h"
#include "app/Icons.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

namespace {

// First index whose ToLowerAscii(id) contains ToLowerAscii(key), else -1.
int FindPresetIndexChars(const std::vector<CharacterAsset>& v, const char* key) {
    const std::string needle = ToLowerAscii(key);
    for (size_t i = 0; i < v.size(); ++i)
        if (ToLowerAscii(v[i].id).find(needle) != std::string::npos) return (int)i;
    return -1;
}

int FindPresetIndexStages(const std::vector<StageAsset>& v, const char* key) {
    const std::string needle = ToLowerAscii(key);
    for (size_t i = 0; i < v.size(); ++i)
        if (ToLowerAscii(v[i].id).find(needle) != std::string::npos) return (int)i;
    return -1;
}

int FindPresetIndexSongs(const std::vector<SongAsset>& v, const char* key) {
    const std::string needle = ToLowerAscii(key);
    for (size_t i = 0; i < v.size(); ++i)
        if (ToLowerAscii(v[i].id).find(needle) != std::string::npos) return (int)i;
    return -1;
}

// "Windows 11 (build N)" / "Windows 10 (build N)".
std::string OsVersionString() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
                               : nullptr;
    if (rtlGetVersion) {
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion(&info) == 0) {
            const char* name = info.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
            return std::string(name) + " (build " + std::to_string(info.dwBuildNumber) + ")";
        }
    }
    return "Windows";
}

} // namespace

// ---------------------------------------------------------------------------
// Leaderboard futures
// ---------------------------------------------------------------------------

void App::RefreshLeaderboard() {
    if (leaderboardFuture_.valid() &&
        leaderboardFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    leaderboard_.reset();
    const std::string url = settings_.leaderboardUrl;
    const std::string cat = kBenchCategories[benchCategory_].id;
    leaderboardFuture_ = std::async(std::launch::async, [url, cat] {
        LeaderboardClient c(url);
        LeaderboardPage p;
        p.category = cat;
        std::string e;
        p.ok = c.Fetch(cat, p, &e);
        p.error = e;
        return p;
    });
}

// ---------------------------------------------------------------------------
// Lobby
// ---------------------------------------------------------------------------

namespace {

void PollNetwork(std::future<LeaderboardPage>& lbFuture, std::optional<LeaderboardPage>& lb,
                 std::future<SubmitResponse>& subFuture, std::optional<SubmitResponse>& sub, bool& refresh) {
    if (lbFuture.valid() && lbFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) lb = lbFuture.get();
    if (subFuture.valid() && subFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        sub = subFuture.get();
        if (sub && sub->ok) refresh = true;
    }
}

const char* ShortLabel(int category) { return kBenchCategories[category].resLabel; }

std::string CategoryTag(int c) { return std::string(kBenchCategories[c].pathLabel) + " · " + kBenchCategories[c].resLabel; }

bool IsGi(int c) { return kBenchCategories[c].offline; }

// "1:23.4" (minutes:seconds with tenths) for render times.
std::string RenderTime(double sec) {
    if (sec < 0) sec = 0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%04.1f", (int)(sec / 60.0), std::fmod(sec, 60.0));
    return buf;
}

} // namespace

void App::StartBenchmarkLoad() {
    const int presetChar = FindPresetIndexChars(library_.characters, kBenchPresetCharacter);
    const int presetStage = FindPresetIndexStages(library_.stages, kBenchPresetStage);
    const int presetSong = FindPresetIndexSongs(library_.songs, kBenchPresetSong);
    benchOfficial_ = presetChar >= 0 && presetStage >= 0 && presetSong >= 0;
    settings_.Save(settingsPath_);
    if (benchOfficial_) {
        StartLoad(LoadTarget::Benchmark, &library_.characters[(size_t)presetChar], &library_.stages[(size_t)presetStage],
                  &library_.songs[(size_t)presetSong]);
    } else if (selCharacter_ >= 0 && selSong_ >= 0) {
        StartLoad(LoadTarget::Benchmark, &library_.characters[(size_t)selCharacter_],
                  selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr, &library_.songs[(size_t)selSong_]);
    }
}

void App::DrawBenchLobby() {
    using namespace ui;
    bool refresh = false;
    PollNetwork(leaderboardFuture_, leaderboard_, submitFuture_, submitResult_, refresh);
    if (refresh) RefreshLeaderboard();

    BeginScreen("##benchlobby");
    DrawAppBar(1);
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = Dp(28.0f), top = Dp(64.0f) + pad + Dp(4.0f);
    const float leftW = Dp(420.0f);
    const float lx = pad;

    const int presetChar = FindPresetIndexChars(library_.characters, kBenchPresetCharacter);
    const int presetStage = FindPresetIndexStages(library_.stages, kBenchPresetStage);
    const int presetSong = FindPresetIndexSongs(library_.songs, kBenchPresetSong);
    benchOfficial_ = presetChar >= 0 && presetStage >= 0 && presetSong >= 0;
    const bool hasSelection = selCharacter_ >= 0 && selSong_ >= 0;

    // ---- left column
    float y = top;
    Text(dl, Font::Bold, size::Hero, ImVec2(lx, y), p.ink, "벤치마크");
    y += Dp(46.0f);
    {
        ImGui::SetCursorScreenPos(ImVec2(lx, y));
        PushFont(Font::Regular, size::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
        ImGui::PushTextWrapPos(lx + leftW);
        if (IsGi(benchCategory_))
            ImGui::TextUnformatted("시네벤치처럼 고정된 장면 한 장을 오프라인 GI 렌더러로 그리고, 끝까지 걸린 시간으로 "
                                   "점수를 매깁니다. 유리 큐브의 굴절·분산·투과와 소프트박스 조명을 이래디언스 캐시와 "
                                   "패스 트레이싱으로 계산합니다. 4K · 픽셀당 4096 샘플 고정.");
        else
            ImGui::TextUnformatted("같은 장면, 같은 작업량(프레임당 1/60초)으로 GPU 성능을 비교합니다. 음소거, 수직 동기화와 "
                                   "업스케일러 끔, 높음 품질. 래스터·RT는 MSAA 4x, PT는 1 spp · 3회 반사.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        PopFont();
        y = ImGui::GetCursorScreenPos().y + Dp(22.0f);
    }

    Text(dl, Font::Semibold, size::Small, ImVec2(lx, y), p.ink2, "렌더링 방식");
    y += Dp(26.0f);
    {
        const int curPath = IsGi(benchCategory_) ? kBenchPathCount : benchCategory_ / 2;
        int pathIdx = curPath;
        ImGui::SetCursorScreenPos(ImVec2(lx, y));
        const char* benchPaths[] = {"래스터", "레이 트레이싱", "패스 트레이싱", "GI 렌더"};
        if (Segmented("##benchpath", benchPaths, kBenchPathCount + 1, &pathIdx, leftW / Dpi(), 36.0f)) {
            const bool ok = pathIdx == 0 || (pathIdx == kBenchPathCount ? renderer_.OfflineSupported()
                                                                         : renderer_.RayTracingSupported());
            if (!ok) {
                pathIdx = curPath;
            } else {
                benchCategory_ = pathIdx == kBenchPathCount
                                     ? kBenchGiRender
                                     : pathIdx * 2 + (IsGi(benchCategory_) ? 0 : benchCategory_ % 2);
                RefreshLeaderboard();
            }
        }
        y += Dp(36.0f);
        if (!renderer_.RayTracingSupported()) {
            PushFont(Font::Regular, size::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
            ImGui::TextUnformatted("이 GPU는 DXR 1.1을 지원하지 않습니다");
            ImGui::PopStyleColor();
            PopFont();
            y = ImGui::GetCursorScreenPos().y;
        }
        y += Dp(22.0f);
    }

    Text(dl, Font::Semibold, size::Small, ImVec2(lx, y), p.ink2, IsGi(benchCategory_) ? "렌더 설정" : "해상도");
    y += Dp(26.0f);
    if (IsGi(benchCategory_)) {
        // the single fixed workload
        const float chh = Dp(60.0f);
        const ImVec2 a(lx, y), b(lx + leftW, y + chh);
        dl->AddRectFilled(a, b, Mix(p.surface, p.accentSoft, 0.6f), Dp(14.0f));
        dl->AddRect(a, b, p.accent, Dp(14.0f), 0, Dp(2.0f));
        Text(dl, Font::Bold, 24.0f, ImVec2(a.x + Dp(18.0f), a.y + Dp(14.0f)), p.accentInk,
             kBenchCategories[kBenchGiRender].resLabel);
        char res[64];
        std::snprintf(res, sizeof(res), "%u × %u  ·  %u spp  ·  GI", kRenderBenchWidth, kRenderBenchHeight,
                      kRenderBenchSamples);
        const ImVec2 rs = TextSize(Font::Regular, size::Small, res);
        Text(dl, Font::Regular, size::Small, ImVec2(b.x - Dp(18.0f) - rs.x, a.y + (chh - rs.y) * 0.5f), p.ink2, res);
        y += chh + Dp(26.0f);
    } else {
        const float cw = (leftW - Dp(12.0f)) * 0.5f, chh = Dp(60.0f);
        for (int r = 0; r < 2; ++r) {
            const int i = (benchCategory_ / 2) * 2 + r;
            const ImVec2 a(lx + r * (cw + Dp(12.0f)), y), b(a.x + cw, y + chh);
            ImGui::PushID(r);
            ImGui::SetCursorScreenPos(a);
            const bool pressed = ImGui::InvisibleButton("##cat", ImVec2(cw, chh));
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const bool sel = benchCategory_ == i;
            const float hv = Anim(ImGui::GetID("##hv"), hovered), sv = Anim(ImGui::GetID("##sv"), sel, 14.0f);
            SoftShadow(dl, a, b, Dp(14.0f), Dp(10.0f), 0.06f + 0.05f * hv, ImVec2(0, Dp(3.0f)));
            dl->AddRectFilled(a, b, Mix(p.surface, p.accentSoft, sv * 0.6f), Dp(14.0f));
            dl->AddRect(a, b, Mix(WithAlpha(p.ink, 0.06f), p.accent, sv), Dp(14.0f), 0, sv > 0.5f ? Dp(2.0f) : 1.0f);
            Text(dl, Font::Bold, 24.0f, ImVec2(a.x + Dp(18.0f), a.y + Dp(14.0f)), sv > 0.5f ? p.accentInk : p.ink,
                 ShortLabel(i));
            char res[32];
            std::snprintf(res, sizeof(res), "%u × %u", kBenchCategories[i].width, kBenchCategories[i].height);
            const ImVec2 rs = TextSize(Font::Regular, size::Small, res);
            Text(dl, Font::Regular, size::Small, ImVec2(b.x - Dp(18.0f) - rs.x, a.y + (chh - rs.y) * 0.5f), p.ink2, res);
            if (pressed && !sel) {
                benchCategory_ = i;
                RefreshLeaderboard();
            }
            ImGui::PopID();
        }
        y += chh + Dp(26.0f);
    }

    Text(dl, Font::Semibold, size::Small, ImVec2(lx, y), p.ink2, "측정 장면");
    y += Dp(26.0f);
    int rbChars[kRenderBenchPerformerCount], rbSongs[kRenderBenchPerformerCount];
    const bool rbAssets = FindRenderBenchAssets(rbChars, rbSongs);
    if (IsGi(benchCategory_)) {
        const float ph = Dp(116.0f);
        const ImVec2 a(lx, y), b(lx + leftW, y + ph + Dp(64.0f));
        Panel(dl, a, b, Dp(14.0f), 0.8f);
        DrawRenderBenchPreview(a.x + Dp(8.0f), a.y + Dp(8.0f), b.x - Dp(8.0f), a.y + ph, Dp(10.0f));
        TextEllipsis(dl, Font::Semibold, size::Small, ImVec2(a.x + Dp(16.0f), a.y + ph + Dp(12.0f)), b.x - Dp(16.0f), p.ink,
                     "Prism  ·  Sour 미쿠 3인  ·  유리 큐브 스튜디오");
        if (rbAssets)
            Badge(dl, ImVec2(a.x + Dp(16.0f), a.y + ph + Dp(36.0f)), "공식 프리셋", WithAlpha(p.accent, 0.16f), p.accentInk);
        else
            Badge(dl, ImVec2(a.x + Dp(16.0f), a.y + ph + Dp(36.0f)), "Sour 미쿠 모델 또는 모션 에셋이 없습니다", p.warnSoft,
                  p.warn);
        y = b.y + Dp(24.0f);
    } else {
        const CharacterAsset* ch = benchOfficial_ ? &library_.characters[(size_t)presetChar]
                                                  : (hasSelection ? &library_.characters[(size_t)selCharacter_] : nullptr);
        const StageAsset* st = benchOfficial_ ? &library_.stages[(size_t)presetStage]
                                              : (hasSelection && selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr);
        const SongAsset* song = benchOfficial_ ? &library_.songs[(size_t)presetSong]
                                               : (hasSelection ? &library_.songs[(size_t)selSong_] : nullptr);
        const float ph = Dp(116.0f);
        const ImVec2 a(lx, y), b(lx + leftW, y + ph + Dp(64.0f));
        Panel(dl, a, b, Dp(14.0f), 0.8f);
        DrawScenePreview(ch, st, a.x + Dp(8.0f), a.y + Dp(8.0f), b.x - Dp(8.0f), a.y + ph, Dp(10.0f));
        const std::string names = (ch ? ch->displayName : std::string("-")) + "  ·  " +
                                  (st ? st->displayName : std::string("스튜디오")) + "  ·  " +
                                  (song ? song->displayName : std::string("-"));
        TextEllipsis(dl, Font::Semibold, size::Small, ImVec2(a.x + Dp(16.0f), a.y + ph + Dp(12.0f)), b.x - Dp(16.0f), p.ink,
                     names.c_str());
        if (benchOfficial_)
            Badge(dl, ImVec2(a.x + Dp(16.0f), a.y + ph + Dp(36.0f)), "공식 프리셋", WithAlpha(p.accent, 0.16f), p.accentInk);
        else
            Badge(dl, ImVec2(a.x + Dp(16.0f), a.y + ph + Dp(36.0f)),
                  hasSelection ? "현재 선택으로 측정, 기록 제출 불가" : "공식 프리셋 에셋이 없습니다", p.warnSoft, p.warn);
        y = b.y + Dp(24.0f);
    }

    ImGui::SetCursorScreenPos(ImVec2(lx, y));
    ImGui::PushItemWidth(leftW);
    TextField("##nick", "닉네임", nicknameEdit_, sizeof(nicknameEdit_), leftW / Dpi(), "리더보드에 표시될 이름");
    ImGui::PopItemWidth();
    y = ImGui::GetCursorScreenPos().y + Dp(18.0f);
    const bool gi = IsGi(benchCategory_);
    const bool rtUnavailable = gi ? !renderer_.OfflineSupported()
                                  : kBenchCategories[benchCategory_].path != RenderPath::Raster &&
                                        !renderer_.RayTracingSupported();
    ImGui::SetCursorScreenPos(ImVec2(lx, y));
    ImGui::BeginDisabled((gi ? !rbAssets : (!benchOfficial_ && !hasSelection)) || rtUnavailable);
    if (Button("##start", gi ? "렌더 시작" : "측정 시작", gi ? icon::Image : icon::Lightning, ButtonKind::Primary,
               ImVec2(leftW / Dpi(), 52.0f))) {
        if (gi) {
            settings_.Save(settingsPath_);
            StartRenderBenchLoad();
        } else {
            StartBenchmarkLoad();
        }
    }
    ImGui::EndDisabled();

    // ---- right: leaderboard
    {
        const ImVec2 a(lx + leftW + Dp(32.0f), top), b(ds.x - pad, ds.y - pad);
        Panel(dl, a, b, Dp(18.0f), 1.0f);
        const float ip = Dp(24.0f);
        Text(dl, Font::Bold, size::Heading, ImVec2(a.x + ip, a.y + ip - Dp(2.0f)), p.ink, "리더보드");
        const ImVec2 hs = TextSize(Font::Bold, size::Heading, "리더보드");
        Badge(dl, ImVec2(a.x + ip + hs.x + Dp(12.0f), a.y + ip + Dp(3.0f)), CategoryTag(benchCategory_).c_str(),
              WithAlpha(p.ink, 0.06f), p.ink2);
        ImGui::SetCursorScreenPos(ImVec2(b.x - ip - Dp(36.0f), a.y + ip - Dp(6.0f)));
        if (IconButton("##refresh", icon::Refresh, "새로고침")) RefreshLeaderboard();

        // columns: rank, nickname, score, tier, avg, low1, gpu, date
        const float tx = a.x + ip, tw = b.x - a.x - ip * 2.0f;
        const float colRank = Dp(52.0f), colScore = Dp(96.0f), colTier = Dp(64.0f), colFps = Dp(84.0f),
                    colLow = Dp(84.0f), colDate = Dp(100.0f);
        const float flex = std::max(Dp(160.0f), tw - colRank - colScore - colTier - colFps - colLow - colDate);
        const float colNick = flex * 0.42f, colGpu = flex - colNick;
        float cx[9];
        cx[0] = tx;
        const float widths[8] = {colRank, colNick, colScore, colTier, colFps, colLow, colGpu, colDate};
        for (int i = 0; i < 8; ++i) cx[i + 1] = cx[i] + widths[i];
        const char* heads[8] = {"순위", "닉네임", "점수", "등급", "평균 FPS", "1% Low", "GPU", "날짜"};
        if (IsGi(benchCategory_)) {
            heads[4] = "M샘플/초";
            heads[5] = "렌더 시간";
        }
        const bool rightAlign[8] = {false, false, true, false, true, true, false, false};
        float hy = a.y + ip + Dp(50.0f);
        for (int i = 0; i < 8; ++i) {
            const ImVec2 s = TextSize(Font::Semibold, size::Caption, heads[i]);
            const float x = rightAlign[i] ? cx[i + 1] - Dp(14.0f) - s.x : cx[i] + (i == 0 ? Dp(8.0f) : 0.0f);
            Text(dl, Font::Semibold, size::Caption, ImVec2(x, hy), p.ink3, heads[i]);
        }
        hy += Dp(26.0f);
        dl->AddLine(ImVec2(tx, hy), ImVec2(tx + tw, hy), p.line);
        const float listTop = hy + Dp(6.0f), listBottom = b.y - ip - Dp(24.0f);

        const bool loading = leaderboardFuture_.valid() &&
                             leaderboardFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        if (loading || !leaderboard_) {
            for (int r = 0; r < 8; ++r) {
                const float ry = listTop + r * Dp(48.0f) + Dp(14.0f);
                if (ry + Dp(20.0f) > listBottom) break;
                Skeleton(dl, ImVec2(cx[0] + Dp(8.0f), ry), ImVec2(cx[0] + Dp(28.0f), ry + Dp(18.0f)), Dp(6.0f));
                Skeleton(dl, ImVec2(cx[1], ry), ImVec2(cx[1] + colNick * 0.6f, ry + Dp(18.0f)), Dp(6.0f));
                Skeleton(dl, ImVec2(cx[3] - Dp(70.0f), ry), ImVec2(cx[3] - Dp(14.0f), ry + Dp(18.0f)), Dp(6.0f));
                Skeleton(dl, ImVec2(cx[6], ry), ImVec2(cx[6] + colGpu * 0.7f, ry + Dp(18.0f)), Dp(6.0f));
            }
        } else if (!leaderboard_->ok) {
            const float my = listTop + Dp(60.0f);
            Icon(dl, icon::Warning, 30.0f, ImVec2((a.x + b.x) * 0.5f, my), p.warn);
            const char* t1 = "리더보드를 불러오지 못했습니다";
            const ImVec2 s1 = TextSize(Font::Semibold, size::Body, t1);
            Text(dl, Font::Semibold, size::Body, ImVec2((a.x + b.x - s1.x) * 0.5f, my + Dp(26.0f)), p.ink, t1);
            const ImVec2 s2 = TextSize(Font::Regular, size::Caption, leaderboard_->error.c_str());
            Text(dl, Font::Regular, size::Caption, ImVec2((a.x + b.x - std::min(s2.x, tw)) * 0.5f, my + Dp(52.0f)), p.ink3,
                 leaderboard_->error.c_str());
            ImGui::SetCursorScreenPos(ImVec2((a.x + b.x) * 0.5f - Dp(60.0f), my + Dp(84.0f)));
            if (Button("##retry", "다시 시도", icon::Refresh, ButtonKind::Secondary, ImVec2(120.0f, 38.0f)))
                RefreshLeaderboard();
        } else if (leaderboard_->top.empty()) {
            const char* t1 = "아직 기록이 없습니다";
            const ImVec2 s1 = TextSize(Font::Semibold, size::Body, t1);
            Text(dl, Font::Semibold, size::Body, ImVec2((a.x + b.x - s1.x) * 0.5f, listTop + Dp(60.0f)), p.ink2, t1);
        } else {
            ImGui::SetCursorScreenPos(ImVec2(tx, listTop));
            ImGui::BeginChild("##lbrows", ImVec2(tw, listBottom - listTop), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            ImDrawList* rl = ImGui::GetWindowDrawList();
            const float rowH = Dp(48.0f);
            const ImVec2 o = ImGui::GetCursorScreenPos();
            for (size_t r = 0; r < leaderboard_->top.size(); ++r) {
                const LeaderboardEntry& e = leaderboard_->top[r];
                const float ry = o.y + r * rowH;
                const float dx = o.x - tx;
                ImGui::SetCursorScreenPos(ImVec2(o.x, ry));
                ImGui::PushID((int)r);
                ImGui::InvisibleButton("##row", ImVec2(tw - Dp(12.0f), rowH));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hovered) rl->AddRectFilled(ImVec2(o.x, ry + Dp(2.0f)), ImVec2(o.x + tw - Dp(12.0f), ry + rowH - Dp(2.0f)),
                                               WithAlpha(p.sunken, 0.9f), Dp(10.0f));
                const float ty = ry + (rowH - Dp(size::Body) * 1.2f) * 0.5f;
                const std::string rank = std::to_string(e.rank);
                if (e.rank >= 1 && e.rank <= 3) {
                    const ImVec2 c(cx[0] + dx + Dp(18.0f), ry + rowH * 0.5f);
                    rl->AddCircleFilled(c, Dp(13.0f), e.rank == 1 ? p.accent : WithAlpha(p.accent, e.rank == 2 ? 0.45f : 0.22f), 24);
                    const ImVec2 rs = TextSize(Font::Bold, size::Small, rank.c_str());
                    Text(rl, Font::Bold, size::Small, ImVec2(c.x - rs.x * 0.5f, c.y - rs.y * 0.5f), p.onAccent, rank.c_str());
                } else {
                    Text(rl, Font::Semibold, size::Body, ImVec2(cx[0] + dx + Dp(12.0f), ty), p.ink3, rank.c_str());
                }
                TextEllipsis(rl, Font::Semibold, size::Body, ImVec2(cx[1] + dx, ty), cx[2] + dx - Dp(12.0f), p.ink, e.nickname.c_str());
                const std::string score = Thousands((uint64_t)std::max(0, e.score));
                const ImVec2 ss = TextSize(Font::Bold, size::Body, score.c_str());
                Text(rl, Font::Bold, size::Body, ImVec2(cx[3] + dx - Dp(14.0f) - ss.x, ty), p.ink, score.c_str());
                Badge(rl, ImVec2(cx[3] + dx + Dp(4.0f), ry + rowH * 0.5f - Dp(11.0f)), e.tier.c_str(), WithAlpha(p.accent, 0.16f),
                      p.accentInk);
                char buf[32];
                const bool giRow = IsGi(benchCategory_);
                std::snprintf(buf, sizeof(buf), giRow ? "%.2f" : "%.1f", e.avgFps);
                ImVec2 bs = TextSize(Font::Regular, size::Body, buf);
                Text(rl, Font::Regular, size::Body, ImVec2(cx[5] + dx - Dp(14.0f) - bs.x, ty), p.ink2, buf);
                if (giRow)
                    std::snprintf(buf, sizeof(buf), "%s", RenderTime(e.frametimeStd).c_str());
                else
                    std::snprintf(buf, sizeof(buf), "%.1f", e.low1Fps);
                bs = TextSize(Font::Regular, size::Body, buf);
                Text(rl, Font::Regular, size::Body, ImVec2(cx[6] + dx - Dp(14.0f) - bs.x, ty), p.ink2, buf);
                TextEllipsis(rl, Font::Regular, size::Small, ImVec2(cx[6] + dx, ty + Dp(1.0f)), cx[7] + dx - Dp(12.0f), p.ink2,
                             e.gpu.c_str());
                Text(rl, Font::Regular, size::Small, ImVec2(cx[7] + dx, ty + Dp(1.0f)), p.ink3, e.date.substr(0, 10).c_str());
            }
            ImGui::SetCursorScreenPos(o);
            ImGui::Dummy(ImVec2(tw - Dp(12.0f), leaderboard_->top.size() * rowH));
            ImGui::EndChild();
        }
        if (leaderboard_ && leaderboard_->ok) {
            const std::string total = "전체 " + Thousands((uint64_t)std::max(0, leaderboard_->totalCount)) + "건";
            Text(dl, Font::Regular, size::Caption, ImVec2(tx, b.y - ip - Dp(14.0f)), p.ink3, total.c_str());
        }
    }
    EndScreen();
}


// ---------------------------------------------------------------------------
// Render benchmark (GI): scene card and progress overlay
// ---------------------------------------------------------------------------

void App::DrawRenderBenchPreview(float x0, float y0, float x1, float y1, float rounding) {
    using namespace ui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = P();
    const ImVec2 a(x0, y0), b(x1, y1);
    if (renderBenchImage_ && renderBenchImageW_ && renderBenchImageH_) {
        // the last finished render, cover-fit
        const float aspect = (x1 - x0) / (y1 - y0), src = (float)renderBenchImageW_ / (float)renderBenchImageH_;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (aspect > src) {
            const float v = src / aspect;
            uv0.y = (1 - v) * 0.5f;
            uv1.y = 1 - uv0.y;
        } else {
            const float u = aspect / src;
            uv0.x = (1 - u) * 0.5f;
            uv1.x = 1 - uv0.x;
        }
        dl->AddImageRounded(ImTextureRef(renderBenchImage_), a, b, uv0, uv1, IM_COL32_WHITE, rounding);
        dl->AddRect(a, b, WithAlpha(p.ink, 0.06f), rounding);
        return;
    }
    // dark studio: backdrop, a pool of light on the floor, the three performers behind a glass cube
    const float w = x1 - x0, h = y1 - y0;
    const float hy = y0 + h * 0.62f;
    dl->AddRectFilled(a, b, IM_COL32(18, 21, 30, 255), rounding);
    dl->PushClipRect(a, b, true);
    dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, hy), IM_COL32(16, 18, 27, 255), IM_COL32(16, 18, 27, 255),
                                IM_COL32(34, 38, 52, 255), IM_COL32(34, 38, 52, 255));
    dl->AddRectFilledMultiColor(ImVec2(x0, hy), ImVec2(x1, y1), IM_COL32(52, 56, 70, 255), IM_COL32(52, 56, 70, 255),
                                IM_COL32(26, 29, 40, 255), IM_COL32(26, 29, 40, 255));
    const ImVec2 pool((x0 + x1) * 0.5f, hy + h * 0.16f);
    for (int i = 6; i >= 1; --i)
        dl->AddEllipseFilled(pool, ImVec2(w * 0.07f * (float)i, h * 0.035f * (float)i), IM_COL32(150, 170, 190, 9));
    // rim light glows (teal left, pink right)
    dl->AddCircleFilled(ImVec2(x0 + w * 0.12f, y0 + h * 0.18f), h * 0.45f, IM_COL32(60, 200, 190, 14), 40);
    dl->AddCircleFilled(ImVec2(x1 - w * 0.12f, y0 + h * 0.22f), h * 0.42f, IM_COL32(240, 100, 170, 14), 40);
    int chars[kRenderBenchPerformerCount], songs[kRenderBenchPerformerCount];
    FindRenderBenchAssets(chars, songs);
    const float xs[3] = {0.5f, 0.28f, 0.72f}, hs[3] = {0.98f, 0.86f, 0.86f};
    for (int k = 2; k >= 0; --k) {   // sides first, the centre performer on top
        if (chars[k] < 0) continue;
        const uint64_t tex = CharacterThumb(chars[k]);
        if (!tex) continue;
        const float th = h * hs[k], tw = th * 0.75f, cx = x0 + w * xs[k];
        const float by = y1 - h * (k == 0 ? 0.04f : 0.10f);
        dl->AddImage(ImTextureRef(tex), ImVec2(cx - tw * 0.5f, by - th), ImVec2(cx + tw * 0.5f, by),
                     ImVec2(0, 0), ImVec2(1, 1), k == 0 ? IM_COL32_WHITE : IM_COL32(215, 220, 230, 255));
    }
    // the glass cube in front (a rotated cube: two visible faces and the top)
    {
        const float s = h * 0.30f, cx = (x0 + x1) * 0.5f + w * 0.02f, base = y1 - h * 0.02f;
        const ImVec2 f0(cx - s * 0.62f, base - s * 0.08f), f1(cx + s * 0.08f, base), f2(cx + s * 0.62f, base - s * 0.12f);
        const ImVec2 up(0, -s * 0.95f), back(s * 0.04f, -s * 0.14f);
        const ImVec2 t0(f0.x + up.x, f0.y + up.y), t1(f1.x + up.x, f1.y + up.y), t2(f2.x + up.x, f2.y + up.y);
        const ImVec2 t3(t0.x + (t2.x - t1.x) + back.x, t0.y + (t2.y - t1.y) + back.y);
        dl->AddQuadFilled(f0, f1, t1, t0, IM_COL32(170, 235, 230, 34));
        dl->AddQuadFilled(f1, f2, t2, t1, IM_COL32(170, 235, 230, 22));
        dl->AddQuadFilled(t0, t1, t2, t3, IM_COL32(220, 250, 248, 40));
        const ImU32 edge = IM_COL32(225, 250, 250, 150);
        dl->AddLine(f0, f1, edge, Dp(1.2f));
        dl->AddLine(f1, f2, edge, Dp(1.2f));
        dl->AddLine(f0, t0, edge, Dp(1.2f));
        dl->AddLine(f1, t1, IM_COL32(255, 255, 255, 210), Dp(1.6f));
        dl->AddLine(f2, t2, edge, Dp(1.2f));
        dl->AddLine(t0, t1, edge, Dp(1.2f));
        dl->AddLine(t1, t2, edge, Dp(1.2f));
        dl->AddLine(t2, t3, WithAlpha(edge, 0.4f), Dp(1.0f));
        dl->AddLine(t3, t0, WithAlpha(edge, 0.4f), Dp(1.0f));
    }
    dl->PopClipRect();
    dl->AddRect(a, b, WithAlpha(p.ink, 0.06f), rounding);
}

void App::DrawRenderBenchOverlay() {
    using namespace ui;
    ImGuiIO& io = ImGui::GetIO();
    const OfflineProgress& pr = renderer_.OfflineStatus();
    const Palette& p = P();
    const float rect0[4] = {0, 0, 0, 0};

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##renderbench", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ds = io.DisplaySize;

    // ---- bottom panel: title, live timer, phase, progress
    const float w = std::min(ds.x - Dp(40.0f), Dp(720.0f)), h = Dp(132.0f);
    const ImVec2 a((ds.x - w) * 0.5f, ds.y - Dp(24.0f) - h), b(a.x + w, a.y + h);
    FrostedPanel(dl, a, b, Dp(20.0f), (ImTextureID)0, rect0);
    const float x0 = a.x + Dp(20.0f), x1 = b.x - Dp(20.0f);
    Icon(dl, icon::Image, Dp(20.0f), ImVec2(x0 + Dp(10.0f), a.y + Dp(28.0f)), p.accent);
    Text(dl, Font::Semibold, size::Title, ImVec2(x0 + Dp(30.0f), a.y + Dp(18.0f)), p.ink, "GI 렌더 벤치마크");
    {
        const char* label = "취소";
        const float cw = Dp(32.0f + 26.0f) + TextSize(Font::Semibold, size::Body, label).x;
        ImGui::SetCursorScreenPos(ImVec2(x1 - cw, a.y + Dp(14.0f)));
        if (Button("##rbcancel", label, icon::X, ButtonKind::Secondary, ImVec2(0, 34)))
            renderBench_.cancelRequested = true;
    }
    // timer, large, left of the cancel button
    {
        const std::string t = RenderTime(renderBench_.beginPending ? 0.0 : renderBench_.elapsed);
        const ImVec2 ts = TextSize(Font::Bold, size::Heading, t.c_str());
        Text(dl, Font::Bold, size::Heading, ImVec2(x1 - Dp(100.0f) - ts.x, a.y + Dp(17.0f)), p.accentInk, t.c_str());
    }
    std::string status;
    float frac = 0.0f;
    const uint32_t spp = std::max(1u, pr.maxSamples);
    if (renderBench_.beginPending || pr.phase == OfflinePhase::Idle) {
        status = "장면 준비 중…";
    } else if (pr.phase == OfflinePhase::Prepass) {
        status = "이래디언스 캐시 프리패스 (" + std::to_string(pr.prepassStep + 1) + "/" +
                 std::to_string(pr.prepassSteps) + ")";
        frac = 0.08f * (float)pr.prepassStep / (float)std::max(1u, pr.prepassSteps);
    } else if (pr.phase == OfflinePhase::Render) {
        status = "패스 트레이싱 · " + std::to_string(pr.samples) + " / " + std::to_string(spp) + " spp";
        frac = 0.08f + 0.92f * (float)pr.samples / (float)spp;
    } else {
        status = "마무리 중…";
        frac = 1.0f;
    }
    Text(dl, Font::Regular, size::Body, ImVec2(x0, a.y + Dp(58.0f)), p.ink2, status.c_str());
    ProgressBar(dl, ImVec2(x0, a.y + Dp(86.0f)), ImVec2(x1, a.y + Dp(92.0f)), std::min(frac, 0.995f));
    {
        char line[96];
        std::snprintf(line, sizeof(line), "%u×%u · %u spp · Prism", pr.width, pr.height, spp);
        const ImVec2 ts = TextSize(Font::Regular, size::Caption, line);
        Text(dl, Font::Regular, size::Caption, ImVec2((a.x + b.x - ts.x) * 0.5f, b.y - Dp(28.0f)), p.ink3, line);
    }

    // ---- top-left pill
    {
        const char* label = "벤치마크 측정 중 · 다른 작업은 점수를 낮춥니다 · Esc 취소";
        const ImVec2 ts = TextSize(Font::Semibold, size::Caption, label);
        const float pw = ts.x + Dp(28.0f), ph = Dp(30.0f);
        const ImVec2 pa(Dp(20.0f), Dp(20.0f)), pb(pa.x + pw, pa.y + ph);
        FrostedPanel(dl, pa, pb, ph * 0.5f, (ImTextureID)0, rect0);
        Text(dl, Font::Semibold, size::Caption, ImVec2(pa.x + Dp(14.0f), pa.y + (ph - ts.y) * 0.5f), p.ink2, label);
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void App::UpdateBenchRun() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    if (benchLastQpc_ != 0) {
        const double ms = (double)(now.QuadPart - benchLastQpc_) * 1000.0 / (double)freq.QuadPart;
        if (benchFrameCounter_ > kBenchWarmupFrames) benchFrameTimes_.push_back((float)ms);
    }
    benchLastQpc_ = (int64_t)now.QuadPart;
    ++benchFrameCounter_;

    playTime_ = benchFrameCounter_ * kBenchSimStep;
    const double endSec = scene_ ? (double)(scene_->endFrame / kMmdFps) : 0.0;
    if (endSec > 0) playTime_ = std::fmod(playTime_, endSec);

    const int target = options_.benchFrames > 0 ? options_.benchFrames : kBenchMeasuredFrames;
    if ((int)benchFrameTimes_.size() >= target) {
        FinishBenchmark();
        return;
    }
    if (ImGui::GetCurrentContext() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        UnloadScene();
        ApplyRenderSettings();
        screen_ = Screen::BenchLobby;
    }
}

void App::DrawBenchRunOverlay() {
    using namespace ui;
    const Palette& p = P();
    const int target = options_.benchFrames > 0 ? options_.benchFrames : kBenchMeasuredFrames;
    const int measured = (int)benchFrameTimes_.size();
    const bool warming = benchFrameCounter_ <= kBenchWarmupFrames;
    float avgFps = 0;
    if (!benchFrameTimes_.empty()) {
        double sum = 0;
        for (float f : benchFrameTimes_) sum += f;
        avgFps = (float)(1000.0 / (sum / (double)benchFrameTimes_.size()));
    }
    float rect[4];
    renderer_.PresentRect(rect[0], rect[1], rect[2], rect[3]);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##benchrun", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = Dp(340.0f), h = Dp(96.0f);
    const ImVec2 a(Dp(20.0f), Dp(20.0f)), b(a.x + w, a.y + h);
    FrostedPanel(dl, a, b, Dp(16.0f), (ImTextureID)renderer_.UiBackdropTexture(), rect);
    const std::string title = std::string(warming ? "워밍업" : "측정 중") + "  ·  " +
                              CategoryTag(benchCategory_);
    Text(dl, Font::Semibold, size::Body, ImVec2(a.x + Dp(18.0f), a.y + Dp(14.0f)), p.ink, title.c_str());
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.0f FPS", avgFps);
    const ImVec2 fs = TextSize(Font::Bold, size::Body, fps);
    Text(dl, Font::Bold, size::Body, ImVec2(b.x - Dp(18.0f) - fs.x, a.y + Dp(14.0f)), p.accentInk, fps);
    ProgressBar(dl, ImVec2(a.x + Dp(18.0f), a.y + Dp(46.0f)), ImVec2(b.x - Dp(18.0f), a.y + Dp(52.0f)),
                (float)measured / (float)std::max(target, 1));
    const std::string frames = Thousands((uint64_t)measured) + " / " + Thousands((uint64_t)target) + " 프레임";
    Text(dl, Font::Regular, size::Caption, ImVec2(a.x + Dp(18.0f), a.y + Dp(64.0f)), p.ink2, frames.c_str());
    const ImVec2 es = TextSize(Font::Regular, size::Caption, "Esc 취소");
    Text(dl, Font::Regular, size::Caption, ImVec2(b.x - Dp(18.0f) - es.x, a.y + Dp(64.0f)), p.ink3, "Esc 취소");
    ImGui::End();
}


void App::FinishBenchmark() {
    double sum = 0;
    for (float f : benchFrameTimes_) sum += f;
    const double durationSec = sum / 1000.0;
    benchResult_ = ComputeBenchmarkResult(benchFrameTimes_, durationSec, kBenchCategories[benchCategory_]);
    LOG_INFO("BENCHMARK %s score=%d tier=%s avg=%.1f low1=%.1f low01=%.1f std=%.2f frames=%d official=%d",
             benchResult_.category.c_str(), benchResult_.score, benchResult_.tier.c_str(),
             benchResult_.avgFps, benchResult_.low1Fps, benchResult_.low01Fps, benchResult_.frametimeStdMs,
             benchResult_.totalFrames, benchOfficial_ ? 1 : 0);
    UnloadScene();
    ApplyRenderSettings();
    submitResult_.reset();
    screen_ = Screen::BenchResult;
    if (options_.quitAfterFrames > 0) {
        if (options_.capturePath.empty()) {
            running_ = false;
        } else {  // automation: show the result screen for a moment, then capture and quit
            options_.startScreen = "result";
            framesInScene_ = std::max(0, options_.quitAfterFrames - 30);
        }
    }
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

void App::DrawBenchResult() {
    using namespace ui;
    bool refresh = false;
    PollNetwork(leaderboardFuture_, leaderboard_, submitFuture_, submitResult_, refresh);
    if (refresh) RefreshLeaderboard();

    BeginScreen("##benchresult");
    DrawAppBar(1);
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float maxW = Dp(1080.0f);
    const float W = std::min(ds.x - Dp(56.0f), maxW);
    const float x0 = (ds.x - W) * 0.5f, top = Dp(64.0f) + Dp(36.0f);
    const float sideW = Dp(340.0f), gap = Dp(24.0f);
    const float mainW = W - sideW - gap;

    // ---- score card
    const bool gi = IsGi(benchCategory_);
    const ImVec2 a(x0, top), b(x0 + mainW, gi ? ds.y - Dp(28.0f) : std::min(ds.y - Dp(28.0f), top + Dp(520.0f)));
    Panel(dl, a, b, Dp(20.0f), 1.0f);
    const float ip = Dp(32.0f);
    const std::string head = std::string("벤치마크 결과  ·  ") + CategoryTag(benchCategory_) +
                             (benchOfficial_ ? "  ·  공식 프리셋" : "  ·  비공식 실행");
    Text(dl, Font::Semibold, size::Small, ImVec2(a.x + ip, a.y + ip), p.ink2, head.c_str());
    const std::string score = Thousands((uint64_t)std::max(0, benchResult_.score));
    const ImVec2 ss = TextSize(Font::Bold, size::Display, score.c_str());
    Text(dl, Font::Bold, size::Display, ImVec2(a.x + ip - Dp(3.0f), a.y + ip + Dp(22.0f)), p.ink, score.c_str());
    // tier: letter in the accent disc + its title
    {
        const float d = Dp(56.0f);
        const ImVec2 c(a.x + ip + ss.x + Dp(28.0f) + d * 0.5f, a.y + ip + Dp(22.0f) + ss.y * 0.5f);
        dl->AddCircleFilled(c, d * 0.5f, p.accent, 40);
        const ImVec2 ts = TextSize(Font::Bold, size::Heading, benchResult_.tier.c_str());
        Text(dl, Font::Bold, size::Heading, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), p.onAccent, benchResult_.tier.c_str());
        Text(dl, Font::Semibold, size::Title, ImVec2(c.x + d * 0.5f + Dp(14.0f), c.y - Dp(12.0f)), p.ink,
             benchResult_.tierTitle.c_str());
    }
    if (gi) {
        // one row of tiles, then the rendered image
        char v[4][40];
        std::snprintf(v[0], 40, "%s", RenderTime(benchResult_.durationSec).c_str());
        std::snprintf(v[1], 40, "%.2f M/s", benchResult_.avgFps);
        std::snprintf(v[2], 40, "%d spp", benchResult_.totalFrames);
        std::snprintf(v[3], 40, "%ux%u", benchResult_.width, benchResult_.height);
        const char* k[4] = {"렌더 시간", "샘플 처리량", "픽셀당 샘플", "해상도"};
        const float gy = a.y + ip + Dp(22.0f) + ss.y + Dp(26.0f);
        const float tw = (mainW - ip * 2.0f - Dp(12.0f) * 3.0f) / 4.0f, th = Dp(72.0f);
        for (int i = 0; i < 4; ++i) {
            const ImVec2 ta(a.x + ip + i * (tw + Dp(12.0f)), gy);
            dl->AddRectFilled(ta, ImVec2(ta.x + tw, ta.y + th), WithAlpha(p.sunken, 0.75f), Dp(12.0f));
            Text(dl, Font::Regular, size::Caption, ImVec2(ta.x + Dp(14.0f), ta.y + Dp(12.0f)), p.ink3, k[i]);
            Text(dl, Font::Bold, size::Title + 2.0f, ImVec2(ta.x + Dp(14.0f), ta.y + Dp(32.0f)), p.ink, v[i]);
        }
        const float sysY = b.y - ip - Dp(18.0f);
        const float iy0 = gy + th + Dp(16.0f), iy1 = sysY - Dp(14.0f);
        if (renderBenchImage_ && iy1 - iy0 > Dp(40.0f)) {
            const float aw = mainW - ip * 2.0f, ah = iy1 - iy0;
            const float src = (float)renderBenchImageW_ / (float)std::max(1u, renderBenchImageH_);
            float iw = aw, ih = aw / src;
            if (ih > ah) {
                ih = ah;
                iw = ah * src;
            }
            const ImVec2 ia(a.x + ip + (aw - iw) * 0.5f, iy0), ib(ia.x + iw, ia.y + ih);
            dl->AddImageRounded(ImTextureRef(renderBenchImage_), ia, ib, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                                Dp(12.0f));
            dl->AddRect(ia, ib, WithAlpha(p.ink, 0.08f), Dp(12.0f));
        }
        const std::string sys = ctx_.Caps().adapterName + "  ·  " + OsVersionString() + "  ·  오프라인 GI (이래디언스 캐시 + 패스 트레이싱)";
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(a.x + ip, sysY), b.x - ip, p.ink2, sys.c_str());
    } else {
    // stats grid (4 x 2 tiles)
        char v[8][40];
        std::snprintf(v[0], 40, "%.1f", benchResult_.avgFps);
        std::snprintf(v[1], 40, "%.1f", benchResult_.low1Fps);
        std::snprintf(v[2], 40, "%.1f", benchResult_.low01Fps);
        std::snprintf(v[3], 40, "%d%%", benchResult_.stabilityPct);
        std::snprintf(v[4], 40, "%.2f ms", benchResult_.frametimeMeanMs);
        std::snprintf(v[5], 40, "%.2f ms", benchResult_.frametimeStdMs);
        std::snprintf(v[6], 40, "%ux%u", benchResult_.width, benchResult_.height);
        std::snprintf(v[7], 40, "%.1f s", benchResult_.durationSec);
        const char* k[8] = {"평균 FPS", "1% Low", "0.1% Low", "안정성", "프레임타임 평균", "프레임타임 편차", "해상도", "측정 시간"};
        const float gy = a.y + ip + Dp(22.0f) + ss.y + Dp(30.0f);
        const float tw = (mainW - ip * 2.0f - Dp(12.0f) * 3.0f) / 4.0f, th = Dp(76.0f);
        for (int i = 0; i < 8; ++i) {
            const ImVec2 ta(a.x + ip + (i % 4) * (tw + Dp(12.0f)), gy + (i / 4) * (th + Dp(12.0f)));
            dl->AddRectFilled(ta, ImVec2(ta.x + tw, ta.y + th), WithAlpha(p.sunken, 0.75f), Dp(12.0f));
            Text(dl, Font::Regular, size::Caption, ImVec2(ta.x + Dp(14.0f), ta.y + Dp(12.0f)), p.ink3, k[i]);
            Text(dl, Font::Bold, size::Title + 2.0f, ImVec2(ta.x + Dp(14.0f), ta.y + Dp(34.0f)), p.ink, v[i]);
        }
        const float sy = gy + th * 2.0f + Dp(12.0f) + Dp(22.0f);
        const std::string sys = ctx_.Caps().adapterName + "  ·  " + OsVersionString() + "  ·  " +
                                Thousands((uint64_t)benchResult_.totalFrames) + " 프레임";
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(a.x + ip, sy), b.x - ip, p.ink2, sys.c_str());
    }

    // ---- side: submit + actions
    {
        const ImVec2 sa(x0 + mainW + gap, top), sb(x0 + W, b.y);
        Panel(dl, sa, sb, Dp(20.0f), 1.0f);
        const float sp = Dp(24.0f);
        const float innerW = sb.x - sa.x - sp * 2.0f;
        Text(dl, Font::Bold, size::Title + 1.0f, ImVec2(sa.x + sp, sa.y + sp), p.ink, "기록 남기기");
        ImGui::SetCursorScreenPos(ImVec2(sa.x + sp, sa.y + sp + Dp(42.0f)));
        const bool submitPending =
            submitFuture_.valid() && submitFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        const bool submitDone = submitResult_ && submitResult_->ok;
        if (benchOfficial_) {
            TextField("##nick", "닉네임", nicknameEdit_, sizeof(nicknameEdit_), innerW / Dpi(), "리더보드에 표시될 이름");
            Gap(12.0f);
            const bool nicknameEmpty = nicknameEdit_[0] == '\0';
            ImGui::SetCursorScreenPos(ImVec2(sa.x + sp, ImGui::GetCursorScreenPos().y));
            ImGui::BeginDisabled(submitPending || submitDone || nicknameEmpty);
            if (Button("##submit", submitDone ? "제출 완료" : (submitPending ? "제출 중" : "리더보드에 제출"),
                       submitDone ? icon::Check : icon::Trophy, ButtonKind::Primary, ImVec2(innerW / Dpi(), 46.0f))) {
                settings_.nickname = nicknameEdit_;
                settings_.Save(settingsPath_);
                const std::string url = settings_.leaderboardUrl;
                SubmitRequest req;
                req.category = benchResult_.category;
                req.nickname = settings_.nickname;
                req.tier = benchResult_.tier;
                char res[32];
                std::snprintf(res, sizeof(res), "%ux%u", benchResult_.width, benchResult_.height);
                req.resolution = res;
                req.gpu = ctx_.Caps().adapterName;
                req.os = OsVersionString();
                req.score = benchResult_.score;
                req.avgFps = benchResult_.avgFps;
                req.low1Fps = benchResult_.low1Fps;
                req.frametimeStd = benchResult_.frametimeStdMs;
                if (gi) {  // GI render: avgFps = million samples per second, frametimeStd = render seconds
                    req.low1Fps = 0.0f;
                    req.frametimeStd = benchResult_.durationSec;
                }
                submitFuture_ = std::async(std::launch::async, [url, req] {
                    LeaderboardClient c(url);
                    return c.Submit(req);
                });
            }
            ImGui::EndDisabled();
            const float my = ImGui::GetCursorScreenPos().y + Dp(10.0f);
            if (submitDone) {
                const std::string msg = std::to_string(submitResult_->rank) + "위  ·  전체 " +
                                        Thousands((uint64_t)std::max(0, submitResult_->totalCount)) + "건";
                Text(dl, Font::Semibold, size::Small, ImVec2(sa.x + sp, my), p.accentInk, msg.c_str());
            } else if (submitResult_ && !submitResult_->ok && !submitResult_->error.empty()) {
                TextEllipsis(dl, Font::Regular, size::Small, ImVec2(sa.x + sp, my), sb.x - sp, p.danger,
                             submitResult_->error.c_str());
            } else if (nicknameEmpty) {
                Text(dl, Font::Regular, size::Caption, ImVec2(sa.x + sp, my), p.ink3, "닉네임을 입력하면 제출할 수 있어요");
            }
        } else {
            PushFont(Font::Regular, size::Small);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
            ImGui::PushTextWrapPos(sb.x - sp);
            ImGui::TextUnformatted(gi ? "기본 설정(4K · 4096 spp)으로 렌더한 결과만 리더보드에 제출할 수 있습니다."
                                      : "공식 프리셋 에셋(Project DIVA 미쿠, theater, World is Mine)으로 측정한 결과만 "
                                        "리더보드에 제출할 수 있습니다.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            PopFont();
        }
        if (gi && !renderBenchSaved_.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(sa.x + sp, sb.y - sp - Dp(42.0f) * 3.0f - Dp(20.0f)));
            if (Button("##openimg", "렌더 이미지 보기", icon::FolderOpen, ButtonKind::Ghost, ImVec2(innerW / Dpi(), 42.0f))) {
                const std::wstring arg = L"/select,\"" + renderBenchSaved_.wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(sa.x + sp, sb.y - sp - Dp(42.0f) * 2.0f - Dp(10.0f)));
        if (Button("##again", "다시 측정", icon::ArrowCcw, ButtonKind::Secondary, ImVec2(innerW / Dpi(), 42.0f))) {
            if (gi)
                StartRenderBenchLoad();
            else
                StartBenchmarkLoad();
        }
        ImGui::SetCursorScreenPos(ImVec2(sa.x + sp, sb.y - sp - Dp(42.0f)));
        if (Button("##lobby", "벤치마크 로비", icon::ArrowLeft, ButtonKind::Ghost, ImVec2(innerW / Dpi(), 42.0f))) {
            screen_ = Screen::BenchLobby;
            RefreshLeaderboard();
        }
    }
    EndScreen();
}

} // namespace mmdx

