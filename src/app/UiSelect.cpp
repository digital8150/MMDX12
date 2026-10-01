#include "app/App.h"

#include <ShlObj.h>

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include <shellapi.h>

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

} // namespace

// ---------------------------------------------------------------------------
// DrawScanning
// ---------------------------------------------------------------------------

void App::DrawScanning() {
    BeginFullScreen("##scanning");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetCursorPos(ImVec2(center.x - 200, center.y - 60));

    ImGui::TextUnformatted("라이브러리 검색 중…");
    const std::string path = PathToUtf8(library_.root.empty() ? ResolveLibraryPath() : library_.root);
    ImGui::TextDisabled("%s", path.c_str());
    const int visited = scanProgress_.filesVisited.load(std::memory_order_relaxed);
    const int total = scanProgress_.filesTotal.load(std::memory_order_relaxed);
    ImGui::ProgressBar((float)visited / (float)std::max(1, total), ImVec2(-1, 0));
    ImGui::Text("%d / %d 파일", visited, total);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// DrawSelect
// ---------------------------------------------------------------------------

namespace {

struct SelectFilter {
    bool Matches(const CharacterAsset& a) const {
        return needle.empty() || ToLowerAscii(a.displayName).find(needle) != std::string::npos ||
               ToLowerAscii(a.id).find(needle) != std::string::npos;
    }
    bool Matches(const StageAsset& a) const {
        return needle.empty() || ToLowerAscii(a.displayName).find(needle) != std::string::npos ||
               ToLowerAscii(a.id).find(needle) != std::string::npos;
    }
    bool Matches(const SongAsset& a) const {
        return needle.empty() || ToLowerAscii(a.displayName).find(needle) != std::string::npos ||
               ToLowerAscii(a.id).find(needle) != std::string::npos;
    }
    std::string needle;
};

} // namespace

void App::DrawSelect() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_);
    BeginFullScreen("##select");

    // 1. Header.
    {
        ImGui::TextUnformatted("MMDX12");
        ImGui::SameLine();
        ImGui::TextDisabled("MikuMikuDance DX12 Player");
        const std::string counts = "캐릭터 " + std::to_string(library_.characters.size()) +
                                   " · 스테이지 " + std::to_string(library_.stages.size()) +
                                   " · 곡 " + std::to_string(library_.songs.size());
        const float countsWidth = ImGui::CalcTextSize(counts.c_str()).x;
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.FramePadding.x * 2.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 1.0f, rightEdge - countsWidth));
        ImGui::TextUnformatted(counts.c_str());
    }
    ImGui::Separator();

    // 2. Library row.
    {
        ImGui::SetNextItemWidth(io.DisplaySize.x * 0.4f);
        ImGui::InputText("##lib", libraryPathEdit_, sizeof(libraryPathEdit_));
        ImGui::SameLine();
        if (ImGui::Button("적용/재검색")) {
            settings_.libraryPath = libraryPathEdit_;
            settings_.Save(settingsPath_);
            StartScan();
        }
        ImGui::SameLine();
        if (ImGui::Button("폴더 열기")) {
            ShellExecuteW(nullptr, L"open", Utf8ToPath(libraryPathEdit_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (!library_.warnings.empty()) {
            if (ImGui::CollapsingHeader(("경고 (" + std::to_string(library_.warnings.size()) + ")").c_str())) {
                for (const std::string& w : library_.warnings) ImGui::TextDisabled("%s", w.c_str());
            }
        }
    }
    ImGui::Separator();

    // 3. Pick list.
    const float footerReserve = 150.0f * dpi;
    const float listHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - footerReserve);
    SelectFilter fc, fs, fg;
    fc.needle = ToLowerAscii(filterCharacter_);
    fs.needle = ToLowerAscii(filterStage_);
    fg.needle = ToLowerAscii(filterSong_);

    if (ImGui::BeginTable("pick", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("캐릭터");
        ImGui::TableSetupColumn("스테이지");
        ImGui::TableSetupColumn("곡 / 모션");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();

        for (int col = 0; col < 3; ++col) {
            ImGui::TableSetColumnIndex(col);
            char* filterBuf = col == 0 ? filterCharacter_ : (col == 1 ? filterStage_ : filterSong_);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint(col == 0 ? "##fc" : (col == 1 ? "##fs" : "##fg"), "검색", filterBuf, 128);
        }
        ImGui::TableNextRow();
        for (int col = 0; col < 3; ++col) {
            ImGui::TableSetColumnIndex(col);
            const float childHeight = listHeight - ImGui::GetFrameHeight() - style.ItemSpacing.y;
            ImGui::BeginChild(col == 0 ? "childChar" : (col == 1 ? "childStage" : "childSong"),
                              ImVec2(0, childHeight), ImGuiChildFlags_Borders);
            if (col == 0) {
                if (library_.characters.empty()) {
                    ImGui::TextDisabled("라이브러리 폴더에 PMX 모델을 넣어주세요");
                }
                for (size_t i = 0; i < library_.characters.size(); ++i) {
                    const CharacterAsset& a = library_.characters[i];
                    if (!fc.Matches(a)) continue;
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable(a.displayName.c_str(), selCharacter_ == (int)i,
                                          ImGuiSelectableFlags_None)) {
                        selCharacter_ = (int)i;
                        settings_.lastCharacter = a.id;
                    }
                    ImGui::TextDisabled("정점 %u · 본 %u · 재질 %u", a.vertexCount, a.boneCount,
                                        a.materialCount);
                    ImGui::PopID();
                }
            } else if (col == 1) {
                ImGui::PushID("nostage");
                if (ImGui::Selectable("(스테이지 없음)", selStage_ == -1, ImGuiSelectableFlags_None))
                    selStage_ = -1;
                ImGui::PopID();
                if (library_.stages.empty()) {
                    ImGui::TextDisabled("라이브러리 폴더에 스테이지를 넣어주세요");
                }
                for (size_t i = 0; i < library_.stages.size(); ++i) {
                    const StageAsset& a = library_.stages[i];
                    if (!fs.Matches(a)) continue;
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable(a.displayName.c_str(), selStage_ == (int)i,
                                          ImGuiSelectableFlags_None)) {
                        selStage_ = (int)i;
                        settings_.lastStage = a.id;
                    }
                    ImGui::TextDisabled("파트 %zu · 정점 %u", a.pmxParts.size(), a.vertexCount);
                    ImGui::PopID();
                }
            } else {
                if (library_.songs.empty()) {
                    ImGui::TextDisabled("라이브러리 폴더에 VMD 모션을 넣어주세요");
                }
                for (size_t i = 0; i < library_.songs.size(); ++i) {
                    const SongAsset& a = library_.songs[i];
                    if (!fg.Matches(a)) continue;
                    const int secs = (int)(a.durationSec + 0.5f);
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable(a.displayName.c_str(), selSong_ == (int)i,
                                          ImGuiSelectableFlags_None)) {
                        selSong_ = (int)i;
                        settings_.lastSong = a.id;
                    }
                    ImGui::TextDisabled("%d:%02d · 카메라 %s · 음악 %s", secs / 60, secs % 60,
                                        a.cameraVmd.empty() ? "X" : "O", a.audioPath.empty() ? "X" : "O");
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    // 4. Footer: graphics row.
    {
        bool changed = false;
        changed |= ImGui::Checkbox("VSync", &settings_.vsync);
        ImGui::SameLine();
        {
            const char* msaaItems[] = {"1x", "2x", "4x", "8x"};
            int msaaIdx = settings_.msaa == 1 ? 0 : settings_.msaa == 2 ? 1 : settings_.msaa == 4 ? 2 : 3;
            ImGui::SetNextItemWidth(90);
            if (ImGui::Combo("MSAA", &msaaIdx, msaaItems, 4)) {
                settings_.msaa = msaaIdx == 0 ? 1 : msaaIdx == 1 ? 2 : msaaIdx == 2 ? 4 : 8;
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        changed |= ImGui::SliderFloat("렌더 스케일", &settings_.renderScale, 0.5f, 2.0f, "%.2fx");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("외곽선", &settings_.drawEdges);
        if (changed) {
            ApplyRenderSettings();
            settings_.Save(settingsPath_);
        }
    }

    // 5. Footer: action buttons.
    {
        const bool canPlay = selCharacter_ >= 0 && selSong_ >= 0;
        const ImVec2 btnSize(0, 40.0f * dpi);

        ImGui::BeginDisabled(!canPlay);
        if (ImGui::Button("▶ 플레이", btnSize)) {
            settings_.Save(settingsPath_);
            StartLoad(LoadTarget::Play, &library_.characters[selCharacter_],
                      selStage_ >= 0 ? &library_.stages[selStage_] : nullptr,
                      &library_.songs[selSong_]);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("벤치마크", btnSize)) {
            screen_ = Screen::BenchLobby;
            RefreshLeaderboard();
        }
        ImGui::SameLine();
        if (ImGui::Button("종료", btnSize)) {
            running_ = false;
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// DrawLoading
// ---------------------------------------------------------------------------

void App::DrawLoading() {
    BeginFullScreen("##loading");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetCursorPos(ImVec2(center.x - 200, center.y - 60));
    ImGui::BeginGroup();
    ImGui::TextUnformatted("불러오는 중…");
    ImGui::TextUnformatted(loadProgress_.Status().c_str());
    ImGui::ProgressBar(loadProgress_.fraction.load(std::memory_order_relaxed), ImVec2(400, 0));

    if (!loadError_.empty() && !loadFuture_.valid()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextUnformatted(loadError_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("돌아가기")) screen_ = Screen::Select;
    }
    ImGui::EndGroup();

    ImGui::End();
}

} // namespace mmdx
