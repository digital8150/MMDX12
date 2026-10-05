#include "app/UiKit.h"
#include "app/Icons.h"
#include "core/I18n.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace mmdx::ui {

namespace {

ImU32 Hex(uint32_t rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, (int)(a * 255.0f + 0.5f));
}

Palette MakePalette() {
    Palette p{};
    p.bg = Hex(0xF3F5F7);
    p.surface = Hex(0xFFFFFF);
    p.sunken = Hex(0xEBEEF1);
    p.line = Hex(0xE2E6EA);
    p.lineStrong = Hex(0xCBD2D9);
    p.ink = Hex(0x13171C);
    p.ink2 = Hex(0x4C5560);
    p.ink3 = Hex(0x7D8792);
    p.accent = Hex(0x39C5BB);        // Hatsune Miku
    p.accentHover = Hex(0x2FB5AB);
    p.accentPress = Hex(0x26A198);
    p.accentInk = Hex(0x0A7F78);     // accent as text/icon on light surfaces (4.8:1 on white)
    p.accentSoft = Hex(0xE2F5F3);
    p.onAccent = Hex(0x052B28);      // label on the accent fill (8.7:1)
    p.danger = Hex(0xC7383F);
    p.dangerSoft = Hex(0xFBE9EA);
    p.warn = Hex(0x9A6400);
    p.warnSoft = Hex(0xFBF1DD);
    return p;
}

const Palette g_palette = MakePalette();
ImFont* g_fonts[3] = {};
float g_dpi = 1.0f;
float g_dt = 1.0f / 60.0f;
double g_time = 0.0;

ImGuiStorage& AnimStorage() {
    static ImGuiStorage s;
    return s;
}

bool IsDisabled() { return (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0; }

} // namespace

const Palette& P() { return g_palette; }

ImU32 WithAlpha(ImU32 c, float a) {
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int s) {
        const float x = (float)((a >> s) & 0xFF), y = (float)((b >> s) & 0xFF);
        return (ImU32)(x + (y - x) * t + 0.5f) << s;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

// ---- fonts / style ---------------------------------------------------------------------

bool LoadFonts(const std::filesystem::path& assetsDir) {
    ImGuiIO& io = ImGui::GetIO();
    const std::filesystem::path fontDir = assetsDir / L"fonts";
    const char* files[3] = {"Pretendard-Regular.otf", "Pretendard-SemiBold.otf", "Pretendard-Bold.otf"};
    const std::string icons = (fontDir / L"Phosphor.ttf").string();
    const std::string iconsFill = (fontDir / L"Phosphor-Fill.ttf").string();
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        const std::string path = (fontDir / files[i]).string();
        // Pretendard ships a few Private Use Area glyphs; leave that range to the icon font.
        static const ImWchar kExcludePua[] = {0xE000, 0xF8FF, 0};
        ImFontConfig base;
        base.GlyphExcludeRanges = kExcludePua;
        ImFont* f = std::filesystem::exists(path) ? io.Fonts->AddFontFromFileTTF(path.c_str(), size::Body, &base) : nullptr;
        if (!f) {
            ok = false;
            f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", size::Body);
            if (!f) f = io.Fonts->AddFontDefault();
        }
        ImFontConfig merge;
        merge.MergeMode = true;
        // Icons: regular weight for body text, filled glyphs for the bold face.
        const std::string& iconFile = i == 2 ? iconsFill : icons;
        if (std::filesystem::exists(iconFile)) {
            ImFontConfig ic = merge;
            ic.GlyphOffset = ImVec2(0, 1.5f);
            io.Fonts->AddFontFromFileTTF(iconFile.c_str(), size::Body, &ic);
        }
        // Japanese / Chinese UI text and CJK names in asset titles: bundled Noto Sans CJK. The face of the
        // active language goes first so shared Han characters use its glyph forms (switching to the other
        // forms fully takes effect after a restart). System fonts are the fallback.
        const bool zhFirst = ActiveLanguage() == Language::Chinese;
        bool haveNoto = false;
        for (int k = 0; k < 2; ++k) {
            const bool sc = (k == 0) == zhFirst;
            const std::string noto = (fontDir / (std::string(sc ? "NotoSansCJKsc-" : "NotoSansCJKjp-") +
                                                 (i == 0 ? "Regular.otf" : "Bold.otf"))).string();
            if (!std::filesystem::exists(noto)) continue;
            io.Fonts->AddFontFromFileTTF(noto.c_str(), size::Body, &merge);
            haveNoto = true;
        }
        if (haveNoto) {
        } else if (std::filesystem::exists(L"C:\\Windows\\Fonts\\YuGothM.ttc"))
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\YuGothM.ttc", size::Body, &merge);
        else if (std::filesystem::exists(L"C:\\Windows\\Fonts\\msgothic.ttc"))
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msgothic.ttc", size::Body, &merge);
        if (std::filesystem::exists(L"C:\\Windows\\Fonts\\msyh.ttc"))
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", size::Body, &merge);
        if (std::filesystem::exists(L"C:\\Windows\\Fonts\\malgun.ttf"))
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", size::Body, &merge);
        g_fonts[i] = f;
    }
    io.FontDefault = g_fonts[0];
    return ok;
}

void SetDpi(float dpi) { g_dpi = dpi > 0.1f ? dpi : 1.0f; }
float Dpi() { return g_dpi; }

void ApplyStyle(float dpi) {
    SetDpi(dpi);
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsLight(&s);
    s.WindowPadding = ImVec2(0, 0);
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildRounding = 0;
    s.ChildBorderSize = 0;
    s.PopupRounding = 12;
    s.PopupBorderSize = 1;
    s.FrameRounding = 10;
    s.FramePadding = ImVec2(12, 9);
    s.ItemSpacing = ImVec2(8, 8);
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 8;
    s.ScrollbarPadding = 2;
    s.GrabRounding = 8;
    s.GrabMinSize = 12;
    s.FrameBorderSize = 0;
    s.ImageRounding = 0;
    const Palette& p = P();
    auto v4 = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = v4(p.ink);
    c[ImGuiCol_TextDisabled] = v4(p.ink3);
    c[ImGuiCol_WindowBg] = v4(p.bg);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v4(p.surface);
    c[ImGuiCol_Border] = v4(p.line);
    c[ImGuiCol_FrameBg] = v4(p.sunken);
    c[ImGuiCol_FrameBgHovered] = v4(Mix(p.sunken, p.lineStrong, 0.35f));
    c[ImGuiCol_FrameBgActive] = v4(Mix(p.sunken, p.lineStrong, 0.5f));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v4(WithAlpha(p.ink3, 0.35f));
    c[ImGuiCol_ScrollbarGrabHovered] = v4(WithAlpha(p.ink3, 0.55f));
    c[ImGuiCol_ScrollbarGrabActive] = v4(WithAlpha(p.ink3, 0.75f));
    c[ImGuiCol_CheckMark] = v4(p.accentInk);
    c[ImGuiCol_SliderGrab] = v4(p.accent);
    c[ImGuiCol_SliderGrabActive] = v4(p.accentPress);
    c[ImGuiCol_TextSelectedBg] = v4(WithAlpha(p.accent, 0.35f));
    c[ImGuiCol_NavCursor] = v4(p.accentInk);
    c[ImGuiCol_InputTextCursor] = v4(p.accentInk);
    c[ImGuiCol_Header] = v4(p.accentSoft);
    c[ImGuiCol_HeaderHovered] = v4(Mix(p.accentSoft, p.accent, 0.15f));
    c[ImGuiCol_HeaderActive] = v4(Mix(p.accentSoft, p.accent, 0.3f));
    c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = v4(WithAlpha(p.sunken, 0.5f));
    c[ImGuiCol_TableBorderLight] = v4(p.line);
    c[ImGuiCol_TableBorderStrong] = v4(p.line);
    s.ScaleAllSizes(dpi);
    s.FontScaleDpi = dpi;
    s.FontSizeBase = size::Body;
}

void PushFont(Font f, float sizePx) { ImGui::PushFont(g_fonts[(int)f], sizePx); }
void PopFont() { ImGui::PopFont(); }

ImVec2 TextSize(Font f, float sizePx, const char* text, const char* end) {
    PushFont(f, sizePx);
    ImVec2 s = ImGui::CalcTextSize(text, end);
    PopFont();
    return s;
}

void NewFrame(float dt) {
    g_dt = std::clamp(dt, 0.0f, 0.1f);
    g_time += g_dt;
}

float Anim(ImGuiID id, bool target, float speed) {
    ImGuiStorage& st = AnimStorage();
    float v = st.GetFloat(id, target ? 1.0f : 0.0f);
    const float goal = target ? 1.0f : 0.0f;
    v += (goal - v) * (1.0f - std::exp(-speed * g_dt * 1.6f));
    if (std::fabs(goal - v) < 0.002f) v = goal;
    st.SetFloat(id, v);
    return v;
}

// ---- primitives --------------------------------------------------------------------------

void SoftShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float spread, float alpha, ImVec2 offset) {
    const int steps = 7;
    const ImU32 base = IM_COL32(18, 52, 58, 255);  // shadows tinted toward the teal-grey ground
    for (int i = steps; i >= 1; --i) {
        const float t = (float)i / steps;
        const float e = spread * t;
        const float k = alpha * (1.0f - t) * (1.0f - t) * 2.2f / steps + alpha * 0.06f / steps;
        dl->AddRectFilled(ImVec2(a.x - e + offset.x, a.y - e + offset.y), ImVec2(b.x + e + offset.x, b.y + e + offset.y),
                          WithAlpha(base, k), rounding + e);
    }
}

void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float elevation) {
    if (elevation > 0.0f) SoftShadow(dl, a, b, rounding, Dp(14.0f) * elevation, 0.10f * elevation, ImVec2(0, Dp(4.0f) * elevation));
    dl->AddRectFilled(a, b, P().surface, rounding);
    dl->AddRect(a, b, WithAlpha(P().line, 0.9f), rounding, 0, 1.0f);
}

void FrostedPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImTextureID tex, const float r[4], float veil) {
    SoftShadow(dl, a, b, rounding, Dp(22.0f), 0.18f, ImVec2(0, Dp(8.0f)));
    if (tex && r[2] > 0 && r[3] > 0) {
        const ImVec2 uv0((a.x - r[0]) / r[2], (a.y - r[1]) / r[3]);
        const ImVec2 uv1((b.x - r[0]) / r[2], (b.y - r[1]) / r[3]);
        dl->AddImageRounded(ImTextureRef(tex), a, b, uv0, uv1, IM_COL32_WHITE, rounding);
        dl->AddRectFilled(a, b, WithAlpha(P().surface, veil), rounding);
    } else {
        dl->AddRectFilled(a, b, WithAlpha(P().surface, 0.94f), rounding);
    }
    dl->AddRect(a, b, WithAlpha(IM_COL32_WHITE, 0.7f), rounding, 0, 1.0f);
}

void Text(ImDrawList* dl, Font f, float sizePx, ImVec2 pos, ImU32 col, const char* text, const char* end) {
    PushFont(f, sizePx);
    dl->AddText(ImVec2(std::round(pos.x), std::round(pos.y)), col, text, end);
    PopFont();
}

void TextEllipsis(ImDrawList* dl, Font f, float sizePx, ImVec2 pos, float maxX, ImU32 col, const char* text) {
    PushFont(f, sizePx);
    const float h = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, ImVec2(std::round(pos.x), std::round(pos.y)), ImVec2(maxX, pos.y + h * 1.4f), maxX,
                              text, nullptr, nullptr);
    ImGui::PopStyleColor();
    PopFont();
}

void Icon(ImDrawList* dl, const char* glyph, float sizePx, ImVec2 center, ImU32 col) {
    PushFont(Font::Regular, sizePx);
    const ImVec2 s = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(std::round(center.x - s.x * 0.5f), std::round(center.y - s.y * 0.5f)), col, glyph);
    PopFont();
}

void Skeleton(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    dl->AddRectFilled(a, b, P().sunken, rounding);
    const float w = b.x - a.x;
    const float band = std::max(w * 0.45f, Dp(60.0f));
    const float t = (float)std::fmod(g_time * 0.9, 1.6) / 1.6f;
    const float x = a.x - band + (w + band * 2.0f) * t;
    dl->PushClipRect(a, b, true);
    const ImU32 c0 = WithAlpha(IM_COL32_WHITE, 0.0f), c1 = WithAlpha(IM_COL32_WHITE, 0.55f);
    dl->AddRectFilledMultiColor(ImVec2(x, a.y), ImVec2(x + band * 0.5f, b.y), c0, c1, c1, c0);
    dl->AddRectFilledMultiColor(ImVec2(x + band * 0.5f, a.y), ImVec2(x + band, b.y), c1, c0, c0, c1);
    dl->PopClipRect();
}

void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float fraction) {
    const float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, P().sunken, r);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction > 0.0f) {
        const float x = a.x + std::max((b.x - a.x) * fraction, r * 2.0f);
        dl->AddRectFilled(a, ImVec2(x, b.y), P().accent, r);
    }
}

void Badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg, ImVec2* outSize) {
    const ImVec2 ts = TextSize(Font::Semibold, size::Caption, text);
    const ImVec2 sz(ts.x + Dp(16.0f), ts.y + Dp(6.0f));
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), bg, sz.y * 0.5f);
    Text(dl, Font::Semibold, size::Caption, ImVec2(pos.x + Dp(8.0f), pos.y + Dp(3.0f)), fg, text);
    if (outSize) *outSize = sz;
}

// ---- widgets -----------------------------------------------------------------------------

bool Button(const char* id, const char* label, const char* icon, ButtonKind kind, ImVec2 size) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID gid = w->GetID(id);
    const float fs = size::Body;
    const ImVec2 ls = label && *label ? TextSize(Font::Semibold, fs, label) : ImVec2(0, 0);
    const float iconW = icon ? Dp(18.0f) : 0.0f;
    const float gap = icon && label && *label ? Dp(8.0f) : 0.0f;
    ImVec2 sz(size.x > 0 ? Dp(size.x) : (size.x < 0 ? ImGui::GetContentRegionAvail().x : Dp(32.0f) + iconW + gap + ls.x),
              size.y > 0 ? Dp(size.y) : Dp(40.0f));
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + sz.x, w->DC.CursorPos.y + sz.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    const bool disabled = IsDisabled();
    if (hovered && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Anim(gid, hovered && !disabled);

    const Palette& p = P();
    ImU32 bg = 0, fg = p.ink, border = 0;
    switch (kind) {
    case ButtonKind::Primary:
        bg = held ? p.accentPress : Mix(p.accent, p.accentHover, h);
        fg = p.onAccent;
        break;
    case ButtonKind::Secondary:
        bg = held ? Mix(p.surface, p.sunken, 1.0f) : Mix(p.surface, p.sunken, h * 0.7f);
        fg = p.ink;
        border = p.line;
        break;
    case ButtonKind::Ghost:
        bg = WithAlpha(p.sunken, (held ? 1.0f : h) * 0.9f);
        fg = p.ink2;
        break;
    case ButtonKind::Danger:
        bg = Mix(p.dangerSoft, WithAlpha(p.danger, 1.0f), held ? 0.25f : h * 0.12f);
        fg = p.danger;
        break;
    }
    if (disabled) {
        bg = kind == ButtonKind::Primary ? Mix(p.sunken, p.lineStrong, 0.2f) : bg;
        fg = p.ink3;
    }
    ImDrawList* dl = w->DrawList;
    const float r = Dp(10.0f);
    const ImVec2 off(0, held && !disabled ? Dp(1.0f) : 0.0f);
    if (kind == ButtonKind::Primary && !disabled)
        SoftShadow(dl, bb.Min, bb.Max, r, Dp(10.0f), 0.10f + 0.08f * h, ImVec2(0, Dp(3.0f)));
    dl->AddRectFilled(ImVec2(bb.Min.x + off.x, bb.Min.y + off.y), ImVec2(bb.Max.x + off.x, bb.Max.y + off.y), bg, r);
    if (border) dl->AddRect(bb.Min, bb.Max, border, r);
    const float contentW = iconW + gap + ls.x;
    float x = std::round(bb.Min.x + (sz.x - contentW) * 0.5f);
    const float cy = bb.Min.y + sz.y * 0.5f + off.y;
    if (icon) {
        Icon(dl, icon, Dp(18.0f) / Dpi() * 1.0f, ImVec2(x + iconW * 0.5f, cy), fg);
        x += iconW + gap;
    }
    if (label && *label) Text(dl, Font::Semibold, fs, ImVec2(x, cy - ls.y * 0.5f), fg, label);
    ImGui::RenderNavCursor(bb, gid);
    return pressed && !disabled;
}

bool IconButton(const char* id, const char* icon, const char* tooltip, bool active, float sizePx) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID gid = w->GetID(id);
    const float s = Dp(sizePx);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + s, w->DC.CursorPos.y + s));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    const bool disabled = IsDisabled();
    if (hovered && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Anim(gid, hovered && !disabled);
    const Palette& p = P();
    ImDrawList* dl = w->DrawList;
    const float r = s * 0.5f;
    if (active)
        dl->AddRectFilled(bb.Min, bb.Max, Mix(p.accentSoft, p.accent, held ? 0.25f : h * 0.12f), r);
    else if (h > 0.01f || held)
        dl->AddRectFilled(bb.Min, bb.Max, WithAlpha(p.ink, held ? 0.10f : 0.06f * h), r);
    const ImU32 fg = disabled ? p.ink3 : (active ? p.accentInk : Mix(p.ink2, p.ink, h));
    Icon(dl, icon, sizePx * 0.56f, ImVec2(bb.Min.x + s * 0.5f, bb.Min.y + s * 0.5f), fg);
    ImGui::RenderNavCursor(bb, gid);
    if (tooltip && hovered) Tooltip(tooltip);
    return pressed && !disabled;
}

bool Segmented(const char* id, const char* const* labels, int count, int* current, float widthPx, float heightPx,
               const char* const* icons) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems || count <= 0) return false;
    ImGui::PushID(id);
    const Palette& p = P();
    const float pad = Dp(4.0f);
    const float h = Dp(heightPx);
    float segW[16] = {};
    float total = 0;
    for (int i = 0; i < count && i < 16; ++i) {
        const ImVec2 ts = TextSize(Font::Semibold, size::Small, labels[i]);
        segW[i] = ts.x + Dp(28.0f) + (icons && icons[i] ? Dp(22.0f) : 0.0f);
        total += segW[i];
    }
    float width = widthPx > 0 ? Dp(widthPx) : (widthPx < 0 ? ImGui::GetContentRegionAvail().x : total + pad * 2.0f);
    if (widthPx != 0) {
        const float each = (width - pad * 2.0f) / count;
        for (int i = 0; i < count; ++i) segW[i] = each;
    }
    const ImVec2 origin = w->DC.CursorPos;
    const ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
    ImGui::ItemSize(bb);
    ImGui::ItemAdd(bb, 0);
    ImDrawList* dl = w->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, p.sunken, h * 0.5f);

    // sliding thumb
    float x = origin.x + pad;
    float selX = x, selW = segW[0];
    for (int i = 0; i < count; ++i) {
        if (i == *current) {
            selX = x;
            selW = segW[i];
        }
        x += segW[i];
    }
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID kx = ImGui::GetID("##thumbx"), kw = ImGui::GetID("##thumbw");
    float tx = st->GetFloat(kx, selX - origin.x), tw = st->GetFloat(kw, selW);
    const float k = 1.0f - std::exp(-18.0f * g_dt);
    tx += (selX - origin.x - tx) * k;
    tw += (selW - tw) * k;
    st->SetFloat(kx, tx);
    st->SetFloat(kw, tw);
    const ImVec2 ta(origin.x + tx, origin.y + pad), tb(origin.x + tx + tw, origin.y + h - pad);
    SoftShadow(dl, ta, tb, (h - pad * 2) * 0.5f, Dp(5.0f), 0.10f, ImVec2(0, Dp(1.0f)));
    dl->AddRectFilled(ta, tb, p.surface, (h - pad * 2) * 0.5f);

    bool changed = false;
    x = origin.x + pad;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        const ImGuiID sid = ImGui::GetID("seg");
        const ImRect sb(ImVec2(x, origin.y), ImVec2(x + segW[i], origin.y + h));
        ImGui::ItemAdd(sb, sid);
        bool hovered = false, held = false;
        if (ImGui::ButtonBehavior(sb, sid, &hovered, &held) && *current != i) {
            *current = i;
            changed = true;
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool sel = i == *current;
        const ImU32 fg = sel ? p.ink : (hovered ? p.ink : p.ink2);
        const ImVec2 ts = TextSize(Font::Semibold, size::Small, labels[i]);
        const float iw = icons && icons[i] ? Dp(22.0f) : 0.0f;
        const float cx = x + (segW[i] - ts.x - iw) * 0.5f;
        const float cy = origin.y + h * 0.5f;
        if (iw > 0) Icon(dl, icons[i], 15.0f, ImVec2(cx + Dp(8.0f), cy), sel ? p.accentInk : fg);
        Text(dl, Font::Semibold, size::Small, ImVec2(cx + iw, cy - ts.y * 0.5f), fg, labels[i]);
        ImGui::RenderNavCursor(sb, sid);
        ImGui::PopID();
        x += segW[i];
    }
    ImGui::PopID();
    return changed;
}

bool Switch(const char* id, const char* label, bool* v, const char* hint) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID gid = w->GetID(id);
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = Dp(hint ? 44.0f : 34.0f);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + width, w->DC.CursorPos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    const bool disabled = IsDisabled();
    if (pressed && !disabled) *v = !*v;
    if (hovered && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const Palette& p = P();
    ImDrawList* dl = w->DrawList;
    const float on = Anim(gid, *v, 14.0f);
    // label
    const ImVec2 ls = TextSize(Font::Regular, size::Body, label);
    const float labelY = hint ? bb.Min.y + Dp(4.0f) : bb.Min.y + (h - ls.y) * 0.5f;
    Text(dl, Font::Regular, size::Body, ImVec2(bb.Min.x, labelY), disabled ? p.ink3 : p.ink, label);
    if (hint) Text(dl, Font::Regular, size::Caption, ImVec2(bb.Min.x, labelY + ls.y + Dp(1.0f)), p.ink3, hint);
    // switch
    const float sw = Dp(38.0f), sh = Dp(22.0f);
    const ImVec2 sa(bb.Max.x - sw, bb.Min.y + (h - sh) * 0.5f), sb(bb.Max.x, sa.y + sh);
    dl->AddRectFilled(sa, sb, Mix(Mix(p.lineStrong, p.ink3, hovered ? 0.3f : 0.0f), p.accent, on), sh * 0.5f);
    const float kr = sh * 0.5f - Dp(2.5f);
    const ImVec2 kc(sa.x + sh * 0.5f + (sw - sh) * on, sa.y + sh * 0.5f);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + Dp(1.0f)), kr + Dp(0.5f), WithAlpha(IM_COL32(10, 40, 40, 255), 0.18f), 24);
    dl->AddCircleFilled(kc, kr, p.surface, 24);
    ImGui::RenderNavCursor(bb, gid);
    return pressed && !disabled;
}

bool SliderRow(const char* id, const char* label, float* v, float vmin, float vmax, const char* fmt) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const Palette& p = P();
    ImDrawList* dl = w->DrawList;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = w->DC.CursorPos;
    char value[32];
    std::snprintf(value, sizeof(value), fmt, *v);
    const ImVec2 vs = TextSize(Font::Semibold, size::Small, value);
    Text(dl, Font::Regular, size::Body, origin, p.ink, label);
    Text(dl, Font::Semibold, size::Small, ImVec2(origin.x + width - vs.x, origin.y + Dp(1.0f)), p.ink2, value);
    ImGui::Dummy(ImVec2(width, Dp(22.0f)));

    const ImGuiID gid = w->GetID(id);
    const float h = Dp(20.0f);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + width, w->DC.CursorPos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    ImGui::ButtonBehavior(bb, gid, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    bool changed = false;
    const float knobR = Dp(8.0f);
    const float x0 = bb.Min.x + knobR, x1 = bb.Max.x - knobR;
    if (held && !IsDisabled()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(x1 - x0, 1.0f), 0.0f, 1.0f);
        const float nv = vmin + (vmax - vmin) * t;
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float t = std::clamp((*v - vmin) / (vmax - vmin), 0.0f, 1.0f);
    const float cy = bb.Min.y + h * 0.5f;
    const float th = Dp(4.0f);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), p.sunken, th);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x0 + (x1 - x0) * t, cy + th * 0.5f), p.accent, th);
    const float hk = Anim(gid, hovered || held);
    const ImVec2 kc(x0 + (x1 - x0) * t, cy);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + Dp(1.0f)), knobR + Dp(1.0f), WithAlpha(IM_COL32(10, 40, 40, 255), 0.16f), 24);
    dl->AddCircleFilled(kc, knobR, p.surface, 24);
    dl->AddCircle(kc, knobR, Mix(p.lineStrong, p.accent, hk), 24, Dp(1.5f));
    ImGui::RenderNavCursor(bb, gid);
    return changed;
}

bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint, float widthPx) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const Palette& p = P();
    const float width = Dp(widthPx), h = Dp(38.0f);
    const ImVec2 origin = w->DC.CursorPos;
    ImDrawList* dl = w->DrawList;
    const ImGuiID fid = w->GetID(id);
    const bool focused = ImGui::GetActiveID() == fid;
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), focused ? p.surface : p.sunken, h * 0.5f);
    if (focused)
        dl->AddRect(origin, ImVec2(origin.x + width, origin.y + h), p.accent, h * 0.5f, 0, Dp(1.5f));
    Icon(dl, icon::Search, 16.0f, ImVec2(origin.x + Dp(20.0f), origin.y + h * 0.5f), p.ink3);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + Dp(36.0f), origin.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(4.0f), (h - Dp(size::Body) * 1.0f) * 0.5f - Dp(1.0f)));
    ImGui::SetNextItemWidth(width - Dp(36.0f + 30.0f));
    PushFont(Font::Regular, size::Body);
    const bool changed = ImGui::InputTextWithHint(id, hint, buf, bufSize);
    PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    bool cleared = false;
    if (buf[0]) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - Dp(32.0f), origin.y + Dp(5.0f)));
        ImGui::PushID(id);
        if (IconButton("clear", icon::X, nullptr, false, 28.0f)) {
            buf[0] = 0;
            cleared = true;
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h));
    ImGui::Dummy(ImVec2(width, 0));
    return changed || cleared;
}

bool TextField(const char* id, const char* label, char* buf, size_t bufSize, float widthPx, const char* hint) {
    const Palette& p = P();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const float width = widthPx < 0 ? ImGui::GetContentRegionAvail().x : Dp(widthPx);
    const float x0 = w->DC.CursorPos.x;
    if (label && *label) {
        Text(w->DrawList, Font::Semibold, size::Small, w->DC.CursorPos, p.ink2, label);
        ImGui::Dummy(ImVec2(width, Dp(22.0f)));
        ImGui::SetCursorScreenPos(ImVec2(x0, w->DC.CursorPos.y));
    }
    const ImVec2 origin = w->DC.CursorPos;
    const float h = Dp(40.0f);
    const ImGuiID fid = w->GetID(id);
    const bool focused = ImGui::GetActiveID() == fid;
    w->DrawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), p.surface, Dp(10.0f));
    w->DrawList->AddRect(origin, ImVec2(origin.x + width, origin.y + h), focused ? p.accent : p.lineStrong, Dp(10.0f),
                         0, focused ? Dp(1.5f) : 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + Dp(8.0f), origin.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(4.0f), (h - Dp(size::Body)) * 0.5f - Dp(1.0f)));
    ImGui::SetNextItemWidth(width - Dp(16.0f));
    PushFont(Font::Regular, size::Body);
    const bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, bufSize) : ImGui::InputText(id, buf, bufSize);
    PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h));
    ImGui::Dummy(ImVec2(width, 0));
    return changed;
}

bool Chip(const char* id, const char* label, const char* icon, bool selected, float widthPx) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID gid = w->GetID(id);
    const Palette& p = P();
    const ImVec2 ls = TextSize(Font::Semibold, size::Small, label);
    const float h = Dp(34.0f);
    const float width = widthPx > 0 ? Dp(widthPx) : ls.x + Dp(icon ? 46.0f : 28.0f);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + width, w->DC.CursorPos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hv = Anim(gid, hovered);
    const float sel = Anim(w->GetID((std::string(id) + "#sel").c_str()), selected, 14.0f);
    ImDrawList* dl = w->DrawList;
    const float r = h * 0.5f;
    dl->AddRectFilled(bb.Min, bb.Max, Mix(Mix(p.surface, p.sunken, hv * 0.6f), p.accentSoft, sel), r);
    dl->AddRect(bb.Min, bb.Max, Mix(p.line, p.accent, sel), r, 0, sel > 0.5f ? Dp(1.5f) : 1.0f);
    const float cw = ls.x + (icon ? Dp(22.0f) : 0.0f);
    float x = bb.Min.x + (width - cw) * 0.5f;
    const float cy = bb.Min.y + h * 0.5f;
    const ImU32 fg = Mix(p.ink2, p.accentInk, sel);
    if (icon) {
        Icon(dl, icon, 15.0f, ImVec2(x + Dp(8.0f), cy), fg);
        x += Dp(22.0f);
    }
    Text(dl, Font::Semibold, size::Small, ImVec2(x, cy - ls.y * 0.5f), Mix(p.ink, p.accentInk, sel), label);
    ImGui::RenderNavCursor(bb, gid);
    return pressed;
}

bool MenuItem(const char* id, const char* label, const char* icon, const char* hint) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID gid = w->GetID(id);
    const Palette& p = P();
    const float h = Dp(34.0f);
    const float width = std::max(Dp(40.0f), ImGui::GetContentRegionAvail().x);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + width, w->DC.CursorPos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hv = Anim(gid, hovered);
    ImDrawList* dl = w->DrawList;
    if (hv > 0.01f) dl->AddRectFilled(bb.Min, bb.Max, WithAlpha(p.ink, 0.06f * hv), Dp(8.0f));
    const float cy = bb.Min.y + h * 0.5f;
    float x = bb.Min.x + Dp(10.0f);
    if (icon) {
        Icon(dl, icon, 16.0f, ImVec2(x + Dp(8.0f), cy), p.ink2);
        x += Dp(26.0f);
    }
    float maxX = bb.Max.x - Dp(10.0f);
    if (hint && *hint) {
        const ImVec2 hs = TextSize(Font::Regular, size::Caption, hint);
        Text(dl, Font::Regular, size::Caption, ImVec2(maxX - hs.x, cy - hs.y * 0.5f), p.ink3, hint);
        maxX -= hs.x + Dp(12.0f);
    }
    const ImVec2 ls = TextSize(Font::Semibold, size::Small, label);
    TextEllipsis(dl, Font::Semibold, size::Small, ImVec2(x, cy - ls.y * 0.5f), maxX, p.ink, label);
    ImGui::RenderNavCursor(bb, gid);
    return pressed;
}

void Tooltip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(10.0f), Dp(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(8.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(P().ink));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(P().surface));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginTooltip()) {
        PushFont(Font::Regular, size::Small);
        ImGui::TextUnformatted(text);
        PopFont();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void Gap(float px) { ImGui::Dummy(ImVec2(1.0f, Dp(px))); }

void SectionLabel(const char* text) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    Text(w->DrawList, Font::Semibold, size::Small, w->DC.CursorPos, P().ink2, text);
    ImGui::Dummy(ImVec2(1.0f, Dp(24.0f)));
}

void BeginScreen(const char* id) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(P().bg));
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
}

void EndScreen() { ImGui::End(); }

} // namespace mmdx::ui
