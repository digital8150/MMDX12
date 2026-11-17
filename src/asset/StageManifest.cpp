// MMDX stage manifest: stage.json parsing (guarded: a wrong-typed key keeps the default, nothing throws), the
// localized-text / enum / glob helpers, semantic validation and binding-target resolution. See StageManifest.h and
// docs/stage_format.md.
#include "asset/StageManifest.h"

#include "core/I18n.h"
#include "core/TextUtil.h"
#include "asset/SceneLightJson.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace mmdx {

// ---- small helpers ---------------------------------------------------------------------------------------------------

std::string StageLanguage() {
    switch (ActiveLanguage()) {
    case Language::Korean: return "ko";
    case Language::English: return "en";
    case Language::Japanese: return "ja";
    case Language::Chinese: return "zh";
    default: return "en";
    }
}

namespace {

std::string Trimmed(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(const std::string& s) { return ToLowerAscii(s); }

bool FromHex(const std::string& s, DirectX::XMFLOAT4& out) {
    if (s.size() != 7 || s[0] != '#') return false;
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    out = {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    return true;
}

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
std::string ReadString(const nlohmann::json& j, const char* key, const std::string& def = {}) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}
// A vec3 from an array of 3 numbers, or from "#rrggbb" (colours in hex read as linear values 0..1).
bool ReadVec3(const nlohmann::json& j, DirectX::XMFLOAT3& out) {
    if (j.is_string()) {
        DirectX::XMFLOAT4 c;
        if (!FromHex(j.get<std::string>(), c)) return false;
        out = {c.x, c.y, c.z};
        return true;
    }
    if (!j.is_array() || j.size() < 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!j[i].is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    return true;
}
bool ReadColor(const nlohmann::json& j, DirectX::XMFLOAT4& out) {
    if (j.is_string()) return FromHex(j.get<std::string>(), out);
    if (!j.is_array() || j.size() < 3) return false;
    for (size_t i = 0; i < j.size() && i < 4; ++i)
        if (!j[i].is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j.size() >= 4 ? j[3].get<float>() : 1.0f};
    return true;
}
const nlohmann::json* ObjectAt(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
}
const nlohmann::json* ArrayAt(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_array() ? &*it : nullptr;
}

StageText Text(const nlohmann::json& j, const char* key) {
    StageText t;
    const auto it = j.find(key);
    if (it == j.end()) return t;
    if (it->is_string()) {
        t.byLang[""] = it->get<std::string>();
    } else if (it->is_object()) {
        for (const auto& [lang, v] : it->items())
            if (v.is_string()) t.byLang[lang] = v.get<std::string>();
    }
    return t;
}

bool HttpUrl(const std::string& s) { return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0; }

// A relative, folder-local path: not absolute, no drive / root, no ".." escaping the stage folder.
bool SafeRelativeFile(const std::string& s, std::string& why) {
    if (s.empty()) {
        why = "empty path";
        return false;
    }
    std::string p = s;
    for (char& c : p)
        if (c == '\\') c = '/';
    const std::filesystem::path fs = Utf8ToPath(p);
    if (fs.is_absolute() || fs.has_root_name() || fs.has_root_directory()) {
        why = "absolute path";
        return false;
    }
    if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':') {
        why = "drive path";
        return false;
    }
    for (const auto& part : fs) {
        if (part == "..") {
            why = "\"..\" is not allowed";
            return false;
        }
    }
    return true;
}

} // namespace

// ---- StageText -------------------------------------------------------------------------------------------------------

const std::string& StageText::Get(const std::string& lang) const {
    static const std::string empty;
    if (!byLang.empty()) {
        for (const char* key : {lang.c_str(), "en", "", "ko"}) {
            const auto it = byLang.find(key);
            if (it != byLang.end()) return it->second;
        }
        return byLang.begin()->second;
    }
    return empty;
}

bool StageText::Empty() const {
    for (const auto& [lang, text] : byLang)
        if (!text.empty()) return false;
    return true;
}

// ---- names / enums ---------------------------------------------------------------------------------------------------

const char* StagePartRoleName(StagePartRole role) {
    switch (role) {
    case StagePartRole::Floor: return "floor";
    case StagePartRole::Props: return "props";
    case StagePartRole::Main: break;
    }
    return "main";
}

bool ParseStagePartRole(const std::string& s, StagePartRole& out) {
    if (s == "main") { out = StagePartRole::Main; return true; }
    if (s == "floor") { out = StagePartRole::Floor; return true; }
    if (s == "props") { out = StagePartRole::Props; return true; }
    return false;
}

const char* StageScreenSourceName(StageScreenSource s) {
    switch (s) {
    case StageScreenSource::Image: return "image";
    case StageScreenSource::Video: return "video";
    case StageScreenSource::Camera: return "camera";
    case StageScreenSource::Program: return "program";
    case StageScreenSource::None: break;
    }
    return "none";
}

bool ParseStageScreenSource(const std::string& s, StageScreenSource& out) {
    if (s == "none") { out = StageScreenSource::None; return true; }
    if (s == "image") { out = StageScreenSource::Image; return true; }
    if (s == "video") { out = StageScreenSource::Video; return true; }
    if (s == "camera") { out = StageScreenSource::Camera; return true; }
    if (s == "program") { out = StageScreenSource::Program; return true; }
    return false;
}

const char* StagePackRoleName(StagePackRole role) {
    switch (role) {
    case StagePackRole::World: return "world";
    case StagePackRole::Screen: return "screen";
    case StagePackRole::Surface: break;
    }
    return "surface";
}

bool ParseStagePackRole(const std::string& s, StagePackRole& out) {
    if (s == "surface") { out = StagePackRole::Surface; return true; }
    if (s == "world") { out = StagePackRole::World; return true; }
    if (s == "screen") { out = StagePackRole::Screen; return true; }
    return false;
}

const char* StageParamTypeName(StageParamType t) {
    switch (t) {
    case StageParamType::Int: return "int";
    case StageParamType::Bool: return "bool";
    case StageParamType::Color: return "color";
    case StageParamType::Enum: return "enum";
    case StageParamType::Float: break;
    }
    return "float";
}

bool ParseStageParamType(const std::string& s, StageParamType& out) {
    if (s == "float") { out = StageParamType::Float; return true; }
    if (s == "int") { out = StageParamType::Int; return true; }
    if (s == "bool") { out = StageParamType::Bool; return true; }
    if (s == "color") { out = StageParamType::Color; return true; }
    if (s == "enum") { out = StageParamType::Enum; return true; }
    return false;
}

StageValue StageParam::DefaultValue() const {
    if (type == StageParamType::Color) return StageValue::Color(defColor);
    return StageValue::Number(def);
}

// ---- glob / stems ----------------------------------------------------------------------------------------------------

bool StageGlobIsPattern(const std::string& pattern) {
    return pattern.find_first_of("*?") != std::string::npos;
}

bool StageGlobMatch(const std::string& pattern, const std::string& text) {
    // Iterative wildcard match with a single backtrack point for '*' (no recursion: patterns are author data).
    const std::string p = Lower(pattern), t = Lower(text);
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::string StageFileStem(const std::string& file) {
    if (file.empty()) return {};
    std::string p = file;
    for (char& c : p)
        if (c == '\\') c = '/';
    const size_t slash = p.find_last_of('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

const char* StageDriverInputName(StageDriverInput in) {
    switch (in) {
    case StageDriverInput::Beat: return "audio.beat";
    case StageDriverInput::Bar: return "audio.bar";
    case StageDriverInput::Onset: return "audio.onset";
    case StageDriverInput::Energy: return "audio.energy";
    case StageDriverInput::Section: return "audio.section";
    case StageDriverInput::Time: return "time";
    }
    return "time";
}

bool ParseStageDriverInput(const std::string& s, StageDriverInput& out) {
    for (StageDriverInput in : {StageDriverInput::Beat, StageDriverInput::Bar, StageDriverInput::Onset,
                                StageDriverInput::Energy, StageDriverInput::Section, StageDriverInput::Time})
        if (s == StageDriverInputName(in)) { out = in; return true; }
    return false;
}

bool ParseStageDriverShape(const std::string& s, StageDriverShape& out) {
    if (s == "pulse") { out = StageDriverShape::Pulse; return true; }
    if (s == "saw") { out = StageDriverShape::Saw; return true; }
    if (s == "sine") { out = StageDriverShape::Sine; return true; }
    if (s == "triangle") { out = StageDriverShape::Triangle; return true; }
    if (s == "square") { out = StageDriverShape::Square; return true; }
    if (s == "ramp") { out = StageDriverShape::Ramp; return true; }
    return false;
}

bool ParseStageDriverBlend(const std::string& s, StageDriverBlend& out) {
    if (s == "set") { out = StageDriverBlend::Set; return true; }
    if (s == "add") { out = StageDriverBlend::Add; return true; }
    if (s == "mul") { out = StageDriverBlend::Mul; return true; }
    return false;
}

bool StageUsesAudio(const StageManifest& manifest) {
    for (const StageDriver& d : manifest.drivers)
        if (StageDriverIsAudio(d.input)) return true;
    return false;
}

// ---- manifest lookups ------------------------------------------------------------------------------------------------

const StageParam* StageManifest::FindParam(const std::string& key) const {
    for (const auto& p : params)
        if (p.key == key) return &p;
    return nullptr;
}

const StageVariant* StageManifest::FindVariant(const std::string& id) const {
    for (const auto& v : variants)
        if (v.id == id) return &v;
    return nullptr;
}

const StageScreen* StageManifest::FindScreen(const std::string& id) const {
    for (const auto& s : screens)
        if (s.id == id) return &s;
    return nullptr;
}

const studio::SceneLight* StageManifest::FindLight(const std::string& name) const {
    for (const auto& l : lights)
        if (l.name == name) return &l;
    return nullptr;
}

const StageVariant* StageManifest::DefaultVariant() const {
    if (!defaultVariant.empty()) return FindVariant(defaultVariant);
    return variants.empty() ? nullptr : &variants.front();
}

std::string StageManifest::DisplayName(const std::string& lang) const {
    if (!name.Empty()) return name.Get(lang);
    return PathToUtf8(dir.filename());
}

// ---- binding targets -------------------------------------------------------------------------------------------------

namespace {

struct TargetField {
    const char* name;
    StageTargetSpec::ValueType type;
};

StageTargetSpec::ValueType LightFieldType(const std::string& field) {
    static const TargetField kFields[] = {
        {"enabled", StageTargetSpec::ValueType::Bool},       {"intensity", StageTargetSpec::ValueType::Float},
        {"color", StageTargetSpec::ValueType::Color},        {"position", StageTargetSpec::ValueType::Vec3},
        {"aim", StageTargetSpec::ValueType::Vec3},           {"direction", StageTargetSpec::ValueType::Vec3},
        {"range", StageTargetSpec::ValueType::Float},        {"coneOuter", StageTargetSpec::ValueType::Float},
        {"coneInner", StageTargetSpec::ValueType::Float},    {"size", StageTargetSpec::ValueType::Vec3},
        {"shadowSoftness", StageTargetSpec::ValueType::Float}, {"shadowDensity", StageTargetSpec::ValueType::Float},
        {"shadowColor", StageTargetSpec::ValueType::Color},  {"rimStrength", StageTargetSpec::ValueType::Float},
        {"rimColor", StageTargetSpec::ValueType::Color},     {"swayPhase", StageTargetSpec::ValueType::Float},
        {"vmdLink", StageTargetSpec::ValueType::Bool},
        // S7b: the spot gobo (rotation degrees, uv scale, deg/s spin, 0..1 focus)
        {"gobo.rotation", StageTargetSpec::ValueType::Float}, {"gobo.scale", StageTargetSpec::ValueType::Float},
        {"gobo.spin", StageTargetSpec::ValueType::Float},     {"gobo.focus", StageTargetSpec::ValueType::Float},
        // S9b-1: the Physical Sky light's values (hours, compass degrees, turbidity, haze, radiance / sun scale)
        {"sky.timeOfDay", StageTargetSpec::ValueType::Float}, {"sky.northDeg", StageTargetSpec::ValueType::Float},
        {"sky.turbidity", StageTargetSpec::ValueType::Float}, {"sky.haze", StageTargetSpec::ValueType::Float},
        {"sky.intensity", StageTargetSpec::ValueType::Float}, {"sky.sunIntensity", StageTargetSpec::ValueType::Float},
        {"sky.ground", StageTargetSpec::ValueType::Color},
        // S9b-3: the sky light's image: rotation degrees and radiance scale (the file / encoding / mirror are base
        // values, like the gobo's file)
        {"sky.mapRotation", StageTargetSpec::ValueType::Float}, {"sky.mapIntensity", StageTargetSpec::ValueType::Float},
        // S9b-4: the physical sky's cloud layer (coverage / opacity / softness 0..1, height and noise size in km, wind
        // direction in degrees and speed in m/s)
        {"sky.cloudCoverage", StageTargetSpec::ValueType::Float}, {"sky.cloudOpacity", StageTargetSpec::ValueType::Float},
        {"sky.cloudSoftness", StageTargetSpec::ValueType::Float}, {"sky.cloudHeight", StageTargetSpec::ValueType::Float},
        {"sky.cloudScale", StageTargetSpec::ValueType::Float},    {"sky.cloudWindDeg", StageTargetSpec::ValueType::Float},
        {"sky.cloudWindSpeed", StageTargetSpec::ValueType::Float},
    };
    for (const auto& f : kFields)
        if (field == f.name) return f.type;
    return static_cast<StageTargetSpec::ValueType>(255);
}

StageTargetSpec::ValueType MaterialFieldType(const std::string& field) {
    static const TargetField kFields[] = {
        {"baseColor", StageTargetSpec::ValueType::Color},    {"roughness", StageTargetSpec::ValueType::Float},
        {"metallic", StageTargetSpec::ValueType::Float},     {"emissive", StageTargetSpec::ValueType::Vec3},
        {"emissiveStrength", StageTargetSpec::ValueType::Float}, {"transmission", StageTargetSpec::ValueType::Float},
        {"ior", StageTargetSpec::ValueType::Float},          {"alpha", StageTargetSpec::ValueType::Float},
        {"mirror", StageTargetSpec::ValueType::Bool},        {"screen", StageTargetSpec::ValueType::String},
        {"castShadow", StageTargetSpec::ValueType::Bool},    {"receiveShadow", StageTargetSpec::ValueType::Bool},
    };
    for (const auto& f : kFields)
        if (field == f.name) return f.type;
    return static_cast<StageTargetSpec::ValueType>(255);
}

StageTargetSpec::ValueType ScreenFieldType(const std::string& field) {
    static const TargetField kFields[] = {
        {"brightness", StageTargetSpec::ValueType::Float}, {"source", StageTargetSpec::ValueType::String},
        {"file", StageTargetSpec::ValueType::String},      {"camera", StageTargetSpec::ValueType::String},
        {"width", StageTargetSpec::ValueType::Int},        {"height", StageTargetSpec::ValueType::Int},
        {"emitLight", StageTargetSpec::ValueType::Bool},   {"enabled", StageTargetSpec::ValueType::Bool},
    };
    for (const auto& f : kFields)
        if (field == f.name) return f.type;
    return static_cast<StageTargetSpec::ValueType>(255);
}

StageTargetSpec::ValueType EnvironmentFieldType(const std::string& field) {
    static const TargetField kFields[] = {
        {"fog.density", StageTargetSpec::ValueType::Float},
        {"fog.color", StageTargetSpec::ValueType::Color},
        {"fog.anisotropy", StageTargetSpec::ValueType::Float},
        {"fog.heightFalloff", StageTargetSpec::ValueType::Float},
        {"ambient.intensity", StageTargetSpec::ValueType::Float},
        {"ambient.skyZenith", StageTargetSpec::ValueType::Color},
        {"ambient.skyHorizon", StageTargetSpec::ValueType::Color},
        {"ambient.ground", StageTargetSpec::ValueType::Color},
    };
    for (const auto& f : kFields)
        if (field == f.name) return f.type;
    return static_cast<StageTargetSpec::ValueType>(255);
}

} // namespace

StageTargetSpec ResolveStageTarget(const StageManifest& manifest, const std::string& target) {
    StageTargetSpec spec;
    const size_t first = target.find('.');
    if (first == std::string::npos || first == 0) {
        spec.error = "target needs a kind (\"" + target + "\")";
        return spec;
    }
    const std::string kind = target.substr(0, first);
    const std::string rest = target.substr(first + 1);
    const size_t last = rest.find_last_of('.');
    if (last == std::string::npos || last == 0 || last + 1 >= rest.size()) {
        spec.error = "target needs an owner and a field (\"" + target + "\")";
        return spec;
    }
    const std::string owner = rest.substr(0, last);
    const std::string field = rest.substr(last + 1);
    if (kind == "lights") {
        spec.kind = StageTargetSpec::Kind::Lights;
        // S7b: a gobo target nests one level ("lights.<name>.gobo.rotation"), so the last dot above split the owner
        // into "<name>.gobo" and the field into "rotation": put the two back together as the field "gobo.rotation".
        // S9b-1: the same for the Physical Sky light's "lights.<name>.sky.timeOfDay" (and the other sky fields).
        std::string lightOwner = owner;
        std::string lightField = field;
        // "sky.intensity" ends in a name that is also a plain light field: the nested reading also applies when the
        // owner the last dot left ("<name>.sky") is not a light.
        if ((int)LightFieldType(field) == 255 || !manifest.FindLight(owner)) {
            for (const char* block : {".gobo", ".sky"}) {
                const size_t n = std::char_traits<char>::length(block);
                if (owner.size() <= n || owner.compare(owner.size() - n, n, block) != 0) continue;
                const std::string nested = std::string(block + 1) + "." + field;
                if ((int)LightFieldType(nested) == 255) continue;
                lightField = nested;
                lightOwner = owner.substr(0, owner.size() - n);
                break;
            }
        }
        spec.type = LightFieldType(lightField);
        if ((int)spec.type == 255) {
            spec.error = "unknown light field \"" + field + "\"";
            return spec;
        }
        if (!manifest.FindLight(lightOwner)) {
            spec.error = "no light named \"" + lightOwner + "\"";
            return spec;
        }
        spec.owner = lightOwner;
        spec.field = lightField;
        spec.ok = true;
        return spec;
    } else if (kind == "materials") {
        spec.kind = StageTargetSpec::Kind::Materials;
        spec.type = MaterialFieldType(field);
        if ((int)spec.type == 255) {
            spec.error = "unknown material field \"" + field + "\"";
            return spec;
        }
        // The rule itself is the owner: `materials.<pattern>.<field>` adjusts the manifest's own material rule.
        bool known = false;
        for (const auto& r : manifest.materials)
            if (r.match == owner) known = true;
        if (!known) {
            spec.error = "no material rule matching \"" + owner + "\"";
            return spec;
        }
    } else if (kind == "screens") {
        spec.kind = StageTargetSpec::Kind::Screens;
        spec.type = ScreenFieldType(field);
        if ((int)spec.type == 255) {
            spec.error = "unknown screen field \"" + field + "\"";
            return spec;
        }
        if (!manifest.FindScreen(owner)) {
            spec.error = "no screen with id \"" + owner + "\"";
            return spec;
        }
    } else if (kind == "packs") {
        spec.kind = StageTargetSpec::Kind::PackParams;
        spec.type = StageTargetSpec::ValueType::Float;
        // packs.<index>.params.<key>
        const size_t dot = rest.find('.');
        if (dot == std::string::npos || rest.compare(dot, 8, ".params.") != 0) {
            spec.error = "pack target must be \"packs.<index>.params.<key>\" (\"" + target + "\")";
            return spec;
        }
        const std::string idx = rest.substr(0, dot);
        const std::string key = rest.substr(dot + 8);
        if (std::any_of(idx.begin(), idx.end(), [](char c) { return !std::isdigit((unsigned char)c); })) {
            spec.error = "pack index is not a number (\"" + idx + "\")";
            return spec;
        }
        spec.packIndex = std::stoi(idx);
        if (spec.packIndex < 0 || (size_t)spec.packIndex >= manifest.packs.size()) {
            spec.error = "no pack at index " + idx;
            return spec;
        }
        if (key.empty()) {
            spec.error = "pack param key is empty";
            return spec;
        }
        spec.owner = idx;
        spec.field = key;
        spec.ok = true;
        return spec;
    } else if (kind == "environment") {
        spec.kind = StageTargetSpec::Kind::Environment;
        spec.type = EnvironmentFieldType(rest);   // the whole remainder is the field ("fog.density")
        if ((int)spec.type == 255) {
            spec.error = "unknown environment field \"" + rest + "\"";
            return spec;
        }
        spec.field = rest;
        spec.ok = true;
        return spec;
    } else {
        spec.error = "unknown target kind \"" + kind + "\"";
        return spec;
    }
    spec.owner = owner;
    spec.field = field;
    spec.ok = true;
    return spec;
}

// ---- param values ----------------------------------------------------------------------------------------------------

bool ParseStageParamValue(const StageParam& param, const nlohmann::json& j, StageValue& out, std::string& error) {
    switch (param.type) {
    case StageParamType::Color: {
        DirectX::XMFLOAT4 c;
        if (!ReadColor(j, c)) {
            error = "expected a colour ([r,g,b(,a)] or \"#rrggbb\") for \"" + param.key + "\"";
            return false;
        }
        out = StageValue::Color(c);
        return true;
    }
    case StageParamType::Bool:
        if (j.is_boolean()) {
            out = StageValue::Number(j.get<bool>() ? 1.0f : 0.0f);
            return true;
        }
        if (j.is_number()) {
            out = StageValue::Number(j.get<double>() != 0.0 ? 1.0f : 0.0f);
            return true;
        }
        break;
    case StageParamType::Enum:
        if (j.is_string()) {
            const std::string v = j.get<std::string>();
            for (size_t i = 0; i < param.options.size(); ++i)
                if (param.options[i].value == v) {
                    out = StageValue::Number((float)i);
                    return true;
                }
            error = "unknown option \"" + v + "\" for \"" + param.key + "\"";
            return false;
        }
        if (j.is_number()) {
            const int i = j.get<int>();
            if (i < 0 || (size_t)i >= param.options.size()) {
                error = "option index out of range for \"" + param.key + "\"";
                return false;
            }
            out = StageValue::Number((float)i);
            return true;
        }
        break;
    default:
        if (j.is_number()) {
            out = StageValue::Number(j.get<float>());
            return true;
        }
        if (j.is_boolean()) {
            out = StageValue::Number(j.get<bool>() ? 1.0f : 0.0f);
            return true;
        }
        break;
    }
    error = "expected " + std::string(StageParamTypeName(param.type)) + " for \"" + param.key + "\"";
    return false;
}

bool StageParamValueFromJson(const StageParam& param, const nlohmann::json& j, StageValue& out, std::string& error) {
    return ParseStageParamValue(param, j, out, error);
}

StageValue ClampStageParamValue(const StageParam& param, const StageValue& in) {
    StageValue out = in;
    switch (param.type) {
    case StageParamType::Color:
        out.type = StageValue::Type::Color;
        out.text.clear();
        for (float* v : {&out.x, &out.y, &out.z, &out.w}) *v = std::clamp(*v, 0.0f, 1.0f);
        return out;
    case StageParamType::Bool:
        return StageValue::Number(in.x != 0.0f ? 1.0f : 0.0f);
    case StageParamType::Enum: {
        const float i = std::round(in.x);
        return StageValue::Number(std::clamp(i, 0.0f, std::max(0.0f, (float)param.options.size() - 1.0f)));
    }
    case StageParamType::Int:
        return StageValue::Number(std::clamp(std::round(in.x), param.min, param.max));
    case StageParamType::Float:
        break;
    }
    return StageValue::Number(std::clamp(in.x, param.min, param.max));
}

// ---- parsing ---------------------------------------------------------------------------------------------------------

namespace {

struct ParseCtx {
    std::string error;
    void Add(const std::string& e) {
        if (!error.empty()) error += "\n";
        error += e;
    }
};

void ParseAuthors(const nlohmann::json& j, std::vector<StageAuthor>& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "authors");
    if (!arr) return;
    for (const auto& a : *arr) {
        if (!a.is_object()) {
            ctx.Add("authors: entry is not an object");
            continue;
        }
        StageAuthor author;
        author.name = Trimmed(ReadString(a, "name"));
        if (author.name.empty()) {
            ctx.Add("authors: an entry has no \"name\"");
            continue;
        }
        author.role = ReadString(a, "role");
        const std::string url = ReadString(a, "url");
        if (!url.empty() && HttpUrl(url)) author.url = url;
        out.push_back(std::move(author));
    }
}

void ParsePreview(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    auto add = [&](const std::string& file) {
        std::string why;
        if (!SafeRelativeFile(file, why)) {
            ctx.Add("preview: " + file + ": " + why);
            return;
        }
        out.previews.push_back(Utf8ToPath(file));
    };
    for (const char* key : {"preview", "previews"}) {
        const auto it = j.find(key);
        if (it == j.end()) continue;
        if (it->is_string()) add(it->get<std::string>());
        else if (it->is_array())
            for (const auto& p : *it)
                if (p.is_string()) add(p.get<std::string>());
                else ctx.Add("preview: entry is not a string");
    }
}

void ParseModels(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "models");
    if (!arr) {
        ctx.Add("models: missing \"models\" array");
        return;
    }
    for (const auto& m : *arr) {
        if (!m.is_object()) {
            ctx.Add("models: entry is not an object");
            continue;
        }
        StagePart part;
        part.file = ReadString(m, "file");
        std::string why;
        if (!SafeRelativeFile(part.file, why)) {
            ctx.Add("models: " + (part.file.empty() ? std::string("entry has no \"file\"") : part.file + ": " + why));
            continue;
        }
        part.name = ReadString(m, "name");
        if (part.name.empty()) part.name = StageFileStem(part.file);
        const std::string role = ReadString(m, "role", "main");
        if (!ParseStagePartRole(Lower(role), part.role)) {
            ctx.Add("models: " + part.file + ": unknown role \"" + role + "\"");
            continue;
        }
        out.models.push_back(std::move(part));
    }
}

void ParseMaterials(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "materials");
    if (!arr) return;
    for (const auto& m : *arr) {
        if (!m.is_object()) {
            ctx.Add("materials: entry is not an object");
            continue;
        }
        StageMaterialRule rule;
        rule.match = ReadString(m, "match");
        if (rule.match.empty()) {
            ctx.Add("materials: entry has no \"match\"");
            continue;
        }
        auto readF = [&](const char* key, std::optional<float>& dst) {
            const auto it = m.find(key);
            if (it == m.end()) return;
            if (!it->is_number()) {
                ctx.Add("materials: " + rule.match + ": \"" + key + "\" is not a number");
                return;
            }
            dst = it->get<float>();
        };
        auto readB = [&](const char* key, std::optional<bool>& dst) {
            const auto it = m.find(key);
            if (it == m.end()) return;
            if (!it->is_boolean()) {
                ctx.Add("materials: " + rule.match + ": \"" + key + "\" is not a boolean");
                return;
            }
            dst = it->get<bool>();
        };
        auto readV = [&](const char* key, std::optional<DirectX::XMFLOAT3>& dst) {
            const auto it = m.find(key);
            if (it == m.end()) return;
            DirectX::XMFLOAT3 v;
            if (!ReadVec3(*it, v)) {
                ctx.Add("materials: " + rule.match + ": \"" + key + "\" is not [r,g,b]");
                return;
            }
            dst = v;
        };
        if (const auto it = m.find("baseColor"); it != m.end()) {
            DirectX::XMFLOAT4 c;
            if (ReadColor(*it, c)) rule.baseColor = c;
            else ctx.Add("materials: " + rule.match + ": \"baseColor\" is not a colour");
        }
        readF("roughness", rule.roughness);
        readF("metallic", rule.metallic);
        readV("emissive", rule.emissive);
        readF("emissiveStrength", rule.emissiveStrength);
        readF("transmission", rule.transmission);
        readF("ior", rule.ior);
        readF("alpha", rule.alpha);
        readB("mirror", rule.mirror);
        readB("castShadow", rule.castShadow);
        readB("receiveShadow", rule.receiveShadow);
        if (const auto it = m.find("screen"); it != m.end()) {
            if (it->is_string()) rule.screen = it->get<std::string>();
            else ctx.Add("materials: " + rule.match + ": \"screen\" is not a string");
        }
        out.materials.push_back(std::move(rule));
    }
}

void ParseLights(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "lights");
    if (!arr) return;
    std::vector<std::string> warnings;
    for (const auto& l : *arr) {
        if (!l.is_object()) {
            ctx.Add("lights: entry is not an object");
            continue;
        }
        studio::SceneLight light;
        if (!studio::SceneLightFromJson(l, light, &warnings)) continue;
        if (Trimmed(light.name).empty()) {
            ctx.Add("lights: an entry has no \"name\" (names are the binding keys)");
            continue;
        }
        out.lights.push_back(std::move(light));
    }
    for (const auto& w : warnings) ctx.Add("lights: " + w);
}

void ParseSockets(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "sockets");
    if (!arr) return;
    for (const auto& s : *arr) {
        if (!s.is_object()) {
            ctx.Add("sockets: entry is not an object");
            continue;
        }
        StageSocket socket;
        socket.name = Trimmed(ReadString(s, "name"));
        if (socket.name.empty()) {
            ctx.Add("sockets: an entry has no \"name\"");
            continue;
        }
        socket.node = ReadString(s, "node");
        int explicitTransform = 0;
        for (const char* key : {"position", "rotation", "scale"}) {
            const auto it = s.find(key);
            if (it == s.end()) continue;
            DirectX::XMFLOAT3 v;
            if (!ReadVec3(*it, v)) {
                ctx.Add("sockets: " + socket.name + ": \"" + key + "\" is not [x,y,z]");
                continue;
            }
            ++explicitTransform;
            if (std::string(key) == "position") socket.position = v;
            else if (std::string(key) == "rotation") socket.rotationDeg = v;
            else socket.scale = v;
        }
        if (socket.node.empty() && explicitTransform == 0) {
            ctx.Add("sockets: " + socket.name + ": neither a \"node\" nor an explicit position / rotation / scale");
            continue;
        }
        out.sockets.push_back(std::move(socket));
    }
}

void ParseCameras(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "cameras");
    if (!arr) return;
    for (const auto& c : *arr) {
        if (!c.is_object()) {
            ctx.Add("cameras: entry is not an object");
            continue;
        }
        StageCamera cam;
        cam.name = Trimmed(ReadString(c, "name"));
        if (cam.name.empty()) {
            ctx.Add("cameras: an entry has no \"name\"");
            continue;
        }
        for (const char* key : {"position", "target"}) {
            const auto it = c.find(key);
            if (it == c.end()) continue;
            DirectX::XMFLOAT3 v;
            if (!ReadVec3(*it, v)) {
                ctx.Add("cameras: " + cam.name + ": \"" + key + "\" is not [x,y,z]");
                continue;
            }
            if (std::string(key) == "position") cam.position = v;
            else cam.target = v;
        }
        cam.fov = (float)ReadDouble(c, "fov", cam.fov);
        out.cameras.push_back(std::move(cam));
    }
}

void ParseScreens(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "screens");
    if (!arr) return;
    for (const auto& s : *arr) {
        if (!s.is_object()) {
            ctx.Add("screens: entry is not an object");
            continue;
        }
        StageScreen screen;
        screen.id = Trimmed(ReadString(s, "id"));
        if (screen.id.empty()) {
            ctx.Add("screens: an entry has no \"id\"");
            continue;
        }
        if (const nlohmann::json* mats = ArrayAt(s, "materials")) {
            for (const auto& m : *mats)
                if (m.is_string()) screen.materials.push_back(m.get<std::string>());
                else ctx.Add("screens: " + screen.id + ": a \"materials\" entry is not a string");
        } else if (const auto one = s.find("material"); one != s.end() && one->is_string()) {
            screen.materials.push_back(one->get<std::string>());
        }
        const std::string source = Lower(ReadString(s, "source", "none"));
        if (!ParseStageScreenSource(source, screen.source)) {
            ctx.Add("screens: " + screen.id + ": unknown source \"" + source + "\"");
            continue;
        }
        screen.file = ReadString(s, "file");
        screen.camera = ReadString(s, "camera");
        screen.width = (uint32_t)std::max(1, ReadInt(s, "width", (int)screen.width));
        screen.height = (uint32_t)std::max(1, ReadInt(s, "height", (int)screen.height));
        screen.brightness = (float)ReadDouble(s, "brightness", screen.brightness);
        screen.emitLight = ReadBool(s, "emitLight", screen.emitLight);
        out.screens.push_back(std::move(screen));
    }
}

void ParseEnvironment(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* env = ObjectAt(j, "environment");
    if (!env) return;
    if (const nlohmann::json* fog = ObjectAt(*env, "fog")) {
        StageFog f;
        f.present = true;
        f.density = (float)ReadDouble(*fog, "density", f.density);
        f.anisotropy = (float)ReadDouble(*fog, "anisotropy", f.anisotropy);
        f.heightFalloff = (float)ReadDouble(*fog, "heightFalloff", f.heightFalloff);
        f.radius = (float)ReadDouble(*fog, "radius", f.radius);
        if (const auto it = fog->find("color"); it != fog->end()) {
            DirectX::XMFLOAT3 c;
            if (ReadVec3(*it, c)) f.color = c;
            else ctx.Add("environment.fog: \"color\" is not [r,g,b]");
        }
        out.environment.fog = f;
    }
    // S9: local fog volumes. Each becomes a FogVolume light on the stage's rig (kind, extent, density, tint, edge).
    if (const nlohmann::json* arr = ArrayAt(*env, "fogVolumes")) {
        for (const auto& v : *arr) {
            if (!v.is_object()) {
                ctx.Add("environment.fogVolumes: entry is not an object");
                continue;
            }
            StageFogVolume fv;
            fv.id = Trimmed(ReadString(v, "id"));
            const std::string shape = Lower(ReadString(v, "shape", "box"));
            if (shape == "sphere") {
                fv.sphere = true;
            } else if (shape != "box") {
                const std::string what = fv.id.empty() ? "#" + std::to_string(out.environment.fogVolumes.size()) : fv.id;
                ctx.Add("environment.fogVolumes: " + what + ": unknown shape \"" + shape + "\"");
            }
            auto readV = [&](const char* key, DirectX::XMFLOAT3& dst) {
                const auto it = v.find(key);
                if (it == v.end()) return;
                DirectX::XMFLOAT3 c;
                if (ReadVec3(*it, c)) dst = c;
                else ctx.Add(std::string("environment.fogVolumes: \"") + key + "\" is not [x,y,z]");
            };
            readV("position", fv.position);
            readV("extent", fv.extent);
            readV("color", fv.color);
            fv.density = (float)ReadDouble(v, "density", fv.density);
            fv.edge = (float)ReadDouble(v, "edge", fv.edge);
            out.environment.fogVolumes.push_back(std::move(fv));
        }
    }
    if (const nlohmann::json* amb = ObjectAt(*env, "ambient")) {
        StageAmbient a;
        a.present = true;
        auto readC = [&](const char* key, std::optional<DirectX::XMFLOAT3>& dst) {
            const auto it = amb->find(key);
            if (it == amb->end()) return;
            DirectX::XMFLOAT3 v;
            if (ReadVec3(*it, v)) dst = v;
            else ctx.Add(std::string("environment.ambient: \"") + key + "\" is not [r,g,b]");
        };
        readC("skyZenith", a.skyZenith);
        readC("skyHorizon", a.skyHorizon);
        readC("ground", a.ground);
        if (const auto it = amb->find("intensity"); it != amb->end()) {
            if (it->is_number()) a.intensity = it->get<float>();
            else ctx.Add("environment.ambient: \"intensity\" is not a number");
        }
        out.environment.ambient = a;
    }
}

void ParsePacks(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "packs");
    if (!arr) return;
    for (const auto& p : *arr) {
        if (!p.is_object()) {
            ctx.Add("packs: entry is not an object");
            continue;
        }
        StagePackRef pack;
        pack.folder = ReadString(p, "folder");
        std::string why;
        if (!SafeRelativeFile(pack.folder, why)) {
            ctx.Add("packs: " + (pack.folder.empty() ? std::string("entry has no \"folder\"") : pack.folder + ": " + why));
            continue;
        }
        const std::string role = Lower(ReadString(p, "role", "surface"));
        if (!ParseStagePackRole(role, pack.role)) {
            ctx.Add("packs: " + pack.folder + ": unknown role \"" + role + "\"");
            continue;
        }
        if (const nlohmann::json* mats = ArrayAt(p, "materials"))
            for (const auto& m : *mats)
                if (m.is_string()) pack.materials.push_back(m.get<std::string>());
        pack.socket = ReadString(p, "socket");
        pack.screen = ReadString(p, "screen");
        if (const nlohmann::json* params = ObjectAt(p, "params"))
            for (const auto& [key, v] : params->items()) {
                if (!v.is_number()) {
                    ctx.Add("packs: " + pack.folder + ": param \"" + key + "\" is not a number");
                    continue;
                }
                pack.params[key] = v.get<float>();
            }
        out.packs.push_back(std::move(pack));
    }
}

void ParseGroups(const nlohmann::json& j, StageManifest& out, ParseCtx&) {
    const nlohmann::json* arr = ArrayAt(j, "groups");
    if (!arr) return;
    for (const auto& g : *arr) {
        if (!g.is_object()) continue;
        StageParamGroup group;
        group.id = ReadString(g, "id");
        if (group.id.empty()) continue;
        group.label = Text(g, "label");
        group.collapsed = ReadBool(g, "collapsed", group.collapsed);
        out.groups.push_back(std::move(group));
    }
}

void ParseParams(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    std::set<std::string> groupIds;
    for (const auto& g : out.groups) groupIds.insert(g.id);
    const nlohmann::json* arr = ArrayAt(j, "params");
    if (!arr) return;
    for (const auto& p : *arr) {
        if (!p.is_object()) {
            ctx.Add("params: entry is not an object");
            continue;
        }
        StageParam param;
        param.key = Trimmed(ReadString(p, "key"));
        if (param.key.empty()) {
            ctx.Add("params: an entry has no \"key\"");
            continue;
        }
        const std::string type = Lower(ReadString(p, "type", "float"));
        if (!ParseStageParamType(type, param.type)) {
            ctx.Add("params: " + param.key + ": unknown type \"" + type + "\"");
            continue;
        }
        param.label = Text(p, "label");
        param.group = ReadString(p, "group");
        if (!param.group.empty() && !groupIds.count(param.group))
            ctx.Add("params: " + param.key + ": unknown group \"" + param.group + "\"");
        param.min = (float)ReadDouble(p, "min", param.type == StageParamType::Int ? 0.0f : param.min);
        param.max = (float)ReadDouble(p, "max", param.type == StageParamType::Int ? 100.0f : param.max);
        if (param.max < param.min) {
            ctx.Add("params: " + param.key + ": max < min");
            std::swap(param.min, param.max);
        }
        if (const nlohmann::json* opts = ArrayAt(p, "options")) {
            for (const auto& o : *opts) {
                StageParamOption opt;
                if (o.is_string()) {
                    opt.value = o.get<std::string>();
                } else if (o.is_object()) {
                    opt.value = ReadString(o, "value");
                    opt.label = Text(o, "label");
                }
                if (opt.value.empty()) {
                    ctx.Add("params: " + param.key + ": an option has no \"value\"");
                    continue;
                }
                param.options.push_back(std::move(opt));
            }
        }
        if (param.type == StageParamType::Enum && param.options.empty()) {
            ctx.Add("params: " + param.key + ": an enum needs \"options\"");
            continue;
        }
        const auto defIt = p.find("default");
        if (defIt != p.end()) {
            std::string err;
            StageValue value;
            if (!ParseStageParamValue(param, *defIt, value, err)) {
                ctx.Add("params: " + err);
            } else if (param.type == StageParamType::Color) {
                param.defColor = value.AsColor();
            } else {
                param.def = value.x;
            }
        } else if (param.type == StageParamType::Enum && !param.options.empty()) {
            param.def = 0.0f;
        }
        if (param.type == StageParamType::Color) {
            param.defColor = ClampStageParamValue(param, param.DefaultValue()).AsColor();
        } else {
            param.def = ClampStageParamValue(param, param.DefaultValue()).x;
        }
        out.params.push_back(std::move(param));
    }
}

void ParseVariants(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "variants");
    if (!arr) return;
    for (const auto& v : *arr) {
        if (!v.is_object()) {
            ctx.Add("variants: entry is not an object");
            continue;
        }
        StageVariant variant;
        variant.id = Trimmed(ReadString(v, "id"));
        if (variant.id.empty()) {
            ctx.Add("variants: an entry has no \"id\"");
            continue;
        }
        variant.label = Text(v, "label");
        if (const nlohmann::json* values = ObjectAt(v, "values")) {
            for (const auto& [key, jv] : values->items()) {
                const StageParam* param = out.FindParam(key);
                if (!param) {
                    ctx.Add("variants: " + variant.id + ": unknown param \"" + key + "\"");
                    continue;
                }
                std::string err;
                StageValue value;
                if (!ParseStageParamValue(*param, jv, value, err)) {
                    ctx.Add("variants: " + variant.id + ": " + err);
                    continue;
                }
                variant.values[key] = ClampStageParamValue(*param, value);
            }
        }
        out.variants.push_back(std::move(variant));
    }
}

void ParseBindings(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "bindings");
    if (!arr) return;
    for (const auto& b : *arr) {
        if (!b.is_object()) {
            ctx.Add("bindings: entry is not an object");
            continue;
        }
        StageBinding binding;
        binding.param = ReadString(b, "param");
        binding.target = ReadString(b, "target");
        if (binding.param.empty() || binding.target.empty()) {
            ctx.Add("bindings: an entry needs \"param\" and \"target\"");
            continue;
        }
        binding.scale = (float)ReadDouble(b, "scale", binding.scale);
        binding.offset = (float)ReadDouble(b, "offset", binding.offset);
        if (const auto g = b.find("gain"); g != b.end()) {
            if (g->is_string()) binding.gain = g->get<std::string>();
            else ctx.Add("bindings: " + binding.param + " -> " + binding.target + ": \"gain\" must be a param key");
        }
        if (const nlohmann::json* remap = ArrayAt(b, "remap")) {
            if (remap->size() != 4) {
                ctx.Add("bindings: " + binding.param + " -> " + binding.target + ": \"remap\" needs 4 numbers");
            } else {
                std::array<float, 4> r{};
                bool ok = true;
                for (size_t i = 0; i < 4; ++i) {
                    if (!(*remap)[i].is_number()) ok = false;
                    else r[i] = (*remap)[i].get<float>();
                }
                if (ok) binding.remap = r;
                else ctx.Add("bindings: " + binding.param + " -> " + binding.target + ": \"remap\" needs 4 numbers");
            }
        }
        out.bindings.push_back(std::move(binding));
    }
}

void ParseCues(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "cues");
    if (!arr) return;
    std::set<std::string> seen;
    for (const auto& c : *arr) {
        if (!c.is_object()) {
            ctx.Add("cues: entry is not an object");
            continue;
        }
        StageCue cue;
        cue.param = ReadString(c, "param");
        if (cue.param.empty()) {
            ctx.Add("cues: an entry has no \"param\"");
            continue;
        }
        if (!seen.insert(cue.param).second) {
            ctx.Add("cues: two tracks for \"" + cue.param + "\"");
            continue;
        }
        cue.stepped = ReadBool(c, "stepped", cue.stepped);
        const StageParam* param = out.FindParam(cue.param);
        const nlohmann::json* keys = ArrayAt(c, "keys");
        if (!keys) {
            ctx.Add("cues: " + cue.param + ": no \"keys\"");
            continue;
        }
        for (const auto& k : *keys) {
            if (!k.is_object()) {
                ctx.Add("cues: " + cue.param + ": a key is not an object");
                continue;
            }
            StageCueKey key;
            key.frame = std::max(0, ReadInt(k, "frame", 0));
            const auto vIt = k.find("value");
            if (vIt == k.end()) {
                ctx.Add("cues: " + cue.param + ": a key has no \"value\"");
                continue;
            }
            std::string err;
            if (param && !ParseStageParamValue(*param, *vIt, key.value, err)) {
                ctx.Add("cues: " + err);
                continue;
            }
            if (param) key.value = ClampStageParamValue(*param, key.value);
            const auto at = std::lower_bound(cue.keys.begin(), cue.keys.end(), key.frame,
                                             [](const StageCueKey& a, int f) { return a.frame < f; });
            if (at != cue.keys.end() && at->frame == key.frame) *at = key;
            else cue.keys.insert(at, key);
        }
        out.cues.push_back(std::move(cue));
    }
}

void ParseDrivers(const nlohmann::json& j, StageManifest& out, ParseCtx& ctx) {
    const nlohmann::json* arr = ArrayAt(j, "drivers");
    if (!arr) {
        if (j.contains("drivers")) ctx.Add("drivers: must be an array");
        return;
    }
    for (const auto& d : *arr) {
        if (!d.is_object()) {
            ctx.Add("drivers: entry is not an object");
            continue;
        }
        StageDriver driver;
        driver.param = ReadString(d, "param");
        if (driver.param.empty()) {
            ctx.Add("drivers: an entry has no \"param\"");
            continue;
        }
        const std::string at = "drivers: " + driver.param + ": ";
        bool ok = true;
        // a present member of the wrong type / an unknown enum string is a problem (absent = the default)
        auto number = [&](const char* key, float& v) -> bool {
            const auto it = d.find(key);
            if (it == d.end()) return false;
            if (!it->is_number()) {
                ctx.Add(at + "\"" + key + "\" must be a number");
                ok = false;
                return false;
            }
            v = it->get<float>();
            return true;
        };
        auto word = [&](const char* key, std::string& v) -> bool {
            const auto it = d.find(key);
            if (it == d.end()) return false;
            if (!it->is_string()) {
                ctx.Add(at + "\"" + key + "\" must be a string");
                ok = false;
                return false;
            }
            v = it->get<std::string>();
            return true;
        };
        std::string s;
        if (!word("input", s)) {
            if (ok) ctx.Add(at + "no \"input\"");
            continue;
        }
        if (!ParseStageDriverInput(Lower(s), driver.input)) {
            ctx.Add(at + "unknown input \"" + s + "\"");
            continue;
        }
        if (word("shape", s) && !ParseStageDriverShape(Lower(s), driver.shape)) {
            ctx.Add(at + "unknown shape \"" + s + "\"");
            ok = false;
        }
        if (word("blend", s) && !ParseStageDriverBlend(Lower(s), driver.blend)) {
            ctx.Add(at + "unknown blend \"" + s + "\"");
            ok = false;
        }
        word("amount", driver.amount);
        number("every", driver.every);
        number("phase", driver.phase);
        driver.hasPower = number("power", driver.power);
        number("duty", driver.duty);
        number("min", driver.min);
        number("max", driver.max);
        if (!ok) continue;
        out.drivers.push_back(std::move(driver));
    }
}

} // namespace

bool ParseStageManifest(const std::string& json, const std::filesystem::path& dir, StageManifest& out,
                        std::string& error) {
    ParseCtx ctx;
    out = StageManifest{};
    out.dir = dir;
    out.manifestPath = dir / Utf8ToPath(kStageManifestName);
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        error = "stage.json is not a JSON object";
        return false;
    }
    const std::string format = Lower(ReadString(j, "format"));
    if (format != kStageFormatName) {
        error = "\"format\" must be \"" + std::string(kStageFormatName) + "\"";
        return false;
    }
    const int version = ReadInt(j, "formatVersion", 0);
    if (version != (int)kStageFormatVersion) {
        error = "unsupported \"formatVersion\" " + std::to_string(version) + " (this build reads " +
                std::to_string(kStageFormatVersion) + ")";
        return false;
    }
    out.id = Trimmed(ReadString(j, "id"));
    if (out.id.empty()) ctx.Add("missing \"id\"");
    out.version = ReadString(j, "version");
    out.name = Text(j, "name");
    out.description = Text(j, "description");
    out.license = ReadString(j, "license");
    out.minAppVersion = Trimmed(ReadString(j, "minAppVersion"));
    if (const nlohmann::json* tags = ArrayAt(j, "tags"))
        for (const auto& t : *tags)
            if (t.is_string()) out.tags.push_back(Lower(Trimmed(t.get<std::string>())));
    ParseAuthors(j, out.authors, ctx);
    ParsePreview(j, out, ctx);
    ParseModels(j, out, ctx);
    ParseMaterials(j, out, ctx);
    ParseLights(j, out, ctx);
    out.gltfLights = ReadBool(j, "gltfLights", false);   // S2a: append the parts' KHR_lights_punctual lights
    ParseSockets(j, out, ctx);
    ParseCameras(j, out, ctx);
    ParseScreens(j, out, ctx);
    ParseEnvironment(j, out, ctx);
    ParsePacks(j, out, ctx);
    ParseGroups(j, out, ctx);
    ParseParams(j, out, ctx);
    ParseVariants(j, out, ctx);
    out.defaultVariant = Trimmed(ReadString(j, "defaultVariant"));
    ParseBindings(j, out, ctx);
    ParseCues(j, out, ctx);
    ParseDrivers(j, out, ctx);

    error = ctx.error;
    return error.empty();
}

bool LoadStageManifest(const std::filesystem::path& stageJson, StageManifest& out, std::string& error) {
    std::ifstream in(stageJson, std::ios::binary);
    if (!in) {
        error = "cannot open " + PathToUtf8(stageJson);
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::filesystem::path dir = stageJson.parent_path();
    if (!ParseStageManifest(buf.str(), dir, out, error)) return false;
    return true;
}

// ---- validation ------------------------------------------------------------------------------------------------------

namespace {

bool IsValidStageId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// Can a binding of `paramType` feed a target of `targetType`?
bool BindingTypesFit(StageParamType paramType, StageTargetSpec::ValueType targetType, std::string& why) {
    using VT = StageTargetSpec::ValueType;
    switch (targetType) {
    case VT::Color:
        if (paramType != StageParamType::Color) {
            why = "the target takes a colour, the param is not a color param";
            return false;
        }
        return true;
    case VT::String:
        if (paramType != StageParamType::Enum) {
            why = "the target takes a string (an enum option's value), the param is not an enum";
            return false;
        }
        return true;
    case VT::Vec3:
        if (paramType == StageParamType::Color) return true;
        if (paramType == StageParamType::Enum) {
            why = "an enum cannot drive a vec3 target";
            return false;
        }
        return true;
    default:
        if (paramType == StageParamType::Color) {
            why = "a color param cannot drive a number / flag target (bind its components individually)";
            return false;
        }
        return true;
    }
}

} // namespace

std::vector<std::string> ValidateStageManifest(const StageManifest& manifest) {
    std::vector<std::string> errors;
    auto err = [&](const std::string& e) { errors.push_back(e); };

    if (!IsValidStageId(manifest.id))
        err("id: \"" + manifest.id + "\" is not [a-z0-9_-]{1,64}");
    if (manifest.models.empty()) err("models: no model parts");
    if (manifest.name.Empty()) err("name: missing (a plain string or a language object)");

    // unique names / ids
    auto uniqueNames = [&](const std::vector<std::string>& names, const char* what) {
        std::set<std::string> seen;
        for (const auto& n : names)
            if (!seen.insert(n).second) err(std::string(what) + ": repeated name \"" + n + "\"");
    };
    {
        std::vector<std::string> parts, lights, sockets, cameras, screens, params, variants, groups;
        for (const auto& m : manifest.models) parts.push_back(m.file);
        for (const auto& l : manifest.lights) lights.push_back(l.name);
        for (const auto& s : manifest.sockets) sockets.push_back(s.name);
        for (const auto& c : manifest.cameras) cameras.push_back(c.name);
        for (const auto& s : manifest.screens) screens.push_back(s.id);
        for (const auto& p : manifest.params) params.push_back(p.key);
        for (const auto& v : manifest.variants) variants.push_back(v.id);
        for (const auto& g : manifest.groups) groups.push_back(g.id);
        uniqueNames(parts, "models");
        uniqueNames(lights, "lights");
        uniqueNames(sockets, "sockets");
        uniqueNames(cameras, "cameras");
        uniqueNames(screens, "screens");
        uniqueNames(params, "params");
        uniqueNames(variants, "variants");
        uniqueNames(groups, "groups");
    }

    // materials: glob syntax and screens
    for (const auto& r : manifest.materials) {
        if (const auto& screen = r.screen; screen && !manifest.FindScreen(*screen))
            err("materials: " + r.match + ": unknown screen \"" + *screen + "\"");
        if (!r.baseColor && !r.roughness && !r.metallic && !r.emissive && !r.emissiveStrength && !r.transmission &&
            !r.ior && !r.alpha && !r.mirror && !r.screen && !r.castShadow && !r.receiveShadow)
            err("materials: " + r.match + ": no override values");
    }

    // screens
    for (const auto& s : manifest.screens) {
        if (s.materials.empty()) err("screens: " + s.id + ": no \"materials\"");
        for (const auto& m : s.materials)
            if (Trimmed(m).empty()) err("screens: " + s.id + ": an empty material name");
        std::string why;
        switch (s.source) {
        case StageScreenSource::Image:
        case StageScreenSource::Video:
            if (s.file.empty()) err("screens: " + s.id + ": source \"" + StageScreenSourceName(s.source) + "\" needs \"file\"");
            else if (!SafeRelativeFile(s.file, why)) err("screens: " + s.id + ": " + s.file + ": " + why);
            break;
        case StageScreenSource::Camera:
            if (s.camera.empty()) err("screens: " + s.id + ": source \"camera\" needs \"camera\"");
            else if (!std::any_of(manifest.cameras.begin(), manifest.cameras.end(),
                                  [&](const StageCamera& c) { return c.name == s.camera; }))
                err("screens: " + s.id + ": unknown camera \"" + s.camera + "\"");
            break;
        case StageScreenSource::Program:
            if (s.file.empty()) err("screens: " + s.id + ": source \"program\" needs \"file\" (the pack folder)");
            else if (!SafeRelativeFile(s.file, why)) err("screens: " + s.id + ": " + s.file + ": " + why);
            break;
        case StageScreenSource::None: break;
        }
        if (s.brightness < 0.0f) err("screens: " + s.id + ": negative brightness");
    }

    // lights
    for (const auto& l : manifest.lights) {
        if (l.v.coneInner > l.v.coneOuter) err("lights: " + l.name + ": coneInner > coneOuter");
        if (l.kind == studio::LightKind::Sun && l.keys.size() > 0)
            err("lights: " + l.name + ": a sun light cannot be keyed (a stage sun takes its keys from the cues)");
        // S7b: the gobo block (the tool checks the file itself: existence, an image extension).
        if (!l.gobo.empty()) {
            std::string why;
            if (!SafeRelativeFile(l.gobo, why)) err("lights: " + l.name + ": gobo.file: " + l.gobo + ": " + why);
            if (!(l.goboScale > 0.0f)) err("lights: " + l.name + ": gobo.scale must be > 0");
            if (l.goboFocus < 0.0f || l.goboFocus > 1.0f) err("lights: " + l.name + ": gobo.focus must be 0..1");
        }
        // S9b-1: the Physical Sky light's ranges (the model clamps too; a typo should still be a stage_check error).
        if (l.kind == studio::LightKind::Sky) {
            if (l.v.skyTimeOfDay < 0.0f || l.v.skyTimeOfDay > 24.0f)
                err("lights: " + l.name + ": sky.timeOfDay must be 0..24 hours");
            if (l.v.skyTurbidity < 1.7f || l.v.skyTurbidity > 10.0f)
                err("lights: " + l.name + ": sky.turbidity must be 1.7..10");
            if (l.v.skyHaze < 0.0f || l.v.skyHaze > 1.0f) err("lights: " + l.name + ": sky.haze must be 0..1");
            if (l.v.skyIntensity < 0.0f) err("lights: " + l.name + ": sky.intensity must be >= 0");
            if (l.v.skySunIntensity < 0.0f) err("lights: " + l.name + ": sky.sunIntensity must be >= 0");
            if (l.v.skyMaxElevationDeg <= 0.0f || l.v.skyMaxElevationDeg >= 90.0f)
                err("lights: " + l.name + ": sky.maxElevationDeg must be 0..90");
            // S9b-3: the sky's image (the tool also checks the file itself: existence, an image extension).
            if (!l.skyMap.empty()) {
                std::string why;
                if (!SafeRelativeFile(l.skyMap, why)) err("lights: " + l.name + ": skyMap: " + l.skyMap + ": " + why);
            }
            studio::SkyMapEncoding enc;
            if (!studio::ParseSkyMapEncoding(l.skyMapEncoding, enc))
                err("lights: " + l.name + ": skyMapEncoding must be auto / linear / srgb / rgbt");
            if (l.v.skyMapIntensity < 0.0f) err("lights: " + l.name + ": sky.mapIntensity must be >= 0");
            // S9b-4: the cloud layer on the physical sky.
            if (l.v.cloudCoverage < 0.0f || l.v.cloudCoverage > 1.0f)
                err("lights: " + l.name + ": sky.cloudCoverage must be 0..1");
            if (l.v.cloudOpacity < 0.0f || l.v.cloudOpacity > 1.0f)
                err("lights: " + l.name + ": sky.cloudOpacity must be 0..1");
            if (l.v.cloudSoftness < 0.02f || l.v.cloudSoftness > 1.0f)
                err("lights: " + l.name + ": sky.cloudSoftness must be 0.02..1");
            if (l.v.cloudHeightKm < 0.3f || l.v.cloudHeightKm > 12.0f)
                err("lights: " + l.name + ": sky.cloudHeight must be 0.3..12 km");
            if (l.v.cloudScaleKm < 0.2f || l.v.cloudScaleKm > 20.0f)
                err("lights: " + l.name + ": sky.cloudScale must be 0.2..20 km");
            if (l.v.cloudWindDeg < -360.0f || l.v.cloudWindDeg > 360.0f)
                err("lights: " + l.name + ": sky.cloudWindDeg must be -360..360");
            if (l.v.cloudWindSpeed < 0.0f || l.v.cloudWindSpeed > 100.0f)
                err("lights: " + l.name + ": sky.cloudWindSpeed must be 0..100 m/s");
        }
    }
    {
        int suns = 0, ambients = 0, skies = 0;
        for (const auto& l : manifest.lights) {
            if (l.kind == studio::LightKind::Sun) ++suns;
            if (l.kind == studio::LightKind::Ambient) ++ambients;
            if (l.kind == studio::LightKind::Sky) ++skies;
        }
        if (suns > 1) err("lights: at most one sun light");
        if (ambients > 1) err("lights: at most one ambient light");
        if (skies > 1) err("lights: at most one sky light");
    }

    // S9: local fog volumes (environment.fogVolumes). A zero / inverted extent has no volume to fill.
    for (size_t i = 0; i < manifest.environment.fogVolumes.size(); ++i) {
        const StageFogVolume& f = manifest.environment.fogVolumes[i];
        const std::string at = "environment.fogVolumes[" + std::to_string(i) + "]";
        if (f.extent.x <= 0.0f || f.extent.y <= 0.0f || f.extent.z <= 0.0f)
            err(at + ": extent must be positive [x,y,z] (half size, MMD units)");
        if (f.edge < 0.0f || f.edge > 1.0f) err(at + ": edge must be 0..1");
        if (f.density < 0.0f) err(at + ": negative density");
    }

    // sockets / cameras
    for (const auto& s : manifest.sockets) {
        if (s.scale.x == 0 || s.scale.y == 0 || s.scale.z == 0) err("sockets: " + s.name + ": zero scale");
    }
    for (const auto& c : manifest.cameras) {
        if (c.fov <= 0.0f || c.fov >= 180.0f) err("cameras: " + c.name + ": fov must be 1..179 degrees");
        if (c.position.x == c.target.x && c.position.y == c.target.y && c.position.z == c.target.z)
            err("cameras: " + c.name + ": position equals target");
    }

    // packs
    for (size_t i = 0; i < manifest.packs.size(); ++i) {
        const StagePackRef& p = manifest.packs[i];
        const std::string at = "packs[" + std::to_string(i) + "]";
        switch (p.role) {
        case StagePackRole::Surface:
            if (p.materials.empty()) err(at + ": a surface pack needs \"materials\"");
            break;
        case StagePackRole::World:
            if (p.socket.empty()) err(at + ": a world pack needs \"socket\"");
            else if (!std::any_of(manifest.sockets.begin(), manifest.sockets.end(),
                                  [&](const StageSocket& s) { return s.name == p.socket; }))
                err(at + ": unknown socket \"" + p.socket + "\"");
            break;
        case StagePackRole::Screen:
            if (p.screen.empty()) err(at + ": a screen pack needs \"screen\"");
            else if (!manifest.FindScreen(p.screen)) err(at + ": unknown screen \"" + p.screen + "\"");
            break;
        }
    }

    // params / variants / default variant
    for (const auto& p : manifest.params) {
        if (p.type == StageParamType::Enum) {
            std::set<std::string> values;
            for (const auto& o : p.options)
                if (!values.insert(o.value).second) err("params: " + p.key + ": repeated option \"" + o.value + "\"");
        }
        if (p.type == StageParamType::Color && !p.options.empty())
            err("params: " + p.key + ": a color param cannot have options");
    }
    for (const auto& v : manifest.variants)
        for (const auto& [key, value] : v.values) {
            (void)value;
            if (!manifest.FindParam(key)) err("variants: " + v.id + ": unknown param \"" + key + "\"");
        }
    if (!manifest.defaultVariant.empty() && !manifest.FindVariant(manifest.defaultVariant))
        err("defaultVariant: unknown variant \"" + manifest.defaultVariant + "\"");

    // bindings
    for (const auto& b : manifest.bindings) {
        const StageParam* param = manifest.FindParam(b.param);
        const std::string at = "bindings: " + b.param + " -> " + b.target + ": ";
        if (!param) {
            err(at + "unknown param");
            continue;
        }
        const StageTargetSpec spec = ResolveStageTarget(manifest, b.target);
        if (!spec.ok) {
            err(at + spec.error);
            continue;
        }
        std::string why;
        if (!BindingTypesFit(param->type, spec.type, why)) err(at + why);
        if (!b.gain.empty()) {
            const StageParam* gain = manifest.FindParam(b.gain);
            if (!gain) err(at + "unknown gain param \"" + b.gain + "\"");
            else if (gain->type != StageParamType::Float && gain->type != StageParamType::Int)
                err(at + "the gain param \"" + b.gain + "\" must be a float or int");
            if (spec.type == StageTargetSpec::ValueType::Bool || spec.type == StageTargetSpec::ValueType::String)
                err(at + "a bool / string target takes no gain");
        }
        if (b.remap) {
            const auto& r = *b.remap;
            if (r[1] == r[0]) err(at + "\"remap\" has an empty input range");
        }
    }

    // cues
    for (const auto& c : manifest.cues) {
        const StageParam* param = manifest.FindParam(c.param);
        if (!param) {
            err("cues: " + c.param + ": unknown param");
            continue;
        }
        if (c.keys.empty()) err("cues: " + c.param + ": no keys");
        if (param->type == StageParamType::Color && c.stepped)
            err("cues: " + c.param + ": a color track cannot be stepped");
    }

    // drivers
    std::set<std::string> driven;
    for (const auto& d : manifest.drivers) driven.insert(d.param);
    for (const auto& d : manifest.drivers) {
        const std::string at = "drivers: " + d.param + ": ";
        const StageParam* param = manifest.FindParam(d.param);
        if (!param) {
            err(at + "unknown param");
        } else if (param->type == StageParamType::Bool || param->type == StageParamType::Enum) {
            err(at + "a " + std::string(StageParamTypeName(param->type)) + " param cannot be driven");
        } else if (param->type == StageParamType::Color && d.blend != StageDriverBlend::Mul) {
            err(at + "a color param takes \"blend\": \"mul\" only");
        }
        if (!(d.every > 0.0f)) err(at + "\"every\" must be > 0");
        if (!(d.power > 0.0f)) err(at + "\"power\" must be > 0");
        if (d.duty < 0.0f || d.duty > 1.0f) err(at + "\"duty\" must be 0..1");
        if (!d.amount.empty()) {
            const StageParam* amount = manifest.FindParam(d.amount);
            if (!amount) err(at + "unknown amount param \"" + d.amount + "\"");
            else if (amount->type != StageParamType::Float)
                err(at + "the amount param \"" + d.amount + "\" must be a float");
            if (driven.count(d.amount)) err(at + "the amount param \"" + d.amount + "\" is itself driven");
        }
    }

    // previews exist
    for (const auto& p : manifest.previews) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(manifest.dir / p, ec))
            err("preview: missing file " + PathToUtf8(p));
    }
    return errors;
}

} // namespace mmdx
