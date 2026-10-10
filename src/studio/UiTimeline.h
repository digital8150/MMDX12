#pragma once
#include "imgui.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mmdx::studio {

struct TimelineKey {
    int frame = 0;
    bool selected = false;
};

struct TimelineRow {
    uint64_t id = 0;            // unique per row, stable across frames
    std::string label;          // UTF-8
    int depth = 0;              // indent level (0 = top)
    bool isGroup = false;       // group rows have a caret and show their children's keys summarised
    bool expanded = true;       // only meaningful for groups; the CALLER hides collapsed children (do not pass them in)
    bool keysEditable = true;   // false: keys are drawn dimmed and cannot be selected or moved (summary rows)
    bool selected = false;      // row picked on the label column: label cell drawn highlighted
    ImU32 tint = 0;             // track-type marker: a thin bar at the label's left edge (0 = none)
    std::vector<TimelineKey> keys;  // sorted by frame ascending
};

// UI state owned by the caller (persisted between frames).
struct TimelineView {
    float pxPerFrame = 6.0f;    // 0.2 .. 40.0 (unscaled px; multiply by ui::Dp inside)
    float scrollFrame = 0.0f;   // frame at the left edge of the key area, >= 0
    float scrollY = 0.0f;       // vertical scroll in screen px
    int rangeStart = -1, rangeEnd = -1;  // frame range (inclusive), -1 = none. Shift+drag on the ruler sets it,
                                         // right click on the ruler clears it. The caller may also write it.
    // transient drag state, written/read only by the widget:
    int dragMode = 0;           // 0 none, 1 scrub, 2 move keys, 3 box select, 4 range select, 5 horizontal scrollbar
    int dragDelta = 0;          // frames the dragged selection is currently offset by (move preview)
    ImVec2 boxStart{0, 0};      // box select anchor in screen px
    uint64_t anchorRow = 0; int anchorFrame = 0;  // key under the mouse when a move drag started
    int rangeAnchor = 0;                 // frame where the range drag started
    int dragMinFrame = 0;                // smallest selected key frame, computed once when a key drag starts
    float hscrollGrab = 0.0f;            // mouse x offset inside the scrollbar thumb while dragging it
};

struct TimelineKeyRef { uint64_t row; int frame; };

enum class SelectMode { Replace, Add, Toggle };
enum class RowClickMode { Replace, Toggle, Range };  // plain, Ctrl, Shift click

// What happened this frame. Zero-initialised by the widget at the start of each call.
struct TimelineEvents {
    bool seek = false; int seekFrame = 0;             // ruler click/drag: new current frame (>= 0)
    bool toggleGroup = false; uint64_t toggledRow = 0; // caret clicked
    bool select = false; SelectMode selectMode = SelectMode::Replace;
    std::vector<TimelineKeyRef> selectKeys;            // keys hit by the click / box (empty + Replace = clear selection)
    bool moveKeys = false; int moveDelta = 0;          // committed on mouse release: move ALL selected keys by this many frames (never 0)
    bool addKeyAt = false; uint64_t addKeyRow = 0; int addKeyFrame = 0;  // double click on an empty spot of an editable, non-group row
    bool rowClick = false; uint64_t rowClickId = 0; RowClickMode rowClickMode = RowClickMode::Replace;  // label clicked
    bool rangeChanged = false;                         // view.rangeStart/rangeEnd were changed by the widget this frame
    bool contextMenu = false; int contextFrame = 0; uint64_t contextRow = 0;  // right click in the key area (row 0 = none)
};

// Draws the timeline into the current ImGui window at the cursor, filling `size` (x or y <= 0 means "remaining space").
//   label column on the left (width 220 unscaled px, rows 24 px tall, ruler 28 px tall),
//   key area on the right. `currentFrame` draws the playhead; `maxFrame` limits horizontal scrolling (plus a margin of 120 frames).
//   Features: label clicks, Shift+drag range on the ruler, right click on the ruler clears the range, right click on the keys asks for the host's key menu, wheel on the ruler zooms, scrollbar at the bottom.
// Returns true if any event flag in `ev` is set.
bool Timeline(const char* id, ImVec2 size, const std::vector<TimelineRow>& rows, int currentFrame, int maxFrame,
              TimelineView& view, TimelineEvents& ev);

} // namespace mmdx::studio
