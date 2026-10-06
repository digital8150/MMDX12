// The lobby's video render dialog (opened from the select screen's "영상 렌더" button).
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/Icons.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"
#include "studio/StudioDoc.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

namespace {

// Wall time as "N분" / "N.N시간" / "N.N일".
std::string DurationKo(double seconds) {
    char buf[32];
    if (seconds >= 86400.0)
        std::snprintf(buf, sizeof(buf), Tr("%.1f일"), seconds / 86400.0);
    else if (seconds >= 3600.0)
        std::snprintf(buf, sizeof(buf), Tr("%.1f시간"), seconds / 3600.0);
    else
        std::snprintf(buf, sizeof(buf), Tr("%d분"), std::max(1, (int)std::lround(seconds / 60.0)));
    return buf;
}

}  // namespace

void App::DrawVideoRenderDialog() {
    using namespace ui;
    if (!videoDialogOpen_) return;
    // opened from the studio's render menu: renders the project (range, motion camera, project audio)
    const bool studio = screen_ == Screen::Studio && studio_ != nullptr;
    if (!studio && (selCharacter_ < 0 || selSong_ < 0 || (size_t)selSong_ >= library_.songs.size())) {
        videoDialogOpen_ = false;
        return;
    }

    VideoRenderConfig& cfg = settings_.video;
    if (!VideoRendererAvailable(cfg.Renderer())) {
        cfg.renderer = (int)VideoRenderer::Raster;
    }
    cfg.Clamp();

    ImGuiIO& io = ImGui::GetIO();
    double studioA = 0, studioB = 0;
    if (studio) StudioRenderRange(studioA, studioB);
    const double durationSec = studio ? studioB - studioA : library_.songs[(size_t)selSong_].durationSec;
    std::string subtitle;
    if (studio) {
        subtitle = studio_->projectPath.empty() ? std::string(Tr("제목 없음")) : PathToUtf8(studio_->projectPath.stem());
        subtitle += std::string(" · ") + Tr("프레임") + " " + std::to_string((int)std::lround(studioA * kMmdFps)) + "–" +
                    std::to_string(std::max(0, (int)std::lround(studioB * kMmdFps) - 1));
    } else {
        subtitle = library_.songs[(size_t)selSong_].displayName;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 24, 32, 120));
    ImGui::Begin("##videodialog", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ds = io.DisplaySize;
    const Palette& p = P();

    const float w = std::min(ds.x - Dp(40.0f), Dp(560.0f));
    const float h = std::min(ds.y - Dp(24.0f), Dp(860.0f));
    const ImVec2 a((ds.x - w) * 0.5f, (ds.y - h) * 0.5f), b(a.x + w, a.y + h);
    Panel(dl, a, b, Dp(20.0f));
    const float pad = Dp(28.0f);
    const float x0 = a.x + pad, x1 = b.x - pad;
    Text(dl, Font::Bold, size::Heading, ImVec2(x0, a.y + Dp(22.0f)), p.ink, Tr("고품질 영상 렌더링"));
    TextEllipsis(dl, Font::Regular, size::Small, ImVec2(x0, a.y + Dp(54.0f)), x1, p.ink2, subtitle.c_str());

    // Settings area (scrolls when the window is short)
    const float top = a.y + Dp(80.0f), bottomBlock = Dp(160.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0, top));
    ImGui::BeginChild("##videosettings", ImVec2(x1 - x0, b.y - top - bottomBlock), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float colW = x1 - x0 - Dp(14.0f);   // leaves room for the scroll bar
    ImGui::PushItemWidth(colW);
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        win->DC.CursorPos.x = win->Pos.x;
        ImGui::BeginGroup();
        ImGui::PushClipRect(win->Pos, ImVec2(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y), true);

        // 0. 범위 (studio with a timeline range: the range or the whole project)
        if (studio && studio_->HasRange()) {
            SectionLabel(Tr("범위"));
            const char* rangeLabels[2] = {Tr("타임라인 범위"), Tr("프로젝트 전체")};
            int whole = studioRenderWhole_ ? 1 : 0;
            if (Segmented("##vrange", rangeLabels, 2, &whole, colW / Dpi(), 36.0f)) studioRenderWhole_ = whole == 1;
            Gap(10.0f);
        }

        // 1. 렌더러
        SectionLabel(Tr("렌더러"));
        const float gapX = Dp(8.0f);
        const float cardW = (colW - gapX) * 0.5f;
        const float cardH = Dp(56.0f);
        const float cardR = Dp(12.0f);
        static const char* const kRendererIcons[kVideoRendererCount] = {
            icon::Lightning, icon::Sun, icon::Sparkle, icon::Cube
        };

        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const int i = row * 2 + col;
                const VideoRenderer r = (VideoRenderer)i;
                const bool avail = VideoRendererAvailable(r);
                const bool selected = (cfg.renderer == i);

                if (col > 0) ImGui::SameLine(0.0f, gapX);

                char idBuf[32];
                std::snprintf(idBuf, sizeof(idBuf), "##vrend_%d", i);
                const ImGuiID gid = ImGui::GetID(idBuf);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const ImRect bb(pos, ImVec2(pos.x + cardW, pos.y + cardH));

                ImGui::ItemSize(bb);
                if (!ImGui::ItemAdd(bb, gid)) continue;

                bool hovered = false, held = false;
                const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);

                if (avail) {
                    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (pressed && cfg.renderer != i) {
                        cfg.renderer = i;
                        cfg.Clamp();
                    }
                }

                ImDrawList* cdl = ImGui::GetWindowDrawList();
                const float hv = Anim(gid, hovered && avail && !selected, 12.0f);
                char selIdBuf[32];
                std::snprintf(selIdBuf, sizeof(selIdBuf), "##vrend_sel_%d", i);
                const float sel = Anim(ImGui::GetID(selIdBuf), selected, 14.0f);

                ImU32 bgCol;
                ImU32 borderCol;
                float borderWidth = 1.0f;
                ImU32 nameCol;
                ImU32 summaryCol;
                ImU32 iconCol;

                if (!avail) {
                    bgCol = p.sunken;
                    borderCol = WithAlpha(p.line, 0.7f);
                    nameCol = p.ink3;
                    summaryCol = p.ink3;
                    iconCol = p.ink3;
                } else {
                    bgCol = Mix(Mix(p.surface, p.sunken, hv * 0.6f), p.accentSoft, sel);
                    borderCol = Mix(Mix(p.line, p.lineStrong, hv), p.accent, sel);
                    borderWidth = sel > 0.5f ? Dp(1.5f) : 1.0f;
                    nameCol = Mix(p.ink, p.accentInk, sel);
                    summaryCol = p.ink3;
                    iconCol = Mix(p.ink2, p.accentInk, sel);
                }

                cdl->AddRectFilled(bb.Min, bb.Max, bgCol, cardR);
                cdl->AddRect(bb.Min, bb.Max, borderCol, cardR, 0, borderWidth);

                const float padX = Dp(12.0f);
                const float textX0 = bb.Min.x + padX;
                const float textX1 = bb.Max.x - padX;

                Icon(cdl, kRendererIcons[i], 16.0f, ImVec2(textX0 + Dp(8.0f), bb.Min.y + Dp(18.0f)), iconCol);
                Text(cdl, Font::Semibold, size::Body, ImVec2(textX0 + Dp(22.0f), bb.Min.y + Dp(9.0f)), nameCol,
                     Tr(kVideoRenderers[i].label));

                const char* summaryText = avail ? Tr(kVideoRenderers[i].summary) : Tr("이 그래픽 카드에서는 사용할 수 없습니다");
                TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(textX0, bb.Min.y + Dp(32.0f)), textX1, summaryCol,
                             summaryText);

                ImGui::RenderNavCursor(bb, gid);
            }
        }

        // shader packs only change the raster / real-time RT renderers: say so where the renderer is chosen
        if (cfg.renderer == (int)VideoRenderer::PathTraced || cfg.renderer == (int)VideoRenderer::OfflineGI) {
            bool anyPack = false;
            if (studio) {
                for (const auto& m : studio_->models)
                    anyPack |= m->kind == studio::ModelKind::Character && !m->shader.pack.empty();
            } else if (selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size()) {
                anyPack = !settings_.CharacterShader(library_.characters[(size_t)selCharacter_].id).pack.empty();
            }
            if (anyPack) {
                Gap(6.0f);
                PushFont(Font::Regular, size::Caption);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.warn));
                ImGui::TextWrapped("%s", Tr("이 렌더러는 셰이더 팩을 쓰지 않습니다. 팩을 적용하려면 래스터나 실시간 RT를 고르세요."));
                ImGui::PopStyleColor();
                PopFont();
            }
        }

        // 2. 화질
        Gap(10.0f);
        SectionLabel(Tr("화질"));
        const char* qLabels[kVideoQualityCount];
        for (int i = 0; i < kVideoQualityCount; ++i) qLabels[i] = Tr(kVideoQualities[i].label);
        Segmented("##vq", qLabels, kVideoQualityCount, &cfg.quality, colW / Dpi(), 36.0f);
        {
            static const char* const kQualityHints[kVideoQualityCount] = {
                Tr("빠르게 확인하는 용도"),
                Tr("무난한 화질"),
                Tr("최종 결과물 추천"),
                Tr("가장 깨끗하지만 가장 오래 걸립니다")
            };
            const int qIdx = std::clamp(cfg.quality, 0, kVideoQualityCount - 1);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            Text(ImGui::GetWindowDrawList(), Font::Regular, size::Caption, ImVec2(pos.x, pos.y + Dp(4.0f)), p.ink3,
                 kQualityHints[qIdx]);
            ImGui::Dummy(ImVec2(colW, Dp(20.0f)));
        }

        // 3. 효과
        Gap(6.0f);
        SectionLabel(Tr("효과"));

        Switch("##bloom", Tr("블룸"), &cfg.bloom, Tr("밝은 부분이 부드럽게 번집니다"));
        if (cfg.bloom) {
            ImGui::Indent(Dp(16.0f));
            Switch("##bloomconv", Tr("콘볼루션 블룸"), &cfg.bloomConvolution, Tr("렌즈 빛 갈라짐이 살아 있는 번짐"));
            ImGui::Unindent(Dp(16.0f));
        }

        Switch("##volumetric", Tr("볼류메트릭 라이트"), &cfg.volumetric, Tr("빛줄기와 안개를 표현합니다"));
        if (cfg.volumetric) {
            ImGui::Indent(Dp(16.0f));
            SliderRow("##voldensity", Tr("안개 밀도"), &cfg.volumetricDensity, 0.25f, 4.0f, "%.2f");
            ImGui::Unindent(Dp(16.0f));
        }

        if (cfg.DofApplies()) {
            Switch("##dof", Tr("피사계 심도"), &cfg.dof, Tr("초점 밖이 흐려집니다"));
            if (cfg.dof) {
                ImGui::Indent(Dp(16.0f));
                SliderRow("##dofaperture", Tr("조리개"), &cfg.dofAperture, 0.2f, 3.0f, "%.1f");
                ImGui::Unindent(Dp(16.0f));
            }
        } else {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            Text(ImGui::GetWindowDrawList(), Font::Regular, size::Caption, ImVec2(pos.x, pos.y + Dp(2.0f)), p.ink3,
                 Tr("오프라인 GI는 피사계 심도와 모션 블러를 자동으로 적용합니다"));
            ImGui::Dummy(ImVec2(colW, Dp(24.0f)));
        }

        // 4. 해상도, 프레임 레이트, 비트 전송률 (the format: changed least, so it comes last)
        bool formatChanged = false;

        Gap(10.0f);
        const float halfW = (colW - gapX * 2.0f) * 0.5f;
        ImGui::BeginGroup();
        SectionLabel(Tr("해상도"));
        const char* resLabels[kVideoResolutionCount];
        for (int i = 0; i < kVideoResolutionCount; ++i) resLabels[i] = kVideoResolutions[i].label;
        formatChanged |= Segmented("##vres", resLabels, kVideoResolutionCount, &cfg.resolution, halfW / Dpi(), 36.0f);
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, gapX * 2.0f);
        ImGui::BeginGroup();
        SectionLabel(Tr("프레임 레이트"));
        const char* fpsLabels[] = {"24", "30", "60"};
        int fpsIdx = cfg.fps == 24 ? 0 : cfg.fps == 30 ? 1 : 2;
        if (Segmented("##vfps", fpsLabels, kVideoFpsCount, &fpsIdx, halfW / Dpi(), 36.0f)) {
            cfg.fps = (int)kVideoFpsChoices[fpsIdx];
            formatChanged = true;
        }
        ImGui::EndGroup();

        const VideoResolution& res = kVideoResolutions[cfg.resolution];
        const int recommended = RecommendedBitrateMbps(res.height, cfg.fps);
        if (formatChanged) cfg.bitrateMbps = recommended;

        Gap(6.0f);
        float mbps = (float)cfg.bitrateMbps;
        if (SliderRow("##vbr", Tr("비트 전송률"), &mbps, 4.0f, 200.0f, "%.0f Mbps")) cfg.bitrateMbps = (int)std::lround(mbps);
        {
            char hint[96];
            std::snprintf(hint, sizeof(hint), Tr("%s · %dp %d fps 추천 %d Mbps"),
                          cfg.bitrateMbps >= recommended ? Tr("깨끗한 화질") : Tr("용량 우선"), (int)res.height, cfg.fps,
                          recommended);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            Text(ImGui::GetWindowDrawList(), Font::Regular, size::Caption, ImVec2(pos.x, pos.y + Dp(2.0f)), p.ink3,
                 hint);
            ImGui::Dummy(ImVec2(colW, Dp(24.0f)));
        }

        ImGui::PopClipRect();
        ImGui::EndGroup();
    }
    ImGui::PopItemWidth();
    // more settings below the fold: fade the bottom edge and show a caret
    if (ImGui::GetScrollMaxY() > 1.0f && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.0f) {
        ImDrawList* cdl = ImGui::GetForegroundDrawList();
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const float fh = Dp(36.0f);
        const ImU32 clear = WithAlpha(p.surface, 0.0f);
        cdl->AddRectFilledMultiColor(ImVec2(wp.x, wp.y + ws.y - fh), ImVec2(wp.x + ws.x, wp.y + ws.y), clear, clear,
                                     p.surface, p.surface);
        Icon(cdl, icon::CaretDown, 16.0f, ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y - Dp(10.0f)), p.ink3);
    }
    ImGui::EndChild();

    // Pinned footer (always visible, separated by a hairline from the scrolling area)
    cfg.Clamp();
    const VideoEstimate est = EstimateVideoRender();
    const VideoResolution& curRes = kVideoResolutions[cfg.resolution];

    const float footerY = b.y - bottomBlock;
    dl->AddLine(ImVec2(x0, footerY), ImVec2(x1, footerY), WithAlpha(p.ink, 0.1f));

    // Big line: 예상 소요 시간 + Badge
    const float y1 = footerY + Dp(14.0f);
    const std::string timeStr = Tr("예상 소요 시간 ") + DurationKo(est.totalSeconds);
    Text(dl, Font::Semibold, size::Title, ImVec2(x0, y1), p.ink, timeStr.c_str());
    const ImVec2 titleSz = TextSize(Font::Semibold, size::Title, timeStr.c_str());
    ImVec2 badgeSz;
    Badge(dl, ImVec2(x0 + titleSz.x + Dp(10.0f), y1 + Dp(1.0f)), est.measured ? Tr("실측") : Tr("추정"),
          est.measured ? p.accent : p.sunken, est.measured ? p.onAccent : p.ink2, &badgeSz);
    // the sample render behind this dialog: a small progress bar next to the badge while it runs
    const VideoProbeStatus probeStatus = ProbeStatus();
    if (probeStatus.phase == VideoProbeStatus::Phase::Measuring) {
        const float bx0 = x0 + titleSz.x + Dp(10.0f) + badgeSz.x + Dp(12.0f);
        const float by = y1 + Dp(1.0f) + badgeSz.y * 0.5f;
        if (x1 - bx0 > Dp(40.0f))
            ProgressBar(dl, ImVec2(bx0, by - Dp(3.0f)), ImVec2(std::min(x1, bx0 + Dp(120.0f)), by + Dp(3.0f)),
                        probeStatus.fraction);
    }

    // Second line: 총 N프레임 · M:SS · W×H · F fps · 파일 약 X.X GB
    const float y2 = footerY + Dp(42.0f);
    char l2[160];
    std::snprintf(l2, sizeof(l2), Tr("총 %d프레임 · %s · %u×%u · %d fps · 파일 약 %.1f GB"), est.frames,
                  MinSec(durationSec).c_str(), curRes.width, curRes.height, cfg.fps, est.fileGigabytes);
    Text(dl, Font::Regular, size::Body, ImVec2(x0, y2), p.ink2, l2);

    // Third line: 프레임당 약 X.X초로 측정한 값입니다 / 실제 시간은 PC 성능과 장면에 따라 달라집니다
    const float y3 = footerY + Dp(66.0f);
    char l3[128];
    if (est.measured) {
        std::snprintf(l3, sizeof(l3),
                      est.secondsPerFrame < 0.1   ? Tr("프레임당 약 %.3f초로 측정한 값입니다")
                      : est.secondsPerFrame < 1.0 ? Tr("프레임당 약 %.2f초로 측정한 값입니다")
                                                  : Tr("프레임당 약 %.1f초로 측정한 값입니다"),
                      est.secondsPerFrame);
    } else if (probeStatus.phase == VideoProbeStatus::Phase::Measuring) {
        std::snprintf(l3, sizeof(l3), Tr("설정에 맞춰 샘플 한 장을 렌더링하며 정확한 시간을 측정하고 있습니다"));
    } else if (probeStatus.phase == VideoProbeStatus::Phase::Preparing) {
        std::snprintf(l3, sizeof(l3), Tr("곧 샘플 한 장을 렌더링해 정확한 시간을 측정합니다"));
    } else {
        std::snprintf(l3, sizeof(l3), "%s", Tr("실제 시간은 PC 성능과 장면에 따라 달라집니다"));
    }
    Text(dl, Font::Regular, size::Caption, ImVec2(x0, y3), p.ink3, l3);

    // Save location line
    const float y4 = footerY + Dp(86.0f);
    const std::string loc = Tr("저장 위치: ") + PathToUtf8(OfflineOutputDir(true));
    TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(x0, y4), x1, p.ink3, loc.c_str());

    // Buttons row
    const float y5 = footerY + Dp(110.0f);
    const bool rendererAvailable = VideoRendererAvailable(cfg.Renderer());

    // On the right: 취소 and 렌더링 시작
    const float startW = Dp(176.0f), cancelW = Dp(96.0f);
    ImGui::SetCursorScreenPos(ImVec2(x1 - startW - Dp(10.0f) - cancelW, y5));
    const bool cancel = Button("##vidcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(96, 40));
    ImGui::SameLine(0.0f, Dp(10.0f));
    ImGui::BeginDisabled(!rendererAvailable);
    const bool start = Button("##vidstart", Tr("렌더링 시작"), icon::FilmStrip, ButtonKind::Primary, ImVec2(176, 40));
    ImGui::EndDisabled();

    ImGui::End();

    if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        videoDialogOpen_ = false;
        settings_.Save(settingsPath_);
        return;
    }
    if (start) {
        if (studio) {
            settings_.video.Clamp();
            settings_.Save(settingsPath_);
            videoDialogOpen_ = false;
            StartStudioRender(true);
        } else {
            StartVideoRenderLoad();
        }
        return;
    }
}

}  // namespace mmdx
