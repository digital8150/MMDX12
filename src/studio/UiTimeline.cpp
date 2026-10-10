#include "UiTimeline.h"
#include "app/UiKit.h"
#include "app/Icons.h"
#include "imgui_internal.h"
#include "ui_probe/UiProbe.h"
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
    const float sbH = Dp(10.0f);

    float keyAreaX = bb.Min.x + colWidth;
    float keyAreaW = std::max(0.0f, bb.Max.x - keyAreaX);
    const float originX = keyAreaX + Dp(12.0f);  // x of frame `scrollFrame` (inset keeps frame-0 keys whole)
    float contentY = bb.Min.y + rulerHeight;
    float contentH = std::max(0.0f, bb.Max.y - sbH - contentY);

    float visibleFrames = (keyAreaW - Dp(12.0f)) / (view.pxPerFrame * Dpi());
    float maxScrollFrame = std::max(0.0f, (float)(maxFrame + 120) - visibleFrames);
    float contentHeightAll = rows.size() * rowHeight;
    float maxScrollY = std::max(0.0f, contentHeightAll - contentH);

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, imguiId, &hovered, &held, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight |
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
            if (io.KeyCtrl || mousePos.y < contentY) {
                float oldPx = view.pxPerFrame;
                view.pxPerFrame = std::clamp(view.pxPerFrame * (io.MouseWheel > 0 ? 1.15f : (1.0f / 1.15f)), 0.2f, 40.0f);
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

    // The key of row `rowIndex` under the mouse (7 px pick radius), or nullptr.
    const auto keyUnderMouse = [&](int rowIndex) -> const TimelineKey* {
        const TimelineRow& r = rows[rowIndex];
        const float rowY = contentY - view.scrollY + rowIndex * rowHeight + rowHeight * 0.5f;
        float bestDistSq = Dp(7.0f) * Dp(7.0f);
        const TimelineKey* bestKey = nullptr;
        const float f0 = view.scrollFrame + (mousePos.x - Dp(7.0f) - originX) / (view.pxPerFrame * Dpi());
        const float f1 = view.scrollFrame + (mousePos.x + Dp(7.0f) - originX) / (view.pxPerFrame * Dpi());
        auto it = std::lower_bound(r.keys.begin(), r.keys.end(), (int)std::floor(f0),
                                   [](const TimelineKey& a, int b) { return a.frame < b; });
        for (; it != r.keys.end() && it->frame <= std::ceil(f1); ++it) {
            const float kx = originX + (it->frame - view.scrollFrame) * view.pxPerFrame * Dpi();
            const float dx = mousePos.x - kx, dy = mousePos.y - rowY;
            const float distSq = dx * dx + dy * dy;
            if (distSq <= bestDistSq) {
                bestDistSq = distSq;
                bestKey = &(*it);
            }
        }
        return bestKey;
    };

    bool leftDown = ImGui::IsMouseDown(0);
    bool leftReleased = ImGui::IsMouseReleased(0);

    if (pressed && io.MouseClicked[0]) {
        if (mousePos.x >= keyAreaX && mousePos.y >= bb.Max.y - sbH) {
            int T = maxFrame + 120;
            float V = visibleFrames;
            if (T > V) {
                float tw = std::max(Dp(24.0f), keyAreaW * V / T);
                float tx = keyAreaX + (view.scrollFrame / maxScrollFrame) * (keyAreaW - tw);
                if (mousePos.x >= tx && mousePos.x <= tx + tw) {
                    view.dragMode = 5;
                    view.hscrollGrab = mousePos.x - tx;
                } else {
                    view.scrollFrame = std::clamp(((mousePos.x - tw * 0.5f - keyAreaX) / (keyAreaW - tw)) * maxScrollFrame, 0.0f, maxScrollFrame);
                    view.dragMode = 5;
                    view.hscrollGrab = tw * 0.5f;
                }
            }
        } else if (mousePos.y >= bb.Min.y && mousePos.y < contentY && mousePos.x >= keyAreaX) {
            if (io.KeyShift) {
                view.dragMode = 4;
                view.rangeAnchor = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
                view.rangeStart = view.rangeEnd = view.rangeAnchor;
                ev.rangeChanged = true;
                changed = true;
            } else {
                view.dragMode = 1;
            }
        } else if (mousePos.x < keyAreaX && mousePos.y >= contentY && mousePos.y < bb.Max.y - sbH) {
            int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                const auto& row = rows[rowIndex];
                if (row.isGroup && mousePos.x < bb.Min.x + Dp(4.0f) + row.depth * Dp(14.0f) + Dp(18.0f)) {
                    ev.toggleGroup = true;
                    ev.toggledRow = row.id;
                    changed = true;
                } else {
                    ev.rowClick = true;
                    ev.rowClickId = row.id;
                    ev.rowClickMode = io.KeyShift ? RowClickMode::Range : (io.KeyCtrl ? RowClickMode::Toggle : RowClickMode::Replace);
                    changed = true;
                    if (ImGui::IsMouseDoubleClicked(0) && row.isGroup) {
                        ev.toggleGroup = true;
                        ev.toggledRow = row.id;
                    }
                }
            }
        } else if (mousePos.x >= keyAreaX && mousePos.y >= contentY && mousePos.y < bb.Max.y - sbH) {
            int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            bool hitKey = false;
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                const auto& r = rows[rowIndex];
                if (r.keysEditable) {
                    const TimelineKey* bestKey = keyUnderMouse(rowIndex);
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
                            view.dragMinFrame = std::min(GetMinSelectedFrame(rows), bestKey->frame);
                        } else {
                            view.dragMinFrame = GetMinSelectedFrame(rows);
                        }
                        if (view.dragMinFrame == 2147483647) view.dragMinFrame = 0;
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

    if (pressed && io.MouseClicked[1]) {
        if (mousePos.x >= keyAreaX && mousePos.y >= contentY && mousePos.y < bb.Max.y - sbH) {
            // key area: the host's key menu; a key under the mouse that is not selected becomes the selection
            const int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            ev.contextMenu = true;
            ev.contextFrame = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                ev.contextRow = rows[rowIndex].id;
                if (rows[rowIndex].keysEditable)
                    if (const TimelineKey* k = keyUnderMouse(rowIndex); k && !k->selected) {
                        ev.select = true;
                        ev.selectMode = SelectMode::Replace;
                        ev.selectKeys.push_back({rows[rowIndex].id, k->frame});
                    }
            }
            changed = true;
        } else if (mousePos.y >= bb.Min.y && mousePos.y < contentY && mousePos.x >= keyAreaX) {
            if (view.rangeStart != -1 || view.rangeEnd != -1) {
                view.rangeStart = -1;
                view.rangeEnd = -1;
                ev.rangeChanged = true;
                changed = true;
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
                if (view.dragMinFrame + view.dragDelta < 0) {
                    view.dragDelta = -view.dragMinFrame;
                }
            } else {
                view.dragDelta = 0;
            }
        } else if (view.dragMode == 4) {
            int f = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            int newStart = std::min(view.rangeAnchor, f);
            int newEnd = std::max(view.rangeAnchor, f);
            if (newStart != view.rangeStart || newEnd != view.rangeEnd) {
                view.rangeStart = newStart;
                view.rangeEnd = newEnd;
                ev.rangeChanged = true;
                changed = true;
            }
        } else if (view.dragMode == 5) {
            int T = maxFrame + 120;
            float V = visibleFrames;
            float tw = std::max(Dp(24.0f), keyAreaW * V / T);
            view.scrollFrame = std::clamp(((mousePos.x - view.hscrollGrab - keyAreaX) / (keyAreaW - tw)) * maxScrollFrame, 0.0f, maxScrollFrame);
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
            
            int firstRow = std::max(0, (int)std::floor((by0 - contentY + view.scrollY) / rowHeight));
            int lastRow = std::min((int)rows.size() - 1, (int)((by1 - contentY + view.scrollY) / rowHeight));
            float f0 = view.scrollFrame + (bx0 - originX) / (view.pxPerFrame * Dpi());
            float f1 = view.scrollFrame + (bx1 - originX) / (view.pxPerFrame * Dpi());
            
            for (int ri = firstRow; ri <= lastRow; ++ri) {
                const auto& r = rows[ri];
                if (!r.keysEditable) continue;
                float rowY = contentY - view.scrollY + ri * rowHeight + rowHeight * 0.5f;
                if (rowY >= by0 && rowY <= by1) {
                    auto it = std::lower_bound(r.keys.begin(), r.keys.end(), (int)std::floor(f0),
                        [](const TimelineKey& a, int b) { return a.frame < b; });
                    for (; it != r.keys.end() && it->frame <= std::ceil(f1); ++it) {
                        float kx = originX + (it->frame - view.scrollFrame) * view.pxPerFrame * Dpi();
                        if (kx >= bx0 && kx <= bx1) {
                            ev.selectKeys.push_back({r.id, it->frame});
                        }
                    }
                }
            }
            changed = true;
        }
        view.dragMode = 0;
    }
    
    if (!leftDown) {
        if (view.dragMode >= 1 && view.dragMode <= 5) view.dragMode = 0;
    }

    view.scrollFrame = std::clamp(view.scrollFrame, 0.0f, maxScrollFrame);
    view.scrollY = std::clamp(view.scrollY, 0.0f, maxScrollY);

    dl->PushClipRect(bb.Min, bb.Max, true);
    
    int firstRow = std::max(0, (int)std::floor(view.scrollY / rowHeight));
    int lastRow = std::min((int)rows.size() - 1, (int)((view.scrollY + contentH) / rowHeight) + 1);
    
    // Draw rows
    for (int ri = firstRow; ri <= lastRow; ++ri) {
        const auto& r = rows[ri];
        float rowY = contentY - view.scrollY + ri * rowHeight;

        ImVec2 rmin(bb.Min.x, rowY);
        ImVec2 rmax(bb.Max.x, rowY + rowHeight);

        ImU32 rowBg = (ri % 2 == 0) ? 0 : WithAlpha(p.sunken, 0.5f);
        if (r.isGroup) rowBg = p.sunken;
        if (rowBg) dl->AddRectFilled(rmin, rmax, rowBg);
        
        if (r.selected) {
            dl->AddRectFilled(rmin, ImVec2(keyAreaX, rmax.y), p.accentSoft);
        }

        if (r.tint) dl->AddRectFilled(ImVec2(rmin.x, rmin.y + Dp(4.0f)), ImVec2(rmin.x + Dp(3.0f), rmax.y - Dp(4.0f)), r.tint, Dp(1.5f));
        float labelX = bb.Min.x + Dp(4.0f) + r.depth * Dp(14.0f);
        if (r.isGroup) {
            Icon(dl, r.expanded ? icon::CaretDown : icon::CaretRight, 13.0f, ImVec2(labelX + Dp(8.0f), rowY + rowHeight * 0.5f), p.ink);
            labelX += Dp(16.0f);
        }
        ImU32 textCol = r.selected ? p.accentInk : p.ink;
        TextEllipsis(dl, r.isGroup ? Font::Semibold : Font::Regular, size::Small,
                     ImVec2(labelX, rowY + (rowHeight - Dp(size::Small * 1.25f)) * 0.5f), keyAreaX - Dp(4.0f), textCol, r.label.c_str());

        dl->AddLine(ImVec2(rmin.x, rmax.y), ImVec2(rmax.x, rmax.y), p.line);
        if (uiprobe::Enabled()) uiprobe::Add("##tlrow", r.label.c_str(), rmin, ImVec2(keyAreaX, rmax.y));   // MCP ui_click
    }
    
    // Ruler
    dl->AddRectFilled(bb.Min, ImVec2(bb.Max.x, contentY), p.surface);
    dl->AddLine(ImVec2(bb.Min.x, contentY), ImVec2(bb.Max.x, contentY), p.lineStrong);

    if (view.rangeStart >= 0 && view.rangeEnd >= view.rangeStart) {
        float xa = originX + (view.rangeStart - view.scrollFrame) * view.pxPerFrame * Dpi() - 0.5f * view.pxPerFrame * Dpi();
        float xb = originX + (view.rangeEnd - view.scrollFrame) * view.pxPerFrame * Dpi() + 0.5f * view.pxPerFrame * Dpi();
        float cxa = std::max(keyAreaX, xa);
        float cxb = std::min(bb.Max.x, xb);
        if (cxa <= cxb) {
            dl->AddRectFilled(ImVec2(cxa, bb.Min.y), ImVec2(cxb, contentY), WithAlpha(p.accent, 0.22f));
            dl->AddRectFilled(ImVec2(cxa, contentY), ImVec2(cxb, bb.Max.y - sbH), WithAlpha(p.accent, 0.07f));
        }
        if (xa >= keyAreaX && xa <= bb.Max.x) {
            dl->AddLine(ImVec2(xa, bb.Min.y), ImVec2(xa, bb.Max.y - sbH), WithAlpha(p.accent, 0.6f));
        }
        if (xb >= keyAreaX && xb <= bb.Max.x) {
            dl->AddLine(ImVec2(xb, bb.Min.y), ImVec2(xb, bb.Max.y - sbH), WithAlpha(p.accent, 0.6f));
        }
    }

    float minTickPx = Dp(60.0f);
    int steps[] = {1, 2, 5, 10, 15, 30, 60, 150, 300, 900, 1800, 3600, 9000, 18000};
    int tickStep = 18000;
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
    
    float f0 = view.scrollFrame - 12.0f / std::max(view.pxPerFrame, 0.01f);
    float f1 = view.scrollFrame + keyAreaW / (view.pxPerFrame * Dpi()) + 2.0f;
    for (int ri = firstRow; ri <= lastRow; ++ri) {
        const auto& r = rows[ri];
        float rowY = contentY - view.scrollY + ri * rowHeight + rowHeight * 0.5f;
        
        auto it = std::lower_bound(r.keys.begin(), r.keys.end(), (int)std::floor(f0),
            [](const TimelineKey& a, int b) { return a.frame < b; });
        for (; it != r.keys.end() && it->frame <= std::ceil(f1); ++it) {
            const auto& k = *it;
            float kx = originX + (k.frame - view.scrollFrame) * view.pxPerFrame * Dpi();
            
            float radius = r.isGroup ? std::min(Dp(4.0f), std::max(Dp(2.0f), view.pxPerFrame * Dpi() * 0.7f)) 
                                     : std::min(Dp(6.5f), std::max(Dp(2.5f), view.pxPerFrame * Dpi() * 1.1f));
            ImU32 col = p.ink2;
            if (!r.keysEditable) col = WithAlpha(p.ink3, 0.8f);
            else if (k.selected) col = p.accent;
            
            const bool dragged = k.selected && r.keysEditable && view.dragMode == 2 && view.dragDelta != 0;
            if (!dragged) drawDiamond(std::round(kx), std::round(rowY), col, radius);
            if (uiprobe::Enabled() && !r.isGroup) {   // MCP: "##tlkey:<row>:<frame>"
                const std::string kid = "##tlkey:" + r.label + ":" + std::to_string(k.frame);
                uiprobe::Add(kid.c_str(), r.label.c_str(), ImVec2(kx - radius, rowY - radius), ImVec2(kx + radius, rowY + radius));
            }

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
    
    int T = maxFrame + 120;
    float V = visibleFrames;
    dl->AddRectFilled(ImVec2(keyAreaX, bb.Max.y - sbH), bb.Max, WithAlpha(p.sunken, 1.0f));
    if (T > V) {
        float tw = std::max(Dp(24.0f), keyAreaW * V / T);
        float tx = keyAreaX + (view.scrollFrame / maxScrollFrame) * (keyAreaW - tw);
        bool isThumbHovered = hovered && mousePos.x >= tx && mousePos.x <= tx + tw && mousePos.y >= bb.Max.y - sbH && mousePos.y <= bb.Max.y;
        dl->AddRectFilled(ImVec2(tx, bb.Max.y - sbH + Dp(2.0f)), ImVec2(tx + tw, bb.Max.y - Dp(2.0f)), 
                          WithAlpha(p.ink3, (view.dragMode == 5 || isThumbHovered) ? 0.85f : 0.55f), Dp(4.0f));
    }
    
    dl->PopClipRect();
    
    return changed;
}

} // namespace mmdx::studio
