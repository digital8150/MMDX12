#pragma once
#include "imgui.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mmdx::studio {

// The caller inserts this many consecutive "lane rows" right after a row whose graph is open; the widget draws
// one value / speed graph lane across their combined height. Row heights stay fixed (no variable rows).
// A block = the kTimelineGraphRows lane rows of one (parent, channel): the value sub-lane is 2.5 rows tall and the
// speed sub-lane 1.5 rows.
inline constexpr int kTimelineGraphRows = 4;
// `graphChannelIndex` of the "all channels" row (listed first among the channel rows): one easing curve for every channel.
inline constexpr int kTimelineGraphAllChannels = 64;

struct TimelineKey {
    int frame = 0;
    bool selected = false;
    // Optional short text drawn to the right of the key (the camera switch row: the camera's name). Empty = none.
    std::string label;
};

// One channel offered by a row's graph lane (its name is the label of its channel sub-row, its curve colour the row's
// dot and the curve).
struct TimelineGraphChannel {
    std::string label;   // UTF-8
    ImU32 color = 0;
};

struct TimelineRow {
    uint64_t id = 0;            // unique per row, stable across frames
    std::string label;          // UTF-8 internal key (track name; VMD/VPD-safe). Never localised.
    std::string displayLabel;   // optional UI-only label (e.g. the PMX English bone name); empty = use `label`
    int depth = 0;              // indent level (0 = top)
    bool isGroup = false;       // group rows have a caret and show their children's keys summarised
    bool expanded = true;       // only meaningful for groups; the CALLER hides collapsed children (do not pass them in)
    bool keysEditable = true;   // false: keys are drawn dimmed and cannot be selected or moved (summary rows)
    bool selected = false;      // row picked on the label column: label cell drawn highlighted
    ImU32 tint = 0;             // track-type marker: a thin bar at the label's left edge (0 = none)
    bool hasStopwatch = false;
    bool animated = false;
    std::vector<TimelineKey> keys;  // sorted by frame ascending
    // Graph lane (read-only value / speed curves). Non-empty graphChannels = this row offers a graph; the caller
    // inserts one channel sub-row per channel and, under every channel whose lane is open, kTimelineGraphRows lane
    // rows. A channel row sets graphOpen while ITS lane is shown.
    std::vector<TimelineGraphChannel> graphChannels;
    bool graphOpen = false;      // caller state: the channel rows below this row (parent) / this channel's lane is shown
    bool graphCurvesDiffer = false;  // the all-channels lane: the segment's channels hold different curves (a drag unifies them)
    uint64_t graphOf = 0;        // channel / lane rows: parent row id (0 = not a child row)
    int graphIndex = 0;          // lane rows: 0..kTimelineGraphRows-1 within the block
    int graphLaneChannel = -1;   // lane rows: the channel of their block (-1 = not a lane row,
                                 // kTimelineGraphAllChannels = the "all channels" block)
    int graphChannelIndex = -1;  // channel rows: >= 0 = the channel this row stands for (-1 = not a channel row,
                                 // kTimelineGraphAllChannels = the "all channels" row)
    ImU32 dotColor = 0;          // channel rows: the channel's colour dot (0 = none)
    // Editable segment of the lane (refreshed by the caller every frame): the frames of the two keys whose arriving
    // segment the handles edit (-1 = none). Shared by every lane block of the row.
    int graphSegFrom = -1, graphSegTo = -1;
    // The curve bytes of that segment, one per channel (index into graphChannels; the caller fills them each frame).
    // The "all channels" block draws / edits channel 0's bytes; the widget keeps them in step with its handle drag.
    std::vector<std::array<uint8_t, 4>> graphCurves;
};

// UI state owned by the caller (persisted between frames).
struct TimelineView {
    float pxPerFrame = 6.0f;    // 0.2 .. 40.0 (unscaled px; multiply by ui::Dp inside)
    float scrollFrame = 0.0f;   // frame at the left edge of the key area, >= 0
    float scrollY = 0.0f;       // vertical scroll in screen px
    int rangeStart = -1, rangeEnd = -1;  // frame range (inclusive), -1 = none. Shift+drag on the ruler sets it,
                                         // right click on the ruler asks for the host's menu (ev.contextOnRuler). The caller may also write it.
    // transient drag state, written/read only by the widget:
    int dragMode = 0;           // 0 none, 1 scrub, 2 move keys, 3 box select, 4 range select, 5 horizontal scrollbar,
                                // 6 audio clip
    int dragDelta = 0;          // frames the dragged selection is currently offset by (move preview)
    uint64_t anchorRow = 0; int anchorFrame = 0;  // key under the mouse when a move drag started
    int rangeAnchor = 0;                 // frame where the range drag started
    int dragMinFrame = 0;                // smallest selected key frame, computed once when a key drag starts
    float hscrollGrab = 0.0f;            // mouse x offset inside the scrollbar thumb while dragging it
    float dragGrabFrame = 0.0f;          // frame under the mouse when a key move drag started (content coordinates)
    float boxAnchorFrame = 0.0f;         // box select anchor as a frame (content coordinates: stays put while the view auto-scrolls)
    float boxAnchorY = 0.0f;             // box select anchor as content px below the rows area's top (content coordinates)
    uint64_t scrollToRow = 0;            // caller writes a row id: the widget scrolls it into view on the next call, then resets it to 0 (0 = no request; unknown id is just reset)
    int graphDrag = 0;                   // transient handle drag: 0 none, 1 = P1, 2 = P2
    uint64_t graphDragRow = 0;           // the dragged block's first lane row id
    bool graphDragMoved = false;         // ... and the pointer moved since the press (a plain click edits nothing)
    // Set by the caller each frame before calling Timeline(): value of `row`'s `channel` at a fractional frame
    // (the caller owns the interpretation). Used only to draw the graph lanes.
    std::function<float(uint64_t row, int channel, float frame)> graphSample;

    // Optional audio lane: a fixed strip between the ruler and the rows (it does not scroll with them) showing the
    // track as a clip from its offset to its end, with its waveform. Dragging the clip moves the offset (whole frames,
    // snapping to the playhead and frame 0; Shift = free). The caller fills these every frame.
    bool audioLane = false;                 // draw the lane
    const char* audioLabel = nullptr;       // UTF-8 track name (label column)
    const float* audioWave = nullptr;       // (peak, rms) pairs 0..1, interleaved; nullptr = not decoded (yet)
    int audioWaveBuckets = 0;               // pair count
    float audioWaveRate = 200.0f;           // pairs per second of audio
    float audioDurationSeconds = 0.0f;      // track length
    float audioOffsetSeconds = 0.0f;        // the audio starts at this timeline time (may be negative)
    float audioFps = 30.0f;                 // timeline frames per second
    bool audioLocked = false;               // the position is fixed: the clip does not drag (the label's lock button
                                            // toggles it through `TimelineEvents::audioLockToggle`)
    // transient clip drag state (dragMode 6), written/read only by the widget:
    float audioDragFromOffset = 0.0f;       // offset when the drag started
    bool audioDragMoved = false;            // the pointer moved past the drag threshold
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
    bool stopwatch = false; uint64_t stopwatchRow = 0;
    bool keyNav = false; uint64_t keyNavRow = 0; int keyNavDir = 0;
    bool rangeChanged = false;                         // view.rangeStart/rangeEnd were changed by the widget this frame
    bool contextMenu = false; int contextFrame = 0; uint64_t contextRow = 0;  // right click in the key area (row 0 = none)
    bool contextOnRuler = false;                       // the context menu event came from the ruler: contextRow = 0, contextFrame = frame under the mouse
    bool toggleGraph = false; uint64_t toggleGraphRow = 0;   // the graph icon of a parent row was clicked
    bool graphChannel = false; uint64_t graphChannelRow = 0; int graphChannelIndex = 0;  // a channel row's label was clicked (parent row + index): that channel's lane toggles
    // a lane handle (P1 / P2) was dragged or double clicked: the new bytes of the segment's curve
    // (`graphCurveChannel` = the block's channel, kTimelineGraphAllChannels = every channel)
    bool graphCurve = false; uint64_t graphCurveRow = 0; int graphCurveChannel = 0; int graphCurveFrame = 0;
    uint8_t graphCurveBytes[4] = {};
    bool graphCurveEnd = false;   // the handle drag ended this frame (one undo step)
    // audio lane: the clip is being dragged (`audioOffsetSeconds` = the live offset, sent when it changes); the drag
    // ended after moving (`audioOffsetEnd`: one undo step from `audioOffsetFrom`); right click on the lane / click on
    // its label asks for the host's audio menu
    bool audioOffset = false; float audioOffsetSeconds = 0.0f;
    bool audioOffsetEnd = false; float audioOffsetFrom = 0.0f;
    bool audioMenu = false;
    bool audioLockToggle = false;                      // the lock button in the lane's label was clicked
};

// Draws the timeline into the current ImGui window at the cursor, filling `size` (x or y <= 0 means "remaining space").
//   label column on the left (width 220 unscaled px, rows 24 px tall, ruler 28 px tall),
//   key area on the right. `currentFrame` draws the playhead; `maxFrame` limits horizontal scrolling (plus a margin of 120 frames).
//   Features: label clicks, Shift+drag range on the ruler, right click on the ruler asks for the host's range menu (ev.contextOnRuler: range start here / range end here / clear range), right click on the keys asks for the host's key menu, wheel on the ruler zooms, scrollbar at the bottom.
// Returns true if any event flag in `ev` is set.
bool Timeline(const char* id, ImVec2 size, const std::vector<TimelineRow>& rows, int currentFrame, int maxFrame,
              TimelineView& view, TimelineEvents& ev);

} // namespace mmdx::studio
