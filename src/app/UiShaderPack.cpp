// Shader pack choice (render/ShaderPack.h): how a choice reaches a model's GPU constants, and the picker shared by the
// library panel, the play bar and the studio inspector. The pack manager screen is UiShaders.cpp.
#include "app/App.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/TextUtil.h"
#include "render/ShaderPack.h"
#include "studio/StudioDoc.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

using namespace ui;

// Folder picker (FOS_PICKFOLDERS): runs on its own STA thread like studio::FileDialog, keeping the
// owner's messages pumped so the enable ping-pong cannot deadlock. Declared in App.h, shared with
// the shader manager screen (UiShaders.cpp).
std::filesystem::path PickPackTextureFolder(HWND owner) {
    std::filesystem::path result;
    std::thread t([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
                DWORD opts = 0;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
                if (SUCCEEDED(dlg->Show(owner))) {
                    ComPtr<IShellItem> item;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        result = path;
                        CoTaskMemFree(path);
                    }
                }
            }
        }
        CoUninitialize();
    });
    // Keep pumping the owner's messages while waiting (see studio/FileDialog.cpp).
    HANDLE h = (HANDLE)t.native_handle();
    while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    t.join();
    return result;
}

std::string PackAuthorLine(const std::vector<PackAuthor>& authors) {
    std::string s;
    for (size_t i = 0; i < authors.size() && i < 3; ++i) s += (i ? ", " : "") + authors[i].name;
    if (authors.size() > 3) s += " +" + std::to_string(authors.size() - 3);
    return s;
}

bool PackMatches(const std::string& needle, const std::string& id, const LocalizedText& name,
                 const std::vector<PackAuthor>& authors, const std::vector<std::string>& tags) {
    if (needle.empty()) return true;
    const auto has = [&](const std::string& s) { return ToLowerAscii(s).find(needle) != std::string::npos; };
    if (has(id) || has(name.Get(PackLanguage())) || has(PackAuthorLine(authors))) return true;
    for (const std::string& t : tags)
        if (has(t)) return true;
    return false;
}

void App::ApplyShaderChoice(GpuModel& gpu, const ShaderChoice& choice) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (pack && !pack->Selectable()) pack = nullptr;
    if (!pack) {
        gpu.SetShaderPack(nullptr, PackParamValues{}, {});
        return;
    }
    // per-character texture folder: the choice's folder, else the pack-level one (registry setting)
    const std::filesystem::path folder =
        choice.textureFolder.empty() ? ShaderPacks().TextureFolder(choice.pack) : Utf8ToPath(choice.textureFolder);
    gpu.SetShaderPack(pack, pack->Resolve(choice.params), folder);
}

ShaderChoice App::PlayShaderChoice() const {
    if (!scene_ || screen_ == Screen::BenchRun || screen_ == Screen::BenchRender) return {};  // fixed workload
    ShaderChoice c = scene_->characterId.empty() ? ShaderChoice{} : settings_.CharacterShader(scene_->characterId);
    if (options_.shaderPackSet) {   // --shader-pack: this run only
        if (c.pack != options_.shaderPack) {
            c.params.clear();
            c.textureFolder.clear();
        }
        c.pack = options_.shaderPack;
    }
    return c;
}

void App::CheckShaderPackErrors() {
    const ShaderPackRegistry& reg = ShaderPacks();
    if (reg.ErrorCount() == shaderPackErrorsSeen_) return;
    shaderPackErrorsSeen_ = reg.ErrorCount();
    const ShaderPack* bad = reg.Find(reg.LastErrorPack());
    toast_ = Toast{};
    toast_.title = Tr("셰이더 팩을 적용하지 못했습니다");
    toast_.detail = (bad ? bad->name.Get(PackLanguage()) : reg.LastErrorPack()) +
                    Tr(" · 기본 셰이딩으로 표시합니다. 셰이더 탭에서 오류를 볼 수 있습니다");
    toast_.error = true;
    toast_.until = timeSeconds_ + 8.0;
}

// ---- previews -------------------------------------------------------------------------------------------------------

static std::string PackThumbKey(const ShaderPack& pack) {
    // the registry generation is part of the key: an edited preview (hot reload) becomes a new entry
    return "p:" + pack.id + ":" + std::to_string(ShaderPacks().Generation());
}

uint64_t App::PackThumb(const ShaderPack& pack) {
    if (pack.preview.empty()) return 0;
    return thumbs_.Get(PackThumbKey(pack), ThumbnailKind::Image, {pack.preview});
}

uint64_t App::RemotePackThumb(const RemotePack& pack) {
    if (pack.previewFile.empty()) return 0;
    return thumbs_.Get("r:" + pack.id + ":" + pack.version, ThumbnailKind::Image, {pack.previewFile});
}

void App::DrawPackImage(ImDrawList* dl, uint64_t tex, const std::string& key, ImVec2 a, ImVec2 b, float rounding,
                        ImDrawFlags flags) {
    const Palette& p = P();
    if (!tex) {   // placeholder: a soft mint field with the pack glyph
        dl->AddRectFilled(a, b, Mix(p.surface, p.accentSoft, 0.7f), rounding, flags);
        Icon(dl, icon::Diamond, std::clamp((b.y - a.y) / Dpi() * 0.35f, 12.0f, 30.0f),
             ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), WithAlpha(p.accentInk, 0.55f));
        return;
    }
    uint32_t w = 0, h = 0;
    thumbs_.Size(key, w, h);
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (w && h) {   // cover-fit: crop the longer side
        const float ra = (b.x - a.x) / std::max(1.0f, b.y - a.y), ri = (float)w / (float)h;
        if (ri > ra) {
            const float k = ra / ri;
            uv0.x = (1 - k) * 0.5f;
            uv1.x = 1 - uv0.x;
        } else {
            const float k = ri / ra;
            uv0.y = (1 - k) * 0.5f;
            uv1.y = 1 - uv0.y;
        }
    }
    dl->AddImageRounded((ImTextureID)tex, a, b, uv0, uv1, IM_COL32_WHITE, rounding, flags);
}

// ---- picker ---------------------------------------------------------------------------------------------------------

bool App::DrawShaderSelector(const char* id, ShaderChoice& choice, float width) {
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();
    const ShaderPack* cur = choice.pack.empty() ? nullptr : reg.Find(choice.pack);
    bool changed = false;

    // the row: preview, name, version / authors, caret
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    const ImVec2 a = win->DC.CursorPos;
    const ImVec2 b(a.x + width, a.y + Dp(54.0f));
    bool hovered = false;
    const std::string hit = std::string(id) + "_row";
    const std::string popup = std::string(id) + "_popup";
    if (CardItem(hit.c_str(), a, b, &hovered)) ImGui::OpenPopup(popup.c_str());
    const float hv = Anim(ImGui::GetID(hit.c_str()), hovered);
    dl->AddRectFilled(a, b, Mix(p.surface, p.sunken, 0.4f + 0.6f * hv), Dp(12.0f));
    dl->AddRect(a, b, Mix(p.line, p.lineStrong, hv), Dp(12.0f));
    const ImVec2 ia(a.x + Dp(7.0f), a.y + Dp(7.0f)), ib(ia.x + Dp(71.0f), b.y - Dp(7.0f));
    if (cur) {
        DrawPackImage(dl, PackThumb(*cur), PackThumbKey(*cur), ia, ib, Dp(8.0f));
    } else {
        dl->AddRectFilled(ia, ib, p.sunken, Dp(8.0f));
        Icon(dl, icon::Cube, 18.0f, ImVec2((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f), p.ink2);
    }
    const float tx = ib.x + Dp(12.0f), tr = b.x - Dp(30.0f);
    const std::string title = cur ? cur->name.Get(lang) : (choice.pack.empty() ? std::string(Tr("MMD 기본")) : choice.pack);
    std::string sub = cur ? "v" + cur->version + (cur->authors.empty() ? "" : "  ·  " + PackAuthorLine(cur->authors))
                          : (choice.pack.empty() ? std::string(Tr("앱 기본 셰이딩")) : std::string(Tr("설치되어 있지 않음")));
    const bool problem = (cur && !cur->Selectable()) || (!cur && !choice.pack.empty());
    if (cur && !cur->Selectable()) sub = Tr("사용할 수 없음 · 셰이더 탭에서 확인");
    TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(tx, a.y + Dp(9.0f)), tr, p.ink, title.c_str());
    TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(30.0f)), tr, problem ? p.warn : p.ink3, sub.c_str());
    Icon(dl, icon::CaretDown, 14.0f, ImVec2(b.x - Dp(16.0f), (a.y + b.y) * 0.5f), p.ink2);
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(width, 0));

    // the list
    ImGui::SetNextWindowPos(ImVec2(a.x, b.y + Dp(6.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(std::max(width, Dp(340.0f)), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup(popup.c_str())) {
        static char filter[96] = {};
        if (ImGui::IsWindowAppearing()) filter[0] = 0;
        const float pw = ImGui::GetContentRegionAvail().x;
        SearchField("##packsearch", filter, sizeof(filter), Tr("이름, 작성자, 태그로 검색"), pw / Dpi());
        Gap(8.0f);
        const std::string needle = ToLowerAscii(filter);
        const float listH = std::min(Dp(372.0f), Dp(58.0f) * (float)(reg.Packs().size() + 1) + Dp(16.0f));
        ImGui::BeginChild("##packlist", ImVec2(pw, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const float lw = ImGui::GetContentRegionAvail().x;
        const auto row = [&](const char* rid, const ShaderPack* pk, bool selected) -> bool {
            const ImVec2 ra = ImGui::GetCursorScreenPos(), rb(ra.x + lw, ra.y + Dp(56.0f));
            bool h = false;
            const bool selectable = !pk || pk->Selectable();
            const bool clicked = CardItem(rid, ra, rb, &h);
            const float t = Anim(ImGui::GetID(rid), h && selectable);
            if (selected) ldl->AddRectFilled(ra, rb, p.accentSoft, Dp(10.0f));
            else if (t > 0.01f) ldl->AddRectFilled(ra, rb, WithAlpha(p.sunken, t), Dp(10.0f));
            const ImVec2 pa(ra.x + Dp(6.0f), ra.y + Dp(6.0f)), pb(pa.x + Dp(78.0f), rb.y - Dp(6.0f));
            if (pk) {
                DrawPackImage(ldl, PackThumb(*pk), PackThumbKey(*pk), pa, pb, Dp(7.0f));
            } else {
                ldl->AddRectFilled(pa, pb, p.sunken, Dp(7.0f));
                Icon(ldl, icon::Cube, 16.0f, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), p.ink2);
            }
            const float x = pb.x + Dp(10.0f), r = rb.x - Dp(28.0f);
            const std::string nm = pk ? pk->name.Get(lang) : std::string(Tr("MMD 기본"));
            std::string sb = pk ? "v" + pk->version + (pk->authors.empty() ? "" : "  ·  " + PackAuthorLine(pk->authors))
                                : std::string(Tr("앱 기본 셰이딩"));
            if (pk && !selectable) sb = Tr("사용할 수 없음 · 셰이더 탭에서 확인");
            TextEllipsis(ldl, Font::Semibold, size::Small, ImVec2(x, ra.y + Dp(10.0f)), r, selectable ? p.ink : p.ink3, nm.c_str());
            TextEllipsis(ldl, Font::Regular, size::Caption, ImVec2(x, ra.y + Dp(30.0f)), r, selectable ? p.ink3 : p.warn,
                         sb.c_str());
            if (selected) Icon(ldl, icon::Check, 14.0f, ImVec2(rb.x - Dp(14.0f), (ra.y + rb.y) * 0.5f), p.accentInk);
            if (pk && h && !pk->description.Empty()) Tooltip(pk->description.Get(lang).c_str());
            ImGui::SetCursorScreenPos(ImVec2(ra.x, rb.y + Dp(2.0f)));
            ImGui::Dummy(ImVec2(lw, 0));
            return clicked && selectable;
        };
        if (needle.empty() && row("##pk_default", nullptr, choice.pack.empty()) && !choice.pack.empty()) {
            choice = {};
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        int shown = 0;
        for (const ShaderPack& pk : reg.Packs()) {
            if (pk.status == PackStatus::Duplicate || pk.type != PackType::Surface ||
                !PackMatches(needle, pk.id, pk.name, pk.authors, pk.tags))
                continue;
            ++shown;
            const std::string rid = "##pk_" + pk.id;
            if (row(rid.c_str(), &pk, choice.pack == pk.id) && choice.pack != pk.id) {
                choice.pack = pk.id;
                choice.params.clear();
                choice.textureFolder.clear();
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!needle.empty() && shown == 0) ImGui::TextDisabled("%s", Tr("검색 결과가 없습니다"));
        ImGui::EndChild();
        Gap(6.0f);
        if (Button("##packmanage", Tr("셰이더 관리 · 온라인에서 받기"), icon::Diamond, ButtonKind::Ghost)) {
            ImGui::CloseCurrentPopup();
            if (!choice.pack.empty()) shaderSelInstalled_ = choice.pack;
            shaderTab_ = 0;
            if (screen_ == Screen::Select) screen_ = Screen::Shaders;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return changed;
}

// 1/2/3 columns for the parameter grid: packs with many params split the grid, small packs stay single column.
static int PackParamColumns(size_t n) {
    if (n <= 6) return 1;
    if (n <= 14) return 2;
    return 3;
}

// Popup width for the pack parameter grid (columns from the pack's param count), clamped to the viewport.
float PackParamsPopupWidth(const ShaderChoice& choice) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    const float w = (float)PackParamColumns(pack ? pack->params.size() : 0) * Dp(280.0f) + Dp(32.0f);
    return std::min(w, ImGui::GetMainViewport()->WorkSize.x - Dp(24.0f));
}

bool App::DrawShaderPackParams(ShaderChoice& choice) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (!pack) return false;
    bool changed = false;
    const std::string lang = PackLanguage();
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ShaderPackRegistry& reg = ShaderPacks();
    // per-character texture folder (pack.json "textures" packs): the folder this model's set loads from, picker +
    // clear + the set's missing count; edits the ShaderChoice (the callers save it)
    if (!pack->textures.empty()) {
        const float w = ImGui::GetContentRegionAvail().x;
        const std::filesystem::path folder =
            choice.textureFolder.empty() ? std::filesystem::path() : Utf8ToPath(choice.textureFolder);
        const std::string shown = folder.empty() ? std::string(Tr("팩 설정 사용"))
                                                 : std::string(Tr("텍스처 폴더")) + ": " + PathToUtf8(folder);
        const uint32_t missing = reg.MissingTextures(pack->id, folder);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), c.x + w - Dp(96.0f),
                     missing ? p.warn : p.ink2, shown.c_str());
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(88.0f), c.y));
        if (IconButton("##choiceTexPick", icon::FolderOpen, Tr("텍스처 폴더 선택"))) {
            const std::filesystem::path picked = PickPackTextureFolder(hwnd_);
            if (!picked.empty()) {
                choice.textureFolder = PathToUtf8(picked);
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(folder.empty());
        if (IconButton("##choiceTexClear", icon::X, Tr("텍스처 폴더 지우기"))) {
            choice.textureFolder.clear();
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
        if (missing) {
            Text(dl, Font::Regular, size::Caption, ImGui::GetCursorScreenPos(), p.warn,
                 (std::to_string(missing) + Tr("개의 팩 텍스처가 없어 흰색으로 표시됩니다")).c_str());
            ImGui::Dummy(ImVec2(w, Dp(18.0f)));
        }
        ImGui::Dummy(ImVec2(w, 0));
        Gap(4.0f);
    }
    if (pack->params.empty()) return changed;
    const PackParamValues values = pack->Resolve(choice.params);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::clamp(std::min((int)(avail / Dp(240.0f)), (int)pack->params.size()), 1, 3);
    if (cols > 1) {
        const size_t n = pack->params.size();
        const int rows = (int)((n + (size_t)cols - 1) / (size_t)cols);   // column-major: ceil(n/cols)
        if (ImGui::BeginTable("##spp_grid", cols, ImGuiTableFlags_SizingStretchSame)) {
            for (int row = 0; row < rows; ++row) {
                ImGui::TableNextRow();
                for (int col = 0; col < cols; ++col) {
                    const size_t i = (size_t)col * (size_t)rows + (size_t)row;   // fill column-major
                    if (i >= n) continue;
                    ImGui::TableSetColumnIndex(col);
                    const ShaderPackParam& sp = pack->params[i];
                    float v = values[i];
                    const std::string id = "##spp_" + sp.key;
                    const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
                    if (SliderRow(id.c_str(), label.c_str(), &v, sp.min, sp.max, "%.2f")) {
                        choice.params[sp.key] = v;
                        changed = true;
                    }
                }
            }
            ImGui::EndTable();
        }
    } else {
        for (size_t i = 0; i < pack->params.size(); ++i) {
            const ShaderPackParam& sp = pack->params[i];
            float v = values[i];
            const std::string id = "##spp_" + sp.key;
            const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
            if (SliderRow(id.c_str(), label.c_str(), &v, sp.min, sp.max, "%.2f")) {
                choice.params[sp.key] = v;
                changed = true;
            }
        }
    }
    Gap(6.0f);
    ImGui::BeginDisabled(choice.params.empty());
    if (Button("##sp_reset", Tr("기본값으로"), icon::ArrowCcw, ButtonKind::Ghost)) {
        choice.params.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

void App::DrawStudioShaderRow(float w) {
    studio::StudioDoc& d = *studio_;
    studio::StudioModel* m = d.Selected();
    if (!m) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("셰이더"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    ShaderChoice choice = m->shader;
    bool changed = DrawShaderSelector("##studioshader", choice, w);
    if (options_.shaderPackSet) {
        PushFont(Font::Regular, size::Caption);
        ImGui::TextDisabled("%s", Tr("--shader-pack 이 이번 실행 동안 우선합니다"));
        PopFont();
    }
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (pack && !pack->params.empty()) {
        Gap(6.0f);
        if (Button("##studioshaderparams", Tr("팩 설정"), icon::Sliders, ButtonKind::Ghost))
            ImGui::OpenPopup("##studioshaderparamspopup");
        ImGui::SetNextWindowSize(ImVec2(PackParamsPopupWidth(choice), 0));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(FLT_MAX, ImGui::GetMainViewport()->WorkSize.y - Dp(24.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
        if (ImGui::BeginPopup("##studioshaderparamspopup")) {
            SectionLabel(pack->name.Get(PackLanguage()).c_str());
            changed |= DrawShaderPackParams(choice);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    if (changed) {
        m->shader = choice;
        ++d.projectVersion;   // saved with the project (no undo step)
    }
}

} // namespace mmdx
