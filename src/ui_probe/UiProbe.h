#pragma once
// Widget registry for automation (MCP ui_items / ui_click): every ImGui item of the last frame with its rect, window
// and label. Fed by Dear ImGui's test-engine hooks (IMGUI_ENABLE_TEST_ENGINE, defined for the imgui target) plus
// UiKit's hand-drawn widgets, which report their id string and visible text through Info(). Off unless enabled.
// UiProbe.cpp is compiled into the imgui library so every target linking imgui has the hook symbols.
#include "imgui.h"
#include <string>
#include <vector>

namespace mmdx::uiprobe {

struct Item {
    ImGuiID id = 0;
    ImVec2 min, max;      // screen pixels
    std::string window;   // the owning window's name (popups: "##Popup_...")
    std::string label;    // visible text (localized; the part before "##")
    std::string idStr;    // the id string (language independent: "##offcancel", "play", ...)
    bool visible = true;  // overlaps its window's clip rect
    bool disabled = false;
    bool isWindow = false;  // the window itself (its title / whole rect), not a widget
};

// Turns the hooks on / off for the current ImGui context (call after ImGui::CreateContext).
void SetEnabled(bool on);
bool Enabled();
// Call right before ImGui::NewFrame: the items gathered during the previous frame become LastFrame().
void BeginFrame();
// Label / id string of an item added this frame (UiKit widgets; stock widgets report through the hooks).
void Info(ImGuiID id, const char* idStr, const char* label);
// A drawn-only element (no ImGui item: timeline rows / keys) in the current window, so automation can find it.
void Add(const char* idStr, const char* label, ImVec2 min, ImVec2 max);
const std::vector<Item>& LastFrame();
// True when another window (a modal, popup, a window drawn on top) is the topmost one under the item's centre,
// i.e. a click there would not reach the item. Uses the window order of the last frame.
bool Covered(const Item& item);

} // namespace mmdx::uiprobe
