#include "UiTimeline.h"
#include "app/UiKit.h"
#include "app/Icons.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace mmdx::studio {

namespace {
using namespace ui;

int GetMinSelectedFrame(const std::vector<TimelineRow>& rows) {
    int minFrame = 2147483647;
    for (const auto& r : rows) {
        if (!r.keysEditable) continue;
        for (const auto& k : r.keys) {
            if (k.selected && k.frame < minFrame) {
                minFrame = k.frame;
            }
        }
    }
    return minFrame;
}
} // namespace

bool Timeline(const char* id, ImVec2 size, const std::vector<TimelineRow>& rows, int currentFrame, int maxFrame,
              TimelineView& view, TimelineEvents& ev) {
    ev = TimelineEvents{};
    bool changed = false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID imguiId = window->GetID(id);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (size.x <= 0.0f) size.x = avail.x;
    if (size.y <= 0.0f) size.y = avail.y;

    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, imguiId)) return false;

    const Palette& p = P();
    ImDrawList* dl = window->DrawList;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;

    float colWidth = Dp(220.0f);
    float rowHeight = Dp(24.0f);
    float rulerHeight = Dp(28.0f);

    float keyAreaX = bb.Min.x + colWidth;
    float keyAreaW = std::max(0.0f, bb.Max.x - keyAreaX);
    const float originX = keyAreaX + Dp(12.0f);  // x of frame `scrollFrame` (inset keeps frame-0 keys whole)
    float contentY = bb.Min.y + rulerHeight;
    float contentH = std::max(0.0f, bb.Max.y - contentY);

    float visibleFrames = (keyAreaW - Dp(12.0f)) / (view.pxPerFrame * Dpi());
    float maxScrollFrame = std::max(0.0f, (float)(maxFrame + 120) - visibleFrames);
    float contentHeightAll = rows.size() * rowHeight;
    float maxScrollY = std::max(0.0f, contentHeightAll - contentH);

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, imguiId, &hovered, &held, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                                                                     ImGuiButtonFlags_PressedOnClick);
    // The wheel scrolls/zooms the timeline, not the host window.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);

    // Middle pan
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(2, 0.0f)) {
        view.scrollFrame -= io.MouseDelta.x / (view.pxPerFrame * Dpi());
        view.scrollY -= io.MouseDelta.y;
    }

    // Wheel
    if (hovered && io.MouseWheel != 0.0f) {
        if (mousePos.x > keyAreaX) {
            if (io.KeyCtrl) {
                float oldPx = view.pxPerFrame;
                view.pxPerFrame = std::clamp(view.pxPerFrame * (io.MouseWheel > 0 ? 1.15f : (1.0f / 1.15f)), 1.0f, 40.0f);
                float mouseFrame = view.scrollFrame + (mousePos.x - originX) / (oldPx * Dpi());
                view.scrollFrame = mouseFrame - (mousePos.x - originX) / (view.pxPerFrame * Dpi());
            } else if (io.KeyShift) {
                view.scrollFrame -= io.MouseWheel * 20.0f * (6.0f / view.pxPerFrame);
            } else {
                view.scrollY -= io.MouseWheel * rowHeight;
            }
        } else {
            view.scrollY -= io.MouseWheel * rowHeight;
        }
    }

    bool leftDown = ImGui::IsMouseDown(0);
    bool leftReleased = ImGui::IsMouseReleased(0);

    if (pressed && io.MouseClicked[0]) {
        if (mousePos.y >= bb.Min.y && mousePos.y < contentY && mousePos.x >= keyAreaX) {
            view.dragMode = 1;
        } else if (mousePos.x < keyAreaX && mousePos.y >= contentY) {
            int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                if (rows[rowIndex].isGroup) {
                    ev.toggleGroup = true;
                    ev.toggledRow = rows[rowIndex].id;
                    changed = true;
                }
            }
        } else if (mousePos.x >= keyAreaX && mousePos.y >= contentY) {
            int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            bool hitKey = false;
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                const auto& r = rows[rowIndex];
                if (r.keysEditable) {
                    float rowY = contentY - view.scrollY + rowIndex * rowHeight + rowHeight * 0.5f;
                    float bestDistSq = Dp(7.0f) * Dp(7.0f);
                    const TimelineKey* bestKey = nullptr;
                    for (const auto& k : r.keys) {
                        float kx = originX + (k.frame - view.scrollFrame) * view.pxPerFrame * Dpi();
                        float dx = mousePos.x - kx;
                        float dy = mousePos.y - rowY;
                        float distSq = dx * dx + dy * dy;
                        if (distSq <= bestDistSq) {
                            bestDistSq = distSq;
                            bestKey = &k;
                        }
                    }
                    if (bestKey) {
                        hitKey = true;
                        view.anchorRow = r.id;
                        view.anchorFrame = bestKey->frame;
                        bool isAlreadySelected = bestKey->selected;
                        ImGui::GetStateStorage()->SetBool(imguiId, isAlreadySelected);
                        bool emitOnPress = !(isAlreadySelected && !io.KeyCtrl && !io.KeyShift);
                        if (emitOnPress) {
                            ev.select = true;
                            ev.selectMode = io.KeyCtrl ? SelectMode::Toggle : (io.KeyShift ? SelectMode::Add : SelectMode::Replace);
                            ev.selectKeys.push_back({r.id, bestKey->frame});
                            changed = true;
                        }
                        view.dragMode = 2;
                        view.dragDelta = 0;
                    }
                }
            }
            if (!hitKey) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                        const auto& r = rows[rowIndex];
                        if (r.keysEditable && !r.isGroup) {
                            ev.addKeyAt = true;
                            ev.addKeyRow = r.id;
                            ev.addKeyFrame = (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame);
                            changed = true;
                        }
                    }
                } else {
                    view.dragMode = 3;
                    view.boxStart = mousePos;
                }
            }
        }
    }

    // ButtonBehavior clears the active id in the release frame, so drags are tracked by view.dragMode.
    if ((held || ImGui::IsItemActive()) && leftDown) {
        if (view.dragMode == 1) {
            ev.seek = true;
            ev.seekFrame = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            changed = true;
        } else if (view.dragMode == 2) {
            float dx = mousePos.x - io.MouseClickedPos[0].x;
            if (std::abs(dx) >= Dp(3.0f)) {
                view.dragDelta = (int)std::round(dx / (view.pxPerFrame * Dpi()));
                int minFrame = GetMinSelectedFrame(rows);
                if (minFrame + view.dragDelta < 0) {
                    view.dragDelta = -minFrame;
                }
            } else {
                view.dragDelta = 0;
            }
        }
    }

    if (leftReleased && view.dragMode != 0) {
        if (view.dragMode == 1) {
            // done
        } else if (view.dragMode == 2) {
            if (view.dragDelta != 0) {
                ev.moveKeys = true;
                ev.moveDelta = view.dragDelta;
                changed = true;
            } else {
                bool wasAlreadySelected = ImGui::GetStateStorage()->GetBool(imguiId);
                if (wasAlreadySelected && !io.KeyCtrl && !io.KeyShift) {
                    ev.select = true;
                    ev.selectMode = SelectMode::Replace;
                    ev.selectKeys.push_back({view.anchorRow, view.anchorFrame});
                    changed = true;
                }
            }
        } else if (view.dragMode == 3) {
            ev.select = true;
            ev.selectMode = (io.KeyShift || io.KeyCtrl) ? SelectMode::Add : SelectMode::Replace;
            float bx0 = std::min(view.boxStart.x, mousePos.x);
            float bx1 = std::max(view.boxStart.x, mousePos.x);
            float by0 = std::min(view.boxStart.y, mousePos.y);
            float by1 = std::max(view.boxStart.y, mousePos.y);
            
            for (size_t ri = 0; ri < rows.size(); ++ri) {
                const auto& r = rows[ri];
                if (!r.keysEditable) continue;
                float rowY = contentY - view.scrollY + ri * rowHeight + rowHeight * 0.5f;
                if (rowY >= by0 && rowY <= by1) {
                    for (const auto& k : r.keys) {
                        float kx = originX + (k.frame - view.scrollFrame) * view.pxPerFrame * Dpi();
                        if (kx >= bx0 && kx <= bx1) {
                            ev.selectKeys.push_back({r.id, k.frame});
                        }
                    }
                }
            }
            changed = true;
        }
        view.dragMode = 0;
    }
    
    if (!leftDown) {
        if (view.dragMode == 1 || view.dragMode == 2 || view.dragMode == 3) view.dragMode = 0;
    }

    if (ImGui::IsWindowFocused() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            ev.deleteKeys = true; changed = true;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
            ev.copyKeys = true; changed = true;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
            ev.pasteKeys = true; changed = true;
        }
    }

    view.scrollFrame = std::clamp(view.scrollFrame, 0.0f, maxScrollFrame);
    view.scrollY = std::clamp(view.scrollY, 0.0f, maxScrollY);

    dl->PushClipRect(bb.Min, bb.Max, true);
    
    // Draw rows
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& r = rows[ri];
        float rowY = contentY - view.scrollY + ri * rowHeight;
        if (rowY + rowHeight < bb.Min.y || rowY > bb.Max.y) continue;

        ImVec2 rmin(bb.Min.x, rowY);
        ImVec2 rmax(bb.Max.x, rowY + rowHeight);

        ImU32 rowBg = (ri % 2 == 0) ? 0 : WithAlpha(p.sunken, 0.5f);
        if (r.isGroup) rowBg = p.sunken;
        if (rowBg) dl->AddRectFilled(rmin, rmax, rowBg);

        float labelX = bb.Min.x + Dp(4.0f) + r.depth * Dp(14.0f);
        if (r.isGroup) {
            Icon(dl, r.expanded ? icon::CaretDown : icon::CaretRight, 13.0f, ImVec2(labelX + Dp(8.0f), rowY + rowHeight * 0.5f), p.ink);
            labelX += Dp(16.0f);
        }
        TextEllipsis(dl, r.isGroup ? Font::Semibold : Font::Regular, size::Small,
                     ImVec2(labelX, rowY + (rowHeight - Dp(size::Small * 1.25f)) * 0.5f), keyAreaX - Dp(4.0f), p.ink, r.label.c_str());

        dl->AddLine(ImVec2(rmin.x, rmax.y), ImVec2(rmax.x, rmax.y), p.line);
    }
    
    // Ruler
    dl->AddRectFilled(bb.Min, ImVec2(bb.Max.x, contentY), p.surface);
    dl->AddLine(ImVec2(bb.Min.x, contentY), ImVec2(bb.Max.x, contentY), p.lineStrong);

    float minTickPx = Dp(60.0f);
    int steps[] = {1, 2, 5, 10, 15, 30, 60, 150, 300, 900, 1800};
    int tickStep = 1800;
    for (int step : steps) {
        if (step * view.pxPerFrame * Dpi() >= minTickPx) {
            tickStep = step;
            break;
        }
    }
    
    int startFrame = std::max(0, (int)std::floor(view.scrollFrame));
    int endFrame = startFrame + (int)std::ceil(keyAreaW / (view.pxPerFrame * Dpi())) + 1;
    
    dl->PushClipRect(ImVec2(keyAreaX, bb.Min.y), bb.Max, true);
    for (int f = (startFrame / tickStep) * tickStep; f <= endFrame; f += tickStep) {
        float x = originX + (f - view.scrollFrame) * view.pxPerFrame * Dpi();
        if (x < keyAreaX || x > bb.Max.x) continue;
        
        dl->AddLine(ImVec2(x, contentY - Dp(6.0f)), ImVec2(x, contentY), p.ink3);
        
        char fstr[32]; snprintf(fstr, sizeof(fstr), "%d", f);
        char sstr[32]; 
        int secs = f / 30;
        int frames = f % 30;
        if (secs >= 60) snprintf(sstr, sizeof(sstr), "%d:%02d.%02d", secs / 60, secs % 60, (frames * 100) / 30);
        else snprintf(sstr, sizeof(sstr), "%d.%02ds", secs, (frames * 100) / 30);
        
        Text(dl, Font::Regular, size::Caption, ImVec2(std::round(x + Dp(4.0f)), std::round(bb.Min.y + Dp(2.0f))), p.ink, fstr);
        Text(dl, Font::Regular, size::Caption - 2.0f, ImVec2(std::round(x + Dp(4.0f)), std::round(bb.Min.y + Dp(14.0f))), p.ink3, sstr);
    }
    
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(keyAreaX, contentY), bb.Max, true);
    auto drawDiamond = [&](float x, float y, ImU32 col, float radius) {
        ImVec2 pts[4] = {
            ImVec2(x, y - radius), ImVec2(x + radius, y),
            ImVec2(x, y + radius), ImVec2(x - radius, y)
        };
        dl->AddConvexPolyFilled(pts, 4, col);
    };
    
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& r = rows[ri];
        float rowY = contentY - view.scrollY + ri * rowHeight + rowHeight * 0.5f;
        if (rowY + rowHeight * 0.5f < contentY || rowY - rowHeight * 0.5f > bb.Max.y) continue;
        
        for (const auto& k : r.keys) {
            float kx = originX + (k.frame - view.scrollFrame) * view.pxPerFrame * Dpi();
            if (kx < keyAreaX - Dp(10.0f) || kx > bb.Max.x + Dp(10.0f)) continue;
            
            float radius = r.isGroup ? Dp(4.0f) : Dp(6.5f);
            ImU32 col = p.ink2;
            if (!r.keysEditable) col = WithAlpha(p.ink3, 0.8f);
            else if (k.selected) col = p.accent;
            
            const bool dragged = k.selected && r.keysEditable && view.dragMode == 2 && view.dragDelta != 0;
            if (!dragged) drawDiamond(std::round(kx), std::round(rowY), col, radius);

            if (dragged) {
                float gx = originX + (k.frame + view.dragDelta - view.scrollFrame) * view.pxPerFrame * Dpi();
                drawDiamond(std::round(gx), std::round(rowY), p.accent, radius);
                ImVec2 pts[4] = {
                    ImVec2(std::round(kx), std::round(rowY - radius)), ImVec2(std::round(kx + radius), std::round(rowY)),
                    ImVec2(std::round(kx), std::round(rowY + radius)), ImVec2(std::round(kx - radius), std::round(rowY))
                };
                dl->AddPolyline(pts, 4, WithAlpha(p.accent, 0.5f), ImDrawFlags_Closed, Dp(1.0f));
            }
        }
    }
    
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(keyAreaX, bb.Min.y), bb.Max, true);
    float phx = std::round(originX + (currentFrame - view.scrollFrame) * view.pxPerFrame * Dpi());
    if (phx >= keyAreaX && phx <= bb.Max.x) {
        dl->AddLine(ImVec2(phx, bb.Min.y), ImVec2(phx, bb.Max.y), p.accent, std::max(1.0f, std::round(Dp(1.5f))));
        ImVec2 hpts[3] = {
            ImVec2(phx - Dp(5.0f), bb.Min.y), ImVec2(phx + Dp(5.0f), bb.Min.y),
            ImVec2(phx, bb.Min.y + Dp(6.0f))
        };
        dl->AddConvexPolyFilled(hpts, 3, p.accent);
    }
    
    if (view.dragMode == 3) {
        dl->AddRectFilled(view.boxStart, mousePos, WithAlpha(p.accent, 0.2f));
        dl->AddRect(view.boxStart, mousePos, p.accent);
    }
    
    dl->PopClipRect();
    
    dl->AddLine(ImVec2(keyAreaX, bb.Min.y), ImVec2(keyAreaX, bb.Max.y), p.lineStrong);
    
    if (maxScrollY > 0) {
        float indH = std::max(Dp(20.0f), contentH * (contentH / contentHeightAll));
        float indY = contentY + (view.scrollY / maxScrollY) * (contentH - indH);
        dl->AddRectFilled(ImVec2(bb.Max.x - Dp(4.0f), indY), ImVec2(bb.Max.x, indY + indH), WithAlpha(p.ink3, 0.5f), Dp(2.0f));
    }
    
    dl->PopClipRect();
    
    return changed;
}

} // namespace mmdx::studio
