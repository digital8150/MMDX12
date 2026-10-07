// Shader pack manager (Screen::Shaders, the "셰이더" tab): installed packs and the online gallery, pack details,
// installing (zip / folder / drop / download), removing, creating a pack from the template.
#include "app/App.h"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"
#include "render/RenderPass.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/FileDialog.h"

namespace mmdx {

using namespace ui;

namespace {

constexpr float kAppBarH = 64.0f;   // as UiSelect.cpp
constexpr float kPad = 28.0f;
constexpr float kPanelW = 460.0f;

void OpenUrl(const std::string& url) {
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
        ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenFolder(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const char* SourceLabel(PackSource s) {
    switch (s) {
    case PackSource::BuiltIn: return Tr("기본 제공");
    case PackSource::Online: return Tr("온라인");
    default: return Tr("직접 설치");
    }
}

// status badge text + colours; nullptr = nothing to say (ready / compiled)
const char* StatusLabel(PackStatus s, ImU32& bg, ImU32& fg) {
    const Palette& p = P();
    switch (s) {
    case PackStatus::CompileError: bg = p.dangerSoft; fg = p.danger; return Tr("컴파일 오류");
    case PackStatus::Incompatible: bg = p.warnSoft; fg = p.warn; return Tr("호환되지 않음");
    case PackStatus::InvalidManifest: bg = p.dangerSoft; fg = p.danger; return Tr("매니페스트 오류");
    case PackStatus::Duplicate: bg = p.warnSoft; fg = p.warn; return Tr("중복된 id");
    default: return nullptr;
    }
}

// Wrapped paragraph in the kit's type.
void Para(const std::string& text, ImU32 col, float sizePx = size::Small, Font font = Font::Regular) {
    if (text.empty()) return;
    PushFont(font, sizePx);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    PopFont();
}

void Caption(const char* label) {
    Gap(14.0f);
    Para(label, P().ink3, size::Caption, Font::Semibold);
    Gap(4.0f);
}

// A row of small badges that wraps (tags).
void BadgeFlow(const std::vector<std::string>& items, float width) {
    if (items.empty()) return;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float x = 0, y = 0, rowH = 0;
    for (const std::string& t : items) {
        const ImVec2 ts = TextSize(Font::Semibold, size::Caption, t.c_str());
        const float w = ts.x + Dp(16.0f);
        if (x > 0 && x + w > width) {
            x = 0;
            y += rowH + Dp(6.0f);
        }
        ImVec2 bs;
        Badge(dl, ImVec2(o.x + x, o.y + y), t.c_str(), WithAlpha(p.ink, 0.06f), p.ink2, &bs);
        rowH = std::max(rowH, bs.y);
        x += bs.x + Dp(6.0f);
    }
    ImGui::Dummy(ImVec2(width, y + rowH));
}

const ShaderPack* InstalledVersionOf(const std::string& id) {
    const ShaderPack* p = ShaderPacks().Find(id);
    return p && p->status != PackStatus::Duplicate ? p : nullptr;
}

// "효과" / "표면" type badge text for a card.
const char* TypeLabel(PackType t) { return t == PackType::Effect ? Tr("효과") : Tr("표면"); }

} // namespace

// ---- per frame ------------------------------------------------------------------------------------------------------

void App::PollShaderPacks() {
    ShaderPackRegistry& reg = ShaderPacks();
    reg.PollChanges(timeSeconds_);   // hot reload: edited packs recompile on their next draw
    for (const ShaderPackStore::Event& e : shaderStore_.Poll(reg)) {
        toast_ = Toast{};
        toast_.title = e.ok ? Tr("셰이더 팩") : Tr("셰이더 팩을 설치하지 못했습니다");
        toast_.detail = e.text;
        toast_.error = !e.ok;
        toast_.until = timeSeconds_ + (e.ok ? 4.0 : 8.0);
    }
    if (!droppedFiles_.empty()) {
        const std::vector<std::filesystem::path> files = std::move(droppedFiles_);
        droppedFiles_.clear();
        // packs can be dropped on the library and the shader screens (not into a scene / the studio)
        if (screen_ == Screen::Select || screen_ == Screen::Shaders || screen_ == Screen::BenchLobby)
            for (const auto& f : files) InstallPackPath(f);
    }
}

void App::InstallPackPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::string id, err;
    bool ok = false;
    if (std::filesystem::is_directory(path, ec)) {
        ok = ShaderPacks().InstallFolder(path, "", id, err);
    } else if (ToLowerAscii(PathToUtf8(path.extension())) == ".zip") {
        ok = ShaderPacks().InstallZip(path, "", id, err);
    } else {
        return;   // not a pack; ignore silently (people drop all sorts of things on windows)
    }
    toast_ = Toast{};
    if (ok) {
        const ShaderPack* p = ShaderPacks().Find(id);
        toast_.title = Tr("셰이더 팩을 설치했습니다");
        toast_.detail = p ? p->name.Get(PackLanguage()) + " v" + p->version : id;
        shaderSelInstalled_ = id;
        shaderTab_ = 0;
        screen_ = Screen::Shaders;
        LOG_INFO("shader pack installed from %s: %s", PathToUtf8(path).c_str(), id.c_str());
    } else {
        toast_.title = Tr("셰이더 팩을 설치하지 못했습니다");
        toast_.detail = err;
        toast_.error = true;
        LOG_WARN("shader pack install from %s failed: %s", PathToUtf8(path).c_str(), err.c_str());
    }
    toast_.until = timeSeconds_ + (ok ? 4.0 : 8.0);
}

// ---- screen ---------------------------------------------------------------------------------------------------------

void App::DrawShaders() {
    BeginScreen("##shaders");
    DrawAppBar(1);
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = Dp(kPad), gap = Dp(24.0f);
    const float top = Dp(kAppBarH) + pad;
    const float panelX = ds.x - pad - Dp(kPanelW);
    const float leftX = pad, leftR = panelX - gap;
    if (shaderTab_ == 1 && shaderStore_.State() == ShaderPackStore::IndexState::Idle) shaderStore_.Refresh(options_.packIndex.empty() ? ShaderPackStore::kDefaultIndexUrl : options_.packIndex);

    // ---- header: tabs, search, actions
    {
        int installedCount = 0;
        for (const ShaderPack& pk : reg.Packs()) installedCount += pk.status != PackStatus::Duplicate;
        const std::string l0 = Tr("설치됨  ") + std::to_string(installedCount);
        const std::string l1 = shaderStore_.State() == ShaderPackStore::IndexState::Ready
                                   ? Tr("온라인  ") + std::to_string(shaderStore_.Packs().size())
                                   : std::string(Tr("온라인"));
        const char* labels[] = {l0.c_str(), l1.c_str()};
        const char* icons[] = {icon::Stack, icon::Globe};
        ImGui::SetCursorScreenPos(ImVec2(leftX, top));
        Segmented("##shadertabs", labels, 2, &shaderTab_, 0.0f, 40.0f, icons);
        const float tabsR = ImGui::GetItemRectMax().x;

        // actions, right-aligned against the panel
        float x = leftR;
        const auto rightButton = [&](const char* id, const char* label, const char* ic, ButtonKind kind) {
            const float w = TextSize(Font::Semibold, size::Small, label).x + Dp(ic ? 52.0f : 32.0f);
            x -= w;
            ImGui::SetCursorScreenPos(ImVec2(x, top + Dp(2.0f)));
            const bool r = Button(id, label, ic, kind, ImVec2(w / Dpi(), 36.0f));
            x -= Dp(8.0f);
            return r;
        };
        if (shaderTab_ == 0) {
            if (rightButton("##newpack", Tr("새 팩 만들기"), icon::Plus, ButtonKind::Secondary)) {
                newPackOpen_ = true;
                newPackId_[0] = newPackName_[0] = 0;
                std::snprintf(newPackAuthor_, sizeof(newPackAuthor_), "%s", settings_.nickname.c_str());
            }
            if (rightButton("##installzip", Tr("zip 설치"), icon::DownloadSimple, ButtonKind::Secondary)) {
                const std::filesystem::path f = studio::OpenFileDialog(hwnd_, {{L"Shader pack (zip)", L"*.zip"}});
                if (!f.empty()) InstallPackPath(f);
            }
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            if (IconButton("##packfolder", icon::FolderOpen, Tr("설치된 팩 폴더 열기"))) OpenFolder(reg.UserRoot());
            x -= Dp(44.0f);
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            if (IconButton("##packreload", icon::Refresh, Tr("팩 다시 불러오기 (파일을 저장하면 자동으로 다시 불러옵니다)"))) {
                ResetShaderSourceHashes();
                reg.Scan();
            }
            x -= Dp(44.0f);
        } else {
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            ImGui::BeginDisabled(shaderStore_.State() == ShaderPackStore::IndexState::Loading);
            if (IconButton("##storerefresh", icon::Refresh, Tr("목록 새로 고침"))) shaderStore_.Refresh(options_.packIndex.empty() ? ShaderPackStore::kDefaultIndexUrl : options_.packIndex);
            ImGui::EndDisabled();
            x -= Dp(44.0f);
            if (rightButton("##storesite", Tr("갤러리 웹사이트"), icon::Globe, ButtonKind::Ghost))
                OpenUrl("https://mmdx.codingbot.kr/" + lang + "/shader-packs/");
        }
        const float sw = std::min(260.0f, std::max(120.0f, (x - tabsR - Dp(16.0f)) / Dpi()));
        if (x - tabsR > Dp(140.0f)) {
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(sw), top + Dp(1.0f)));
            SearchField("##packfilter", shaderFilter_, sizeof(shaderFilter_), Tr("이름, 작성자, 태그"), sw);
        }
    }

    // ---- grid
    const float gridTop = top + Dp(40.0f) + Dp(20.0f);
    ImGui::SetCursorScreenPos(ImVec2(leftX - Dp(6.0f), gridTop - Dp(6.0f)));
    ImGui::BeginChild("##packgrid", ImVec2(leftR - leftX + Dp(12.0f), ds.y - gridTop - pad + Dp(12.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float inset = Dp(6.0f);
        const float availW = ImGui::GetContentRegionAvail().x - inset * 2.0f - Dp(10.0f);
        const ImVec2 origin(ImGui::GetCursorScreenPos().x + inset, ImGui::GetCursorScreenPos().y + inset);
        const float cgap = Dp(16.0f);
        const float minW = Dp(240.0f);
        const int cols = std::max(1, (int)((availW + cgap) / (minW + cgap)));
        const float cw = (availW - cgap * (cols - 1)) / cols;
        const float imgH = cw * 9.0f / 16.0f, chH = imgH + Dp(92.0f);
        const std::string needle = ToLowerAscii(shaderFilter_);
        float contentH = 0;
        int n = 0;

        // one card; returns true when clicked
        const auto card = [&](const char* id, bool selected, uint64_t tex, const std::string& key, const std::string& name,
                              const std::string& sub, const std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>>& badges,
                              float progress) {
            const int col = n % cols, row = n / cols;
            ++n;
            const ImVec2 ca(origin.x + col * (cw + cgap), origin.y + row * (chH + cgap));
            const ImVec2 cb(ca.x + cw, ca.y + chH);
            contentH = std::max(contentH, cb.y - origin.y);
            if (!ImGui::IsRectVisible(ImVec2(ca.x, ca.y - Dp(20)), ImVec2(cb.x, cb.y + Dp(20)))) return false;
            bool hovered = false;
            const bool clicked = CardItem(id, ca, cb, &hovered);
            const float hv = Anim(ImGui::GetID(id), hovered);
            const float sv = Anim(ImGui::GetID((std::string(id) + "s").c_str()), selected, 14.0f);
            const float lift = -Dp(3.0f) * hv;
            const ImVec2 a0(ca.x, ca.y + lift), b0(cb.x, cb.y + lift);
            const float r = Dp(14.0f);
            SoftShadow(dl, a0, b0, r, Dp(10.0f + 8.0f * hv), 0.07f + 0.07f * hv, ImVec2(0, Dp(3.0f + 3.0f * hv)));
            dl->AddRectFilled(a0, b0, p.surface, r);
            DrawPackImage(dl, tex, key, a0, ImVec2(b0.x, a0.y + imgH), r, ImDrawFlags_RoundCornersTop);
            if (sv > 0.01f) dl->AddRect(a0, b0, WithAlpha(p.accent, sv), r, 0, Dp(2.0f));
            const float tx = a0.x + Dp(14.0f), tr = b0.x - Dp(14.0f);
            float ty = a0.y + imgH + Dp(12.0f);
            TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(tx, ty), tr, p.ink, name.c_str());
            ty += Dp(22.0f);
            TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(tx, ty), tr, p.ink3, sub.c_str());
            ty += Dp(24.0f);
            float bx = tx;
            for (const auto& [text, col2] : badges) {
                ImVec2 bs;
                Badge(dl, ImVec2(bx, ty), text.c_str(), col2.first, col2.second, &bs);
                bx += bs.x + Dp(6.0f);
            }
            if (progress >= 0.0f) ProgressBar(dl, ImVec2(tx, b0.y - Dp(10.0f)), ImVec2(tr, b0.y - Dp(6.0f)), progress);
            return clicked;
        };

        if (shaderTab_ == 0) {
            for (const ShaderPack& pk : reg.Packs()) {
                if (!PackMatches(needle, pk.id, pk.name, pk.authors, pk.tags)) continue;
                std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>> badges;
                badges.push_back({SourceLabel(pk.source), {WithAlpha(p.ink, 0.06f), p.ink2}});
                if (pk.type == PackType::Effect)   // surface is the default: badge the effects only
                    badges.push_back({TypeLabel(pk.type), {p.accentSoft, p.accentInk}});
                ImU32 bg = 0, fg = 0;
                if (const char* st = StatusLabel(pk.status, bg, fg)) badges.push_back({st, {bg, fg}});
                const std::string sub = "v" + pk.version + (pk.authors.empty() ? "" : "  ·  " + PackAuthorLine(pk.authors));
                ImGui::PushID(PathToUtf8(pk.dir).c_str());
                if (card("##pc", shaderSelInstalled_ == pk.id, PackThumb(pk),
                         "p:" + pk.id + ":" + std::to_string(reg.Generation()), pk.name.Get(lang), sub, badges, -1.0f))
                    shaderSelInstalled_ = pk.id;
                ImGui::PopID();
            }
            if (n == 0) {
                const char* msg = reg.Packs().empty() ? Tr("설치된 셰이더 팩이 없습니다. 온라인 탭에서 받거나 zip을 창에 끌어 놓으세요")
                                                      : Tr("검색 결과가 없습니다");
                Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3, msg);
            }
        } else {
            const auto st = shaderStore_.State();
            if (st == ShaderPackStore::IndexState::Loading) {
                Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3, Tr("온라인 목록을 불러오는 중..."));
            } else if (st == ShaderPackStore::IndexState::Failed) {
                Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.warn, shaderStore_.Error().c_str());
                ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + Dp(40.0f)));
                if (Button("##storeretry", Tr("다시 시도"), icon::Refresh)) shaderStore_.Refresh(options_.packIndex.empty() ? ShaderPackStore::kDefaultIndexUrl : options_.packIndex);
            } else {
                for (const RemotePack& rp : shaderStore_.Packs()) {
                    if (!PackMatches(needle, rp.id, rp.name, rp.authors, rp.tags)) continue;
                    std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>> badges;
                    const ShaderPack* inst = InstalledVersionOf(rp.id);
                    if (!rp.Compatible()) badges.push_back({Tr("호환되지 않음"), {p.warnSoft, p.warn}});
                    else if (inst && net::CompareVersions(rp.version, inst->version) > 0)
                        badges.push_back({Tr("업데이트 있음"), {p.accentSoft, p.accentInk}});
                    else if (inst) badges.push_back({Tr("설치됨"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                    if (rp.builtIn) badges.push_back({Tr("앱에 포함"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                    const std::string sub = "v" + rp.version + (rp.authors.empty() ? "" : "  ·  " + PackAuthorLine(rp.authors));
                    ImGui::PushID(rp.id.c_str());
                    if (card("##rc", shaderSelRemote_ == rp.id, RemotePackThumb(rp), "r:" + rp.id + ":" + rp.version,
                             rp.name.Get(lang), sub, badges, shaderStore_.Installing(rp.id) ? shaderStore_.Progress(rp.id) : -1.0f))
                        shaderSelRemote_ = rp.id;
                    ImGui::PopID();
                }
                if (n == 0)
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         shaderStore_.Packs().empty() ? Tr("아직 갤러리에 팩이 없습니다") : Tr("검색 결과가 없습니다"));
            }
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(availW, contentH + inset));
    }
    ImGui::EndChild();

    // ---- detail panel
    {
        const ImVec2 pa(panelX, top), pb(ds.x - pad, ds.y - pad);
        Panel(ImGui::GetWindowDrawList(), pa, pb, Dp(22.0f), 1.0f);
        if (shaderTab_ == 0) DrawShaderPackDetail(pa.x, pa.y, pb.x, pb.y);
        else DrawRemotePackDetail(pa.x, pa.y, pb.x, pb.y);
    }
    DrawNewPackDialog();
    DrawToast();
    EndScreen();
}

void App::DrawShaderPackDetail(float x0, float y0, float x1, float y1) {
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();
    const ShaderPack* pk = reg.Find(shaderSelInstalled_);
    if (!pk && !reg.Packs().empty()) {
        shaderSelInstalled_ = reg.Packs().front().id;
        pk = &reg.Packs().front();
    }
    const float ip = Dp(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + ip, y0 + ip));
    ImGui::BeginChild("##packdetail", ImVec2(x1 - x0 - ip * 2.0f + Dp(8.0f), y1 - y0 - ip * 2.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float w = x1 - x0 - ip * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (!pk) {
        Para(Tr("셰이더 팩은 캐릭터의 재질 셰이딩을 바꿔 끼우는 확장입니다. 누구나 만들어 공유할 수 있습니다."), p.ink2);
        Gap(10.0f);
        if (Button("##docs0", Tr("만드는 법 보기"), icon::Info, ButtonKind::Ghost))
            OpenUrl("https://mmdx.codingbot.kr/" + lang + "/docs/shader-packs/");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + w * 9.0f / 16.0f);
        DrawPackImage(dl, PackThumb(*pk), "p:" + pk->id + ":" + std::to_string(reg.Generation()), a, b, Dp(12.0f));
        ImGui::Dummy(ImVec2(w, b.y - a.y));
    }
    Gap(14.0f);
    Para(pk->name.Get(lang), p.ink, size::Heading, Font::Bold);
    Para("v" + pk->version + "  ·  " + pk->id + "  ·  " + SourceLabel(pk->source), p.ink3, size::Caption);

    // problems first: authors need them, users need to know why it is not selectable
    if (!pk->Selectable() || pk->status == PackStatus::CompileError) {
        Gap(12.0f);
        ImU32 bg = 0, fg = 0;
        const char* st = StatusLabel(pk->status, bg, fg);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(a.x + Dp(12.0f), a.y + Dp(10.0f)));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - Dp(24.0f));
        Para(st ? st : "", fg, size::Small, Font::Semibold);
        Gap(4.0f);
        std::string msg = pk->statusMessage;
        if (msg.size() > 1500) msg = msg.substr(0, 1500) + "\n...";
        Para(msg.empty() ? std::string(Tr("mmdx12.log 에 자세한 내용이 있습니다")) : msg, p.ink2, size::Caption);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        const ImVec2 gb = ImGui::GetItemRectMax();
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(a, ImVec2(a.x + w, gb.y + Dp(10.0f)), bg ? bg : p.sunken, Dp(10.0f));
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(a.x, gb.y + Dp(10.0f)));
        ImGui::Dummy(ImVec2(w, 0));
    }

    Gap(14.0f);
    if (shaderDetailTab_ < 0 || shaderDetailTab_ > 2) shaderDetailTab_ = 0;
    const char* tabLabels[] = {Tr("정보"), Tr("설정"), Tr("관리")};
    Segmented("##packdetailtab", tabLabels, 3, &shaderDetailTab_, w / Dpi(), 36.0f);

    if (shaderDetailTab_ == 0) {
        if (!pk->description.Empty()) {
            Gap(12.0f);
            Para(pk->description.Get(lang), p.ink2);
        }
        if (!pk->recommendedFor.Empty()) {
            Caption(Tr("이런 모델에 맞춰 만들었어요"));
            Para(pk->recommendedFor.Get(lang), p.ink2);
        }
        if (pk->type == PackType::Surface) {
            Caption(Tr("지원하는 렌더 경로"));
            Para(pk->hasPtSurface ? Tr("래스터 · 레이 트레이싱 · 패스 트레이싱 · 오프라인 GI에 모두 적용") : Tr("래스터 · 레이 트레이싱에 적용 (이 팩은 패스 트레이싱 · 오프라인 GI용 셰이더가 없어 기본 셰이딩)"), p.ink2);
        }
        if (!pk->authors.empty()) {
            Caption(Tr("만든 사람"));
            for (size_t i = 0; i < pk->authors.size(); ++i) {
                const PackAuthor& a = pk->authors[i];
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Text(dl, Font::Semibold, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), p.ink, a.name.c_str());
                if (!a.role.empty())
                    Text(dl, Font::Regular, size::Caption,
                         ImVec2(c.x + TextSize(Font::Semibold, size::Small, a.name.c_str()).x + Dp(8.0f), c.y + Dp(7.0f)), p.ink3,
                         a.role.c_str());
                if (!a.url.empty()) {
                    ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(32.0f), c.y));
                    ImGui::PushID((int)i);
                    if (IconButton("##authorlink", icon::LinkSimple, a.url.c_str(), false, 28.0f)) OpenUrl(a.url);
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
                ImGui::Dummy(ImVec2(w, 0));
            }
        }
        if (!pk->license.empty() || !pk->homepage.empty() || !pk->repository.empty()) {
            Caption(Tr("라이선스 · 링크"));
            if (!pk->license.empty()) Para(pk->license, p.ink2);
            if (!pk->homepage.empty()) {
                Gap(4.0f);
                if (Button("##home", Tr("홈페이지"), icon::Globe, ButtonKind::Ghost)) OpenUrl(pk->homepage);
                if (!pk->repository.empty()) ImGui::SameLine();
            }
            if (!pk->repository.empty() && Button("##repo", Tr("소스 저장소"), icon::LinkSimple, ButtonKind::Ghost))
                OpenUrl(pk->repository);
        }
        if (!pk->tags.empty()) {
            Caption(Tr("태그"));
            BadgeFlow(pk->tags, w);
        }
    } else if (shaderDetailTab_ == 1) {
        if (!pk->params.empty()) {
            Caption(Tr("조절할 수 있는 값"));
            for (const ShaderPackParam& sp : pk->params) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
                char range[64];
                std::snprintf(range, sizeof(range), "%.2f  (%.2f – %.2f)", sp.def, sp.min, sp.max);
                TextEllipsis(dl, Font::Regular, size::Small, c, c.x + w * 0.6f, p.ink2, label.c_str());
                const ImVec2 rs = TextSize(Font::Regular, size::Caption, range);
                Text(dl, Font::Regular, size::Caption, ImVec2(c.x + w - rs.x, c.y + Dp(1.0f)), p.ink3, range);
                ImGui::Dummy(ImVec2(w, Dp(22.0f)));
            }
        }
        if (!pk->textures.empty()) {
            Caption(Tr("팩 텍스처"));
            std::string list;
            for (size_t i = 0; i < pk->textures.size(); ++i) {
                if (i) list += ",  ";
                list += pk->textures[i].file + (pk->textures[i].clamp ? " (clamp)" : "");
            }
            Para(list, p.ink3, size::Caption);
            const uint32_t missing = reg.MissingTextures(pk->id, reg.TextureFolder(pk->id));
            if (missing)
                Para(std::to_string(missing) +
                         Tr("개의 팩 텍스처가 없어 흰색으로 표시됩니다 - 텍스처 폴더를 지정하세요"),
                     p.warn, size::Caption);
            // the user texture folder: game textures that can't be redistributed (picker + clear)
            const std::filesystem::path folder = reg.TextureFolder(pk->id);
            ImGui::PushID("##packtexfolder");
            {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const std::string shown =
                    std::string(Tr("텍스처 폴더")) + ": " + (folder.empty() ? std::string(Tr("없음")) : PathToUtf8(folder));
                TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), c.x + w - Dp(96.0f), p.ink2,
                             shown.c_str());
                ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(88.0f), c.y));
                if (IconButton("##pick", icon::FolderOpen, Tr("텍스처 폴더 선택"))) {
                    const std::filesystem::path picked = PickPackTextureFolder(hwnd_);
                    if (!picked.empty()) {
                        reg.SetTextureFolder(pk->id, picked);
                        settings_.packTextureFolders[pk->id] = PathToUtf8(picked);
                        settings_.Save(settingsPath_);
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(folder.empty());
                if (IconButton("##clear", icon::X, Tr("텍스처 폴더 지우기"))) {
                    reg.SetTextureFolder(pk->id, {});
                    settings_.packTextureFolders.erase(pk->id);
                    settings_.Save(settingsPath_);
                }
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
                ImGui::Dummy(ImVec2(w, 0));
            }
            ImGui::PopID();
        }
        if (pk->params.empty() && pk->textures.empty()) {
            Gap(12.0f);
            Para(Tr("조절할 수 있는 항목이 없습니다"), p.ink3, size::Caption);
        }
        if (pk->type == PackType::Effect) {
            Gap(12.0f);
            Para(Tr("화면 효과는 로비의 셰이더 섹션(\"화면 효과\")과 재생 바의 ✦ 버튼에서 켜고 순서를 바꿀 수 있습니다."), p.ink3, size::Caption);
        }
    } else if (shaderDetailTab_ == 2) {
        Gap(12.0f);
        if (Button("##openpack", Tr("폴더 열기"), icon::FolderOpen, ButtonKind::Secondary)) OpenFolder(pk->dir);
        if (pk->source != PackSource::BuiltIn) {
            ImGui::SameLine();
            if (Button("##removepack", Tr("삭제"), icon::Trash, ButtonKind::Danger)) ImGui::OpenPopup("##removeconfirm");
        }
        Gap(8.0f);
        if (Button("##packdocs", Tr("셰이더 팩 만드는 법"), icon::Info, ButtonKind::Ghost))
            OpenUrl("https://mmdx.codingbot.kr/" + lang + "/docs/shader-packs/");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(18.0f), Dp(16.0f)));
        if (ImGui::BeginPopup("##removeconfirm")) {
            const std::string q = pk->name.Get(lang) + Tr(" 팩을 삭제할까요? 이 팩을 쓰던 캐릭터는 기본 셰이딩으로 돌아갑니다.");
            ImGui::PushTextWrapPos(Dp(300.0f));
            Para(q, p.ink);
            ImGui::PopTextWrapPos();
            Gap(10.0f);
            if (Button("##removeyes", Tr("삭제"), icon::Trash, ButtonKind::Danger)) {
                std::string err;
                const std::string id = pk->id;
                if (!reg.Uninstall(id, err)) {
                    toast_ = Toast{};
                    toast_.title = Tr("삭제하지 못했습니다");
                    toast_.detail = err;
                    toast_.error = true;
                    toast_.until = timeSeconds_ + 6.0;
                }
                shaderSelInstalled_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Button("##removeno", Tr("취소"), nullptr, ButtonKind::Ghost)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---- effect stack editor ------------------------------------------------------------------------------------------------------

// The ordered effect stack (RenderSettings::packEffects): per entry an enable switch, up / down / remove and the pack's
// sliders; "효과 추가" appends an installed effect pack. True = the stack changed this frame (the caller saves + applies).
bool App::DrawEffectStackEditor(std::vector<EffectStackEntry>& stack, bool header) {
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();

    if (header) SectionLabel(Tr("효과 스택"));
    Para(Tr("위에서 아래로 화면 효과를 차례로 적용합니다. 영상·스틸 렌더에도 같은 스택이 쓰입니다."), p.ink3, size::Caption);
    Gap(6.0f);

    // Structural edits (move / remove) are applied after the loop so every entry keeps drawing on the frame a slider
    // changes; breaking out early shrank the layout mid-drag and made the panel jitter.
    bool changed = false;
    size_t swapWith = SIZE_MAX, removeAt = SIZE_MAX, swapA = 0;
    for (size_t i = 0; i < stack.size(); ++i) {
        EffectStackEntry& e = stack[i];
        const ShaderPack* pk = reg.Find(e.pack);
        const bool usable = pk && pk->Selectable() && pk->type == PackType::Effect;
        const std::string name = pk ? pk->name.Get(lang) : e.pack;
        const char* where = !pk ? Tr("설치되어 있지 않음")
                            : !usable ? Tr("사용할 수 없음 · 셰이더 탭에서 확인")
                            : pk->stage == PackEffectStage::PreBloom ? Tr("블룸 앞 (HDR)") : Tr("톤맵 뒤");
        ImGui::PushID((int)i);
        bool on = e.enabled;
        if (Switch("##on", name.c_str(), &on, where)) {
            e.enabled = on;
            changed = true;
        }
        if (IconButton("##up", icon::CaretUp, Tr("위로"), false, 28.0f) && i > 0) {
            swapA = i - 1;
            swapWith = i;
        }
        ImGui::SameLine();
        if (IconButton("##down", icon::CaretDown, Tr("아래로"), false, 28.0f) && i + 1 < stack.size()) {
            swapA = i;
            swapWith = i + 1;
        }
        ImGui::SameLine();
        if (IconButton("##remove", icon::X, Tr("스택에서 제거"), false, 28.0f)) removeAt = i;
        if (usable && e.enabled && !pk->params.empty()) {
            const PackParamValues values = pk->Resolve(e.params);
            for (size_t k = 0; k < pk->params.size(); ++k) {
                const ShaderPackParam& sp = pk->params[k];
                float v = values[k];
                const std::string sid = "##fx_" + sp.key;
                const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
                if (SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, "%.2f")) {
                    e.params[sp.key] = v;
                    changed = true;
                }
            }
        }
        ImGui::PopID();
        Gap(8.0f);
    }
    if (removeAt != SIZE_MAX) {
        stack.erase(stack.begin() + (long)removeAt);
        changed = true;
    } else if (swapWith != SIZE_MAX) {
        std::swap(stack[swapA], stack[swapWith]);
        changed = true;
    }

    std::vector<const ShaderPack*> addable;
    for (const ShaderPack& pk : reg.Packs()) {
        if (pk.type != PackType::Effect || !pk.Selectable()) continue;
        bool in = false;
        for (const EffectStackEntry& e : stack) in |= e.pack == pk.id;
        if (!in) addable.push_back(&pk);
    }
    if (!addable.empty()) {
        if (Button("##fxadd", Tr("효과 추가"), icon::Plus, ButtonKind::Secondary)) ImGui::OpenPopup("##fxaddpopup");
        ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
        if (ImGui::BeginPopup("##fxaddpopup")) {
            for (const ShaderPack* pk : addable) {
                const std::string rid = "##fxadd_" + pk->id;
                if (MenuItem(rid.c_str(), pk->name.Get(lang).c_str())) {
                    stack.push_back({pk->id, true, {}, {}});
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    if (stack.empty()) Para(Tr("효과가 없습니다. 효과 팩을 추가하면 위에서 아래로 차례로 적용됩니다."), p.ink3, size::Caption);
    return changed;
}

void App::DrawRemotePackDetail(float x0, float y0, float x1, float y1) {
    const Palette& p = P();
    const std::string lang = PackLanguage();
    const RemotePack* rp = nullptr;
    for (const RemotePack& r : shaderStore_.Packs())
        if (r.id == shaderSelRemote_) rp = &r;
    if (!rp && !shaderStore_.Packs().empty()) {
        rp = &shaderStore_.Packs().front();
        shaderSelRemote_ = rp->id;
    }
    const float ip = Dp(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + ip, y0 + ip));
    ImGui::BeginChild("##remotedetail", ImVec2(x1 - x0 - ip * 2.0f + Dp(8.0f), y1 - y0 - ip * 2.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float w = x1 - x0 - ip * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (!rp) {
        Para(Tr("온라인 갤러리의 팩은 mmdx.codingbot.kr 에 올라온 것들입니다. 받은 파일은 크기와 SHA-256 을 확인한 뒤 설치합니다."), p.ink2);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + w * 9.0f / 16.0f);
        DrawPackImage(dl, RemotePackThumb(*rp), "r:" + rp->id + ":" + rp->version, a, b, Dp(12.0f));
        ImGui::Dummy(ImVec2(w, b.y - a.y));
    }
    Gap(14.0f);
    Para(rp->name.Get(lang), p.ink, size::Heading, Font::Bold);
    char meta[160];
    std::snprintf(meta, sizeof(meta), "v%s  ·  %s  ·  %.1f KB", rp->version.c_str(), rp->id.c_str(), rp->size / 1024.0);
    Para(meta, p.ink3, size::Caption);

    // install / update
    Gap(14.0f);
    const ShaderPack* inst = InstalledVersionOf(rp->id);
    if (shaderStore_.Installing(rp->id)) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ProgressBar(dl, a, ImVec2(a.x + w, a.y + Dp(6.0f)), shaderStore_.Progress(rp->id));
        ImGui::Dummy(ImVec2(w, Dp(12.0f)));
        Para(Tr("받는 중..."), p.ink3, size::Caption);
    } else if (!rp->Compatible()) {
        Para(Tr("이 버전의 MMDX12 에서는 쓸 수 없는 팩입니다. 앱을 업데이트하세요."), p.warn);
    } else if (inst && inst->source == PackSource::BuiltIn && net::CompareVersions(rp->version, inst->version) <= 0) {
        Para(Tr("앱에 이미 포함되어 있습니다."), p.ink3);
    } else {
        const bool newer = inst && net::CompareVersions(rp->version, inst->version) > 0;
        ImGui::BeginDisabled(inst && !newer);
        const char* label = !inst ? Tr("설치") : (newer ? Tr("업데이트") : Tr("최신 버전이 설치되어 있음"));
        if (Button("##storeinstall", label, inst && !newer ? icon::Check : icon::DownloadSimple, ButtonKind::Primary,
                   ImVec2(w / Dpi(), 44.0f)))
            shaderStore_.Install(*rp);
        ImGui::EndDisabled();
        if (newer) Para(Tr("설치된 버전: v") + inst->version, p.ink3, size::Caption);
    }

    if (!rp->description.Empty()) {
        Gap(12.0f);
        Para(rp->description.Get(lang), p.ink2);
    }
    if (!rp->recommendedFor.Empty()) {
        Caption(Tr("이런 모델에 맞춰 만들었어요"));
        Para(rp->recommendedFor.Get(lang), p.ink2);
    }
    if (!rp->authors.empty()) {
        Caption(Tr("만든 사람"));
        for (size_t i = 0; i < rp->authors.size(); ++i) {
            const PackAuthor& a = rp->authors[i];
            const ImVec2 c = ImGui::GetCursorScreenPos();
            Text(dl, Font::Semibold, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), p.ink, a.name.c_str());
            if (!a.role.empty())
                Text(dl, Font::Regular, size::Caption,
                     ImVec2(c.x + TextSize(Font::Semibold, size::Small, a.name.c_str()).x + Dp(8.0f), c.y + Dp(7.0f)), p.ink3,
                     a.role.c_str());
            if (!a.url.empty()) {
                ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(32.0f), c.y));
                ImGui::PushID((int)i);
                if (IconButton("##authorlink", icon::LinkSimple, a.url.c_str(), false, 28.0f)) OpenUrl(a.url);
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
            ImGui::Dummy(ImVec2(w, 0));
        }
    }
    if (!rp->license.empty() || !rp->homepage.empty() || !rp->repository.empty()) {
        Caption(Tr("라이선스 · 링크"));
        if (!rp->license.empty()) Para(rp->license, p.ink2);
        if (!rp->homepage.empty()) {
            Gap(4.0f);
            if (Button("##rhome", Tr("홈페이지"), icon::Globe, ButtonKind::Ghost)) OpenUrl(rp->homepage);
            if (!rp->repository.empty()) ImGui::SameLine();
        }
        if (!rp->repository.empty() && Button("##rrepo", Tr("소스 저장소"), icon::LinkSimple, ButtonKind::Ghost))
            OpenUrl(rp->repository);
    }
    if (!rp->tags.empty()) {
        Caption(Tr("태그"));
        BadgeFlow(rp->tags, w);
    }
    Caption(Tr("무결성"));
    Para("SHA-256  " + rp->sha256, p.ink3, size::Caption);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void App::DrawNewPackDialog() {
    if (!newPackOpen_) return;
    const Palette& p = P();
    ImGui::OpenPopup("##newpackdlg");
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(440.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    if (ImGui::BeginPopupModal("##newpackdlg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        const float w = ImGui::GetContentRegionAvail().x;
        Para(Tr("새 셰이더 팩"), p.ink, size::Title, Font::Bold);
        Gap(4.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        Para(Tr("주석이 달린 템플릿으로 shader_packs 폴더에 팩을 만듭니다. 앱을 켜 둔 채로 surface.hlsl 을 고치면 저장할 때마다 바로 반영됩니다."),
             p.ink2);
        ImGui::PopTextWrapPos();
        Gap(10.0f);
        if (Chip("##npsurface", Tr("표면"), nullptr, !newPackEffect_)) newPackEffect_ = false;
        ImGui::SameLine();
        if (Chip("##npeffect", Tr("효과"), nullptr, newPackEffect_)) newPackEffect_ = true;
        Gap(10.0f);
        TextField("##npid", Tr("id (영문 소문자, 숫자, _ -)"), newPackId_, sizeof(newPackId_), w / Dpi(), "my_toon");
        Gap(8.0f);
        TextField("##npname", Tr("이름"), newPackName_, sizeof(newPackName_), w / Dpi(), Tr("내 툰 셰이더"));
        Gap(8.0f);
        TextField("##npauthor", Tr("만든 사람"), newPackAuthor_, sizeof(newPackAuthor_), w / Dpi());
        const std::string id = newPackId_;
        const char* problem = nullptr;
        if (!id.empty() && !ValidShaderPackId(id)) problem = Tr("id 는 영문 소문자, 숫자, _, - 만 쓸 수 있습니다");
        else if (!id.empty() && ShaderPacks().Find(id)) problem = Tr("이미 있는 id 입니다");
        if (problem) {
            Gap(6.0f);
            Para(problem, p.danger, size::Caption);
        }
        Gap(16.0f);
        ImGui::BeginDisabled(id.empty() || problem || !newPackName_[0]);
        if (Button("##npcreate", Tr("만들고 폴더 열기"), icon::Plus, ButtonKind::Primary)) {
            std::filesystem::path dir;
            std::string err;
            if (ShaderPacks().CreateFromTemplate(id, newPackName_, newPackAuthor_, dir, err, newPackEffect_)) {
                shaderSelInstalled_ = id;
                OpenFolder(dir);
                newPackOpen_ = false;
                ImGui::CloseCurrentPopup();
            } else {
                toast_ = Toast{};
                toast_.title = Tr("팩을 만들지 못했습니다");
                toast_.detail = err;
                toast_.error = true;
                toast_.until = timeSeconds_ + 6.0;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("##npcancel", Tr("취소"), nullptr, ButtonKind::Ghost)) {
            newPackOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

} // namespace mmdx
