#include "app/App.h"

#include <cmath>

#include "core/Log.h"
#include "imgui.h"
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
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
            if (!playing_ && playTime_ * kMmdFps >= s->endFrame) playTime_ = 0;
            playing_ = !playing_;
            if (s->hasAudio) {
                if (playing_) {
                    audio_.Seek(playTime_);
                    audio_.Play();
                } else {
                    audio_.Pause();
                }
            }
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

void App::DrawPlayOverlay() {
    ImGuiIO& io = ImGui::GetIO();
    const SceneRuntime* s = scene_.get();
    if (!s) return;

    const bool showBar = overlayVisible_ && (!playing_ || timeSeconds_ - lastMouseMoveTime_ < 3.0);

    // --- Stats window (top-left), visible whenever overlayVisible_.
    if (overlayVisible_) {
        ImGui::SetNextWindowPos(ImVec2(16, 16));
        ImGui::SetNextWindowBgAlpha(0.45f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##stats", nullptr, flags)) {
            ImGui::Text("FPS %.0f (%.2f ms)", io.Framerate, 1000.0f / std::max(io.Framerate, 0.01f));
            ImGui::Text("GPU %.2f ms", renderer_.Stats().gpuFrameMs);
            ImGui::Text("%ux%u · MSAA %ux · draw %u · tri %.1fK", ctx_.Width(), ctx_.Height(),
                        renderer_.Settings().msaaSamples, renderer_.Stats().drawCalls,
                        renderer_.Stats().triangles / 1000.0);
            ImGui::TextUnformatted(ctx_.Caps().adapterName.c_str());
        }
        ImGui::End();
    }

    // --- Bottom bar.
    if (!showBar) return;

    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_);
    const float barHeight = 90.0f * dpi;
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, barHeight));
    const ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##playbar", nullptr, barFlags)) {
        if (ImGui::Button(playing_ ? "일시정지" : "재생")) {
            if (!playing_ && playTime_ * kMmdFps >= (double)s->endFrame) playTime_ = 0;
            playing_ = !playing_;
            if (s->hasAudio) {
                if (playing_) {
                    audio_.Seek(playTime_);
                    audio_.Play();
                } else {
                    audio_.Pause();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("처음으로")) {
            playTime_ = 0;
            if (s->hasAudio) audio_.Seek(playTime_);
        }
        ImGui::SameLine();
        const auto fmtTime = [](double t) {
            if (t < 0) t = 0;
            const int secs = (int)t;
            return std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
        };
        ImGui::TextUnformatted((fmtTime(playTime_) + " / " + fmtTime(s->endFrame / kMmdFps)).c_str());
        ImGui::SameLine();

        float t = (float)playTime_;
        // Seek bar takes what is left after the fixed-width controls on its right.
        const float rightControls = ImGui::GetFontSize() * 26.0f;
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - rightControls));
        if (ImGui::SliderFloat("##seek", &t, 0.0f, s->endFrame / kMmdFps, "")) {
            playTime_ = t;
            if (s->hasAudio) audio_.Seek(playTime_);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            playTime_ = t;
            if (s->hasAudio) audio_.Seek(playTime_);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::SliderFloat("볼륨", &settings_.volume, 0.0f, 1.0f)) audio_.SetVolume(settings_.volume);
        ImGui::SameLine();

        // "모션 카메라" checkbox on the same line (disabled when no camera).
        if (!s->camera) ImGui::BeginDisabled();
        ImGui::Checkbox("모션 카메라", &useMotionCamera_);
        if (!s->camera) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("선택 화면 (Esc)")) {
            UnloadScene();
            screen_ = Screen::Select;
        }

        ImGui::TextDisabled("Space 재생/정지 · ←/→ 5초 이동 · C 카메라 · F1 UI · 드래그 회전 · 휠 줌");
    }
    ImGui::End();
}

} // namespace mmdx
