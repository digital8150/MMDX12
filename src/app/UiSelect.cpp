// Library screens: scanning, the library (select) screen, loading.
#include "app/App.h"
#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"

#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/FileDialog.h"

namespace mmdx {

using namespace ui;

namespace {

constexpr float kAppBarH = 64.0f;
constexpr float kPad = 28.0f;
constexpr float kPanelW = 380.0f;

bool MatchesFilter(const std::string& needle, const std::string& a, const std::string& b) {
    return needle.empty() || ToLowerAscii(a).find(needle) != std::string::npos ||
           ToLowerAscii(b).find(needle) != std::string::npos;
}

// Card hit area: an invisible ImGui item so hover/click/nav work like any widget.
bool CardItem(const char* id, ImVec2 a, ImVec2 b, bool* hovered) {
    ImGui::SetCursorScreenPos(a);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    *hovered = ImGui::IsItemHovered();
    if (*hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

void CheckBadge(ImDrawList* dl, ImVec2 c, float t) {
    if (t <= 0.01f) return;
    const float r = Dp(12.0f) * (0.6f + 0.4f * t);
    dl->AddCircleFilled(ImVec2(c.x, c.y + Dp(1.0f)), r + Dp(1.5f), WithAlpha(IM_COL32(10, 40, 40, 255), 0.18f * t), 24);
    dl->AddCircleFilled(c, r, WithAlpha(P().accent, t), 24);
    Icon(dl, icon::Check, 14.0f, c, WithAlpha(P().onAccent, t));
}

} // namespace

// ---------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------

uint64_t App::CharacterThumb(int index) {
    if (index < 0 || index >= (int)library_.characters.size()) return 0;
    const CharacterAsset& a = library_.characters[(size_t)index];
    return thumbs_.Get("c:" + a.id, ThumbnailKind::Character, {a.modelPath});
}

uint64_t App::StageThumb(int index) {
    if (index < 0 || index >= (int)library_.stages.size()) return 0;
    const StageAsset& a = library_.stages[(size_t)index];
    return thumbs_.Get("s:" + a.id, ThumbnailKind::Stage, a.parts);
}

void App::DrawAppBar(int activeNav) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = P();
    const float W = ImGui::GetIO().DisplaySize.x;
    const float h = Dp(kAppBarH);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, h), p.surface);
    dl->AddLine(ImVec2(0, h - 0.5f), ImVec2(W, h - 0.5f), p.line);

    // Wordmark: the product name in bold ink, the "12" in Miku teal.
    const float x0 = Dp(kPad);
    const ImVec2 ws = TextSize(Font::Bold, 20.0f, "MMDX");
    const float ty = (h - ws.y) * 0.5f;
    Text(dl, Font::Bold, 20.0f, ImVec2(x0, ty), p.ink, "MMDX");
    Text(dl, Font::Bold, 20.0f, ImVec2(x0 + ws.x, ty), p.accentInk, "12");
    const float navX = x0 + TextSize(Font::Bold, 20.0f, "MMDX12").x + Dp(36.0f);

    // Primary navigation.
    ImGui::SetCursorScreenPos(ImVec2(navX, (h - Dp(38.0f)) * 0.5f));
    const char* nav[] = {Tr("라이브러리"), Tr("벤치마크")};
    const char* navIcons[] = {icon::Stack, icon::Gauge};
    int sel = activeNav;
    if (Segmented("##nav", nav, 2, &sel, 0.0f, 38.0f, navIcons)) {
        if (sel == 1) {
            screen_ = Screen::BenchLobby;
            RefreshLeaderboard();
        } else {
            screen_ = Screen::Select;
        }
    }

    // Library folder (right side): opens a small editor popup.
    const std::string path = PathToUtf8(library_.root.empty() ? ResolveLibraryPath() : library_.root);
    const float pillW = Dp(300.0f);
    const float right = W - Dp(kPad);
    const ImVec2 pa(right - pillW - Dp(86.0f), (h - Dp(36.0f)) * 0.5f);
    ImGui::SetCursorScreenPos(pa);
    bool hovered = false;
    const bool clicked = CardItem("##libpath", pa, ImVec2(pa.x + pillW, pa.y + Dp(36.0f)), &hovered);
    const float hv = Anim(ImGui::GetID("##libpathanim"), hovered);
    dl->AddRectFilled(pa, ImVec2(pa.x + pillW, pa.y + Dp(36.0f)), Mix(p.sunken, Mix(p.sunken, p.lineStrong, 0.5f), hv),
                      Dp(18.0f));
    Icon(dl, icon::FolderOpen, 16.0f, ImVec2(pa.x + Dp(20.0f), pa.y + Dp(18.0f)), p.ink2);
    TextEllipsis(dl, Font::Regular, size::Small, ImVec2(pa.x + Dp(36.0f), pa.y + Dp(9.0f)), pa.x + pillW - Dp(14.0f),
                 p.ink2, path.c_str());
    if (hovered) Tooltip(Tr("라이브러리 폴더 변경"));
    if (clicked) ImGui::OpenPopup("##libpopup");
    ImGui::SetCursorScreenPos(ImVec2(right - Dp(76.0f), (h - Dp(36.0f)) * 0.5f));
    if (IconButton("##lang", icon::Globe, Tr("언어"))) ImGui::OpenPopup("##langpopup");
    ImGui::SetNextWindowPos(ImVec2(right - Dp(40.0f), h + Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(Dp(200.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##langpopup")) {
        const Language langs[] = {Language::Auto, Language::Korean, Language::English, Language::Japanese,
                                Language::Chinese};
        for (const Language l : langs) {
            const std::string label = l == Language::Auto ? std::string(Tr("시스템 언어")) + "  (" +
                                                                LanguageName(DetectSystemLanguage()) + ")"
                                                          : std::string(LanguageName(l));
            if (Chip((std::string("##lang") + std::to_string((int)l)).c_str(), label.c_str(), nullptr,
                     settings_.language == (int)l, -1.0f)) {
                settings_.language = (int)l;
                SetLanguage(l);
                settings_.Save(settingsPath_);
                ImGui::CloseCurrentPopup();
            }
            Gap(4.0f);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(right - Dp(36.0f), (h - Dp(36.0f)) * 0.5f));
    if (IconButton("##rescan", icon::Refresh, Tr("라이브러리 다시 검색"))) StartScan();

    ImGui::SetNextWindowPos(ImVec2(pa.x + pillW + Dp(86.0f), h + Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(Dp(440.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(18.0f), Dp(16.0f)));
    if (ImGui::BeginPopup("##libpopup")) {
        TextField("##libedit", Tr("라이브러리 폴더"), libraryPathEdit_, sizeof(libraryPathEdit_), -1.0f);
        Gap(4.0f);
        PushFont(Font::Regular, size::Caption);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        ImGui::TextWrapped(Tr("캐릭터(PMX·VRM·glTF·FBX), 스테이지, VMD 모션과 음원이 들어 있는 폴더입니다. characters·stages·songs 폴더에 나눠 두면 확실하고, 아무렇게나 두어도 내용으로 분류합니다. 카드를 우클릭하면 종류를 바꾸거나 숨길 수 있습니다."));
        ImGui::PopStyleColor();
        PopFont();
        Gap(8.0f);
        if (Button("##libapply", Tr("적용하고 다시 검색"), icon::Refresh, ButtonKind::Primary)) {
            settings_.libraryPath = libraryPathEdit_;
            settings_.Save(settingsPath_);
            ImGui::CloseCurrentPopup();
            StartScan();
        }
        ImGui::SameLine();
        if (Button("##libopen", Tr("탐색기에서 열기"), icon::FolderOpen, ButtonKind::Secondary))
            ShellExecuteW(nullptr, L"open", Utf8ToPath(libraryPathEdit_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        int hiddenCount = 0;
        for (auto& [rel, k] : LoadLibraryOverrides().byPath) hiddenCount += k == AssetKind::Hidden;
        if (hiddenCount > 0) {
            Gap(6.0f);
            const std::string label = Tr("숨긴 항목 다시 표시") + std::string(" (") + std::to_string(hiddenCount) + ")";
            if (Button("##libunhide", label.c_str(), icon::Eye, ButtonKind::Ghost)) {
                ImGui::CloseCurrentPopup();
                ClearHiddenOverrides();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// Right-click menu on a library card: reclassify, hide, or show the files in Explorer.
void App::AssetContextMenu(const std::vector<std::filesystem::path>& files, bool character) {
    if (files.empty()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    ImGui::SetNextWindowSize(ImVec2(Dp(240.0f), 0));
    if (ImGui::BeginPopupContextItem("##assetmenu")) {
        const LibraryOverrides ov = LoadLibraryOverrides();
        bool overridden = false;
        const std::filesystem::path root = library_.root.lexically_normal();
        for (const auto& f : files) {
            std::string rel = PathToUtf8(f.lexically_normal().lexically_relative(root));
            for (char& c : rel)
                if (c == '\\') c = '/';
            overridden |= ov.byPath.count(rel) > 0;
        }
        std::optional<AssetKind> pick;
        if (character && Chip("##asstage", Tr("스테이지로 사용"), icon::Mountains, false, -1.0f)) pick = AssetKind::Stage;
        if (!character && files.size() == 1 && Chip("##aschar", Tr("캐릭터로 사용"), icon::User, false, -1.0f))
            pick = AssetKind::Character;
        Gap(4.0f);
        if (Chip("##ashide", Tr("숨기기"), icon::EyeSlash, false, -1.0f)) pick = AssetKind::Hidden;
        if (overridden) {
            Gap(4.0f);
            if (Chip("##asauto", Tr("자동 분류로 되돌리기"), icon::Refresh, false, -1.0f)) pick = AssetKind::Auto;
        }
        Gap(4.0f);
        if (Chip("##asfolder", Tr("탐색기에서 보기"), icon::FolderOpen, false, -1.0f)) {
            const std::wstring args = L"/select,\"" + files[0].lexically_normal().wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            ImGui::CloseCurrentPopup();
        }
        if (pick) {
            ImGui::CloseCurrentPopup();
            SetLibraryOverride(files, *pick);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// Stage thumbnail (or the studio backdrop) with the character portrait standing in front.
void App::DrawScenePreview(const CharacterAsset* ch, const StageAsset* st, float x0, float y0, float x1, float y1,
                           float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = P();
    const ImVec2 a(x0, y0), b(x1, y1);
    const int stageIndex = st ? (int)(st - library_.stages.data()) : -1;
    const uint64_t stageTex = st ? StageThumb(stageIndex) : 0;
    if (st && stageTex) {
        // cover-fit the 16:10 thumbnail
        const float aspect = (x1 - x0) / (y1 - y0), src = 512.0f / 320.0f;
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
        dl->AddRectFilled(a, b, IM_COL32(28, 32, 40, 255), rounding);
        dl->AddImageRounded(ImTextureRef(stageTex), a, b, uv0, uv1, IM_COL32_WHITE, rounding);
    } else if (st) {
        Skeleton(dl, a, b, rounding);
    } else {
        // Studio: the same sky-to-floor ramp the renderer uses without a stage.
        dl->AddRectFilled(a, b, IM_COL32(222, 233, 242, 255), rounding);
        const float hy = y0 + (y1 - y0) * 0.58f;
        dl->PushClipRect(a, b, true);
        dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, hy), IM_COL32(176, 205, 232, 255),
                                    IM_COL32(176, 205, 232, 255), IM_COL32(232, 239, 245, 255),
                                    IM_COL32(232, 239, 245, 255));
        dl->AddRectFilledMultiColor(ImVec2(x0, hy), ImVec2(x1, y1), IM_COL32(220, 229, 238, 255),
                                    IM_COL32(220, 229, 238, 255), IM_COL32(196, 210, 224, 255),
                                    IM_COL32(196, 210, 224, 255));
        dl->PopClipRect();
    }
    if (ch) {
        const int ci = (int)(ch - library_.characters.data());
        const uint64_t tex = CharacterThumb(ci);
        const float h = (y1 - y0) * 0.96f, w = h * 0.75f;
        const float cx = (x0 + x1) * 0.5f;
        if (tex) {
            dl->PushClipRect(a, b, true);
            dl->AddImage(ImTextureRef(tex), ImVec2(cx - w * 0.5f, y1 - h), ImVec2(cx + w * 0.5f, y1));
            dl->PopClipRect();
        }
    }
    dl->AddRect(a, b, WithAlpha(p.ink, 0.06f), rounding);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void App::DrawScanning() {
    BeginScreen("##scanning");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = P();
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    const float w = Dp(360.0f);

    const ImVec2 ws = TextSize(Font::Bold, 34.0f, "MMDX12");
    const ImVec2 wm = TextSize(Font::Bold, 34.0f, "MMDX");
    const float y = c.y - Dp(70.0f);
    Text(dl, Font::Bold, 34.0f, ImVec2(c.x - ws.x * 0.5f, y), p.ink, "MMDX");
    Text(dl, Font::Bold, 34.0f, ImVec2(c.x - ws.x * 0.5f + wm.x, y), p.accentInk, "12");

    const int visited = scanProgress_.filesVisited.load(std::memory_order_relaxed);
    const int total = scanProgress_.filesTotal.load(std::memory_order_relaxed);
    const char* label = Tr("라이브러리를 살펴보는 중");
    const ImVec2 ls = TextSize(Font::Semibold, size::Body, label);
    Text(dl, Font::Semibold, size::Body, ImVec2(c.x - ls.x * 0.5f, y + ws.y + Dp(22.0f)), p.ink2, label);
    const float by = y + ws.y + Dp(54.0f);
    if (total > 0)
        ProgressBar(dl, ImVec2(c.x - w * 0.5f, by), ImVec2(c.x + w * 0.5f, by + Dp(6.0f)), (float)visited / (float)total);
    else
        Skeleton(dl, ImVec2(c.x - w * 0.5f, by), ImVec2(c.x + w * 0.5f, by + Dp(6.0f)), Dp(3.0f));
    const std::string count = total > 0 ? Thousands((uint64_t)visited) + " / " + Thousands((uint64_t)total) + Tr(" 파일")
                                        : Thousands((uint64_t)visited) + Tr(" 파일");
    const ImVec2 cs = TextSize(Font::Regular, size::Caption, count.c_str());
    Text(dl, Font::Regular, size::Caption, ImVec2(c.x - cs.x * 0.5f, by + Dp(16.0f)), p.ink3, count.c_str());
    EndScreen();
}

// ---------------------------------------------------------------------------
// Library (select)
// ---------------------------------------------------------------------------

void App::DrawSelect() {
    BeginScreen("##select");
    DrawAppBar(0);
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = Dp(kPad), gap = Dp(24.0f);
    const float top = Dp(kAppBarH) + pad;
    const float panelX = ds.x - pad - Dp(kPanelW);
    const float leftX = pad, leftR = panelX - gap;

    // ---- header row: category tabs + search
    {
        const std::string l0 = Tr("캐릭터  ") + std::to_string(library_.characters.size());
        const std::string l1 = Tr("스테이지  ") + std::to_string(library_.stages.size());
        const std::string l2 = Tr("곡  ") + std::to_string(library_.songs.size());
        const char* labels[] = {l0.c_str(), l1.c_str(), l2.c_str()};
        const char* icons[] = {icon::Person, icon::Mountains, icon::Music};
        ImGui::SetCursorScreenPos(ImVec2(leftX, top));
        Segmented("##tabs", labels, 3, &libraryTab_, 0.0f, 40.0f, icons);
        char* filter = libraryTab_ == 0 ? filterCharacter_ : (libraryTab_ == 1 ? filterStage_ : filterSong_);
        const float sw = std::min(280.0f, (leftR - leftX) / Dpi() * 0.4f);
        ImGui::SetCursorScreenPos(ImVec2(leftR - Dp(sw), top + Dp(1.0f)));
        const char* hints[] = {Tr("캐릭터 검색"), Tr("스테이지 검색"), Tr("곡 검색")};
        SearchField("##search", filter, 128, hints[libraryTab_], sw);
    }

    // ---- content grid
    const float gridTop = top + Dp(40.0f) + Dp(20.0f);
    ImGui::SetCursorScreenPos(ImVec2(leftX - Dp(6.0f), gridTop - Dp(6.0f)));
    ImGui::BeginChild("##grid", ImVec2(leftR - leftX + Dp(12.0f), ds.y - gridTop - pad + Dp(12.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float inset = Dp(6.0f);
        const float availW = ImGui::GetContentRegionAvail().x - inset * 2.0f - Dp(10.0f);
        const ImVec2 origin(ImGui::GetCursorScreenPos().x + inset, ImGui::GetCursorScreenPos().y + inset);
        const float cgap = Dp(16.0f);
        float contentH = 0;

        if (libraryTab_ == 0) {
            const std::string needle = ToLowerAscii(filterCharacter_);
            const float minW = Dp(172.0f);
            const int cols = std::max(1, (int)((availW + cgap) / (minW + cgap)));
            const float cw = (availW - cgap * (cols - 1)) / cols;
            const float thumbH = cw * 4.0f / 3.0f, ch = thumbH + Dp(64.0f);
            int n = 0;
            for (size_t i = 0; i < library_.characters.size(); ++i) {
                const CharacterAsset& a = library_.characters[i];
                if (!MatchesFilter(needle, a.displayName, a.id)) continue;
                const int col = n % cols, row = n / cols;
                ++n;
                const ImVec2 ca(origin.x + col * (cw + cgap), origin.y + row * (ch + cgap));
                const ImVec2 cb(ca.x + cw, ca.y + ch);
                contentH = std::max(contentH, cb.y - origin.y);
                if (!ImGui::IsRectVisible(ImVec2(ca.x, ca.y - Dp(20)), ImVec2(cb.x, cb.y + Dp(20)))) continue;
                ImGui::PushID((int)i);
                bool hovered = false;
                if (CardItem("##card", ca, cb, &hovered)) {
                    selCharacter_ = (int)i;
                    settings_.lastCharacter = a.id;
                }
                AssetContextMenu({a.modelPath}, true);
                const bool sel = selCharacter_ == (int)i;
                const float hv = Anim(ImGui::GetID("##hv"), hovered);
                const float sv = Anim(ImGui::GetID("##sv"), sel, 14.0f);
                const float lift = -Dp(3.0f) * hv;
                const ImVec2 a0(ca.x, ca.y + lift), b0(cb.x, cb.y + lift);
                const float r = Dp(14.0f);
                SoftShadow(dl, a0, b0, r, Dp(10.0f + 8.0f * hv), 0.07f + 0.07f * hv, ImVec2(0, Dp(3.0f + 3.0f * hv)));
                dl->AddRectFilled(a0, b0, p.surface, r);
                const ImVec2 ta(a0.x, a0.y), tb(b0.x, a0.y + thumbH);
                dl->AddRectFilled(ta, tb, Mix(p.surface, p.accentSoft, 0.85f), r, ImDrawFlags_RoundCornersTop);
                // light floor fade behind the portrait
                dl->AddRectFilledMultiColor(ImVec2(ta.x, tb.y - thumbH * 0.35f), tb, WithAlpha(p.surface, 0.0f),
                                            WithAlpha(p.surface, 0.0f), WithAlpha(p.surface, 0.55f),
                                            WithAlpha(p.surface, 0.55f));
                if (const uint64_t tex = CharacterThumb((int)i))
                    dl->AddImageRounded(ImTextureRef(tex), ta, tb, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r,
                                        ImDrawFlags_RoundCornersTop);
                else
                    Skeleton(dl, ImVec2(ta.x + Dp(10), ta.y + Dp(10)), ImVec2(tb.x - Dp(10), tb.y - Dp(10)), Dp(10.0f));
                TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(a0.x + Dp(14.0f), tb.y + Dp(12.0f)), b0.x - Dp(12.0f),
                             p.ink, a.displayName.c_str());
                const std::string meta = (a.format != "PMX" ? a.format + "  ·  " : std::string()) + Tr("정점 ") +
                                         Thousands(a.vertexCount) + Tr("  ·  본 ") + Thousands(a.boneCount);
                TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(a0.x + Dp(14.0f), tb.y + Dp(35.0f)),
                             b0.x - Dp(12.0f), p.ink3, meta.c_str());
                if (sv > 0.01f) dl->AddRect(a0, b0, WithAlpha(p.accent, sv), r, 0, Dp(2.0f));
                else dl->AddRect(a0, b0, WithAlpha(p.ink, 0.05f), r);
                CheckBadge(dl, ImVec2(b0.x - Dp(20.0f), a0.y + Dp(20.0f)), sv);
                ImGui::PopID();
            }
            if (n == 0) {
                const char* msg = library_.characters.empty() ? Tr("라이브러리의 characters 폴더에 모델을 넣어 주세요")
                                                              : Tr("검색 결과가 없습니다");
                const ImVec2 ms = TextSize(Font::Semibold, size::Title, msg);
                Text(dl, Font::Semibold, size::Title, ImVec2(origin.x + (availW - ms.x) * 0.5f, origin.y + Dp(80.0f)),
                     p.ink2, msg);
                contentH = Dp(160.0f);
            }
        } else if (libraryTab_ == 1) {
            const std::string needle = ToLowerAscii(filterStage_);
            const float minW = Dp(250.0f);
            const int cols = std::max(1, (int)((availW + cgap) / (minW + cgap)));
            const float cw = (availW - cgap * (cols - 1)) / cols;
            const float thumbH = cw * 10.0f / 16.0f, ch = thumbH + Dp(64.0f);
            int n = 0;
            for (int i = -1; i < (int)library_.stages.size(); ++i) {
                const StageAsset* a = i >= 0 ? &library_.stages[(size_t)i] : nullptr;
                if (a && !MatchesFilter(needle, a->displayName, a->id)) continue;
                const int col = n % cols, row = n / cols;
                ++n;
                const ImVec2 ca(origin.x + col * (cw + cgap), origin.y + row * (ch + cgap));
                const ImVec2 cb(ca.x + cw, ca.y + ch);
                contentH = std::max(contentH, cb.y - origin.y);
                if (!ImGui::IsRectVisible(ImVec2(ca.x, ca.y - Dp(20)), ImVec2(cb.x, cb.y + Dp(20)))) continue;
                ImGui::PushID(i);
                bool hovered = false;
                if (CardItem("##card", ca, cb, &hovered)) {
                    selStage_ = i;
                    settings_.lastStage = a ? a->id : std::string();
                }
                if (a) AssetContextMenu(a->parts, false);
                const bool sel = selStage_ == i;
                const float hv = Anim(ImGui::GetID("##hv"), hovered);
                const float sv = Anim(ImGui::GetID("##sv"), sel, 14.0f);
                const float lift = -Dp(3.0f) * hv;
                const ImVec2 a0(ca.x, ca.y + lift), b0(cb.x, cb.y + lift);
                const float r = Dp(14.0f);
                SoftShadow(dl, a0, b0, r, Dp(10.0f + 8.0f * hv), 0.07f + 0.07f * hv, ImVec2(0, Dp(3.0f + 3.0f * hv)));
                dl->AddRectFilled(a0, b0, p.surface, r);
                const ImVec2 ta(a0.x, a0.y), tb(b0.x, a0.y + thumbH);
                if (a) {
                    if (const uint64_t tex = StageThumb(i)) {
                        dl->AddRectFilled(ta, tb, IM_COL32(28, 32, 40, 255), r, ImDrawFlags_RoundCornersTop);
                        dl->AddImageRounded(ImTextureRef(tex), ta, tb, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r,
                                            ImDrawFlags_RoundCornersTop);
                    } else {
                        Skeleton(dl, ImVec2(ta.x + Dp(10), ta.y + Dp(10)), ImVec2(tb.x - Dp(10), tb.y - Dp(10)), Dp(10.0f));
                    }
                } else {
                    dl->PushClipRect(ta, tb, true);
                    DrawScenePreview(nullptr, nullptr, ta.x, ta.y, tb.x, tb.y + r, r);
                    dl->PopClipRect();
                    Icon(dl, icon::Sun, 30.0f, ImVec2((ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f), WithAlpha(p.ink2, 0.55f));
                }
                const char* name = a ? a->displayName.c_str() : Tr("스튜디오");
                TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(a0.x + Dp(14.0f), tb.y + Dp(12.0f)), b0.x - Dp(12.0f),
                             p.ink, name);
                const std::string meta = a ? (a->format != "PMX" ? a->format + "  ·  " : std::string()) + Tr("파트 ") +
                                                 std::to_string(a->parts.size()) + Tr("  ·  정점 ") + Thousands(a->vertexCount)
                                           : std::string(Tr("스테이지 없이 밝은 바닥 위에서"));
                TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(a0.x + Dp(14.0f), tb.y + Dp(35.0f)),
                             b0.x - Dp(12.0f), p.ink3, meta.c_str());
                if (sv > 0.01f) dl->AddRect(a0, b0, WithAlpha(p.accent, sv), r, 0, Dp(2.0f));
                else dl->AddRect(a0, b0, WithAlpha(p.ink, 0.05f), r);
                CheckBadge(dl, ImVec2(b0.x - Dp(20.0f), a0.y + Dp(20.0f)), sv);
                ImGui::PopID();
            }
        } else {
            const std::string needle = ToLowerAscii(filterSong_);
            const float rowH = Dp(68.0f), rgap = Dp(8.0f);
            int n = 0;
            for (size_t i = 0; i < library_.songs.size(); ++i) {
                const SongAsset& a = library_.songs[i];
                if (!MatchesFilter(needle, a.displayName, a.id)) continue;
                const ImVec2 ra(origin.x, origin.y + n * (rowH + rgap)), rb(origin.x + availW, ra.y + rowH);
                ++n;
                contentH = std::max(contentH, rb.y - origin.y);
                if (!ImGui::IsRectVisible(ra, rb)) continue;
                ImGui::PushID((int)i);
                bool hovered = false;
                if (CardItem("##row", ra, rb, &hovered)) {
                    selSong_ = (int)i;
                    settings_.lastSong = a.id;
                }
                const bool sel = selSong_ == (int)i;
                const float hv = Anim(ImGui::GetID("##hv"), hovered);
                const float sv = Anim(ImGui::GetID("##sv"), sel, 14.0f);
                const float r = Dp(14.0f);
                SoftShadow(dl, ra, rb, r, Dp(8.0f), 0.05f + 0.05f * hv, ImVec2(0, Dp(2.0f)));
                dl->AddRectFilled(ra, rb, Mix(Mix(p.surface, p.sunken, hv * 0.35f), p.accentSoft, sv), r);
                dl->AddRect(ra, rb, Mix(WithAlpha(p.ink, 0.05f), p.accent, sv), r, 0, sv > 0.5f ? Dp(2.0f) : 1.0f);
                const ImVec2 ia(ra.x + Dp(14.0f), ra.y + Dp(14.0f)), ib(ia.x + Dp(40.0f), ia.y + Dp(40.0f));
                dl->AddRectFilled(ia, ib, Mix(p.accentSoft, p.surface, sv), Dp(10.0f));
                Icon(dl, icon::Music, 20.0f, ImVec2((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f), p.accentInk);
                const float tx = ib.x + Dp(14.0f);
                const float durX = rb.x - Dp(64.0f);
                TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(tx, ra.y + Dp(14.0f)), durX - Dp(16.0f), p.ink,
                             a.displayName.c_str());
                // feature badges
                float bx = tx;
                const float by = ra.y + Dp(38.0f);
                ImVec2 bs;
                if (!a.audioPath.empty()) {
                    Badge(dl, ImVec2(bx, by), Tr("음원"), WithAlpha(p.accent, 0.16f), p.accentInk, &bs);
                    bx += bs.x + Dp(6.0f);
                } else {
                    Badge(dl, ImVec2(bx, by), Tr("음원 없음"), p.warnSoft, p.warn, &bs);
                    bx += bs.x + Dp(6.0f);
                }
                if (!a.cameraVmd.empty()) {
                    Badge(dl, ImVec2(bx, by), Tr("카메라 모션"), WithAlpha(p.ink, 0.06f), p.ink2, &bs);
                    bx += bs.x + Dp(6.0f);
                }
                if (!a.extraVmds.empty()) Badge(dl, ImVec2(bx, by), Tr("표정"), WithAlpha(p.ink, 0.06f), p.ink2, &bs);
                const std::string dur = MinSec(a.durationSec);
                const ImVec2 dsz = TextSize(Font::Semibold, size::Body, dur.c_str());
                if (sv > 0.01f)
                    CheckBadge(dl, ImVec2(rb.x - Dp(28.0f), ra.y + rowH * 0.5f), sv);
                const float durRight = sv > 0.01f ? rb.x - Dp(52.0f) : rb.x - Dp(20.0f);
                Text(dl, Font::Semibold, size::Body, ImVec2(durRight - dsz.x, ra.y + (rowH - dsz.y) * 0.5f), p.ink2,
                     dur.c_str());
                ImGui::PopID();
            }
            if (n == 0) {
                const char* msg = library_.songs.empty() ? Tr("라이브러리 폴더에 VMD 댄스 모션을 넣어 주세요")
                                                         : Tr("검색 결과가 없습니다");
                const ImVec2 ms = TextSize(Font::Semibold, size::Title, msg);
                Text(dl, Font::Semibold, size::Title, ImVec2(origin.x + (availW - ms.x) * 0.5f, origin.y + Dp(80.0f)),
                     p.ink2, msg);
                contentH = Dp(160.0f);
            }
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(availW, contentH + inset * 2.0f));
    }
    ImGui::EndChild();

    // ---- right panel: the scene being assembled + graphics + play
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 pa(panelX, top), pb(ds.x - pad, ds.y - pad);
        Panel(dl, pa, pb, Dp(18.0f), 1.0f);
        const float ip = Dp(20.0f);
        const float innerW = pb.x - pa.x - ip * 2.0f;
        const CharacterAsset* ch = selCharacter_ >= 0 ? &library_.characters[(size_t)selCharacter_] : nullptr;
        const StageAsset* st = selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr;
        const SongAsset* song = selSong_ >= 0 ? &library_.songs[(size_t)selSong_] : nullptr;

        // preview
        const float prevH = innerW * (advancedOpen_ ? 0.34f : 10.0f / 16.0f);
        DrawScenePreview(ch, st, pa.x + ip, pa.y + ip, pb.x - ip, pa.y + ip + prevH, Dp(12.0f));
        if (!ch) {
            const char* msg = Tr("캐릭터를 골라 주세요");
            const ImVec2 ms = TextSize(Font::Semibold, size::Body, msg);
            Text(dl, Font::Semibold, size::Body, ImVec2(pa.x + ip + (innerW - ms.x) * 0.5f, pa.y + ip + prevH * 0.5f - ms.y),
                 WithAlpha(p.ink2, 0.8f), msg);
        }
        float y = pa.y + ip + prevH + Dp(18.0f);
        TextEllipsis(dl, Font::Bold, size::Heading, ImVec2(pa.x + ip, y), pb.x - ip, song ? p.ink : p.ink3,
                     song ? song->displayName.c_str() : Tr("곡을 골라 주세요"));
        y += Dp(32.0f);
        {
            std::string sub = (ch ? ch->displayName : std::string(Tr("캐릭터 미선택"))) + "  ·  " +
                              (st ? st->displayName : std::string(Tr("스튜디오")));
            if (song) sub += "  ·  " + MinSec(song->durationSec);
            TextEllipsis(dl, Font::Regular, size::Small, ImVec2(pa.x + ip, y), pb.x - ip, p.ink2, sub.c_str());
        }
        y += Dp(34.0f);
        dl->AddLine(ImVec2(pa.x + ip, y), ImVec2(pb.x - ip, y), p.line);
        y += Dp(16.0f);

        // settings column (scrolls when the detailed settings are open)
        const float playH = Dp(52.0f);
        const float studioH = Dp(40.0f + 10.0f);  // the studio row under play / video
        const float bottomBlock = playH + studioH + Dp(20.0f) + (ch && song ? 0.0f : Dp(26.0f));
        ImGui::SetCursorScreenPos(ImVec2(pa.x + ip, y));
        ImGui::BeginChild("##settings", ImVec2(innerW + Dp(8.0f), pb.y - y - bottomBlock - Dp(8.0f)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        ImGui::PushItemWidth(innerW);
        {
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            win->DC.CursorPos.x = win->Pos.x;
            const float colW = innerW;
            ImGui::BeginGroup();
            ImGui::PushClipRect(win->Pos, ImVec2(win->Pos.x + colW + Dp(4.0f), win->Pos.y + win->Size.y), true);
            SectionLabel(Tr("그래픽 품질"));
            const char* q[] = {Tr("낮음"), Tr("보통"), Tr("높음"), Tr("최고")};
            int preset = settings_.graphicsPreset;
            if (Segmented("##quality", q, 4, &preset, colW / Dpi(), 36.0f)) {
                ApplyGraphicsPreset(preset);
                ApplyRenderSettings();
                settings_.Save(settingsPath_);
            }
            Gap(18.0f);
            SectionLabel(Tr("렌더링"));
            const char* paths[] = {Tr("래스터"), Tr("레이 트레이싱"), Tr("패스 트레이싱")};
            int path = settings_.renderPath;
            if (Segmented("##renderpath", paths, 3, &path, colW / Dpi(), 36.0f)) {
                if (path != 0 && !renderer_.RayTracingSupported()) path = 0;
                settings_.renderPath = path;
                ApplyRenderSettings();
                settings_.Save(settingsPath_);
            }
            if (!renderer_.RayTracingSupported()) {
                PushFont(Font::Regular, size::Caption);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(P().ink3));
                ImGui::TextUnformatted(Tr("이 GPU는 DXR 1.1 레이 트레이싱을 지원하지 않습니다"));
                ImGui::PopStyleColor();
                PopFont();
            }
            Gap(14.0f);
            SectionLabel(Tr("업스케일러"));
            const char* ups[] = {Tr("끔"), "DLSS", "FSR", "XeSS"};
            int up = settings_.upscaler;
            if (Segmented("##upscaler", ups, 4, &up, colW / Dpi(), 36.0f)) {
                if (up != 0 && !renderer_.UpscalerAvailable((UpscalerKind)up)) up = 0;
                settings_.upscaler = up;
                ApplyRenderSettings();
                settings_.Save(settingsPath_);
            }
            {
                std::string unavail;
                for (int k = 1; k <= 3; ++k) {
                    if (!renderer_.UpscalerAvailable((UpscalerKind)k)) {
                        if (!unavail.empty()) unavail += ", ";
                        unavail += ups[k];
                    }
                }
                if (!unavail.empty()) {
                    const std::string cap = Tr("사용할 수 없음: ") + unavail;
                    PushFont(Font::Regular, size::Caption);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(P().ink3));
                    ImGui::TextUnformatted(cap.c_str());
                    ImGui::PopStyleColor();
                    PopFont();
                }
            }
            if (settings_.upscaler != 0) {
                Gap(10.0f);
                const char* qs[] = {Tr("네이티브"), Tr("품질"), Tr("균형"), Tr("성능"), Tr("울트라")};
                int q = settings_.upscalerQuality;
                if (Segmented("##upq", qs, 5, &q, colW / Dpi(), 34.0f)) {
                    settings_.upscalerQuality = q;
                    ApplyRenderSettings();
                    settings_.Save(settingsPath_);
                }
            }
            Gap(18.0f);
            SectionLabel(Tr("조명"));
            const char* lightIcons[] = {icon::Sun, icon::CircleHalf, icon::Sparkle, icon::Moon};
            const float chipW = (colW / Dpi() - 8.0f) * 0.5f;
            for (int i = 0; i < kLightingPresetCount; ++i) {
                if (i % 2) ImGui::SameLine(0, Dp(8.0f));
                ImGui::PushID(i);
                if (Chip("##light", LightingPresetName((LightingPreset)i), lightIcons[i], settings_.lighting == i, chipW)) {
                    settings_.lighting = i;
                    settings_.Save(settingsPath_);
                }
                ImGui::PopID();
            }
            Gap(14.0f);
            // detailed settings disclosure
            {
                ImGuiWindow* w = ImGui::GetCurrentWindow();
                const ImVec2 da = w->DC.CursorPos;
                bool hovered = false;
                if (CardItem("##advanced", da, ImVec2(da.x + colW, da.y + Dp(34.0f)), &hovered)) advancedOpen_ = !advancedOpen_;
                const float rot = Anim(ImGui::GetID("##advrot"), advancedOpen_, 14.0f);
                Text(w->DrawList, Font::Semibold, size::Small, ImVec2(da.x, da.y + Dp(8.0f)), hovered ? p.ink : p.ink2,
                     Tr("세부 설정"));
                Icon(w->DrawList, rot > 0.5f ? icon::CaretDown : icon::CaretRight, 14.0f,
                     ImVec2(da.x + colW - Dp(10.0f), da.y + Dp(17.0f)), p.ink2);
            }
            if (advancedOpen_) {
                bool changed = false;
                changed |= Switch("##shadows", Tr("그림자"), &settings_.shadows);
                changed |= Switch("##ssao", Tr("앰비언트 오클루전"), &settings_.ssao);
                changed |= Switch("##ssr", Tr("화면 공간 반사"), &settings_.ssr);
                changed |= Switch("##bloom", Tr("블룸"), &settings_.bloom);
                changed |= Switch("##taa", "TAA", &settings_.taa, Tr("시간 누적 안티에일리어싱"));
                changed |= Switch("##edges", Tr("외곽선"), &settings_.drawEdges);
                changed |= Switch("##motionlight", Tr("모션 조명·그림자"), &settings_.motionLighting,
                                  Tr("카메라 VMD의 조명과 셀프 섀도 키를 따릅니다"));
                changed |= Switch("##vsync", Tr("수직 동기화"), &settings_.vsync);
                Gap(6.0f);
                SectionLabel("MSAA");
                const char* m[] = {Tr("끔"), "2x", "4x", "8x"};
                int mi = settings_.msaa == 1 ? 0 : settings_.msaa == 2 ? 1 : settings_.msaa == 4 ? 2 : 3;
                if (Segmented("##msaa", m, 4, &mi, colW / Dpi(), 34.0f)) {
                    settings_.msaa = mi == 0 ? 1 : mi == 1 ? 2 : mi == 2 ? 4 : 8;
                    changed = true;
                }
                if (settings_.renderPath == 2) {
                    Gap(14.0f);
                    SectionLabel(Tr("패스 트레이싱 샘플"));
                    const char* s[] = {"1", "2", "4"};
                    int si = settings_.ptSamples == 1 ? 0 : settings_.ptSamples == 2 ? 1 : 2;
                    if (Segmented("##ptsamples", s, 3, &si, colW / Dpi(), 34.0f)) {
                        settings_.ptSamples = si == 0 ? 1 : si == 1 ? 2 : 4;
                        ApplyRenderSettings();
                        settings_.Save(settingsPath_);
                    }
                    SectionLabel(Tr("반사 횟수"));
                    const char* bn[] = {"2", "3", "4", "6"};
                    int bi = settings_.ptBounces == 2 ? 0 : settings_.ptBounces == 3 ? 1 : settings_.ptBounces == 4 ? 2 : 3;
                    if (Segmented("##ptbounces", bn, 4, &bi, colW / Dpi(), 34.0f)) {
                        settings_.ptBounces = bi == 0 ? 2 : bi == 1 ? 3 : bi == 2 ? 4 : 6;
                        ApplyRenderSettings();
                        settings_.Save(settingsPath_);
                    }
                }
                Gap(14.0f);
                changed |= SliderRow("##scale", Tr("렌더 스케일"), &settings_.renderScale, 0.5f, 2.0f, "%.2fx");
                Gap(10.0f);
                changed |= SliderRow("##exposure", Tr("노출"), &settings_.exposure, 0.5f, 2.0f, "%.2f");
                Gap(10.0f);
                if (Switch("##physics", Tr("물리 연산"), &settings_.physics, Tr("머리카락과 옷의 흔들림"))) settings_.Save(settingsPath_);
                Gap(14.0f);
                SectionLabel(Tr("효과"));
                bool fx = false;
                fx |= Switch("##dof", Tr("피사계 심도"), &settings_.dof, Tr("캐릭터에 초점을 맞추고 배경을 흐림"));
                if (settings_.dof) { Gap(6.0f); fx |= SliderRow("##dofap", Tr("조리개"), &settings_.dofAperture, 0.2f, 3.0f, "%.2f"); Gap(6.0f); }
                fx |= Switch("##vol", Tr("볼류메트릭 라이트"), &settings_.volumetric, Tr("빛줄기와 조명 산란"));
                if (settings_.volumetric) { Gap(6.0f); fx |= SliderRow("##vold", Tr("안개 밀도"), &settings_.volumetricDensity, 0.25f, 4.0f, "%.2f"); Gap(6.0f); }
                fx |= Switch("##bloomconv", Tr("컨볼루션 블룸"), &settings_.bloomConvolution, Tr("FFT 스타버스트 블룸"));
                Gap(10.0f);
                SectionLabel(Tr("컬러 LUT"));
                int lutCount = 1 + (int)luts_.size();
                for (int i = 0; i < lutCount; ++i) {
                    if (i % 2) ImGui::SameLine(0, Dp(8.0f));
                    ImGui::PushID(i);
                    const ColorLutEntry* e = i > 0 ? &luts_[(size_t)i - 1] : nullptr;
                    if (Chip("##lut", e ? Tr(e->displayName.c_str()) : Tr("없음"), icon::Image,
                             settings_.colorLut == (e ? e->id : std::string()), chipW)) {
                        settings_.colorLut = e ? e->id : std::string();
                        fx = true;
                    }
                    ImGui::PopID();
                }
                if (!settings_.colorLut.empty()) fx |= SliderRow("##lutint", Tr("LUT 강도"), &settings_.lutIntensity, 0.0f, 1.0f, "%.2f");
                if (fx) {
                    ApplyRenderSettings();
                    settings_.Save(settingsPath_);
                }
                if (changed) {
                    // touching an effect toggle leaves the named presets
                    settings_.graphicsPreset = 4;
                    ApplyRenderSettings();
                    settings_.Save(settingsPath_);
                }
            }
            ImGui::PopClipRect();
            ImGui::EndGroup();
        }
        ImGui::PopItemWidth();
        ImGui::EndChild();

        // play
        const bool canPlay = ch && song;
        float by = pb.y - ip - studioH - playH;
        if (!canPlay) {
            const char* hint = !ch ? Tr("캐릭터와 곡을 고르면 시작할 수 있어요") : Tr("곡을 고르면 시작할 수 있어요");
            const ImVec2 hs = TextSize(Font::Regular, size::Caption, hint);
            Text(dl, Font::Regular, size::Caption, ImVec2(pa.x + ip + (innerW - hs.x) * 0.5f, by - Dp(24.0f)), p.ink3, hint);
        }
        ImGui::SetCursorScreenPos(ImVec2(pa.x + ip, by));
        ImGui::BeginDisabled(!canPlay);
        const float videoW = 138.0f;  // dp
        if (Button("##play", Tr("플레이"), icon::Play, ButtonKind::Primary, ImVec2(innerW / Dpi() - videoW - 10.0f, 52.0f))) {
            settings_.Save(settingsPath_);
            StartLoad(LoadTarget::Play, ch, st, song);
        }
        ImGui::SameLine(0.0f, Dp(10.0f));
        if (Button("##video", Tr("영상 렌더"), icon::FilmStrip, ButtonKind::Secondary, ImVec2(videoW, 52.0f))) {
            settings_.Save(settingsPath_);
            videoDialogOpen_ = true;
        }
        ImGui::EndDisabled();
        // Studio: the selected scene as a starting point, or an empty project when no character is picked; the
        // menu next to it starts empty, opens a project file or a recent one.
        ImGui::SetCursorScreenPos(ImVec2(pa.x + ip, by + playH + Dp(10.0f)));
        const float menuW = 40.0f;  // dp
        const bool studioClicked = Button("##studio", ch ? Tr("스튜디오에서 편집") : Tr("새 스튜디오 프로젝트"),
                                          ch ? icon::Sliders : icon::FilePlus, ButtonKind::Ghost,
                                          ImVec2(innerW / Dpi() - menuW - 8.0f, 40.0f));
        Tooltip(ch ? Tr("고른 캐릭터·스테이지·곡으로 스튜디오를 엽니다") : Tr("아무것도 고르지 않은 빈 프로젝트로 시작합니다"));
        ImGui::SameLine(0.0f, Dp(8.0f));
        const ImVec2 menuAt = ImGui::GetCursorScreenPos();
        if (IconButton("##studiomenu", icon::DotsThree, Tr("스튜디오: 새 프로젝트, 열기, 최근 프로젝트"), false, menuW))
            ImGui::OpenPopup("##studioentry");
        if (studioClicked) {
            settings_.Save(settingsPath_);
            if (ch) StartStudioLoad(ch, st, song);
            else StartStudioEmpty();
        }
        ImGui::SetNextWindowPos(ImVec2(menuAt.x + Dp(menuW), menuAt.y - Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
        if (ImGui::BeginPopup("##studioentry")) {
            std::filesystem::path open;
            bool empty = false;
            if (MenuItem("##entrynew", Tr("빈 프로젝트로 시작"), icon::FilePlus)) empty = true;
            if (MenuItem("##entryopen", Tr("프로젝트 열기…"), icon::FolderOpen)) {
                ImGui::CloseCurrentPopup();
                open = studio::OpenFileDialog(hwnd_, {{L"MMDX12 Studio", L"*.mmdxproj"}});
            }
            if (!settings_.recentProjects.empty()) {
                Gap(8.0f);
                SectionLabel(Tr("최근 프로젝트"));
                for (size_t i = 0; i < settings_.recentProjects.size(); ++i) {
                    const std::filesystem::path rp = Utf8ToPath(settings_.recentProjects[i]);
                    ImGui::PushID((int)i);
                    if (MenuItem("##recent", PathToUtf8(rp.stem()).c_str(), icon::FolderSimple)) open = rp;
                    Tooltip(settings_.recentProjects[i].c_str());
                    ImGui::PopID();
                }
            }
            if (empty || !open.empty()) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            if (empty) {
                settings_.Save(settingsPath_);
                StartStudioEmpty();
            } else if (!open.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(open, ec)) {
                    StartStudioProjectLoad(open, false);
                } else {
                    settings_.RemoveRecentProject(PathToUtf8(open));
                    settings_.Save(settingsPath_);
                    toast_ = {Tr("프로젝트 파일이 없습니다"), PathToUtf8(open), {}, true, timeSeconds_ + 5.0};
                }
            }
        }
        ImGui::PopStyleVar();
    }

    // keyboard: Enter starts playback
    if (selCharacter_ >= 0 && selSong_ >= 0 && !videoDialogOpen_ && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        StartLoad(LoadTarget::Play, &library_.characters[(size_t)selCharacter_],
                  selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr, &library_.songs[(size_t)selSong_]);
    EndScreen();
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void App::DrawLoading() {
    BeginScreen("##loading");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = P();
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    const float w = Dp(520.0f), prevH = w * 9.0f / 16.0f;
    const float h = prevH + Dp(150.0f);
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    Panel(dl, a, b, Dp(18.0f), 1.2f);

    const bool sceneLoad = loadTarget_ == LoadTarget::Play || loadTarget_ == LoadTarget::OfflineVideo ||
                           loadTarget_ == LoadTarget::Studio;
    // a studio project shows its own name over the plain studio backdrop, not the library selection
    const bool project = loadTarget_ == LoadTarget::Studio && !studioLoadTitle_.empty();
    const CharacterAsset* ch = selCharacter_ >= 0 && sceneLoad && !project ? &library_.characters[(size_t)selCharacter_] : nullptr;
    const StageAsset* st = selStage_ >= 0 && sceneLoad && !project ? &library_.stages[(size_t)selStage_] : nullptr;
    const SongAsset* song = selSong_ >= 0 && sceneLoad && !project ? &library_.songs[(size_t)selSong_] : nullptr;
    const float ip = Dp(14.0f);
    if (loadTarget_ == LoadTarget::RenderBench)
        DrawRenderBenchPreview(a.x + ip, a.y + ip, b.x - ip, a.y + ip + prevH - ip, Dp(12.0f));
    else
        DrawScenePreview(ch, st, a.x + ip, a.y + ip, b.x - ip, a.y + ip + prevH - ip, Dp(12.0f));

    float y = a.y + prevH + Dp(16.0f);
    const char* title = loadTarget_ == LoadTarget::Benchmark     ? Tr("벤치마크 준비 중")
                        : loadTarget_ == LoadTarget::RenderBench ? Tr("GI 렌더 벤치마크 준비 중")
                        : project                                ? studioLoadTitle_.c_str()
                                                                 : (song ? song->displayName.c_str() : Tr("불러오는 중"));
    TextEllipsis(dl, Font::Bold, size::Title + 2.0f, ImVec2(a.x + Dp(24.0f), y), b.x - Dp(24.0f), p.ink, title);
    y += Dp(34.0f);
    const bool failed = !loadError_.empty() && !loadFuture_.valid();
    if (failed) {
        ImVec2 bs;
        Badge(dl, ImVec2(a.x + Dp(24.0f), y), Tr("불러오지 못했습니다"), p.dangerSoft, p.danger, &bs);
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(a.x + Dp(24.0f), y + bs.y + Dp(8.0f)), b.x - Dp(24.0f), p.ink2,
                     loadError_.c_str());
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(24.0f) - Dp(120.0f), b.y - Dp(24.0f) - Dp(40.0f)));
        if (Button("##back", Tr("돌아가기"), icon::ArrowLeft, ButtonKind::Secondary, ImVec2(120.0f, 40.0f)))
            screen_ = sceneLoad ? Screen::Select : Screen::BenchLobby;
    } else {
        const std::string status = loadProgress_.Status();
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(a.x + Dp(24.0f), y), b.x - Dp(24.0f), p.ink2,
                     status.empty() ? Tr("준비 중") : status.c_str());
        y += Dp(30.0f);
        ProgressBar(dl, ImVec2(a.x + Dp(24.0f), y), ImVec2(b.x - Dp(24.0f), y + Dp(6.0f)),
                    loadProgress_.fraction.load(std::memory_order_relaxed));
    }
    EndScreen();
}

} // namespace mmdx
