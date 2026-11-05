// Studio project file (.mmdxproj) I/O: atomic file writes and UTF-8 JSON, independent of the App
// and the renderer (see StudioProject.h for the format and the contracts of each function).
#include "studio/StudioProject.h"
#include "core/TextUtil.h"
#include "asset/SceneLightJson.h"
#include <json.hpp>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

namespace mmdx::studio {

static void WriteFovExact(nlohmann::json& object, const std::vector<CameraKf>& keys) {
    nlohmann::json values = nlohmann::json::array();
    for (const CameraKf& k : keys)
        if (std::fabs(k.fovDeg - std::round(k.fovDeg)) > 1e-4f)
            values.push_back({k.frame, k.fovDeg});
    if (!values.empty()) object["fovExact"] = std::move(values);
}

static void ReadFovExact(const nlohmann::json& object, std::vector<CameraKf>& keys) {
    if (!object.contains("fovExact") || !object["fovExact"].is_array()) return;
    for (const auto& entry : object["fovExact"]) {
        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_unsigned() || !entry[1].is_number()) continue;
        const auto frame = entry[0].get<uint32_t>();
        const auto it = std::find_if(keys.begin(), keys.end(), [frame](const CameraKf& k) { return k.frame == frame; });
        if (it != keys.end()) it->fovDeg = entry[1].get<float>();
    }
}

const char* ModelKindName(ModelKind k) {
    switch (k) {
    case ModelKind::Stage: return "stage";
    case ModelKind::Prop: return "prop";
    case ModelKind::Effect: return "effect";
    case ModelKind::Character: break;
    }
    return "character";
}

bool ParseModelKind(const std::string& s, ModelKind& out) {
    if (s == "character") { out = ModelKind::Character; return true; }
    if (s == "stage") { out = ModelKind::Stage; return true; }
    if (s == "prop") { out = ModelKind::Prop; return true; }
    if (s == "effect") { out = ModelKind::Effect; return true; }
    return false;
}

DirectX::XMMATRIX PropOffsetMatrix(const PropAttach& a) {
    return DirectX::XMMatrixScaling(a.scale, a.scale, a.scale) *
           DirectX::XMMatrixRotationRollPitchYaw(DirectX::XMConvertToRadians(a.rotationDeg.x),
                                                 DirectX::XMConvertToRadians(a.rotationDeg.y),
                                                 DirectX::XMConvertToRadians(a.rotationDeg.z)) *
           DirectX::XMMatrixTranslation(a.translation.x, a.translation.y, a.translation.z);
}

size_t StudioWorld::ApproxBytes() const {
    return sizeof(StudioWorld) + name.size() + fx.ApproxBytes();
}

bool StudioWorld::operator==(const StudioWorld& o) const {
    return name == o.name && fx == o.fx && place == o.place;
}

size_t StudioCamera::ApproxBytes() const {
    return sizeof(StudioCamera) + name.size() + track.ApproxBytes();
}

bool StudioCamera::operator==(const StudioCamera& o) const {
    return name == o.name && visible == o.visible && stageUid == o.stageUid && track.camera.size() == o.track.camera.size() &&
           std::equal(track.camera.begin(), track.camera.end(), o.track.camera.begin(), [](const CameraKf& a, const CameraKf& b) {
               return a.frame == b.frame && a.distance == b.distance && a.target.x == b.target.x &&
                      a.target.y == b.target.y && a.target.z == b.target.z && a.rotation.x == b.rotation.x &&
                      a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z && a.fovDeg == b.fovDeg &&
                      a.perspective == b.perspective && std::memcmp(a.interp, b.interp, sizeof(a.interp)) == 0;
           });
}

size_t StudioStage::ApproxBytes() const {
    size_t bytes = sizeof(StudioStage) + stageId.size() + variant.size();
    for (const auto& [k, v] : base) bytes += k.size() + sizeof(StageValue);
    for (const EffectParamTrack& t : tracks)
        bytes += sizeof(EffectParamTrack) + t.param.size() + t.keys.capacity() * sizeof(EffectParamKey);
    for (const std::string& c : cuesOff) bytes += sizeof(std::string) + c.size();
    for (const auto& [id, s] : screens)
        bytes += id.size() + sizeof(StageScreenOverride) + (s.source ? s.source->size() : 0) +
                 (s.file ? s.file->size() : 0) + (s.camera ? s.camera->size() : 0);
    return bytes;
}

bool StudioStage::operator==(const StudioStage& o) const {
    if (stageId != o.stageId || manifestPath != o.manifestPath || variant != o.variant || base != o.base ||
        cuesOff != o.cuesOff || screens != o.screens || tracks.size() != o.tracks.size())
        return false;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const EffectParamTrack& a = tracks[i];
        const EffectParamTrack& b = o.tracks[i];
        if (a.param != b.param || a.keys.size() != b.keys.size()) return false;
        for (size_t k = 0; k < a.keys.size(); ++k)
            if (a.keys[k].frame != b.keys[k].frame || a.keys[k].value != b.keys[k].value ||
                a.keys[k].stepped != b.keys[k].stepped)
                return false;
    }
    return true;
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

// ---- S3: stage param values (a number, or [r,g,b(,a)] for a colour; an enum is its option index) ----
nlohmann::json StageValueJson(const StageValue& v) {
    if (v.type == StageValue::Type::Color) return nlohmann::json::array({v.x, v.y, v.z, v.w});
    if (v.type == StageValue::Type::Text) return v.text;
    return v.x;
}

bool ReadStageValue(const nlohmann::json& j, StageValue& out) {
    if (j.is_number()) {
        out = StageValue::Number(j.get<float>());
        return true;
    }
    if (j.is_boolean()) {
        out = StageValue::Number(j.get<bool>() ? 1.0f : 0.0f);
        return true;
    }
    if (j.is_array() && j.size() >= 3 && j.size() <= 4) {
        float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        for (size_t i = 0; i < j.size(); ++i) {
            if (!j[i].is_number()) return false;
            c[i] = j[i].get<float>();
        }
        out = StageValue::Color(DirectX::XMFLOAT4{c[0], c[1], c[2], c[3]});
        return true;
    }
    return false;
}

// ---- scene lights (editor "lights", version 2; the JSON itself: asset/SceneLightJson.h) ----

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
        // S4: one VMD per extra camera ("<stem> - camera <name>.vmd"), camera keys only.
        std::vector<std::wstring> cameraVmdNames(data.cameras.size());
        for (size_t i = 0; i < data.cameras.size(); ++i) {
            if (data.cameras[i].track.camera.empty()) continue;
            cameraVmdNames[i] = allocName(stem + L" - camera " + Utf8ToWide(SanitizeFileName(data.cameras[i].name)));
        }

        std::vector<std::wstring> modelVmdNames(data.models.size());  // empty = no VMD for this model
        for (size_t i = 0; i < data.models.size(); ++i) {
            const ProjectModel& m = data.models[i];
            if (!m.motion.HasVmdKeys()) continue;
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
        for (size_t i = 0; i < data.cameras.size(); ++i) {
            if (cameraVmdNames[i].empty()) continue;
            MotionData camOnly;  // an extra camera carries camera keys only
            camOnly.camera = data.cameras[i].track.camera;
            VmdMotion vmd = camOnly.ToVmd();
            vmd.modelName = "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E";
            if (!SaveVmdAtomic(dir / cameraVmdNames[i], vmd, error)) return false;
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
                    if (old.contains("cameras") && old["cameras"].is_array())
                        for (const auto& c : old["cameras"])
                            if (c.is_object() && c.contains("motion")) collect(c["motion"]);
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
            if (m.kind == ModelKind::Effect && !m.fx.empty()) jm["fx"] = RelPath(m.fx, dir);
            jm["libraryId"] = m.libraryId;            jm["visible"] = m.visible;
            if (m.physicsOff) jm["physics"] = false;  // P2.2: only written when off (absent = physics on)
            // constant (stopwatch-off) bone / morph tracks: names of single frame-0 key tracks flagged static
            {
                nlohmann::json sb = nlohmann::json::array();
                for (const std::string& n : m.motion.staticBones)
                    if (IsStaticBoneTrack(m.motion, n)) sb.push_back(n);
                if (!sb.empty()) jm["staticBones"] = std::move(sb);
                nlohmann::json sm = nlohmann::json::array();
                for (const std::string& n : m.motion.staticMorphs)
                    if (IsStaticMorphTrack(m.motion, n)) sm.push_back(n);
                if (!sm.empty()) jm["staticMorphs"] = std::move(sm);
            }
            if (m.kind != ModelKind::Prop && m.kind != ModelKind::Effect && !(m.place == PropAttach{})) {  // identity placement is not written
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
            // S1b: the variant chosen for an MMDX stage, saved as the model's "stageVariant" (absent = the default).
            // Version 4: the variant lives on the stage entity instead; only the part's stage uid is written.
            if (m.stageUid != 0) jm["stageUid"] = m.stageUid;
            else if (!m.stageVariant.empty()) jm["stageVariant"] = m.stageVariant;
            // P5: the per-model PBR material source (Default is not written); the .emd path is relative to the project.
            // Written for every kind (character, stage, prop).
            if (m.material.ray) {
                jm["material"] = {{"ray", true}};
                if (!m.material.emd.empty()) jm["material"]["emd"] = RelPath(Utf8ToPath(m.material.emd), dir);
            }
            // M6a: the per-model MME choice (absent = characters None / stages Auto); the file is relative.
            {
                const MmeChoice mmeDefault = m.kind == ModelKind::Stage ? MmeChoice{MmeChoice::Auto, {}}
                                                                        : MmeChoice{};
                if (!(m.mme == mmeDefault)) {
                    jm["mme"] = {{"mode", m.mme.mode == MmeChoice::Auto ? "auto"
                                             : m.mme.mode == MmeChoice::File ? "file"
                                                                             : "none"}};
                    if (m.mme.mode == MmeChoice::File && !m.mme.file.empty())
                        jm["mme"]["file"] = RelPath(m.mme.file, dir);
                }
            }
            // M5c: an effect object shares the accessory key track (its keys drive the controller panel).
            if (m.kind == ModelKind::Prop || m.kind == ModelKind::Effect) {
                const PropAttach& a = m.attach;
                jm["attach"] = {
                    {"parent", a.parent},
                    {"bone", a.bone},
                    {"t", {a.translation.x, a.translation.y, a.translation.z}},
                    {"r", {a.rotationDeg.x, a.rotationDeg.y, a.rotationDeg.z}},
                    {"s", a.scale},
                };
                // the accessory keys (no VMD equivalent); a parent is 1 + the model index (0 = the world origin)
                if (!m.motion.accessory.empty()) {
                    nlohmann::json acc = nlohmann::json::array();
                    for (const AccessoryKf& k : m.motion.accessory)
                        acc.push_back({{"frame", k.frame},
                                       {"t", {k.translation.x, k.translation.y, k.translation.z}},
                                       {"r", {k.rotationDeg.x, k.rotationDeg.y, k.rotationDeg.z}},
                                       {"s", k.scale},
                                       {"opacity", k.opacity},
                                       {"visible", k.visible},
                                       {"parent", (int)k.parentUid},
                                       {"bone", k.bone}});
                    jm["accessory"] = std::move(acc);
                }
            }
            jm["motion"] = modelVmdNames[i].empty() ? nlohmann::json(nullptr)
                                                    : nlohmann::json(WideToUtf8(modelVmdNames[i]));
            models.push_back(std::move(jm));
        }
        j["models"] = std::move(models);
        j["camera"] = camName.empty() ? nlohmann::json(nullptr) : nlohmann::json(WideToUtf8(camName));
        WriteFovExact(j, data.camera.camera);
        // Version 5: the extra cameras (uid / name / visible / stageUid + their camera VMD) and the switch track.
        {
            nlohmann::json cams = nlohmann::json::array();
            for (size_t i = 0; i < data.cameras.size(); ++i) {
                const StudioCamera& c = data.cameras[i];
                nlohmann::json jc;
                jc["uid"] = c.uid;
                jc["name"] = c.name;
                jc["visible"] = c.visible;
                if (c.stageUid != 0) jc["stageUid"] = c.stageUid;
                jc["motion"] = cameraVmdNames[i].empty() ? nlohmann::json(nullptr)
                                                         : nlohmann::json(WideToUtf8(cameraVmdNames[i]));
                WriteFovExact(jc, c.track.camera);
                cams.push_back(std::move(jc));
            }
            j["cameras"] = std::move(cams);
        }
        // M6b: the project's own MME post stack (effects after the scene, replaces the play-mode stack).
        {
            nlohmann::json mme = nlohmann::json::array();
            for (const MmePostEntry& e : data.mmePost)
                mme.push_back({{"path", RelPath(e.path, dir)}, {"enabled", e.enabled}});
            j["mmeEffects"] = std::move(mme);
        }
        // Version 3: the project's effect stack (order = stack order).
        {
            nlohmann::json effects = nlohmann::json::array();
            for (const StudioEffect& e : data.effects) {
                nlohmann::json je;
                je["pack"] = e.entry.pack;
                je["enabled"] = e.entry.enabled;
                nlohmann::json params = nlohmann::json::object();
                for (const auto& [k, v] : e.entry.params) params[k] = v;
                je["params"] = std::move(params);
                if (!e.entry.textureFolder.empty()) je["textureFolder"] = e.entry.textureFolder;
                nlohmann::json tracks = nlohmann::json::array();
                for (const EffectParamTrack& t : e.tracks) {
                    nlohmann::json keys = nlohmann::json::array();
                    for (const EffectParamKey& k : t.keys) keys.push_back({k.frame, k.value, k.stepped ? 1 : 0});
                    tracks.push_back({{"param", t.param}, {"keys", std::move(keys)}});
                }
                je["tracks"] = std::move(tracks);
                effects.push_back(std::move(je));
            }
            j["effects"] = std::move(effects);
        }
        // Version 3: placed world entities (same track JSON as effects + "place").
        {
            nlohmann::json worlds = nlohmann::json::array();
            for (const StudioWorld& w : data.worlds) {
                const StudioEffect& e = w.fx;
                nlohmann::json jw;
                jw["name"] = w.name;
                jw["pack"] = e.entry.pack;
                jw["enabled"] = e.entry.enabled;
                nlohmann::json params = nlohmann::json::object();
                for (const auto& [k, v] : e.entry.params) params[k] = v;
                jw["params"] = std::move(params);
                if (!e.entry.textureFolder.empty()) jw["textureFolder"] = e.entry.textureFolder;
                nlohmann::json tracks = nlohmann::json::array();
                for (const EffectParamTrack& t : e.tracks) {
                    nlohmann::json keys = nlohmann::json::array();
                    for (const EffectParamKey& k : t.keys) keys.push_back({k.frame, k.value, k.stepped ? 1 : 0});
                    tracks.push_back({{"param", t.param}, {"keys", std::move(keys)}});
                }
                jw["tracks"] = std::move(tracks);
                jw["place"] = {
                    {"t", {w.place.translation.x, w.place.translation.y, w.place.translation.z}},
                    {"r", {w.place.rotationDeg.x, w.place.rotationDeg.y, w.place.rotationDeg.z}},
                    {"s", w.place.scale},
                };
                worlds.push_back(std::move(jw));
            }
            j["worlds"] = std::move(worlds);
        }
        // Version 4: MMDX stage entities (variant / base values / keyed params / cues off; the installed stage is never
        // written, so this array is the whole override state).
        {
            nlohmann::json stages = nlohmann::json::array();
            for (const StudioStage& st : data.stages) {
                nlohmann::json js;
                js["uid"] = st.uid;
                js["stageId"] = st.stageId;
                js["path"] = RelPath(st.manifest ? st.manifest->manifestPath : st.manifestPath, dir);
                js["variant"] = st.variant;
                nlohmann::json base = nlohmann::json::object();
                for (const auto& [k, v] : st.base) base[k] = StageValueJson(v);
                js["base"] = std::move(base);
                nlohmann::json tracks = nlohmann::json::array();
                for (const EffectParamTrack& t : st.tracks) {
                    nlohmann::json keys = nlohmann::json::array();
                    for (const EffectParamKey& k : t.keys) keys.push_back({k.frame, k.value, k.stepped ? 1 : 0});
                    tracks.push_back({{"param", t.param}, {"keys", std::move(keys)}});
                }
                js["tracks"] = std::move(tracks);
                nlohmann::json cues = nlohmann::json::array();
                for (const std::string& c : st.cuesOff) cues.push_back(c);
                js["cuesOff"] = std::move(cues);
                // S5 (version 6): the project's screen overrides (absent members keep the manifest's values)
                nlohmann::json screens = nlohmann::json::object();
                for (const auto& [id, s] : st.screens) {
                    nlohmann::json so;
                    if (s.source) so["source"] = *s.source;
                    if (s.file) so["file"] = RelPath(Utf8ToPath(*s.file), dir);
                    if (s.camera) so["camera"] = *s.camera;
                    if (s.brightness) so["brightness"] = *s.brightness;
                    screens[id] = std::move(so);
                }
                js["screens"] = std::move(screens);
                stages.push_back(std::move(js));
            }
            j["stages"] = std::move(stages);
        }
        if (data.audioPath.empty()) {
            j["audio"] = nullptr;
        } else {
            j["audio"] = {{"path", RelPath(data.audioPath, dir)}, {"offset", data.audioOffset}};
        }
        if (data.bgVideoPath.empty()) {
            j["backgroundVideo"] = nullptr;
        } else {
            j["backgroundVideo"] = {{"path", RelPath(data.bgVideoPath, dir)},
                                    {"offset", data.bgVideoOffset},
                                    {"asBackground", data.bgVideoAsBackground}};
        }
        {   // the gravity and the wind, the defaults too (the file says what it was simulated with)
            PhysicsEnv pe = data.physicsEnv;
            pe.Sanitize();
            j["physicsWorld"] = {
                {"gravity", {{"acceleration", pe.gravity},
                             {"direction", {pe.gravityDir.x, pe.gravityDir.y, pe.gravityDir.z}}}},
                {"wind", {{"direction", {pe.windDir.x, pe.windDir.y, pe.windDir.z}},
                          {"strength", pe.windStrength},
                          {"gust", pe.windGust},
                          {"gustFrequency", pe.windGustFreq},
                          {"jitter", pe.windJitter}}},
            };
        }
        {
            nlohmann::json ed;
            ed["frame"] = data.editor.frame;
            ed["selectedModel"] = data.editor.selectedModel;
            ed["useMotionCamera"] = data.editor.useMotionCamera;
            ed["useShadowTrack"] = data.editor.useShadowTrack;
            ed["showCameraPath"] = data.editor.showCameraPath;
            const int aspectIdx = AspectIndexFromWH(data.editor.aspectW, data.editor.aspectH);
            ed["aspect"] = {{"w", kAspectPresets[aspectIdx].w}, {"h", kAspectPresets[aspectIdx].h}};
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
            for (const SceneLight& l : data.editor.lights) lights.push_back(SceneLightToJson(l));
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
        for (const std::wstring& name : cameraVmdNames)   // S4: the extra cameras' VMDs are this save's files too
            if (!name.empty())
                writtenLower.insert(LowerAsciiWide((dir / name).lexically_normal().generic_wstring()));
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
        std::map<uint32_t, uint32_t> stageUidRemap;  // a repeated stage uid -> its fresh one (part models / lights)
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
                if (kind == ModelKind::Effect) pm.fx = ResolvePath(ReadString(m, "fx", ""), dir);
                pm.libraryId = ReadString(m, "libraryId", "");
                pm.visible = ReadBool(m, "visible", true);
                pm.physicsOff = !ReadBool(m, "physics", true);  // absent = physics on; false = baked / off
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
                // P5: the per-model PBR material source (absent = Default); read for every kind
                if (m.contains("material") && m["material"].is_object()) {
                    const nlohmann::json& mt = m["material"];
                    pm.material.ray = ReadBool(mt, "ray", false);
                    if (pm.material.ray) {
                        const std::string emd = ReadString(mt, "emd", "");
                        if (!emd.empty()) pm.material.emd = PathToUtf8(ResolvePath(emd, dir));
                    }
                }
                // S1b: an MMDX stage part's saved variant (absent = the manifest's default variant); version 4 files
                // carry the stage entity uid instead and the variant in "stages".
                pm.stageVariant = ReadString(m, "stageVariant", "");
                pm.stageUid = (uint32_t)std::max(0, ReadInt(m, "stageUid", 0));
                // M6a: the per-model MME choice (absent = characters None / stages Auto)
                pm.mme = kind == ModelKind::Stage ? MmeChoice{MmeChoice::Auto, {}} : MmeChoice{};
                if (m.contains("mme") && m["mme"].is_object()) {
                    const nlohmann::json& mm = m["mme"];
                    const std::string mode = ReadString(mm, "mode", "none");
                    pm.mme.mode = mode == "auto" ? MmeChoice::Auto : mode == "file" ? MmeChoice::File : MmeChoice::None;
                    if (pm.mme.mode == MmeChoice::File) {
                        const std::string file = ReadString(mm, "file", "");
                        if (!file.empty()) pm.mme.file = ResolvePath(file, dir);
                    }
                }
                if ((kind == ModelKind::Prop || kind == ModelKind::Effect) && m.contains("attach") &&
                    m["attach"].is_object()) {
                    const nlohmann::json& a = m["attach"];  // "attach" is read for props / effect objects only
                    pm.attach.parent = ReadInt(a, "parent", -1);
                    pm.attach.bone = ReadString(a, "bone", "");
                    pm.attach.translation = ReadVec3(a, "t", pm.attach.translation);
                    pm.attach.rotationDeg = ReadVec3(a, "r", pm.attach.rotationDeg);
                    pm.attach.scale = (float)ReadDouble(a, "s", pm.attach.scale);
                }
                // the accessory keys (a parent is 1 + the project model index here; converted by the caller)
                if ((kind == ModelKind::Prop || kind == ModelKind::Effect) && m.contains("accessory") &&
                    m["accessory"].is_array()) {
                    for (const nlohmann::json& jk : m["accessory"]) {
                        if (!jk.is_object()) continue;
                        AccessoryKf k;
                        k.frame = std::max(0, ReadInt(jk, "frame", 0));
                        k.translation = ReadVec3(jk, "t", k.translation);
                        k.rotationDeg = ReadVec3(jk, "r", k.rotationDeg);
                        k.scale = (float)ReadDouble(jk, "s", k.scale);
                        k.opacity = std::clamp((float)ReadDouble(jk, "opacity", k.opacity), 0.0f, 1.0f);
                        k.visible = ReadBool(jk, "visible", k.visible);
                        k.parentUid = (uint32_t)std::max(0, ReadInt(jk, "parent", 0));
                        k.bone = ReadString(jk, "bone", "");
                        UpsertKey(pm.motion.accessory, k);
                    }
                }
                if (m.contains("motion") && m["motion"].is_string()) {
                    const std::filesystem::path vmdPath = ResolvePath(m["motion"].get<std::string>(), dir);
                    VmdMotion vmd;
                    if (LoadVmd(vmdPath, vmd)) {
                        std::vector<AccessoryKf> accessory = std::move(pm.motion.accessory);  // not in the VMD
                        pm.motion = MotionData::FromVmd(vmd);
                        pm.motion.accessory = std::move(accessory);
                    } else {
                        if (warnings) warnings->push_back("motion missing: " + PathToUtf8(vmdPath));
                    }
                }
                // constant (stopwatch-off) bone / morph tracks (after the motion load, which replaces it)
                if (m.contains("staticBones") && m["staticBones"].is_array())
                    for (const nlohmann::json& n : m["staticBones"])
                        if (n.is_string()) pm.motion.staticBones.insert(n.get<std::string>());
                if (m.contains("staticMorphs") && m["staticMorphs"].is_array())
                    for (const nlohmann::json& n : m["staticMorphs"])
                        if (n.is_string()) pm.motion.staticMorphs.insert(n.get<std::string>());
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

        ReadFovExact(j, out.camera.camera);

        // Version 5: the extra cameras (their camera keys come from their own VMD) and the switch track.
        if (version >= 5 && j.contains("cameras") && j["cameras"].is_array()) {
            for (const nlohmann::json& jc : j["cameras"]) {
                if (!jc.is_object()) continue;
                StudioCamera c;
                c.uid = (uint32_t)std::max(0, ReadInt(jc, "uid", 0));
                if (c.uid == 0) continue;  // uid 0 is the main camera
                c.name = ReadString(jc, "name", "");
                if (c.name.empty()) c.name = "camera " + std::to_string(c.uid);
                c.visible = ReadBool(jc, "visible", c.visible);
                c.stageUid = (uint32_t)std::max(0, ReadInt(jc, "stageUid", 0));
                if (jc.contains("motion") && jc["motion"].is_string()) {
                    const std::filesystem::path vmdPath = ResolvePath(jc["motion"].get<std::string>(), dir);
                    VmdMotion vmd;
                    if (LoadVmd(vmdPath, vmd)) c.track.camera = MotionData::FromVmd(vmd).camera;
                    else if (warnings) warnings->push_back("camera missing: " + PathToUtf8(vmdPath));
                }
                ReadFovExact(jc, c.track.camera);
                out.cameras.push_back(std::move(c));
            }
        }

        if (j.contains("audio") && j["audio"].is_object()) {
            out.audioPath = ResolvePath(ReadString(j["audio"], "path", ""), dir);
            out.audioOffset = ReadDouble(j["audio"], "offset", out.audioOffset);
        }

        if (j.contains("backgroundVideo") && j["backgroundVideo"].is_object()) {
            out.bgVideoPath = ResolvePath(ReadString(j["backgroundVideo"], "path", ""), dir);
            out.bgVideoOffset = ReadDouble(j["backgroundVideo"], "offset", out.bgVideoOffset);
            out.bgVideoAsBackground = ReadBool(j["backgroundVideo"], "asBackground", out.bgVideoAsBackground);
        }

        // The gravity and the wind (absent: MMD's default gravity and no wind, as older files were simulated).
        if (const nlohmann::json* pw = ObjectAt(j, "physicsWorld")) {
            PhysicsEnv& pe = out.physicsEnv;
            if (const nlohmann::json* g = ObjectAt(*pw, "gravity")) {
                pe.gravity = (float)ReadDouble(*g, "acceleration", pe.gravity);
                pe.gravityDir = ReadVec3(*g, "direction", pe.gravityDir);
            }
            if (const nlohmann::json* w = ObjectAt(*pw, "wind")) {
                pe.windDir = ReadVec3(*w, "direction", pe.windDir);
                pe.windStrength = (float)ReadDouble(*w, "strength", pe.windStrength);
                pe.windGust = (float)ReadDouble(*w, "gust", pe.windGust);
                pe.windGustFreq = (float)ReadDouble(*w, "gustFrequency", pe.windGustFreq);
                pe.windJitter = (float)ReadDouble(*w, "jitter", pe.windJitter);
            }
            pe.Sanitize();
        }

        // M6b: the project's own MME post stack.
        if (j.contains("mmeEffects") && j["mmeEffects"].is_array()) {
            for (const auto& je : j["mmeEffects"]) {
                if (!je.is_object()) continue;
                MmePostEntry e;
                e.path = ResolvePath(ReadString(je, "path", ""), dir);
                e.enabled = ReadBool(je, "enabled", true);
                if (!e.path.empty()) out.mmePost.push_back(std::move(e));
            }
        }

        // Version 3: the project's effect stack. Missing keys keep defaults; malformed entries are skipped with a warning.
        if (version >= 3 && j.contains("effects") && j["effects"].is_array()) {
            for (const auto& je : j["effects"]) {
                if (!je.is_object()) {
                    if (warnings) warnings->push_back("malformed effect entry skipped");
                    continue;
                }
                StudioEffect e;
                e.entry.pack = ReadString(je, "pack", "");
                if (e.entry.pack.empty()) {
                    if (warnings) warnings->push_back("effect entry without a pack skipped");
                    continue;
                }
                e.entry.enabled = ReadBool(je, "enabled", true);
                if (je.contains("params") && je["params"].is_object())
                    for (const auto& [k, v] : je["params"].items())
                        if (v.is_number()) e.entry.params[k] = v.get<float>();
                e.entry.textureFolder = ReadString(je, "textureFolder", "");
                if (je.contains("tracks") && je["tracks"].is_array()) {
                    for (const auto& jt : je["tracks"]) {
                        if (!jt.is_object()) continue;
                        EffectParamTrack t;
                        t.param = ReadString(jt, "param", "");
                        if (t.param.empty()) continue;
                        if (jt.contains("keys") && jt["keys"].is_array()) {
                            for (const auto& jk : jt["keys"]) {
                                if (!jk.is_array() || jk.size() < 2 || !jk[0].is_number() || !jk[1].is_number())
                                    continue;
                                EffectParamKey k;
                                k.frame = jk[0].get<int32_t>();
                                k.value = jk[1].get<float>();
                                k.stepped = jk.size() >= 3 && jk[2].is_number() && jk[2].get<int>() != 0;
                                UpsertParamKey(t, k);
                            }
                        }
                        e.tracks.push_back(std::move(t));
                    }
                }
                out.effects.push_back(std::move(e));
            }
        }

        // Version 3: world entities. Missing keys keep defaults; malformed entries are skipped with a warning.
        if (version >= 3 && j.contains("worlds") && j["worlds"].is_array()) {
            for (const auto& jw : j["worlds"]) {
                if (!jw.is_object()) {
                    if (warnings) warnings->push_back("malformed world entry skipped");
                    continue;
                }
                StudioWorld w;
                w.name = ReadString(jw, "name", "");
                w.fx.entry.pack = ReadString(jw, "pack", "");
                if (w.fx.entry.pack.empty()) {
                    if (warnings) warnings->push_back("world entry without a pack skipped");
                    continue;
                }
                w.fx.entry.enabled = ReadBool(jw, "enabled", true);
                if (jw.contains("params") && jw["params"].is_object())
                    for (const auto& [k, v] : jw["params"].items())
                        if (v.is_number()) w.fx.entry.params[k] = v.get<float>();
                w.fx.entry.textureFolder = ReadString(jw, "textureFolder", "");
                if (jw.contains("tracks") && jw["tracks"].is_array()) {
                    for (const auto& jt : jw["tracks"]) {
                        if (!jt.is_object()) continue;
                        EffectParamTrack t;
                        t.param = ReadString(jt, "param", "");
                        if (t.param.empty()) continue;
                        if (jt.contains("keys") && jt["keys"].is_array()) {
                            for (const auto& jk : jt["keys"]) {
                                if (!jk.is_array() || jk.size() < 2 || !jk[0].is_number() || !jk[1].is_number())
                                    continue;
                                EffectParamKey k;
                                k.frame = jk[0].get<int32_t>();
                                k.value = jk[1].get<float>();
                                k.stepped = jk.size() >= 3 && jk[2].is_number() && jk[2].get<int>() != 0;
                                UpsertParamKey(t, k);
                            }
                        }
                        w.fx.tracks.push_back(std::move(t));
                    }
                }
                if (jw.contains("place") && jw["place"].is_object()) {
                    const nlohmann::json& pl = jw["place"];
                    w.place.translation = ReadVec3(pl, "t", w.place.translation);
                    w.place.rotationDeg = ReadVec3(pl, "r", w.place.rotationDeg);
                    w.place.scale = (float)ReadDouble(pl, "s", w.place.scale);
                }
                out.worlds.push_back(std::move(w));
            }
        }

        // Version 4: MMDX stage entities. The manifest itself is resolved by the loader (from `path`), so a project can
        // be read (and round-tripped) without the stage folder being present.
        if (version >= 4 && j.contains("stages") && j["stages"].is_array()) {
            for (const auto& js : j["stages"]) {
                if (!js.is_object()) {
                    if (warnings) warnings->push_back("malformed stage entry skipped");
                    continue;
                }
                StudioStage st;
                st.uid = (uint32_t)std::max(0, ReadInt(js, "uid", 0));
                st.stageId = ReadString(js, "stageId", "");
                st.manifestPath = ResolvePath(ReadString(js, "path", ""), dir);
                st.variant = ReadString(js, "variant", "");
                if (js.contains("base") && js["base"].is_object()) {
                    for (const auto& [k, v] : js["base"].items()) {
                        StageValue value;
                        if (ReadStageValue(v, value)) st.base[k] = value;
                    }
                }
                if (js.contains("tracks") && js["tracks"].is_array()) {
                    for (const auto& jt : js["tracks"]) {
                        if (!jt.is_object()) continue;
                        EffectParamTrack t;
                        t.param = ReadString(jt, "param", "");
                        if (t.param.empty()) continue;
                        if (jt.contains("keys") && jt["keys"].is_array()) {
                            for (const auto& jk : jt["keys"]) {
                                if (!jk.is_array() || jk.size() < 2 || !jk[0].is_number() || !jk[1].is_number())
                                    continue;
                                EffectParamKey k;
                                k.frame = jk[0].get<int32_t>();
                                k.value = jk[1].get<float>();
                                k.stepped = jk.size() >= 3 && jk[2].is_number() && jk[2].get<int>() != 0;
                                UpsertParamKey(t, k);
                            }
                        }
                        st.tracks.push_back(std::move(t));
                    }
                }
                if (js.contains("cuesOff") && js["cuesOff"].is_array()) {
                    for (const auto& c : js["cuesOff"])
                        if (c.is_string()) st.cuesOff.insert(c.get<std::string>());
                }
                // S5 (version 6): the project's screen overrides (a relative file resolves against the project folder)
                if (version >= 6 && js.contains("screens") && js["screens"].is_object()) {
                    for (const auto& [id, so] : js["screens"].items()) {
                        if (id.empty() || !so.is_object()) continue;
                        StageScreenOverride ov;
                        if (so.contains("source") && so["source"].is_string())
                            ov.source = so["source"].get<std::string>();
                        if (so.contains("file") && so["file"].is_string()) {
                            const std::string f = so["file"].get<std::string>();
                            ov.file = f.empty() ? std::string() : PathToUtf8(ResolvePath(f, dir));
                        }
                        if (so.contains("camera") && so["camera"].is_string())
                            ov.camera = so["camera"].get<std::string>();
                        if (so.contains("brightness") && so["brightness"].is_number())
                            ov.brightness = so["brightness"].get<float>();
                        if (ov.source || ov.file || ov.camera || ov.brightness) st.screens[id] = std::move(ov);
                    }
                }
                out.stages.push_back(std::move(st));
            }
            // the uids identify the stages (part models / lights refer to them): a missing, zero or repeated one gets a
            // fresh uid (the references are remapped once the lights are read, at the end of this function)
            uint32_t nextUid = 1;
            for (const StudioStage& st : out.stages) nextUid = std::max(nextUid, st.uid + 1);
            std::set<uint32_t> used;
            for (StudioStage& st : out.stages) {
                if (st.uid == 0 || !used.insert(st.uid).second) {
                    const uint32_t old = st.uid;
                    st.uid = nextUid++;
                    if (old != 0) stageUidRemap[old] = st.uid;
                }
            }
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
            if (e.contains("aspect")) {
                int w = 16, h = 9;
                const auto& a = e["aspect"];
                if (a.is_object()) {
                    w = ReadInt(a, "w", 16);
                    h = ReadInt(a, "h", 9);
                } else if (a.is_string()) {
                    const int idx = AspectIndexFromLabel(a.get<std::string>());
                    if (idx >= 0) { w = kAspectPresets[idx].w; h = kAspectPresets[idx].h; }
                } else if (a.is_array() && a.size() == 2 && a[0].is_number() && a[1].is_number()) {
                    w = a[0].get<int>();
                    h = a[1].get<int>();
                }
                const int idx = AspectIndexFromWH(w, h);
                ed.aspectW = kAspectPresets[idx].w;
                ed.aspectH = kAspectPresets[idx].h;
            } else {
                ed.aspectW = 16;
                ed.aspectH = 9;
            }
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
                        if (jl.is_object() && SceneLightFromJson(jl, l, warnings)) ed.lights.push_back(std::move(l));
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
            if (pm.kind != ModelKind::Prop && pm.kind != ModelKind::Effect) continue;
            const auto it = remap.find(pm.attach.parent);
            pm.attach.parent = it == remap.end() ? -1 : it->second;
            // accessory keys: a parent is 1 + the old model index (0 = the world); remap through the same map
            for (AccessoryKf& k : pm.motion.accessory) {
                if (k.parentUid == 0) continue;
                const auto ai = remap.find((int)k.parentUid - 1);
                k.parentUid = ai == remap.end() || ai->second < 0 ? 0 : (uint32_t)(ai->second + 1);
            }
        }
        // S3: a stage reference no stage has becomes 0 (a freestanding MMDX stage part / an ordinary scene light)
        {
            std::set<uint32_t> stageUids;
            for (const StudioStage& st : out.stages) stageUids.insert(st.uid);
            const auto fix = [&](uint32_t& uid) {
                if (uid == 0) return;
                const auto it = stageUidRemap.find(uid);
                if (it != stageUidRemap.end()) uid = it->second;
                if (!stageUids.count(uid)) uid = 0;
            };
            for (ProjectModel& pm : out.models) fix(pm.stageUid);
            for (SceneLight& l : out.editor.lights) fix(l.stageUid);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace mmdx::studio
