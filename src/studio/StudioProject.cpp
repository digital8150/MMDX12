// Studio project file (.mmdxproj) I/O: atomic file writes and UTF-8 JSON, independent of the App
// and the renderer (see StudioProject.h for the format and the contracts of each function).
#include "studio/StudioProject.h"
#include "core/TextUtil.h"
#include <json.hpp>
#include <Windows.h>
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
DirectX::XMFLOAT3 ReadVec3(const nlohmann::json& j, const char* key, DirectX::XMFLOAT3 def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return def;
    for (int i = 0; i < 3; ++i)
        if (!(*it)[i].is_number()) return def;
    return {(float)(*it)[0].get<double>(), (float)(*it)[1].get<double>(), (float)(*it)[2].get<double>()};
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
            if (m.kind == ModelKind::Character && !m.shader.pack.empty()) {  // the default shading is not written
                nlohmann::json params = nlohmann::json::object();
                for (const auto& [k, v] : m.shader.params) params[k] = v;
                jm["shader"] = {{"pack", m.shader.pack}, {"params", params}};
                if (!m.shader.textureFolder.empty())
                    jm["shader"]["textureFolder"] = m.shader.textureFolder;
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
            ed["useLightTrack"] = data.editor.useLightTrack;  // also for older readers (the lighting object wins)
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
            // the studio's lighting source + spot rig (LightRig); older readers ignore it
            {
                const LightRig& rig = data.editor.lighting;
                nlohmann::json li;
                li["source"] = LightSourceName(rig.source);
                li["preset"] = rig.presetIndex;
                if (rig.key.enabled) {
                    li["key"] = {
                        {"direction", {rig.key.direction.x, rig.key.direction.y, rig.key.direction.z}},
                        {"color", {rig.key.color.x, rig.key.color.y, rig.key.color.z}},
                        {"intensity", rig.key.intensity},
                        {"rimStrength", rig.key.rimStrength},
                        {"rimColor", {rig.key.rimColor.x, rig.key.rimColor.y, rig.key.rimColor.z}},
                    };
                }
                if (!rig.spots.empty()) {
                    nlohmann::json spots = nlohmann::json::array();
                    for (const SpotLight& s : rig.spots) {
                        nlohmann::json js;
                        js["name"] = s.name;
                        js["mode"] = SpotModeName(s.mode);
                        js["position"] = {s.position.x, s.position.y, s.position.z};
                        js["aim"] = {s.aim.x, s.aim.y, s.aim.z};
                        js["color"] = {s.color.x, s.color.y, s.color.z};
                        js["intensity"] = s.intensity;
                        js["cone"] = s.coneOuter;
                        js["enabled"] = s.enabled;
                        js["phase"] = s.swingPhase;
                        if (!s.keys.empty()) {
                            nlohmann::json keys = nlohmann::json::array();
                            for (const SpotKf& k : s.keys)
                                keys.push_back({{"frame", k.frame},
                                                {"position", {k.position.x, k.position.y, k.position.z}},
                                                {"aim", {k.aim.x, k.aim.y, k.aim.z}},
                                                {"color", {k.color.x, k.color.y, k.color.z}},
                                                {"intensity", k.intensity},
                                                {"cone", k.coneOuter}});
                            js["keys"] = std::move(keys);
                        }
                        spots.push_back(std::move(js));
                    }
                    li["spots"] = std::move(spots);
                }
                li["frontFill"] = rig.frontFill;
                ed["lighting"] = std::move(li);
            }
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

        if (j.contains("editor") && j["editor"].is_object()) {
            const nlohmann::json& e = j["editor"];
            ProjectEditor ed;  // defaults, then overwrite what the file carries
            ed.frame = ReadInt(e, "frame", ed.frame);
            ed.selectedModel = ReadInt(e, "selectedModel", ed.selectedModel);
            bool legacyUseLightTrack = true;
            const bool hadLegacy = e.contains("useLightTrack") && (*e.find("useLightTrack")).is_boolean();
            ed.useMotionCamera = ReadBool(e, "useMotionCamera", ed.useMotionCamera);
            legacyUseLightTrack = ReadBool(e, "useLightTrack", legacyUseLightTrack);
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
            // ---- lighting source: the "lighting" object, else the legacy useLightTrack bool ----
            const auto liIt = e.find("lighting");
            if (liIt != e.end() && liIt->is_object()) {
                const nlohmann::json& li = *liIt;
                ParseLightSource(ReadString(li, "source", "vmd"), ed.lighting.source);
                ed.lighting.presetIndex = ReadInt(li, "preset", ed.lighting.presetIndex);
                ed.lighting.frontFill = ReadBool(li, "frontFill", ed.lighting.frontFill);
                const auto keyIt = li.find("key");
                if (keyIt != li.end() && keyIt->is_object()) {
                    ed.lighting.key.enabled = true;
                    const nlohmann::json& k = *keyIt;
                    ed.lighting.key.direction = ReadVec3(k, "direction", ed.lighting.key.direction);
                    ed.lighting.key.color = ReadVec3(k, "color", ed.lighting.key.color);
                    ed.lighting.key.intensity = (float)ReadDouble(k, "intensity", ed.lighting.key.intensity);
                    ed.lighting.key.rimStrength = (float)ReadDouble(k, "rimStrength", ed.lighting.key.rimStrength);
                    ed.lighting.key.rimColor = ReadVec3(k, "rimColor", ed.lighting.key.rimColor);
                }
                const auto spotsIt = li.find("spots");
                if (spotsIt != li.end() && spotsIt->is_array()) {
                    for (const auto& sp : *spotsIt) {
                        if (!sp.is_object()) continue;
                        SpotLight s;
                        s.name = ReadString(sp, "name", "");
                        ParseSpotMode(ReadString(sp, "mode", "auto"), s.mode);
                        s.position = ReadVec3(sp, "position", s.position);
                        s.aim = ReadVec3(sp, "aim", s.aim);
                        s.color = ReadVec3(sp, "color", s.color);
                        s.intensity = (float)ReadDouble(sp, "intensity", s.intensity);
                        s.coneOuter = (float)ReadDouble(sp, "cone", s.coneOuter);
                        s.coneInner = std::min(s.coneOuter * 0.6f, s.coneOuter);
                        s.enabled = ReadBool(sp, "enabled", s.enabled);
                        s.swingPhase = (float)ReadDouble(sp, "phase", s.swingPhase);
                        const auto keysIt = sp.find("keys");
                        if (keysIt != sp.end() && keysIt->is_array()) {
                            for (const auto& kk : *keysIt) {
                                if (!kk.is_object()) continue;
                                SpotKf kf;
                                kf.frame = ReadInt(kk, "frame", 0);
                                kf.position = ReadVec3(kk, "position", kf.position);
                                kf.aim = ReadVec3(kk, "aim", kf.aim);
                                kf.color = ReadVec3(kk, "color", kf.color);
                                kf.intensity = (float)ReadDouble(kk, "intensity", kf.intensity);
                                kf.coneOuter = (float)ReadDouble(kk, "cone", kf.coneOuter);
                                s.keys.push_back(kf);
                            }
                            std::sort(s.keys.begin(), s.keys.end(),
                                      [](const SpotKf& a, const SpotKf& b) { return a.frame < b.frame; });
                        }
                        if (!s.name.empty() && !ed.lighting.Spot(s.name)) ed.lighting.spots.push_back(std::move(s));
                    }
                }
                ed.useLightTrack = ed.lighting.source == LightSource::VmdTrack;  // keep the legacy view in step
            } else if (hadLegacy) {
                // v1 project: true -> VmdTrack only when the camera VMD actually carries light keys, else Preset;
                // false -> Preset (the user had turned the light track off)
                ed.lighting.source = legacyUseLightTrack && !out.camera.light.empty() ? LightSource::VmdTrack
                                                                                      : LightSource::Preset;
                ed.useLightTrack = ed.lighting.source == LightSource::VmdTrack;
            } else {
                ed.useLightTrack = legacyUseLightTrack;
            }
            out.editor = ed;
        }

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
