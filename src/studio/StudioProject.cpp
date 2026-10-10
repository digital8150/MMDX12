// Studio project file (.mmdxproj) I/O: atomic file writes and UTF-8 JSON, independent of the App
// and the renderer (see StudioProject.h for the format and the contracts of each function).
#include "studio/StudioProject.h"
#include "core/TextUtil.h"
#include <json.hpp>
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace mmdx::studio {

const char* ModelKindName(ModelKind k) {
    switch (k) {
    case ModelKind::Stage: return "stage";
    case ModelKind::Prop: return "prop";
    case ModelKind::Character: break;
    }
    return "character";
}

bool ParseModelKind(const std::string& s, ModelKind& out) {
    if (s == "character") { out = ModelKind::Character; return true; }
    if (s == "stage") { out = ModelKind::Stage; return true; }
    if (s == "prop") { out = ModelKind::Prop; return true; }
    return false;
}

DirectX::XMMATRIX PropOffsetMatrix(const PropAttach& a) {
    return DirectX::XMMatrixScaling(a.scale, a.scale, a.scale) *
           DirectX::XMMatrixRotationRollPitchYaw(DirectX::XMConvertToRadians(a.rotationDeg.x),
                                                 DirectX::XMConvertToRadians(a.rotationDeg.y),
                                                 DirectX::XMConvertToRadians(a.rotationDeg.z)) *
           DirectX::XMMatrixTranslation(a.translation.x, a.translation.y, a.translation.z);
}

namespace {

// ASCII-only lowercase (multibyte UTF-8 bytes are never folded): for the case-insensitive
// file-name comparisons, which only need to match through the ASCII subset on Windows.
std::wstring LowerAsciiWide(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return out;
}

// Absolute form with '/' separators, used by "recoveryOf" (always absolute) and by RelPath
// when the model lives on another drive.
std::string AbsPathUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.lexically_normal().generic_wstring());
}

// Relative form with '/' separators; absolute when the drive differs (see header). May start
// with "../" for files stored next to (not under) the project folder.
std::string RelPath(const std::filesystem::path& p, const std::filesystem::path& dir) {
    if (p.empty()) return "";
    const std::wstring pRoot = p.root_name().wstring();
    const std::wstring dirRoot = dir.root_name().wstring();
    if (!pRoot.empty() && !dirRoot.empty() && LowerAsciiWide(pRoot) != LowerAsciiWide(dirRoot))
        return AbsPathUtf8(p);
    const std::filesystem::path rel = p.lexically_normal().lexically_relative(dir.lexically_normal());
    return WideToUtf8(rel.generic_wstring());
}

// Inverse of RelPath: empty -> empty, absolute paths kept, relative ones anchored at `dir`.
std::filesystem::path ResolvePath(const std::string& s, const std::filesystem::path& dir) {
    if (s.empty()) return {};
    const std::filesystem::path p = Utf8ToPath(s);
    if (p.is_absolute()) return p;
    return (dir / p).lexically_normal();
}

// ---- guarded nlohmann reads: a wrong-typed key keeps the default, nothing throws ----
int ReadInt(const nlohmann::json& j, const char* key, int def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<int>();
}
double ReadDouble(const nlohmann::json& j, const char* key, double def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}
bool ReadBool(const nlohmann::json& j, const char* key, bool def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}
std::string ReadString(const nlohmann::json& j, const char* key, const std::string& def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}
DirectX::XMFLOAT2 ReadVec2(const nlohmann::json& j, const char* key, DirectX::XMFLOAT2 def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 2) return def;
    for (int i = 0; i < 2; ++i)
        if (!(*it)[i].is_number()) return def;
    return {(float)(*it)[0].get<double>(), (float)(*it)[1].get<double>()};
}
DirectX::XMFLOAT3 ReadVec3(const nlohmann::json& j, const char* key, DirectX::XMFLOAT3 def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return def;
    for (int i = 0; i < 3; ++i)
        if (!(*it)[i].is_number()) return def;
    return {(float)(*it)[0].get<double>(), (float)(*it)[1].get<double>(), (float)(*it)[2].get<double>()};
}
// The member `key` when it is an object, else nullptr.
const nlohmann::json* ObjectAt(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
}

// ---- scene lights (editor "lights", version 2) ----
nlohmann::json Vec3Json(const DirectX::XMFLOAT3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

nlohmann::json LightValuesJson(const LightValues& v) {
    return {
        {"position", Vec3Json(v.position)}, {"aim", Vec3Json(v.aim)},     {"direction", Vec3Json(v.direction)},
        {"color", Vec3Json(v.color)},       {"intensity", v.intensity},   {"range", v.range},
        {"coneOuter", v.coneOuter},         {"coneInner", v.coneInner},   {"size", {v.size.x, v.size.y}},
    };
}

nlohmann::json SceneLightJson(const SceneLight& l) {
    nlohmann::json j;
    j["uid"] = l.uid;
    j["kind"] = LightKindName(l.kind);
    j["name"] = l.name;
    j["enabled"] = l.enabled;
    j["values"] = LightValuesJson(l.v);
    j["sun"] = {{"vmdLink", l.vmdLink}, {"rimStrength", l.rimStrength}, {"rimColor", Vec3Json(l.rimColor)}};
    j["spot"] = {{"aim", AimModeName(l.aimMode)},
                 {"target", l.targetUid},
                 {"part", TargetPartName(l.targetPart)},
                 {"swayPhase", l.swayPhase}};
    j["sky"] = {{"zenith", Vec3Json(l.skyZenith)}, {"horizon", Vec3Json(l.skyHorizon)}, {"ground", Vec3Json(l.groundColor)}};
    j["shadow"] = {{"type", ShadowTypeName(l.shadow)},
                   {"softness", l.shadowSoftness},
                   {"density", l.shadowDensity},
                   {"color", Vec3Json(l.shadowColor)}};
    j["falloff"] = FalloffTypeName(l.falloff);
    j["affectDiffuse"] = l.affectDiffuse;
    j["affectSpecular"] = l.affectSpecular;
    j["viewportVisible"] = l.viewportVisible;
    nlohmann::json keys = nlohmann::json::array();
    for (const LightKey& k : l.keys) keys.push_back({{"frame", k.frame}, {"values", LightValuesJson(k.v)}});
    j["keys"] = std::move(keys);
    return j;
}

void ReadLightValues(const nlohmann::json& j, LightValues& v) {
    v.position = ReadVec3(j, "position", v.position);
    v.aim = ReadVec3(j, "aim", v.aim);
    v.direction = ReadVec3(j, "direction", v.direction);
    v.color = ReadVec3(j, "color", v.color);
    v.intensity = (float)ReadDouble(j, "intensity", v.intensity);
    v.range = (float)ReadDouble(j, "range", v.range);
    v.coneOuter = (float)ReadDouble(j, "coneOuter", v.coneOuter);
    v.coneInner = (float)ReadDouble(j, "coneInner", v.coneInner);
    v.size = ReadVec2(j, "size", v.size);
}

// One "lights" entry. Missing keys keep the SceneLight defaults; a key without some values holds the light's base values.
// False (with a warning) for an unknown kind.
bool ReadSceneLight(const nlohmann::json& jl, SceneLight& l, std::vector<std::string>* warnings) {
    const std::string kindStr = ReadString(jl, "kind", "");
    if (!ParseLightKind(kindStr, l.kind)) {
        if (warnings) warnings->push_back("unknown light kind: " + kindStr);
        return false;
    }
    l.uid = (uint32_t)std::max(0, ReadInt(jl, "uid", 0));
    l.name = ReadString(jl, "name", "");
    l.enabled = ReadBool(jl, "enabled", l.enabled);
    if (const nlohmann::json* v = ObjectAt(jl, "values")) ReadLightValues(*v, l.v);
    if (const nlohmann::json* s = ObjectAt(jl, "sun")) {
        l.vmdLink = ReadBool(*s, "vmdLink", l.vmdLink);
        l.rimStrength = (float)ReadDouble(*s, "rimStrength", l.rimStrength);
        l.rimColor = ReadVec3(*s, "rimColor", l.rimColor);
    }
    if (const nlohmann::json* s = ObjectAt(jl, "spot")) {
        ParseAimMode(ReadString(*s, "aim", ""), l.aimMode);
        l.targetUid = (uint32_t)std::max(0, ReadInt(*s, "target", 0));
        ParseTargetPart(ReadString(*s, "part", ""), l.targetPart);
        l.swayPhase = (float)ReadDouble(*s, "swayPhase", l.swayPhase);
    }
    if (const nlohmann::json* s = ObjectAt(jl, "sky")) {
        l.skyZenith = ReadVec3(*s, "zenith", l.skyZenith);
        l.skyHorizon = ReadVec3(*s, "horizon", l.skyHorizon);
        l.groundColor = ReadVec3(*s, "ground", l.groundColor);
    }
    if (const nlohmann::json* s = ObjectAt(jl, "shadow")) {
        ParseShadowType(ReadString(*s, "type", ""), l.shadow);
        l.shadowSoftness = (float)ReadDouble(*s, "softness", l.shadowSoftness);
        l.shadowDensity = (float)ReadDouble(*s, "density", l.shadowDensity);
        l.shadowColor = ReadVec3(*s, "color", l.shadowColor);
    }
    ParseFalloffType(ReadString(jl, "falloff", ""), l.falloff);
    l.affectDiffuse = ReadBool(jl, "affectDiffuse", l.affectDiffuse);
    l.affectSpecular = ReadBool(jl, "affectSpecular", l.affectSpecular);
    l.viewportVisible = ReadBool(jl, "viewportVisible", l.viewportVisible);
    const auto keysIt = jl.find("keys");
    if (keysIt != jl.end() && keysIt->is_array()) {
        for (const nlohmann::json& jk : *keysIt) {
            if (!jk.is_object()) continue;
            LightKey k;
            k.frame = std::max(0, ReadInt(jk, "frame", 0));
            k.v = l.v;
            if (const nlohmann::json* v = ObjectAt(jk, "values")) ReadLightValues(*v, k.v);
            UpsertKey(l.keys, k);  // sorted by frame; a repeated frame keeps the last key
        }
    }
    return true;
}

// ---- version 1 lighting -> scene lights ----
// Version 1 projects stored the studio lighting as a source choice (VMD track / preset / custom rig), an optional key
// light override and a spot rig ("lighting" in the editor object; before that only "useLightTrack"). These structs
// read those fields (with the defaults of the old rig) for ConvertLegacyLighting.
enum class LegacySource { Vmd, Preset, Custom };
enum class LegacyMode { Auto, Center, Head, Manual };

struct LegacySpotKey {
    int frame = 0;
    DirectX::XMFLOAT3 position{0, 45, -15}, aim{0, 0, 0}, color{1, 1, 1};
    float intensity = 2.6f, cone = 0.24f;
};
struct LegacySpot {
    std::string name;
    LegacyMode mode = LegacyMode::Auto;
    DirectX::XMFLOAT3 position{0, 45, -15}, aim{0, 0, 0}, color{1.0f, 0.98f, 0.92f};
    float intensity = 2.6f, cone = 0.24f;
    bool enabled = true;
    float phase = 0.0f;
    std::vector<LegacySpotKey> keys;
};
struct LegacyLighting {
    LegacySource source = LegacySource::Vmd;
    int preset = 0;
    bool keyEnabled = false;  // the "key" object was present
    DirectX::XMFLOAT3 keyDirection{-0.5f, -1.0f, 0.5f}, keyColor{0.6f, 0.6f, 0.6f}, keyRimColor{1.0f, 0.97f, 0.92f};
    float keyIntensity = 1.0f, keyRimStrength = 0.35f;
    std::vector<LegacySpot> spots;
    bool frontFill = false;
};

LegacyLighting ReadLegacyLighting(const nlohmann::json& li) {
    LegacyLighting old;
    const std::string source = ReadString(li, "source", "vmd");  // anything else keeps the old default: the VMD track
    if (source == "preset") old.source = LegacySource::Preset;
    else if (source == "custom") old.source = LegacySource::Custom;
    old.preset = ReadInt(li, "preset", old.preset);
    old.frontFill = ReadBool(li, "frontFill", old.frontFill);
    if (const nlohmann::json* k = ObjectAt(li, "key")) {
        old.keyEnabled = true;
        old.keyDirection = ReadVec3(*k, "direction", old.keyDirection);
        old.keyColor = ReadVec3(*k, "color", old.keyColor);
        old.keyIntensity = (float)ReadDouble(*k, "intensity", old.keyIntensity);
        old.keyRimStrength = (float)ReadDouble(*k, "rimStrength", old.keyRimStrength);
        old.keyRimColor = ReadVec3(*k, "rimColor", old.keyRimColor);
    }
    const auto spotsIt = li.find("spots");
    if (spotsIt != li.end() && spotsIt->is_array()) {
        for (const nlohmann::json& sp : *spotsIt) {
            if (!sp.is_object()) continue;
            LegacySpot s;
            s.name = ReadString(sp, "name", "");
            const std::string mode = ReadString(sp, "mode", "auto");
            if (mode == "center") s.mode = LegacyMode::Center;
            else if (mode == "head") s.mode = LegacyMode::Head;
            else if (mode == "manual") s.mode = LegacyMode::Manual;
            s.position = ReadVec3(sp, "position", s.position);
            s.aim = ReadVec3(sp, "aim", s.aim);
            s.color = ReadVec3(sp, "color", s.color);
            s.intensity = (float)ReadDouble(sp, "intensity", s.intensity);
            s.cone = (float)ReadDouble(sp, "cone", s.cone);
            s.enabled = ReadBool(sp, "enabled", s.enabled);
            s.phase = (float)ReadDouble(sp, "phase", s.phase);
            const auto keysIt = sp.find("keys");
            if (keysIt != sp.end() && keysIt->is_array()) {
                for (const nlohmann::json& kk : *keysIt) {
                    if (!kk.is_object()) continue;
                    LegacySpotKey k;
                    k.frame = ReadInt(kk, "frame", 0);
                    k.position = ReadVec3(kk, "position", k.position);
                    k.aim = ReadVec3(kk, "aim", k.aim);
                    k.color = ReadVec3(kk, "color", k.color);
                    k.intensity = (float)ReadDouble(kk, "intensity", k.intensity);
                    k.cone = (float)ReadDouble(kk, "cone", k.cone);
                    s.keys.push_back(k);
                }
            }
            // spots were identified by their (unique) names: a nameless or repeated one never existed
            const bool repeated = std::any_of(old.spots.begin(), old.spots.end(),
                                              [&](const LegacySpot& o) { return o.name == s.name; });
            if (!s.name.empty() && !repeated) old.spots.push_back(std::move(s));
        }
    }
    return old;
}

// The scene lights that reproduce a version 1 lighting setup:
//   VMD track: the preset's lights, the sun linked to the camera VMD light track (the key override did not apply).
//   Preset:    the preset's lights, the sun unlinked, with the key override on the sun when it was enabled.
//   Custom:    the preset's sun and ambient light (as in Preset), the rig's spots in order (the cone's inner angle was
//              derived: 0.6 * outer) and the front fill instead of the preset's own spots and fill.
// Uids are 1..N in list order.
std::vector<SceneLight> ConvertLegacyLighting(const LegacyLighting& old, LegacySource source) {
    uint32_t counter = 1;
    std::vector<SceneLight> lights = PresetLights(old.preset, DirectX::XMFLOAT3{0.0f, 10.0f, 0.0f}, counter);
    SceneLight* sun = nullptr;
    for (SceneLight& l : lights)
        if (l.kind == LightKind::Sun) { sun = &l; break; }
    if (sun) sun->vmdLink = source == LegacySource::Vmd;
    if (source == LegacySource::Vmd) return lights;
    if (sun && old.keyEnabled) {
        sun->v.direction = old.keyDirection;
        sun->v.color = old.keyColor;
        sun->v.intensity = old.keyIntensity;
        sun->rimStrength = old.keyRimStrength;
        sun->rimColor = old.keyRimColor;
    }
    if (source == LegacySource::Custom) {
        lights.erase(std::remove_if(lights.begin(), lights.end(),
                                    [](const SceneLight& l) { return l.kind == LightKind::Spot || l.kind == LightKind::Point; }),
                     lights.end());
        for (const LegacySpot& s : old.spots) {
            SceneLight l;
            l.name = s.name;
            l.kind = LightKind::Spot;
            l.enabled = s.enabled;
            l.v.position = s.position;
            l.v.aim = s.aim;
            l.v.color = s.color;
            l.v.intensity = s.intensity;
            l.v.range = 140.0f;
            l.v.coneOuter = s.cone;
            l.v.coneInner = std::min(s.cone * 0.6f, s.cone);
            switch (s.mode) {
            case LegacyMode::Auto: l.aimMode = AimMode::Sway; break;
            case LegacyMode::Center: l.aimMode = AimMode::Target; l.targetPart = TargetPart::Centre; break;
            case LegacyMode::Head: l.aimMode = AimMode::Target; l.targetPart = TargetPart::Head; break;
            case LegacyMode::Manual: l.aimMode = AimMode::Manual; break;
            }
            l.swayPhase = s.phase;
            for (const LegacySpotKey& k : s.keys) {
                LightKey lk;
                lk.frame = k.frame;
                lk.v = l.v;
                lk.v.position = k.position;
                lk.v.aim = k.aim;
                lk.v.color = k.color;
                lk.v.intensity = k.intensity;
                lk.v.coneOuter = k.cone;
                lk.v.coneInner = std::min(k.cone * 0.6f, k.cone);
                UpsertKey(l.keys, lk);
            }
            lights.push_back(std::move(l));
        }
        if (old.frontFill) {  // the warm front fill of the old rig (the concert preset's fill, at the default focus)
            SceneLight fill;
            fill.name = "채움광";
            fill.kind = LightKind::Point;
            fill.v.position = {0.0f, 32.0f, -40.0f};
            fill.v.color = {0.917f, 0.83f, 0.72f};
            fill.v.intensity = 0.55f;
            fill.v.range = 120.0f;
            lights.push_back(std::move(fill));
        }
    }
    for (size_t i = 0; i < lights.size(); ++i) lights[i].uid = (uint32_t)i + 1;
    return lights;
}

} // namespace

bool WriteFileAtomic(const std::filesystem::path& file, const std::string& bytes, std::string* error) {
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.native() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + PathToUtf8(file) + ": cannot open temporary file";
            return false;
        }
        if (!bytes.empty()) f.write(bytes.data(), (std::streamsize)bytes.size());
        f.close();
        if (!f) {  // the stream state still holds a failure that happened while writing or closing
            std::filesystem::remove(tmp, ec);
            if (error) *error = "cannot write " + PathToUtf8(file) + ": temporary file write failed";
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(tmp, ec);
        if (error)
            *error = "cannot write " + PathToUtf8(file) + ": rename failed (GetLastError() " +
                     std::to_string(code) + ")";
        return false;
    }
    return true;
}

bool SaveVmdAtomic(const std::filesystem::path& file, const VmdMotion& vmd, std::string* error) {
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.native() + L".tmp";
    if (!SaveVmd(tmp, vmd, error)) {  // SaveVmd already reported; add the target file for context
        if (error && !error->empty()) *error = "cannot write " + PathToUtf8(file) + ": " + *error;
        std::filesystem::remove(tmp, ec);  // also its own inner temp, on early failures
        std::filesystem::remove(tmp.wstring() + L".tmp", ec);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(tmp, ec);
        if (error)
            *error = "cannot write " + PathToUtf8(file) + ": rename failed (GetLastError() " +
                     std::to_string(code) + ")";
        return false;
    }
    return true;
}

std::string SanitizeFileName(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (const char ch : utf8) {  // multibyte UTF-8 bytes are >= 0x80, so only ASCII is touched
        const unsigned char b = (unsigned char)ch;
        const bool invalid = b < 0x20 || b == '\\' || b == '/' || b == ':' || b == '*' || b == '?' ||
                             b == '"' || b == '<' || b == '>' || b == '|';
        out.push_back(invalid ? '_' : ch);
    }
    size_t begin = 0, end = out.size();
    while (begin < end && (out[begin] == ' ' || out[begin] == '.')) ++begin;
    while (end > begin && (out[end - 1] == ' ' || out[end - 1] == '.')) --end;
    out = out.substr(begin, end - begin);
    return out.empty() ? "model" : out;
}

bool SaveProject(const std::filesystem::path& file, const ProjectData& data, std::string* error) {
    try {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::absolute(file, ec);
        dir = dir.parent_path();
        const std::wstring stem = file.stem().wstring();

        // ---- name allocation: camera first, then models in order; unique case-insensitively ----
        std::set<std::wstring> usedLower;
        auto allocName = [&](const std::wstring& base) -> std::wstring {
            for (int n = 1;; ++n) {
                std::wstring cand = base;
                if (n > 1) cand += L" (" + std::to_wstring(n) + L")";
                cand += L".vmd";
                const std::wstring lower = LowerAsciiWide(cand);
                if (!usedLower.count(lower)) {
                    usedLower.insert(lower);
                    return cand;
                }
            }
        };

        std::wstring camName;
        if (!data.camera.camera.empty() || !data.camera.light.empty() || !data.camera.shadow.empty())
            camName = allocName(stem + L" - camera");

        std::vector<std::wstring> modelVmdNames(data.models.size());  // empty = no VMD for this model
        for (size_t i = 0; i < data.models.size(); ++i) {
            const ProjectModel& m = data.models[i];
            if (m.motion.Empty()) continue;
            modelVmdNames[i] = allocName(stem + L" - " + Utf8ToWide(SanitizeFileName(m.name)));
        }

        // ---- write every VMD first: a failure leaves the old project file untouched ----
        for (size_t i = 0; i < data.models.size(); ++i) {
            if (modelVmdNames[i].empty()) continue;
            VmdMotion vmd = data.models[i].motion.ToVmd();  // ToVmd copies modelName
            if (!SaveVmdAtomic(dir / modelVmdNames[i], vmd, error)) return false;
        }
        if (!camName.empty()) {
            MotionData camOnly;  // bones/morphs of data.camera are ignored (see header)
            camOnly.camera = data.camera.camera;
            camOnly.light = data.camera.light;
            camOnly.shadow = data.camera.shadow;
            VmdMotion vmd = camOnly.ToVmd();
            vmd.modelName = "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E";  // カメラ・照明
            if (!SaveVmdAtomic(dir / camName, vmd, error)) return false;
        }

        // ---- remember which VMDs the previous version referenced, for the cleanup below ----
        std::vector<std::filesystem::path> oldFiles;
        if (std::filesystem::exists(file, ec)) {
            std::ifstream in(file, std::ios::binary);
            if (in) {
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const nlohmann::json old = nlohmann::json::parse(text, nullptr, false);
                if (!old.is_discarded() && old.is_object()) {
                    auto collect = [&](const nlohmann::json& ref) {
                        if (ref.is_string()) oldFiles.push_back(ResolvePath(ref.get<std::string>(), dir));
                    };
                    if (old.contains("camera")) collect(old["camera"]);
                    if (old.contains("models") && old["models"].is_array())
                        for (const auto& m : old["models"])
                            if (m.is_object() && m.contains("motion")) collect(m["motion"]);
                }
            }
        }

        // ---- the JSON ----
        nlohmann::json j;
        j["format"] = kProjectFormatName;
        j["version"] = kProjectFormatVersion;
        nlohmann::json models = nlohmann::json::array();
        for (size_t i = 0; i < data.models.size(); ++i) {
            const ProjectModel& m = data.models[i];
            nlohmann::json jm;
            jm["name"] = m.name;
            jm["kind"] = ModelKindName(m.kind);
            jm["path"] = RelPath(m.path, dir);
            jm["libraryId"] = m.libraryId;
            jm["visible"] = m.visible;
            if (m.kind != ModelKind::Prop && !(m.place == PropAttach{})) {  // identity placement is not written
                jm["place"] = {
                    {"t", {m.place.translation.x, m.place.translation.y, m.place.translation.z}},
                    {"r", {m.place.rotationDeg.x, m.place.rotationDeg.y, m.place.rotationDeg.z}},
                    {"s", m.place.scale},
                };
            }
            // the default shading alone is not written; settings remembered for other packs are
            if (m.kind == ModelKind::Character &&
                (!m.shader.pack.empty() || !m.shader.remembered.empty() || !m.shader.materials.empty())) {
                nlohmann::json params = nlohmann::json::object();
                for (const auto& [k, v] : m.shader.params) params[k] = v;
                jm["shader"] = {{"pack", m.shader.pack}, {"params", params}};
                if (!m.shader.textureFolder.empty())
                    jm["shader"]["textureFolder"] = m.shader.textureFolder;
                if (!m.shader.materials.empty()) {   // per-material class / default shading, by material name
                    nlohmann::json mats = nlohmann::json::object();
                    for (const auto& [name, cls] : m.shader.materials) mats[name] = cls;
                    jm["shader"]["materials"] = mats;
                }
                if (!m.shader.remembered.empty()) {
                    nlohmann::json rem = nlohmann::json::object();
                    for (const auto& [packId, memo] : m.shader.remembered) {
                        nlohmann::json mp = nlohmann::json::object();
                        for (const auto& [k, v] : memo.params) mp[k] = v;
                        rem[packId] = {{"params", mp}};
                        if (!memo.textureFolder.empty()) rem[packId]["textureFolder"] = memo.textureFolder;
                    }
                    jm["shader"]["remembered"] = rem;
                }
            }
            if (m.kind == ModelKind::Prop) {  // "attach" is written for props only
                const PropAttach& a = m.attach;
                jm["attach"] = {
                    {"parent", a.parent},
                    {"bone", a.bone},
                    {"t", {a.translation.x, a.translation.y, a.translation.z}},
                    {"r", {a.rotationDeg.x, a.rotationDeg.y, a.rotationDeg.z}},
                    {"s", a.scale},
                };
            }
            jm["motion"] = modelVmdNames[i].empty() ? nlohmann::json(nullptr)
                                                    : nlohmann::json(WideToUtf8(modelVmdNames[i]));
            models.push_back(std::move(jm));
        }
        j["models"] = std::move(models);
        j["camera"] = camName.empty() ? nlohmann::json(nullptr) : nlohmann::json(WideToUtf8(camName));
        if (data.audioPath.empty()) {
            j["audio"] = nullptr;
        } else {
            j["audio"] = {{"path", RelPath(data.audioPath, dir)}, {"offset", data.audioOffset}};
        }
        {
            nlohmann::json ed;
            ed["frame"] = data.editor.frame;
            ed["selectedModel"] = data.editor.selectedModel;
            ed["useMotionCamera"] = data.editor.useMotionCamera;
            ed["useShadowTrack"] = data.editor.useShadowTrack;
            ed["showCameraPath"] = data.editor.showCameraPath;
            ed["loop"] = data.editor.loop;
            ed["physics"] = data.editor.physics;
            ed["range"] = {data.editor.rangeStart, data.editor.rangeEnd};
            ed["pxPerFrame"] = data.editor.pxPerFrame;
            const DirectX::XMFLOAT3& t = data.editor.camTarget;
            ed["freeCamera"] = {
                {"target", {t.x, t.y, t.z}},
                {"yaw", data.editor.camYaw},
                {"pitch", data.editor.camPitch},
                {"distance", data.editor.camDistance},
                {"fov", data.editor.camFovDeg},
            };
            // the scene lights, always written (an empty array when there are none)
            nlohmann::json lights = nlohmann::json::array();
            for (const SceneLight& l : data.editor.lights) lights.push_back(SceneLightJson(l));
            ed["lights"] = std::move(lights);
            // the DoF focus track (no VMD equivalent); a target is 1 + the model index (0 none)
            nlohmann::json focus = nlohmann::json::array();
            for (const FocusKf& k : data.camera.focus)
                focus.push_back({{"frame", k.frame},
                                 {"mode", k.mode == FocusMode::Target   ? "target"
                                          : k.mode == FocusMode::Manual ? "manual"
                                                                        : "auto"},
                                 {"bone", k.bone == FocusBone::UpperBody ? "upperBody"
                                          : k.bone == FocusBone::Center  ? "center"
                                                                         : "head"},
                                 {"target", k.target},
                                 {"distance", k.distance},
                                 {"aperture", k.aperture},
                                 {"transition", k.transition}});
            ed["focus"] = std::move(focus);
            j["editor"] = std::move(ed);
        }
        if (!data.recoveryOf.empty())
            j["recoveryOf"] = AbsPathUtf8(data.recoveryOf);  // autosaves always point at the original

        if (!WriteFileAtomic(file, j.dump(2), error)) return false;

        // ---- cleanup: delete stale VMDs the old JSON referenced (same folder, "<stem> - *.vmd") ----
        std::set<std::wstring> writtenLower;
        if (!camName.empty())
            writtenLower.insert(LowerAsciiWide((dir / camName).lexically_normal().generic_wstring()));
        for (const std::wstring& name : modelVmdNames)
            if (!name.empty())
                writtenLower.insert(LowerAsciiWide((dir / name).lexically_normal().generic_wstring()));
        const std::wstring dirLower = LowerAsciiWide(dir.lexically_normal().generic_wstring());
        const std::wstring prefixLower = LowerAsciiWide(stem + L" - ");
        for (const std::filesystem::path& old : oldFiles) {
            const std::filesystem::path norm = old.lexically_normal();
            if (writtenLower.count(LowerAsciiWide(norm.generic_wstring()))) continue;
            if (LowerAsciiWide(norm.parent_path().generic_wstring()) != dirLower) continue;
            const std::wstring fn = LowerAsciiWide(norm.filename().wstring());
            if (fn.compare(0, prefixLower.size(), prefixLower) != 0) continue;
            if (fn.size() < prefixLower.size() + 4 || fn.compare(fn.size() - 4, 4, L".vmd") != 0) continue;
            std::filesystem::remove(old, ec);  // best effort, ignore failure
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadProject(const std::filesystem::path& file, ProjectData& out, std::string* error,
                 std::vector<std::string>* warnings) {
    out = ProjectData{};
    try {
        std::string text;
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                if (error) *error = "cannot open " + PathToUtf8(file);
                return false;
            }
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0) {
                if (error) *error = "cannot open " + PathToUtf8(file);
                return false;
            }
            in.seekg(0, std::ios::beg);
            text.resize((size_t)size);
            if (size > 0) in.read(text.data(), (std::streamsize)size);
            if (size > 0 && !in) {  // short read
                if (error) *error = "cannot open " + PathToUtf8(file);
                return false;
            }
        }

        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            if (error) *error = "not a project file";
            return false;
        }
        if (ReadString(j, "format", "") != kProjectFormatName) {
            if (error) *error = "not an MMDX12 project";
            return false;
        }
        const int version = ReadInt(j, "version", 0);
        if (version > kProjectFormatVersion) {
            if (error) *error = "made by a newer MMDX12 (version " + std::to_string(version) + ")";
            return false;
        }

        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::absolute(file, ec).parent_path();

        // old JSON index -> out.models index; -1 = skipped (bad entry / unknown kind)
        std::map<int, int> remap;
        if (j.contains("models") && j["models"].is_array()) {
            const nlohmann::json& arr = j["models"];
            for (size_t idx = 0; idx < arr.size(); ++idx) {
                const nlohmann::json& m = arr[idx];
                const int oldIndex = (int)idx;
                if (!m.is_object()) {
                    remap[oldIndex] = -1;
                    continue;
                }
                ModelKind kind = ModelKind::Character;
                const std::string kindStr = ReadString(m, "kind", "character");
                if (!ParseModelKind(kindStr, kind)) {
                    if (warnings) warnings->push_back("unknown model kind: " + kindStr);
                    remap[oldIndex] = -1;
                    continue;
                }
                ProjectModel pm;
                pm.name = ReadString(m, "name", "");
                pm.kind = kind;
                pm.path = ResolvePath(ReadString(m, "path", ""), dir);
                pm.libraryId = ReadString(m, "libraryId", "");
                pm.visible = ReadBool(m, "visible", true);
                if (kind != ModelKind::Prop && m.contains("place") && m["place"].is_object()) {
                    const nlohmann::json& pl = m["place"];  // older projects have none: identity
                    pm.place.translation = ReadVec3(pl, "t", pm.place.translation);
                    pm.place.rotationDeg = ReadVec3(pl, "r", pm.place.rotationDeg);
                    pm.place.scale = (float)ReadDouble(pl, "s", pm.place.scale);
                }
                if (kind == ModelKind::Character && m.contains("shader") && m["shader"].is_object()) {
                    const nlohmann::json& sh = m["shader"];  // older projects have none: the default shading
                    pm.shader.pack = ReadString(sh, "pack", "");
                    if (sh.contains("params") && sh["params"].is_object())
                        for (const auto& [k, v] : sh["params"].items())
                            if (v.is_number()) pm.shader.params[k] = v.get<float>();
                    pm.shader.textureFolder = ReadString(sh, "textureFolder", "");
                    if (sh.contains("materials") && sh["materials"].is_object())
                        for (const auto& [name, v] : sh["materials"].items())
                            if (v.is_number_integer() && v.get<int>() >= kMaterialPackOff && v.get<int>() <= (int)PackClass::Weapon)
                                pm.shader.materials[name] = v.get<int>();
                    if (sh.contains("remembered") && sh["remembered"].is_object()) {
                        for (const auto& [packId, mv] : sh["remembered"].items()) {
                            if (!mv.is_object()) continue;
                            ShaderMemo memo;
                            if (mv.contains("params") && mv["params"].is_object())
                                for (const auto& [k, v] : mv["params"].items())
                                    if (v.is_number()) memo.params[k] = v.get<float>();
                            memo.textureFolder = ReadString(mv, "textureFolder", "");
                            pm.shader.remembered[packId] = std::move(memo);
                        }
                    }
                }
                if (kind == ModelKind::Prop && m.contains("attach") && m["attach"].is_object()) {
                    const nlohmann::json& a = m["attach"];  // "attach" is read for props only
                    pm.attach.parent = ReadInt(a, "parent", -1);
                    pm.attach.bone = ReadString(a, "bone", "");
                    pm.attach.translation = ReadVec3(a, "t", pm.attach.translation);
                    pm.attach.rotationDeg = ReadVec3(a, "r", pm.attach.rotationDeg);
                    pm.attach.scale = (float)ReadDouble(a, "s", pm.attach.scale);
                }
                if (m.contains("motion") && m["motion"].is_string()) {
                    const std::filesystem::path vmdPath = ResolvePath(m["motion"].get<std::string>(), dir);
                    VmdMotion vmd;
                    if (LoadVmd(vmdPath, vmd)) {
                        pm.motion = MotionData::FromVmd(vmd);
                    } else {
                        if (warnings) warnings->push_back("motion missing: " + PathToUtf8(vmdPath));
                    }
                }
                remap[oldIndex] = (int)out.models.size();
                out.models.push_back(std::move(pm));
            }
        }

        if (j.contains("camera") && j["camera"].is_string()) {
            const std::filesystem::path vmdPath = ResolvePath(j["camera"].get<std::string>(), dir);
            VmdMotion vmd;
            if (LoadVmd(vmdPath, vmd)) {
                MotionData full = MotionData::FromVmd(vmd);
                MotionData cam;  // take ONLY camera/light/shadow; keep modelName
                cam.modelName = std::move(full.modelName);
                cam.camera = std::move(full.camera);
                cam.light = std::move(full.light);
                cam.shadow = std::move(full.shadow);
                out.camera = std::move(cam);
            } else if (warnings) {
                warnings->push_back("camera missing: " + PathToUtf8(vmdPath));
            }
        }

        if (j.contains("audio") && j["audio"].is_object()) {
            out.audioPath = ResolvePath(ReadString(j["audio"], "path", ""), dir);
            out.audioOffset = ReadDouble(j["audio"], "offset", out.audioOffset);
        }

        // The editor state. A version 1 file's lighting (a source choice + spot rig, or only "useLightTrack") is converted
        // to scene lights, also when the file has no editor object (the old default: the VMD track, Studio preset).
        ProjectEditor ed;  // defaults, then overwrite what the file carries
        LegacyLighting legacy;
        bool hadLegacyLighting = false, hadLegacyUseLightTrack = false, legacyUseLightTrack = true;
        if (j.contains("editor") && j["editor"].is_object()) {
            const nlohmann::json& e = j["editor"];
            ed.frame = ReadInt(e, "frame", ed.frame);
            ed.selectedModel = ReadInt(e, "selectedModel", ed.selectedModel);
            ed.useMotionCamera = ReadBool(e, "useMotionCamera", ed.useMotionCamera);
            ed.useShadowTrack = ReadBool(e, "useShadowTrack", ed.useShadowTrack);
            ed.showCameraPath = ReadBool(e, "showCameraPath", ed.showCameraPath);
            ed.loop = ReadBool(e, "loop", ed.loop);
            ed.physics = ReadBool(e, "physics", ed.physics);
            const auto range = e.find("range");
            if (range != e.end() && range->is_array() && range->size() == 2 && (*range)[0].is_number() &&
                (*range)[1].is_number()) {
                ed.rangeStart = (*range)[0].get<int>();
                ed.rangeEnd = (*range)[1].get<int>();
            }
            ed.pxPerFrame = (float)ReadDouble(e, "pxPerFrame", ed.pxPerFrame);
            if (e.contains("freeCamera") && e["freeCamera"].is_object()) {
                const nlohmann::json& fc = e["freeCamera"];
                ed.camTarget = ReadVec3(fc, "target", ed.camTarget);
                ed.camYaw = (float)ReadDouble(fc, "yaw", ed.camYaw);
                ed.camPitch = (float)ReadDouble(fc, "pitch", ed.camPitch);
                ed.camDistance = (float)ReadDouble(fc, "distance", ed.camDistance);
                ed.camFovDeg = (float)ReadDouble(fc, "fov", ed.camFovDeg);
            }
            if (version >= 2) {
                // the scene lights: absent means none (not the defaults)
                const auto lightsIt = e.find("lights");
                if (lightsIt != e.end() && lightsIt->is_array()) {
                    for (const nlohmann::json& jl : *lightsIt) {
                        SceneLight l;
                        if (jl.is_object() && ReadSceneLight(jl, l, warnings)) ed.lights.push_back(std::move(l));
                    }
                }
            } else {
                hadLegacyUseLightTrack = e.contains("useLightTrack") && e.find("useLightTrack")->is_boolean();
                legacyUseLightTrack = ReadBool(e, "useLightTrack", legacyUseLightTrack);
                if (const nlohmann::json* li = ObjectAt(e, "lighting")) {
                    legacy = ReadLegacyLighting(*li);
                    hadLegacyLighting = true;
                }
            }
        }
        if (j.contains("editor") && j["editor"].is_object()) {
            const auto focusIt = j["editor"].find("focus");
            if (focusIt != j["editor"].end() && focusIt->is_array()) {
                for (const nlohmann::json& jk : *focusIt) {
                    if (!jk.is_object()) continue;
                    FocusKf k;
                    k.frame = std::max(0, ReadInt(jk, "frame", 0));
                    const std::string mode = ReadString(jk, "mode", "auto");
                    k.mode = mode == "target" ? FocusMode::Target : mode == "manual" ? FocusMode::Manual : FocusMode::Auto;
                    const std::string bone = ReadString(jk, "bone", "head");
                    k.bone = bone == "upperBody" ? FocusBone::UpperBody : bone == "center" ? FocusBone::Center : FocusBone::Head;
                    k.target = (uint32_t)std::max(0, ReadInt(jk, "target", 0));
                    k.distance = std::clamp((float)ReadDouble(jk, "distance", k.distance), 0.5f, 3000.0f);
                    k.aperture = std::clamp((float)ReadDouble(jk, "aperture", k.aperture), 0.0f, 3.0f);
                    k.transition = std::clamp(ReadInt(jk, "transition", 0), 0, 600);
                    UpsertKey(out.camera.focus, k);
                }
            }
        }
        if (version >= 2) {
            // uids identify the lights: a missing, zero or repeated one gets a fresh uid
            uint32_t nextUid = 1;
            for (const SceneLight& l : ed.lights) nextUid = std::max(nextUid, l.uid + 1);
            std::set<uint32_t> used;
            for (SceneLight& l : ed.lights) {
                if (l.uid == 0 || used.count(l.uid)) l.uid = nextUid++;
                used.insert(l.uid);
            }
        } else {
            // the old rule: the "lighting" object's source; else useLightTrack (true only with light keys in the camera
            // VMD, else the preset); else the default, the VMD track
            LegacySource source = LegacySource::Vmd;
            if (hadLegacyLighting) source = legacy.source;
            else if (hadLegacyUseLightTrack) source = legacyUseLightTrack && !out.camera.light.empty() ? LegacySource::Vmd : LegacySource::Preset;
            ed.lights = ConvertLegacyLighting(legacy, source);
        }
        out.editor = ed;

        out.recoveryOf = ResolvePath(ReadString(j, "recoveryOf", ""), dir);

        // Remap prop "parent" indices through the old->new map; a parent that points at a skipped
        // model, or out of range, becomes -1 (the world origin).
        for (ProjectModel& pm : out.models) {
            if (pm.kind != ModelKind::Prop) continue;
            const auto it = remap.find(pm.attach.parent);
            pm.attach.parent = it == remap.end() ? -1 : it->second;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace mmdx::studio
