#include "app/App.h"

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"

namespace mmdx {

namespace {

constexpr ImGuiWindowFlags kFullScreenFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoResize |
                                              ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoBringToFrontOnFocus;

void BeginFullScreen(const char* title) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(title, nullptr, kFullScreenFlags);
}

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

namespace {

void PollFutures(App& app) = delete;

} // namespace

// ---------------------------------------------------------------------------
// Lobby
// ---------------------------------------------------------------------------

void App::DrawBenchLobby() {
    BeginFullScreen("##benchlobby");

    ImGui::TextUnformatted("MMDX12 벤치마크");
    ImGui::Separator();

    // Poll network futures (also done in DrawBenchResult).
    if (leaderboardFuture_.valid() &&
        leaderboardFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        leaderboard_ = leaderboardFuture_.get();
    }
    if (submitFuture_.valid() &&
        submitFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        submitResult_ = submitFuture_.get();
        if (submitResult_ && submitResult_->ok) RefreshLeaderboard();
    }

    // Category radios.
    for (size_t i = 0; i < std::size(kBenchCategories); ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(kBenchCategories[i].label, benchCategory_ == (int)i)) {
            if (benchCategory_ != (int)i) {
                benchCategory_ = (int)i;
                RefreshLeaderboard();
            }
        }
    }

    // Preset resolution.
    const int presetChar = FindPresetIndexChars(library_.characters, kBenchPresetCharacter);
    const int presetStage = FindPresetIndexStages(library_.stages, kBenchPresetStage);
    const int presetSong = FindPresetIndexSongs(library_.songs, kBenchPresetSong);
    benchOfficial_ = presetChar >= 0 && presetStage >= 0 && presetSong >= 0;

    if (benchOfficial_) {
        ImGui::Text("프리셋: %s / %s / %s", library_.characters[presetChar].displayName.c_str(),
                    library_.stages[presetStage].displayName.c_str(),
                    library_.songs[presetSong].displayName.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::TextUnformatted(
            "공식 프리셋 에셋(미쿠 DIVA / theater / World is Mine)이 없어 현재 선택으로 측정합니다 — 기록 "
            "제출 불가");
        ImGui::PopStyleColor();
    }

    ImGui::TextDisabled("고정 워크로드: 60초 분량 모션을 프레임당 1/60초씩 진행, 음소거, VSync 끔, MSAA 4x, "
                        "고정 해상도");

    // Nickname.
    ImGui::SetNextItemWidth(240);
    ImGui::InputText("닉네임", nicknameEdit_, sizeof(nicknameEdit_));

    // Start button: official preset, or a current character+song selection.
    const bool hasSelection = selCharacter_ >= 0 && selSong_ >= 0;
    if (benchOfficial_) {
        if (ImGui::Button("측정 시작")) {
            settings_.Save(settingsPath_);
            StartLoad(LoadTarget::Benchmark, &library_.characters[presetChar],
                      &library_.stages[presetStage], &library_.songs[presetSong]);
        }
    } else if (hasSelection) {
        if (ImGui::Button("측정 시작")) {
            settings_.Save(settingsPath_);
            StartLoad(LoadTarget::Benchmark, &library_.characters[selCharacter_],
                      selStage_ >= 0 ? &library_.stages[selStage_] : nullptr,
                      &library_.songs[selSong_]);
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("측정 시작");
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("새로고침")) RefreshLeaderboard();
    ImGui::SameLine();
    if (ImGui::Button("뒤로")) screen_ = Screen::Select;

    ImGui::Separator();

    // Leaderboard.
    if (leaderboardFuture_.valid() &&
        leaderboardFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ImGui::TextUnformatted("불러오는 중…");
    } else if (leaderboard_ && !leaderboard_->ok) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", leaderboard_->error.c_str());
    } else if (leaderboard_ && leaderboard_->ok) {
        if (ImGui::BeginTable("lbTable", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("순위");
            ImGui::TableSetupColumn("닉네임");
            ImGui::TableSetupColumn("점수");
            ImGui::TableSetupColumn("등급");
            ImGui::TableSetupColumn("평균 FPS");
            ImGui::TableSetupColumn("1% Low");
            ImGui::TableSetupColumn("GPU");
            ImGui::TableSetupColumn("날짜");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const LeaderboardEntry& e : leaderboard_->top) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", e.rank);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.nickname.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", e.score);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.tier.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", e.avgFps);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", e.low1Fps);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.gpu.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.date.substr(0, 10).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Text("총 %d건", leaderboard_->totalCount);
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
    const BenchmarkCategory& cat = kBenchCategories[benchCategory_];
    const int target = options_.benchFrames > 0 ? options_.benchFrames : kBenchMeasuredFrames;
    const int measured = (int)benchFrameTimes_.size();
    const bool warming = benchFrameCounter_ <= kBenchWarmupFrames;

    ImGui::SetNextWindowPos(ImVec2(16, 16));
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##benchrun", nullptr, flags)) {
        ImGui::Text("벤치마크 측정 중 — %s", cat.label);
        ImGui::TextUnformatted(warming ? "워밍업" : "측정 중");
        ImGui::ProgressBar((float)measured / (float)std::max(target, 1), ImVec2(260, 0));
        float avgFps = 0;
        if (!benchFrameTimes_.empty()) {
            double sum = 0;
            for (float f : benchFrameTimes_) sum += f;
            avgFps = (float)(1000.0 / (sum / (double)benchFrameTimes_.size()));
        }
        ImGui::Text("현재 %.0f FPS", avgFps);
        ImGui::TextUnformatted("Esc: 취소");
    }
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
    if (options_.quitAfterFrames > 0) running_ = false;
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

void App::DrawBenchResult() {
    const BenchmarkCategory& cat = kBenchCategories[benchCategory_];
    BeginFullScreen("##benchresult");

    // Poll futures.
    if (leaderboardFuture_.valid() &&
        leaderboardFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        leaderboard_ = leaderboardFuture_.get();
    }
    if (submitFuture_.valid() &&
        submitFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        submitResult_ = submitFuture_.get();
        if (submitResult_ && submitResult_->ok) RefreshLeaderboard();
    }

    ImGui::SetWindowFontScale(3.0f);
    ImGui::Text("%d", benchResult_.score);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Text("%s · %s", benchResult_.tier.c_str(), benchResult_.tierTitle.c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("benchStats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        const auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        };
        const auto rowF = [&](const char* k, float v, const char* fmt) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), fmt, v);
            row(k, buf);
        };
        const auto rowI = [&](const char* k, int v) { row(k, std::to_string(v)); };

        rowF("평균 FPS", benchResult_.avgFps, "%.1f");
        rowF("1% Low", benchResult_.low1Fps, "%.1f");
        rowF("0.1% Low", benchResult_.low01Fps, "%.1f");
        rowF("최저 FPS", benchResult_.minFps, "%.1f");
        rowF("최고 FPS", benchResult_.maxFps, "%.1f");
        rowF("프레임타임 평균", benchResult_.frametimeMeanMs, "%.2f ms");
        rowF("프레임타임 표준편차", benchResult_.frametimeStdMs, "%.2f ms");
        char stabilityBuf[32];
        std::snprintf(stabilityBuf, sizeof(stabilityBuf), "%d %%", benchResult_.stabilityPct);
        row("안정성", stabilityBuf);
        rowI("프레임 수", benchResult_.totalFrames);
        rowF("측정 시간", benchResult_.durationSec, "%.1f s");
        row("해상도", std::to_string(benchResult_.width) + "x" + std::to_string(benchResult_.height));
        row("GPU", ctx_.Caps().adapterName);
        row("OS", OsVersionString());
        ImGui::EndTable();
    }

    ImGui::Spacing();

    if (benchOfficial_) {
        const bool submitPending =
            submitFuture_.valid() &&
            submitFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        const bool submitDone = submitResult_ && submitResult_->ok;
        const bool nicknameEmpty = nicknameEdit_[0] == '\0';

        ImGui::SetNextItemWidth(240);
        ImGui::InputText("닉네임", nicknameEdit_, sizeof(nicknameEdit_));

        if (submitPending || submitDone || nicknameEmpty) ImGui::BeginDisabled();
        if (ImGui::Button("기록 제출")) {
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
            submitFuture_ = std::async(std::launch::async, [url, req] {
                LeaderboardClient c(url);
                return c.Submit(req);
            });
        }
        if (submitPending || submitDone || nicknameEmpty) ImGui::EndDisabled();

        if (submitPending) {
            ImGui::SameLine();
            ImGui::TextUnformatted("제출 중…");
        } else if (submitResult_ && submitResult_->ok) {
            ImGui::Text("제출 완료! %d위 / %d건", submitResult_->rank, submitResult_->totalCount);
        } else if (submitResult_ && !submitResult_->ok && !submitResult_->error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", submitResult_->error.c_str());
        }
    } else {
        ImGui::TextDisabled("비공식 실행이라 제출할 수 없습니다");
    }

    ImGui::Spacing();

    // Re-run with the same assets as the lobby start.
    const int presetChar = FindPresetIndexChars(library_.characters, kBenchPresetCharacter);
    const int presetStage = FindPresetIndexStages(library_.stages, kBenchPresetStage);
    const int presetSong = FindPresetIndexSongs(library_.songs, kBenchPresetSong);
    const bool official = presetChar >= 0 && presetStage >= 0 && presetSong >= 0;
    {
        const bool hasSelection = selCharacter_ >= 0 && selSong_ >= 0;
        if (official) {
            if (ImGui::Button("다시 측정")) {
                settings_.Save(settingsPath_);
                StartLoad(LoadTarget::Benchmark, &library_.characters[presetChar],
                          &library_.stages[presetStage], &library_.songs[presetSong]);
            }
        } else if (hasSelection) {
            if (ImGui::Button("다시 측정")) {
                settings_.Save(settingsPath_);
                StartLoad(LoadTarget::Benchmark, &library_.characters[selCharacter_],
                          selStage_ >= 0 ? &library_.stages[selStage_] : nullptr,
                          &library_.songs[selSong_]);
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("다시 측정");
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("로비로")) {
            screen_ = Screen::BenchLobby;
            RefreshLeaderboard();
        }
    }

    ImGui::End();
}

} // namespace mmdx
