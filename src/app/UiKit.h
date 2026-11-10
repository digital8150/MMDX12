#pragma once
// MMDX12 UI kit on top of Dear ImGui: light / dark theme tokens, Pretendard + Phosphor fonts, and
// the custom widgets every screen uses (buttons, segmented controls, switches, cards...).
// Sizes are given in unscaled pixels and multiplied by the monitor DPI internally.
#include "imgui.h"
#include <cstdint>
#include <filesystem>
#include <string>

namespace mmdx::ui {

struct Palette {
    ImU32 bg, surface, sunken, line, lineStrong;
    ImU32 ink, ink2, ink3;
    ImU32 accent, accentHover, accentPress, accentInk, accentSoft, onAccent;
    ImU32 danger, dangerSoft, warn, warnSoft;
    ImU32 knob;   // switch / slider knobs (white on light, a light grey on dark so off states still read)
};
const Palette& P();
// Colour theme. P() always returns the live palette: switching rewrites it in place (call ApplyStyle
// afterwards so the ImGui style colours follow; App::ApplyTheme does both).
enum class Theme { Light = 0, Dark = 1 };
void SetTheme(Theme t);
Theme CurrentTheme();
inline bool IsDark() { return CurrentTheme() == Theme::Dark; }
ImU32 WithAlpha(ImU32 c, float a);  // replaces the alpha
ImU32 Mix(ImU32 a, ImU32 b, float t);

enum class Font { Regular = 0, Semibold = 1, Bold = 2 };
namespace size {
inline constexpr float Caption = 12.5f, Small = 13.5f, Body = 14.5f, Title = 17.0f, Heading = 22.0f,
                       Hero = 30.0f, Display = 64.0f;
}

// Loads Pretendard (+ Phosphor icons, + system JP/CN fallbacks). `assetsDir` holds fonts/.
bool LoadFonts(const std::filesystem::path& assetsDir);
void ApplyStyle(float dpi);
void SetDpi(float dpi);
float Dpi();
inline float Dp(float v) { return v * Dpi(); }
void PushFont(Font f, float sizePx);
void PopFont();
ImVec2 TextSize(Font f, float sizePx, const char* text, const char* end = nullptr);

// Per-frame time for animations (call once per frame before drawing).
void NewFrame(float dt);
// Smoothly animated 0..1 value bound to an id (rises toward `target` with ~120 ms ease).
float Anim(ImGuiID id, bool target, float speed = 10.0f);

// --- drawing primitives -------------------------------------------------------------------
void SoftShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float spread, float alpha,
                ImVec2 offset = ImVec2(0, 0));
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float elevation = 1.0f);
// Frosted panel: blurred scene backdrop (tex, mapped through `sceneRect` x,y,w,h) under a
// translucent white veil. Falls back to an opaque surface when tex == 0.
void FrostedPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImTextureID tex, const float sceneRect[4],
                  float veil = 0.72f);
void Text(ImDrawList* dl, Font f, float sizePx, ImVec2 pos, ImU32 col, const char* text, const char* end = nullptr);
// Single line, clipped with an ellipsis at maxX.
void TextEllipsis(ImDrawList* dl, Font f, float sizePx, ImVec2 pos, float maxX, ImU32 col, const char* text);
void Icon(ImDrawList* dl, const char* glyph, float sizePx, ImVec2 center, ImU32 col);
void Skeleton(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding);  // loading shimmer

// --- widgets (ImGui items: advance the cursor, support hover/active/disabled/nav) ----------
enum class ButtonKind { Primary, Secondary, Ghost, Danger };
bool Button(const char* id, const char* label, const char* icon = nullptr, ButtonKind kind = ButtonKind::Secondary,
            ImVec2 size = ImVec2(0, 0));
bool IconButton(const char* id, const char* icon, const char* tooltip, bool active = false, float sizePx = 36.0f);
// Pill segmented control; returns true when the selection changed. `disabled` (optional, one per label) greys out
// individual options: clicking them does nothing. `tooltips` (optional) shows on hover of the option.
bool Segmented(const char* id, const char* const* labels, int count, int* current, float widthPx = 0.0f,
               float heightPx = 36.0f, const char* const* icons = nullptr, const bool* disabled = nullptr,
               const char* const* tooltips = nullptr);
// Label on the left, switch on the right, full available width.
bool Switch(const char* id, const char* label, bool* v, const char* hint = nullptr);
bool SliderRow(const char* id, const char* label, float* v, float vmin, float vmax, const char* fmt);
// Swatch + three labelled channel fields + a picker popup (RGB / HSV / OKLab tabs, hex input, old/new preview).
// `rgb` holds the display values exactly as stored (sRGB 0..1; no conversion of stored data). `hdr` keeps values
// unclamped above 1. `activated` / `deactivated` report a gesture starting / ending on any inner control or the
// popup picker (for key-edit capture), independent of ImGui's last-item state.
bool ColorField(const char* id, float rgb[3], bool* activated = nullptr, bool* deactivated = nullptr, bool hdr = false);
bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint, float widthPx);
bool TextField(const char* id, const char* label, char* buf, size_t bufSize, float widthPx, const char* hint = nullptr);
// Choice chip with an icon (lighting presets etc.).
bool Chip(const char* id, const char* label, const char* icon, bool selected, float widthPx = 0.0f);
// Full-width popup menu row: icon, label, optional right-aligned hint (shortcut / caret). Returns true when clicked.
bool MenuItem(const char* id, const char* label, const char* icon = nullptr, const char* hint = nullptr);
// Card hit area (screen rect a..b): an invisible item so hover / click / nav work like any widget. `label` names it
// for automation (MCP ui_click): the card's visible title.
bool CardItem(const char* id, ImVec2 a, ImVec2 b, bool* hovered, const char* label = nullptr);
// After-Effects style stopwatch ("애니메이션 켜기"): icon button, accent when `animated`, p.ink3 otherwise.
// Tooltip "애니메이션 켜기" / "애니메이션 끄기 (키프레임 모두 삭제)". True when clicked.
bool Stopwatch(const char* id, bool animated, float sizePx = 22.0f);
// Key navigation cluster ◀ ◆ ▶ for one track: returns -1 (jump to the previous key), 1 (next key), 2 (toggle the key
// at the playhead) or 0 (nothing). `hasPrev` / `hasNext` disable the carets when there is no such key; the diamond is
// filled accent when `keyedHere` (a key at the playhead).
int KeyNav(const char* id, bool hasPrev, bool keyedHere, bool hasNext, float sizePx = 22.0f);
void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float fraction);
void Badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg, ImVec2* outSize = nullptr);
void Tooltip(const char* text);  // after an item, when hovered
// After an edited item: its gesture ended (a drag released, a typed value entered or cancelled, another field of the
// same group taken over). A drag field turns into its text input on a plain click (io.ConfigDragClickToInputText),
// and in that frame ImGui reports the item as deactivated although the same id is active again: not an end, while
// ImGui::IsItemDeactivated() alone ends the edit before the value is typed. A group (DragFloat3) deactivated and
// activated anew in one frame moved to another of its fields: an end.
inline bool ItemEditEnded() {
    return ImGui::IsItemDeactivated() && (!ImGui::IsItemActive() || ImGui::IsItemActivated());
}
void Gap(float px);              // vertical spacing
void SectionLabel(const char* text);

// Full-window host for a screen (no padding, background = bg).
void BeginScreen(const char* id);
void EndScreen();

} // namespace mmdx::ui
