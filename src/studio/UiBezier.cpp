#include "UiBezier.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mmdx::studio {

// Korean source strings (I18n keys), translated with Tr() when drawn.
const char* const kBezierPresetNames[kBezierPresetCount] = {
    "선형", "천천히 시작", "천천히 끝", "천천히 시작과 끝", "빠르게 시작"
};

void BezierPreset(int index, uint8_t out[4]) {
    switch (index) {
        case 1: out[0]=42; out[1]=0; out[2]=108; out[3]=64; break;
        case 2: out[0]=20; out[1]=70; out[2]=87; out[3]=127; break;
        case 3: out[0]=64; out[1]=0; out[2]=64; out[3]=127; break;
        case 4: out[0]=0; out[1]=64; out[2]=64; out[3]=127; break;
        case 0:
        default: out[0]=20; out[1]=20; out[2]=107; out[3]=107; break;
    }
}

bool BezierCurveEditor(const char* id, uint8_t c[4], float plotSize) {
    using namespace ui;
    bool changed = false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID imguiId = window->GetID(id);
    
    float sz = Dp(plotSize);
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + sz, pos.y + sz));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, imguiId)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, imguiId, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    
    auto toScreen = [&](float bx, float by) {
        return ImVec2(pos.x + (bx / 127.0f) * sz, pos.y + sz - (by / 127.0f) * sz);
    };
    auto fromScreen = [&](ImVec2 p, float& bx, float& by) {
        bx = std::clamp((p.x - pos.x) / sz * 127.0f, 0.0f, 127.0f);
        by = std::clamp((pos.y + sz - p.y) / sz * 127.0f, 0.0f, 127.0f);
    };

    ImVec2 p1 = toScreen(c[0], c[1]);
    ImVec2 p2 = toScreen(c[2], c[3]);
    
    ImGuiStorage* st = ImGui::GetStateStorage();
    int activeHandle = st->GetInt(imguiId, 0);

    if (pressed) {
        float d1 = std::hypot(mouse.x - p1.x, mouse.y - p1.y);
        float d2 = std::hypot(mouse.x - p2.x, mouse.y - p2.y);
        if (d1 < Dp(10.0f) || d2 < Dp(10.0f)) {
            activeHandle = (d1 <= d2) ? 1 : 2;
        } else {
            activeHandle = 0;
        }
        st->SetInt(imguiId, activeHandle);
    }
    
    if (held && activeHandle != 0) {
        float bx, by;
        fromScreen(mouse, bx, by);
        int ibx = (int)std::round(bx);
        int iby = (int)std::round(by);
        if (activeHandle == 1) {
            if (c[0] != ibx || c[1] != iby) { c[0] = ibx; c[1] = iby; changed = true; }
        } else {
            if (c[2] != ibx || c[3] != iby) { c[2] = ibx; c[3] = iby; changed = true; }
        }
    } else if (!held) {
        st->SetInt(imguiId, 0);
    }
    
    ImDrawList* dl = window->DrawList;
    const Palette& p = P();
    
    dl->AddRectFilled(bb.Min, bb.Max, p.sunken);
    for (int i = 1; i < 4; ++i) {
        float offset = (sz * i) / 4.0f;
        dl->AddLine(ImVec2(pos.x, pos.y + offset), ImVec2(pos.x + sz, pos.y + offset), p.line);
        dl->AddLine(ImVec2(pos.x + offset, pos.y), ImVec2(pos.x + offset, pos.y + sz), p.line);
    }
    dl->AddRect(bb.Min, bb.Max, p.lineStrong);
    
    ImVec2 p0 = toScreen(0, 0);
    ImVec2 p3 = toScreen(127, 127);
    p1 = toScreen(c[0], c[1]);
    p2 = toScreen(c[2], c[3]);
    
    dl->AddLine(p0, p1, p.ink3);
    dl->AddLine(p3, p2, p.ink3);
    
    ImVec2 pts[48];
    for (int i = 0; i < 48; ++i) {
        float t = i / 47.0f;
        float mt = 1.0f - t;
        float b0 = mt * mt * mt;
        float b1 = 3.0f * mt * mt * t;
        float b2 = 3.0f * mt * t * t;
        float b3 = t * t * t;
        pts[i] = ImVec2(
            b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
            b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y
        );
    }
    dl->AddPolyline(pts, 48, p.accent, 0, Dp(2.0f));
    
    auto drawHandle = [&](ImVec2 hp, bool isActive) {
        float r = isActive ? Dp(8.0f) : Dp(6.0f);
        dl->AddCircleFilled(hp, r, p.accent);
        dl->AddCircle(hp, r, p.surface, 0, Dp(1.5f));
    };
    
    bool h1Hovered = !held && std::hypot(mouse.x - p1.x, mouse.y - p1.y) < Dp(10.0f);
    bool h2Hovered = !held && std::hypot(mouse.x - p2.x, mouse.y - p2.y) < Dp(10.0f);
    
    drawHandle(p1, activeHandle == 1 || h1Hovered);
    drawHandle(p2, activeHandle == 2 || h2Hovered);
    
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushID(id);
    
    // Presets and fields use the full line under the plot (the plot may be centred).
    ImGui::Dummy(ImVec2(0.0f, Dp(4.0f)));
    const float lineX = ImGui::GetCursorScreenPos().x;
    const float rightX = lineX + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < kBezierPresetCount; ++i) {
        const char* label = Tr(kBezierPresetNames[i]);
        const float expectedW = ui::TextSize(ui::Font::Semibold, ui::size::Body, label).x + Dp(32.0f);  // ui::Button auto width
        if (i > 0 && ImGui::GetItemRectMax().x + style.ItemSpacing.x + expectedW <= rightX) ImGui::SameLine();
        ImGui::PushID(i);
        const bool clicked = ui::Button("##preset", label, nullptr, ui::ButtonKind::Secondary,
                                        ImVec2(0.0f, 28.0f));
        ImGui::PopID();
        if (clicked) {
            BezierPreset(i, c);
            changed = true;
        }
    }
    
    int vals[4] = { c[0], c[1], c[2], c[3] };
    const char* labels[] = { "##x1", "##y1", "##x2", "##y2" };
    float w = (rightX - lineX - style.ItemSpacing.x * 3) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(Dp(40.0f), w));
        if (ImGui::DragInt(labels[i], &vals[i], 0.5f, 0, 127, "%d")) {
            c[i] = (uint8_t)std::clamp(vals[i], 0, 127);
            changed = true;
        }
    }
    ImGui::PopID();
    
    return changed;
}

} // namespace mmdx::studio
