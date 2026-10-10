#include "ui_probe/UiProbe.h"
#include "imgui_internal.h"
#include <cstring>
#include <unordered_map>

namespace mmdx::uiprobe {
namespace {

bool g_on = false;
std::vector<Item> g_cur, g_last;
std::unordered_map<ImGuiID, size_t> g_index;   // id -> g_cur slot (this frame)

void SplitLabel(const char* s, std::string& text, std::string& idStr) {
    if (!s) return;
    const char* hash = std::strstr(s, "##");
    if (hash) {
        text.assign(s, hash);
        idStr = hash;
    } else {
        text = s;
        idStr = s;
    }
}

} // namespace

void SetEnabled(bool on) {
    g_on = on;
    if (ImGuiContext* ctx = ImGui::GetCurrentContext()) ctx->TestEngineHookItems = on;
    if (!on) {
        g_cur.clear();
        g_last.clear();
        g_index.clear();
    }
}

bool Enabled() { return g_on; }

void BeginFrame() {
    if (!g_on) return;
    if (ImGuiContext* ctx = ImGui::GetCurrentContext()) ctx->TestEngineHookItems = true;   // enabled before the context
    g_last.swap(g_cur);
    g_cur.clear();
    g_index.clear();
}

void Info(ImGuiID id, const char* idStr, const char* label) {
    if (!g_on) return;
    const auto it = g_index.find(id);
    if (it == g_index.end()) return;
    Item& item = g_cur[it->second];
    if (idStr) item.idStr = idStr;
    if (label) {
        std::string text, unused;
        SplitLabel(label, text, unused);
        item.label = text;
    }
}

void Add(const char* idStr, const char* label, ImVec2 min, ImVec2 max) {
    if (!g_on) return;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    Item& item = g_cur.emplace_back();
    item.id = w ? w->GetID(idStr) : ImHashStr(idStr);
    item.min = min;
    item.max = max;
    item.idStr = idStr;
    if (label) item.label = label;
    if (w) {
        item.window = w->Name;
        item.visible = ImRect(min, max).Overlaps(w->ClipRect);
    }
}

const std::vector<Item>& LastFrame() { return g_last; }

bool Covered(const Item& item) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return false;
    ImGuiWindow* own = ImGui::FindWindowByName(item.window.c_str());
    if (!own) return false;
    const ImVec2 c((item.min.x + item.max.x) * 0.5f, (item.min.y + item.max.y) * 0.5f);
    for (int i = ctx->Windows.Size - 1; i >= 0; --i) {   // back to front = topmost first
        ImGuiWindow* w = ctx->Windows[i];
        if (!w->WasActive || w->Hidden || (w->Flags & ImGuiWindowFlags_NoMouseInputs)) continue;
        if (!w->OuterRectClipped.Contains(c)) continue;
        if (w == own || w->RootWindow == own->RootWindow) return false;
        if (own->ParentWindow && w == own->ParentWindow) return false;
        return true;
    }
    return false;
}

} // namespace mmdx::uiprobe

using namespace mmdx::uiprobe;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    if (!g_on || id == 0) return;
    ImGuiWindow* w = ctx->CurrentWindow;
    const auto it = g_index.find(id);
    Item* item;
    if (it != g_index.end()) {
        item = &g_cur[it->second];   // added twice in a frame (a window's title bar, etc.): keep the last rect
    } else {
        g_index[id] = g_cur.size();
        item = &g_cur.emplace_back();
        item->id = id;
    }
    item->min = bb.Min;
    item->max = bb.Max;
    if (w) {
        item->window = w->Name;
        item->visible = bb.Overlaps(w->ClipRect) && w->Active && !w->Hidden;
        item->isWindow = w->ID == id;
    }
    item->disabled = data && (data->ItemFlags & ImGuiItemFlags_Disabled);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (!g_on) return;
    const auto it = g_index.find(id);
    if (it == g_index.end() || !label) return;
    Item& item = g_cur[it->second];
    if (!item.idStr.empty()) return;   // UiKit already named it
    SplitLabel(label, item.label, item.idStr);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    const auto it = g_index.find(id);
    return it == g_index.end() ? nullptr : g_cur[it->second].label.c_str();
}
