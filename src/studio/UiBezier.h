#pragma once
#include "imgui.h"
#include <cstdint>

namespace mmdx::studio {

// Interpolation curve editor for one VMD channel. `c` = {x1, y1, x2, y2}, each 0..127, the two control points of a cubic Bezier
// from (0,0) to (127,127) (same definition as mmdx::Bezier::FromBytes in src/anim/Motion.h: P1=(x1,y1)/127, P2=(x2,y2)/127).
// Draws a square plot of the given edge length (unscaled px, multiplied by ui::Dp internally) with a grid, the curve, the two handles
// (circles joined by lines to the end points) that can be dragged with the mouse (snapped to integers, clamped to 0..127),
// followed by a row of preset buttons and four numeric drag fields (x1 y1 x2 y2). Returns true when `c` changed this frame.
bool BezierCurveEditor(const char* id, uint8_t c[4], float plotSize, const uint8_t (*ghosts)[4], int ghostCount, const ImU32* ghostColors);
inline bool BezierCurveEditor(const char* id, uint8_t c[4], float plotSize = 180.0f) {
    return BezierCurveEditor(id, c, plotSize, nullptr, 0, nullptr);
}
ImU32 BezierChannelColor(int channel);  // 0 X red, 1 Y green, 2 Z blue, 3 rotation amber, 4 distance violet, 5 fov grey

// Presets used by the buttons (also exposed for callers): returns the 4 bytes of preset `index`.
// 0 Linear {20,20,107,107}  1 Ease in {42,0,108,64}  2 Ease out {20,70,87,127}  3 Ease in-out {64,0,64,127}  4 Fast then slow {0,64,64,127}  (index out of range -> Linear)
void BezierPreset(int index, uint8_t out[4]);
inline constexpr int kBezierPresetCount = 5;
extern const char* const kBezierPresetNames[kBezierPresetCount];  // Korean I18n keys (pass through mmdx::Tr)

} // namespace mmdx::studio
