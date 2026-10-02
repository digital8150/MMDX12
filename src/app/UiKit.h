#pragma once
// MMDX12 UI kit on top of Dear ImGui: light theme tokens, Pretendard + Phosphor fonts, and
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
};
const Palette& P();
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
// Pill segmented control; returns true when the selection changed.
bool Segmented(const char* id, const char* const* labels, int count, int* current, float widthPx = 0.0f,
               float heightPx = 36.0f, const char* const* icons = nullptr);
// Label on the left, switch on the right, full available width.
bool Switch(const char* id, const char* label, bool* v, const char* hint = nullptr);
bool SliderRow(const char* id, const char* label, float* v, float vmin, float vmax, const char* fmt);
bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint, float widthPx);
bool TextField(const char* id, const char* label, char* buf, size_t bufSize, float widthPx, const char* hint = nullptr);
// Choice chip with an icon (lighting presets etc.).
bool Chip(const char* id, const char* label, const char* icon, bool selected, float widthPx = 0.0f);
void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float fraction);
void Badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg, ImVec2* outSize = nullptr);
void Tooltip(const char* text);  // after an item, when hovered
void Gap(float px);              // vertical spacing
void SectionLabel(const char* text);

// Full-window host for a screen (no padding, background = bg).
void BeginScreen(const char* id);
void EndScreen();

} // namespace mmdx::ui
