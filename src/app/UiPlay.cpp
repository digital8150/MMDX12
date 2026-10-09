#include "app/App.h"

#include <cmath>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"
#include "core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"

namespace mmdx {

// ---------------------------------------------------------------------------
// Play update
// ---------------------------------------------------------------------------

void App::UpdatePlay(double dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) lastMouseMoveTime_ = timeSeconds_;

    const SceneRuntime* s = scene_.get();
    if (!s) return;

    const auto seekTo = [&](double t) {
        playTime_ = std::clamp(t, 0.0, (double)(s->endFrame / kMmdFps));
        if (s->hasAudio) audio_.Seek(playTime_);
    };

    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) SetPlaying(!playing_);
        if (ImGui::IsKeyPressed(ImGuiKey_L)) {
            settings_.lighting = (settings_.lighting + 1) % kLightingPresetCount;
            settings_.Save(settingsPath_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            UnloadScene();
            screen_ = Screen::Select;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) overlayVisible_ ^= 1;
        if (s->camera && ImGui::IsKeyPressed(ImGuiKey_C)) useMotionCamera_ ^= 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) seekTo(playTime_ - 5.0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) seekTo(playTime_ + 5.0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) seekTo(0.0);
    }

    if (playing_) {
        if (s->hasAudio && audio_.IsPlaying()) {
            const double a = audio_.PositionSeconds();
            const double predicted = playTime_ + dt;
            playTime_ = std::fabs(predicted - a) > 0.05 ? a : predicted;
        } else {
            playTime_ += dt;
        }
        if (playTime_ * kMmdFps >= (double)s->endFrame) {
            playTime_ = s->endFrame / kMmdFps;
            playing_ = false;
            audio_.Pause();
        }
    }

    if (!useMotionCamera_ || !s->camera) UpdateFreeCamera();

    // after the time step, so a still taken during playback has the last live frame as shutter open
    if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_P) &&
        renderer_.OfflineSupported())
        StartOfflineStill();
}

void App::SetPlaying(bool play) {
    const SceneRuntime* s = scene_.get();
    if (!s) return;
    if (play && playTime_ * kMmdFps >= s->endFrame) playTime_ = 0;
    playing_ = play;
    if (s->hasAudio) {
        if (playing_) {
            audio_.Seek(playTime_);
            audio_.Play();
        } else {
            audio_.Pause();
        }
    }
}

// ---------------------------------------------------------------------------
// Free camera
// ---------------------------------------------------------------------------

void App::UpdateFreeCamera() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;

    FreeCamera& cam = freeCam_;

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = io.MouseDelta;
        cam.yaw += delta.x * 0.006f;
        cam.pitch = std::clamp(cam.pitch + delta.y * 0.006f, -1.45f, 1.45f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 delta = io.MouseDelta;
        // right = (cos(yaw), 0, sin(yaw)), up = (0, 1, 0); pan = (-dx*right + dy*up) * distance * 0.0015
        const float rx = std::cos(cam.yaw), rz = std::sin(cam.yaw);
        const float scale = cam.distance * 0.0015f;
        cam.target.x += (-delta.x * rx) * scale;
        cam.target.y += (delta.y) * scale;
        cam.target.z += (-delta.x * rz) * scale;
    }
    const float wheel = io.MouseWheel;
    if (wheel != 0.0f) {
        cam.distance = std::clamp(cam.distance * std::pow(0.88f, wheel), 2.0f, 600.0f);
    }
}


// ---------------------------------------------------------------------------
// Overlay UI
// ---------------------------------------------------------------------------

namespace {

// Seek bar: thin track that thickens on hover, knob while hovered or dragged.
bool SeekBar(const char* id, float width, float height, double* t, double duration) {
    using namespace ui;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID gid = w->GetID(id);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + width, w->DC.CursorPos.y + height));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    ImGui::ButtonBehavior(bb, gid, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    if (hovered || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const Palette& p = P();
    ImDrawList* dl = w->DrawList;
    const float hv = Anim(gid, hovered || held, 14.0f);
    const float th = Dp(4.0f + 2.0f * hv);
    const float cy = bb.Min.y + height * 0.5f;
    const float frac = duration > 0 ? (float)std::clamp(*t / duration, 0.0, 1.0) : 0.0f;
    const float mx = ImGui::GetIO().MousePos.x;
    const float hoverFrac = std::clamp((mx - bb.Min.x) / std::max(width, 1.0f), 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(bb.Min.x, cy - th * 0.5f), ImVec2(bb.Max.x, cy + th * 0.5f), WithAlpha(p.ink, 0.12f), th);
    if (hovered && !held)
        dl->AddRectFilled(ImVec2(bb.Min.x, cy - th * 0.5f), ImVec2(bb.Min.x + width * hoverFrac, cy + th * 0.5f),
                          WithAlpha(p.ink, 0.10f), th);
    dl->AddRectFilled(ImVec2(bb.Min.x, cy - th * 0.5f), ImVec2(bb.Min.x + width * frac, cy + th * 0.5f), p.accent, th);
    if (hv > 0.01f) {
        const ImVec2 kc(bb.Min.x + width * frac, cy);
        const float kr = Dp(7.0f) * hv;
        dl->AddCircleFilled(ImVec2(kc.x, kc.y + Dp(1.0f)), kr + Dp(1.0f), WithAlpha(IM_COL32(10, 40, 40, 255), 0.2f * hv), 24);
        dl->AddCircleFilled(kc, kr, p.surface, 24);
        dl->AddCircle(kc, kr, p.accent, 24, Dp(2.0f));
    }
    if (hovered && !held) {
        const std::string label = MinSec(duration * hoverFrac);
        const ImVec2 ls = TextSize(Font::Semibold, size::Caption, label.c_str());
        const ImVec2 ta(mx - ls.x * 0.5f - Dp(8.0f), bb.Min.y - ls.y - Dp(14.0f));
        dl->AddRectFilled(ta, ImVec2(ta.x + ls.x + Dp(16.0f), ta.y + ls.y + Dp(8.0f)), p.ink, Dp(6.0f));
        Text(dl, Font::Semibold, size::Caption, ImVec2(ta.x + Dp(8.0f), ta.y + Dp(4.0f)), p.surface, label.c_str());
    }
    if (held && duration > 0) {
        *t = duration * hoverFrac;
        return true;
    }
    return false;
}

} // namespace

void App::DrawPlayOverlay() {
    using namespace ui;
    ImGuiIO& io = ImGui::GetIO();
    const SceneRuntime* s = scene_.get();
    if (!s) return;
    const Palette& p = P();

    const bool showBar = overlayVisible_ && (!playing_ || timeSeconds_ - lastMouseMoveTime_ < 3.0);
    const float show = Anim(ImGui::GetID("##overlayShow"), showBar, 7.0f);
    if (show < 0.005f) return;

    float rect[4];
    renderer_.PresentRect(rect[0], rect[1], rect[2], rect[3]);
    const ImTextureID backdrop = (ImTextureID)renderer_.UiBackdropTexture();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##playoverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ease = 1.0f - (1.0f - show) * (1.0f - show) * (1.0f - show);
    const ImVec2 ds = io.DisplaySize;

    // ---- title block (top left), slides up when hidden
    {
        const SongAsset* song = selSong_ >= 0 ? &library_.songs[(size_t)selSong_] : nullptr;
        const CharacterAsset* ch = selCharacter_ >= 0 ? &library_.characters[(size_t)selCharacter_] : nullptr;
        const StageAsset* st = selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr;
        const std::string title = song ? song->displayName : std::string(Tr("재생 중"));
        const std::string sub =
            (ch ? ch->displayName : std::string()) + "  ·  " + (st ? st->displayName : std::string(Tr("스튜디오")));
        const ImVec2 ts = TextSize(Font::Semibold, size::Title, title.c_str());
        const ImVec2 ss = TextSize(Font::Regular, size::Caption, sub.c_str());
        const float w = std::min(std::max(ts.x, ss.x) + Dp(40.0f), Dp(520.0f));
        const float h = Dp(62.0f);
        const float y = Dp(20.0f) - (h + Dp(30.0f)) * (1.0f - ease);
        const ImVec2 a(Dp(20.0f), y), b(a.x + w, y + h);
        FrostedPanel(dl, a, b, Dp(16.0f), backdrop, rect);
        TextEllipsis(dl, Font::Semibold, size::Title, ImVec2(a.x + Dp(20.0f), a.y + Dp(11.0f)), b.x - Dp(16.0f), p.ink,
                     title.c_str());
        TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(a.x + Dp(20.0f), a.y + Dp(36.0f)), b.x - Dp(16.0f), p.ink2,
                     sub.c_str());
    }

    // ---- performance pill (top right)
    {
        char buf[200];
        const RenderStats& rs = renderer_.Stats();
        int n = std::snprintf(buf, sizeof(buf), "%.0f FPS   GPU %.2f ms   %ux%u", io.Framerate, rs.gpuFrameMs,
                              rs.internalWidth, rs.internalHeight);
        if (rs.outputWidth != rs.internalWidth || rs.outputHeight != rs.internalHeight)
            n += std::snprintf(buf + n, sizeof(buf) - n, " → %ux%u", rs.outputWidth, rs.outputHeight);
        if (rs.renderPath == RenderPath::Raster && renderer_.Settings().upscaler == UpscalerKind::None) {
            n += std::snprintf(buf + n, sizeof(buf) - n, "   MSAA %ux", renderer_.Settings().msaaSamples);
        } else if (rs.renderPath == RenderPath::RayTraced) {
            n += std::snprintf(buf + n, sizeof(buf) - n, "   RT");
        } else if (rs.renderPath == RenderPath::PathTraced) {
            n += std::snprintf(buf + n, sizeof(buf) - n, "   PT");
        }
        if (rs.upscaler != UpscalerKind::None) {
            const char* upName = rs.upscaler == UpscalerKind::DLSS ? "DLSS"
                                 : rs.upscaler == UpscalerKind::FSR ? "FSR"
                                 : "XeSS";
            n += std::snprintf(buf + n, sizeof(buf) - n, "   %s", upName);
        }
        (void)n;
        const ImVec2 bs = TextSize(Font::Semibold, size::Caption, buf);
        const float w = bs.x + Dp(28.0f), h = Dp(34.0f);
        const float y = Dp(20.0f) - (h + Dp(30.0f)) * (1.0f - ease);
        const ImVec2 a(ds.x - Dp(20.0f) - w, y), b(ds.x - Dp(20.0f), y + h);
        FrostedPanel(dl, a, b, h * 0.5f, backdrop, rect);
        Text(dl, Font::Semibold, size::Caption, ImVec2(a.x + Dp(14.0f), a.y + (h - bs.y) * 0.5f), p.ink2, buf);
    }

    // ---- control bar (bottom), slides down when hidden
    const float barW = std::min(ds.x - Dp(40.0f), Dp(1000.0f));
    const float barH = Dp(76.0f);
    const float barY = ds.y - Dp(24.0f) - barH + (barH + Dp(40.0f)) * (1.0f - ease);
    const ImVec2 ba((ds.x - barW) * 0.5f, barY), bb(ba.x + barW, barY + barH);
    FrostedPanel(dl, ba, bb, Dp(22.0f), backdrop, rect, 0.68f);
    const float cy = ba.y + barH * 0.5f;
    float x = ba.x + Dp(16.0f);

    // play / pause
    {
        const float d = Dp(48.0f);
        const ImVec2 a(x, cy - d * 0.5f);
        ImGui::SetCursorScreenPos(a);
        const bool pressed = ImGui::InvisibleButton("##playpause", ImVec2(d, d));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float hv = Anim(ImGui::GetID("##pphv"), hovered);
        SoftShadow(dl, a, ImVec2(a.x + d, a.y + d), d * 0.5f, Dp(8.0f), 0.14f + 0.06f * hv, ImVec2(0, Dp(2.0f)));
        dl->AddCircleFilled(ImVec2(a.x + d * 0.5f, a.y + d * 0.5f), d * 0.5f, Mix(p.accent, p.accentHover, hv), 32);
        const char* glyph = playing_ ? icon::Pause : icon::Play;
        PushFont(Font::Bold, 22.0f);  // the bold face carries the filled icon set
        const ImVec2 gs = ImGui::CalcTextSize(glyph);
        dl->AddText(ImVec2(std::round(a.x + (d - gs.x) * 0.5f + (playing_ ? 0.0f : Dp(1.5f))),
                           std::round(a.y + (d - gs.y) * 0.5f)),
                    p.onAccent, glyph);
        PopFont();
        if (hovered) Tooltip(playing_ ? Tr("일시정지 (Space)") : Tr("재생 (Space)"));
        if (pressed) SetPlaying(!playing_);
        x += d + Dp(8.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
    if (IconButton("##restart", icon::SkipBack, Tr("처음부터"), false, 40.0f)) {
        playTime_ = 0;
        if (s->hasAudio) audio_.Seek(playTime_);
    }
    x += Dp(40.0f) + Dp(12.0f);

    // time
    const double duration = s->endFrame / kMmdFps;
    {
        const std::string cur = MinSec(playTime_), tot = " / " + MinSec(duration);
        const ImVec2 cs = TextSize(Font::Semibold, size::Body, cur.c_str());
        Text(dl, Font::Semibold, size::Body, ImVec2(x, cy - cs.y * 0.5f), p.ink, cur.c_str());
        Text(dl, Font::Regular, size::Body, ImVec2(x + cs.x, cy - cs.y * 0.5f), p.ink3, tot.c_str());
        x += Dp(96.0f);
    }

    // right cluster: volume icon + slider, offline still, camera, lighting, divider, exit
    // (volume, still, camera, light, [size], [shader], effects, divider, exit)
    const int optionalButtons = s->characterId.empty() ? 1 : 3;   // size + shader only with a character; effects always
    const float rightW = Dp(36.0f + 4.0f + 88.0f + 12.0f + 44.0f + 44.0f + 50.0f + 44.0f * optionalButtons + 11.0f + 40.0f + 16.0f);
    const float seekW = std::max(Dp(120.0f), bb.x - rightW - x - Dp(20.0f));
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(14.0f)));
    double t = playTime_;
    if (SeekBar("##seek", seekW, Dp(28.0f), &t, duration)) {
        playTime_ = t;
        if (s->hasAudio) audio_.Seek(playTime_);
    }
    x += seekW + Dp(20.0f);

    // volume
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(18.0f)));
    const bool muted = settings_.volume <= 0.001f;
    if (IconButton("##mute", muted ? icon::SpeakerX : (settings_.volume < 0.5f ? icon::SpeakerLow : icon::SpeakerHigh),
                   muted ? Tr("소리 켜기") : Tr("음소거"), false, 36.0f)) {
        static float lastVolume = 0.8f;
        if (muted) {
            settings_.volume = lastVolume > 0.01f ? lastVolume : 0.8f;
        } else {
            lastVolume = settings_.volume;
            settings_.volume = 0.0f;
        }
        audio_.SetVolume(settings_.volume);
    }
    x += Dp(36.0f) + Dp(4.0f);
    {
        const float vw = Dp(88.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(10.0f)));
        ImGui::InvisibleButton("##vol", ImVec2(vw, Dp(20.0f)));
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (hovered || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (held) {
            settings_.volume = std::clamp((io.MousePos.x - x) / vw, 0.0f, 1.0f);
            audio_.SetVolume(settings_.volume);
        }
        const float th = Dp(4.0f);
        dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + vw, cy + th * 0.5f), WithAlpha(p.ink, 0.12f), th);
        dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + vw * settings_.volume, cy + th * 0.5f), p.ink2, th);
        const float hv = Anim(ImGui::GetID("##volhv"), hovered || held);
        if (hv > 0.01f) dl->AddCircleFilled(ImVec2(x + vw * settings_.volume, cy), Dp(6.0f) * hv, p.ink, 24);
        if (hovered || held) {
            char vbuf[32];
            std::snprintf(vbuf, sizeof(vbuf), "%s %d%%", Tr("볼륨"), (int)std::lround(settings_.volume * 100.0f));
            ImGui::SetTooltip("%s", vbuf);
        }
        x += vw + Dp(12.0f);
    }

    // offline render: high-quality still (videos are rendered from the select screen)
    {
        const bool ok = renderer_.OfflineSupported();
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        ImGui::BeginDisabled(!ok);
        if (IconButton("##shot", icon::Camera, ok ? Tr("고품질 스크린샷 (P)") : Tr("고품질 렌더는 DXR 지원 GPU가 필요합니다"),
                       false, 40.0f))
            StartOfflineStill();
        x += Dp(44.0f);
        ImGui::EndDisabled();
    }

    // camera, lighting
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
    ImGui::BeginDisabled(!s->camera);
    if (IconButton("##cam", icon::VideoCamera, useMotionCamera_ ? Tr("모션 카메라 켜짐 (C)") : Tr("자유 카메라 (C)"),
                   useMotionCamera_ && s->camera, 40.0f))
        useMotionCamera_ = !useMotionCamera_;
    ImGui::EndDisabled();
    x += Dp(44.0f);
    {
        const char* lightIcons[] = {icon::Sun, icon::CircleHalf, icon::Sparkle, icon::Moon};
        const std::string tip = std::string(Tr("조명: ")) + LightingPresetName((LightingPreset)settings_.lighting) + " (L)";
        // a menu of the named presets (clicking used to cycle blindly through them; L still cycles)
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        if (IconButton("##light", lightIcons[settings_.lighting], tip.c_str(), false, 40.0f)) ImGui::OpenPopup("##lightmenu");
        ImGui::SetNextWindowPos(ImVec2(x, cy - Dp(28.0f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(200.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(8.0f)));
        if (ImGui::BeginPopup("##lightmenu")) {
            for (int i = 0; i < kLightingPresetCount; ++i) {
                ImGui::PushID(i);
                const char* hint = i == settings_.lighting ? Tr("사용 중") : nullptr;
                if (MenuItem("##preset", LightingPresetName((LightingPreset)i), lightIcons[i], hint)) {
                    settings_.lighting = i;
                    settings_.Save(settingsPath_);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        x += Dp(40.0f) + Dp(10.0f);
    }
    // character size (saved per character; for models whose height does not suit the song's camera)
    if (!s->characterId.empty()) {
        const float scale = settings_.CharacterScale(s->characterId);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        const ImVec2 btn = ImGui::GetCursorScreenPos();
        if (IconButton("##size", icon::ArrowsOut, Tr("캐릭터 크기"), std::fabs(scale - 1.0f) > 0.005f, 40.0f))
            ImGui::OpenPopup("##sizepopup");
        ImGui::SetNextWindowPos(ImVec2(btn.x + Dp(20.0f), btn.y - Dp(10.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(260.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
        if (ImGui::BeginPopup("##sizepopup")) {
            float v = scale;
            if (SliderRow("##charscale", Tr("캐릭터 크기"), &v, 0.5f, 2.0f, "%.2fx")) {
                v = std::round(v * 100.0f) / 100.0f;
                if (std::fabs(v - 1.0f) < 0.03f) v = 1.0f;  // snap to the original size
                settings_.SetCharacterScale(s->characterId, v);
                settings_.Save(settingsPath_);
            }
            Gap(8.0f);
            if (Button("##charscalereset", Tr("원래 크기"), icon::Refresh, ButtonKind::Ghost)) {
                settings_.SetCharacterScale(s->characterId, 1.0f);
                settings_.Save(settingsPath_);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        x += Dp(44.0f);
    }
    // shader pack (saved per character; render/ShaderPack.h)
    if (!s->characterId.empty()) {
        ShaderChoice choice = settings_.CharacterShader(s->characterId);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        const ImVec2 btn = ImGui::GetCursorScreenPos();
        if (IconButton("##shader", icon::Diamond, Tr("셰이더"), !PlayShaderChoice().pack.empty(), 40.0f))
            ImGui::OpenPopup("##shaderpopup");
        ImGui::SetNextWindowPos(ImVec2(btn.x + Dp(20.0f), btn.y - Dp(10.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(PackParamsPopupWidth(choice), 0));
        // cap the height to the space above the button so the popup never runs off the top of the screen
        const float maxH = std::max(Dp(60.0f), (btn.y - Dp(10.0f)) - ImGui::GetMainViewport()->WorkPos.y - Dp(12.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
        if (ImGui::BeginPopup("##shaderpopup")) {
            SectionLabel(Tr("셰이더"));
            bool changed = DrawShaderSelector("##playshader", choice, ImGui::GetContentRegionAvail().x);
            Gap(8.0f);
            changed |= DrawShaderPackParams(choice);
            if (changed) {
                settings_.SetCharacterShader(s->characterId, choice);
                settings_.Save(settingsPath_);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        x += Dp(44.0f);
    }
    // screen effect packs (the ordered stack, render/ShaderPack.h "type": "effect")
    {
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        const ImVec2 btn = ImGui::GetCursorScreenPos();
        if (IconButton("##effects", icon::Sparkle, Tr("화면 효과"), !settings_.effectStack.empty(), 40.0f))
            ImGui::OpenPopup("##effectspopup");
        ImGui::SetNextWindowPos(ImVec2(btn.x + Dp(20.0f), btn.y - Dp(10.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(320.0f), 0));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0), ImVec2(FLT_MAX, std::max(Dp(60.0f), (btn.y - Dp(10.0f)) - ImGui::GetMainViewport()->WorkPos.y - Dp(12.0f))));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
        if (ImGui::BeginPopup("##effectspopup")) {
            if (DrawEffectStackEditor(settings_.effectStack)) {
                settings_.Save(settingsPath_);
                ApplyRenderSettings();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        x += Dp(44.0f);
    }
    dl->AddLine(ImVec2(x, cy - Dp(14.0f)), ImVec2(x, cy + Dp(14.0f)), WithAlpha(p.ink, 0.12f));
    x += Dp(11.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
    const bool exit = IconButton("##exit", icon::X, Tr("라이브러리로 (Esc)"), false, 40.0f);
    ImGui::End();
    if (exit) {
        UnloadScene();
        screen_ = Screen::Select;
    }
}

} // namespace mmdx
