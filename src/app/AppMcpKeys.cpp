// Bulk key read / write tools (B2): camera, scene light, bone and morph key lists as JSON. Every write is a single
// undo step ("AI: <tool>"); everything is validated before anything changes.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/McpKeyUtil.h"
#include "core/TextUtil.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"

namespace mmdx {

using namespace studio;

namespace {

constexpr size_t kMaxKeys = 20000;
const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター

bool ReadVec3(const nlohmann::json& j, DirectX::XMFLOAT3& out) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!j[i].is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    return true;
}

bool ReadVec3(const nlohmann::json& j, float out[3]) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!j[i].is_number()) return false;
    for (int i = 0; i < 3; ++i) out[i] = j[i].get<float>();
    return true;
}

bool ReadVec2(const nlohmann::json& j, DirectX::XMFLOAT2& out) {
    if (!j.is_array() || j.size() != 2) return false;
    for (int i = 0; i < 2; ++i)
        if (!j[i].is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>()};
    return true;
}

bool ReadBool(const nlohmann::json& j, bool& out) {
    if (j.is_boolean()) {
        out = j.get<bool>();
        return true;
    }
    if (j.is_number_integer()) {
        out = j.get<int>() != 0;
        return true;
    }
    return false;
}

nlohmann::json Vec3Json(const DirectX::XMFLOAT3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}
nlohmann::json QuatJson(const DirectX::XMFLOAT4& q) {
    return nlohmann::json::array({q.x, q.y, q.z, q.w});
}

DirectX::XMFLOAT3 PerformerFocus(const StudioDoc& d) {
    for (const auto& m : d.models) {
        if (m->kind == ModelKind::Character && m->visible && m->pmx && m->inst) {
            const int c = m->pmx->FindBone(kCenterBone);
            if (c >= 0) return m->inst->BoneWorldPosition(c);
        }
    }
    return {0.0f, 10.0f, 0.0f};
}

bool ParseMode(const nlohmann::json& args, std::string& mode, std::string& error) {
    mode = args.value("mode", std::string("merge"));
    if (mode != "merge" && mode != "replace_range" && mode != "replace_all") {
        error = "mode must be merge, replace_range or replace_all";
        return false;
    }
    return true;
}

bool ReadFrame(const nlohmann::json& j, int& frame, std::string& error) {
    if (!j.is_number_integer()) {
        error = "frame must be an integer";
        return false;
    }
    frame = j.get<int>();
    if (frame < 0) {
        error = "frame must be >= 0";
        return false;
    }
    return true;
}

int LightIndexByUid(const StudioDoc& d, uint32_t uid) {
    for (int i = 0; i < (int)d.lights.size(); ++i)
        if (d.lights[(size_t)i].uid == uid) return i;
    return -1;
}

// The keyable LightValues fields of a scene light. `out` must already hold the base values.
bool ApplyLightValues(const nlohmann::json& j, LightValues& out, std::string& error) {
    if (!j.is_object()) {
        error = "light values must be an object";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& k = it.key();
        if (k == "position") {
            if (!ReadVec3(it.value(), out.position)) { error = "position must be [x,y,z]"; return false; }
        } else if (k == "aim") {
            if (!ReadVec3(it.value(), out.aim)) { error = "aim must be [x,y,z]"; return false; }
        } else if (k == "direction") {
            if (!ReadVec3(it.value(), out.direction)) { error = "direction must be [x,y,z]"; return false; }
        } else if (k == "color") {
            if (!ReadVec3(it.value(), out.color)) { error = "color must be [r,g,b]"; return false; }
        } else if (k == "intensity") {
            if (!it.value().is_number()) { error = "intensity must be a number"; return false; }
            out.intensity = it.value().get<float>();
        } else if (k == "range") {
            if (!it.value().is_number()) { error = "range must be a number"; return false; }
            out.range = it.value().get<float>();
        } else if (k == "cone_outer_deg") {
            if (!it.value().is_number()) { error = "cone_outer_deg must be a number"; return false; }
            out.coneOuter = DirectX::XMConvertToRadians(it.value().get<float>());
        } else if (k == "cone_inner_deg") {
            if (!it.value().is_number()) { error = "cone_inner_deg must be a number"; return false; }
            out.coneInner = DirectX::XMConvertToRadians(it.value().get<float>());
        } else if (k == "size") {
            if (!ReadVec2(it.value(), out.size)) { error = "size must be [w,h]"; return false; }
        } else {
            error = "unknown light value: " + k;
            return false;
        }
    }
    if (out.coneInner > out.coneOuter) out.coneInner = out.coneOuter;  // the UI clamps cones the same way
    return true;
}

} // namespace

bool App::McpWriteCameraKeys(const nlohmann::json& keysJson, const std::string& mode, size_t& changed,
                             std::string& error, bool pushUndo) {
    changed = 0;
    if (!studio_) {
        error = "Studio is not active";
        return false;
    }
    StudioDoc& d = *studio_;
    if (!keysJson.is_array() || keysJson.empty()) {
        error = "keys must be a non-empty array";
        return false;
    }
    if (keysJson.size() > kMaxKeys) {
        error = "Too many keys (max 20000)";
        return false;
    }
    if (mode != "merge" && mode != "replace_range" && mode != "replace_all") {
        error = "mode must be merge, replace_range or replace_all";
        return false;
    }

    struct NewKey {
        int frame = 0;
        bool hasTarget = false, hasYaw = false, hasPitch = false, hasRoll = false, hasDist = false;
        bool hasFov = false, hasPerspective = false;
        DirectX::XMFLOAT3 target{};
        float yaw = 0, pitch = 0, roll = 0, dist = 0, fov = 0;
        bool perspective = true;
        nlohmann::json curve;
    };
    std::vector<NewKey> parsed;
    parsed.reserve(keysJson.size());
    for (const nlohmann::json& kj : keysJson) {
        if (!kj.is_object()) { error = "every key must be an object"; return false; }
        NewKey k;
        if (!ReadFrame(kj.contains("frame") ? kj["frame"] : nlohmann::json(), k.frame, error)) return false;
        if (kj.contains("target")) {
            if (!ReadVec3(kj["target"], k.target)) { error = "target must be [x,y,z]"; return false; }
            k.hasTarget = true;
        }
        auto number = [&](const char* name, float& out, bool& has) {
            if (!kj.contains(name)) return true;
            if (!kj[name].is_number()) {
                error = std::string(name) + " must be a number";
                return false;
            }
            out = kj[name].get<float>();
            has = true;
            return true;
        };
        if (!number("yaw_deg", k.yaw, k.hasYaw) || !number("pitch_deg", k.pitch, k.hasPitch) ||
            !number("roll_deg", k.roll, k.hasRoll) || !number("dist", k.dist, k.hasDist) ||
            !number("fov_deg", k.fov, k.hasFov)) {
            return false;
        }
        if (kj.contains("perspective")) {
            if (!ReadBool(kj["perspective"], k.perspective)) {
                error = "perspective must be a boolean";
                return false;
            }
            k.hasPerspective = true;
        }
        if (kj.contains("curve")) {
            uint8_t tmp[24];
            FillLinearCameraInterp(tmp);
            if (!ApplyCameraCurve(kj["curve"], tmp, error)) return false;
            k.curve = kj["curve"];
        }
        parsed.push_back(std::move(k));
    }

    // validate replace_range before editing
    int minFrame = std::numeric_limits<int>::max(), maxFrame = std::numeric_limits<int>::min();
    for (const NewKey& k : parsed) {
        minFrame = std::min(minFrame, k.frame);
        maxFrame = std::max(maxFrame, k.frame);
    }

    std::vector<TrackState> before{CaptureTrack(d, -1, RowKind::Camera, "")};
    const std::vector<CameraKf> original = d.SelectedCameraTrack().camera;  // missing fields evaluate against the pre-edit track
    if (mode == "replace_all") {
        d.SelectedCameraTrack().camera.clear();
    } else if (mode == "replace_range") {
        d.SelectedCameraTrack().camera.erase(std::remove_if(d.SelectedCameraTrack().camera.begin(), d.SelectedCameraTrack().camera.end(),
                                             [&](const CameraKf& k) {
                                                 return k.frame >= minFrame && k.frame <= maxFrame;
                                             }),
                              d.SelectedCameraTrack().camera.end());
    }

    for (const NewKey& nk : parsed) {
        CameraKf base;
        if (!original.empty()) base = SampleCamera(original, nk.frame);
        else FillLinearCameraInterp(base.interp);
        CameraKf k = base;
        k.frame = nk.frame;
        if (nk.hasTarget) k.target = nk.target;
        if (nk.hasYaw || nk.hasPitch || nk.hasRoll) {
            const float pitch = nk.hasPitch ? nk.pitch : -DirectX::XMConvertToDegrees(base.rotation.x);
            const float yaw = nk.hasYaw ? nk.yaw : DirectX::XMConvertToDegrees(base.rotation.y);
            const float roll = nk.hasRoll ? nk.roll : DirectX::XMConvertToDegrees(base.rotation.z);
            k.rotation.x = -DirectX::XMConvertToRadians(pitch);
            k.rotation.y = DirectX::XMConvertToRadians(yaw);
            k.rotation.z = DirectX::XMConvertToRadians(roll);
        }
        if (nk.hasDist) k.distance = -nk.dist;
        if (nk.hasFov) k.fovDeg = std::clamp(nk.fov, 1.0f, 125.0f);
        if (nk.hasPerspective) k.perspective = nk.perspective;
        if (!ApplyCameraCurve(nk.curve, k.interp, error)) return false;
        UpsertKey(d.SelectedCameraTrack().camera, k);
        ++changed;
    }

    if (changed > 0) {
        d.useMotionCamera = true;
        if (pushUndo) StudioPushTrackEdit("AI: studio_set_camera_keys", before);
    }
    return true;
}

bool App::ExecuteMcpKeys(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise) {
    const bool cameraGet = tool == "studio_get_camera_keys";
    const bool cameraSet = tool == "studio_set_camera_keys";
    const bool lightSet = tool == "studio_set_light";
    const bool boneGet = tool == "studio_get_bone_keys";
    const bool boneSet = tool == "studio_set_bone_keys";
    const bool delKeys = tool == "studio_delete_keys";
    if (!cameraGet && !cameraSet && !lightSet && !boneGet && !boneSet && !delKeys) return false;

    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;

    // -----------------------------------------------------------------------
    // studio_get_camera_keys
    // -----------------------------------------------------------------------
    if (cameraGet) {
        int start = args.value("start", 0);
        int end = args.contains("end") ? args["end"].get<int>() : std::numeric_limits<int>::max();
        nlohmann::json keys = nlohmann::json::array();
        std::vector<int> frames;
        for (const CameraKf& k : d.SelectedCameraTrack().camera) {
            frames.push_back(k.frame);
            if (k.frame < start || k.frame > end) continue;
            CameraPose pose;
            pose.target = k.target;
            pose.rotation = k.rotation;
            pose.distance = k.distance;
            pose.fovDeg = (float)k.fovDeg;
            DirectX::XMFLOAT3 eye;
            CameraMotion::ToView(pose, nullptr, &eye);
            nlohmann::json o;
            o["frame"] = k.frame;
            o["target"] = Vec3Json(k.target);
            o["yaw_deg"] = DirectX::XMConvertToDegrees(k.rotation.y);
            o["pitch_deg"] = -DirectX::XMConvertToDegrees(k.rotation.x);
            o["roll_deg"] = DirectX::XMConvertToDegrees(k.rotation.z);
            o["dist"] = -k.distance;
            o["fov_deg"] = k.fovDeg;
            o["perspective"] = k.perspective;
            o["curve"] = CameraCurveToJson(k.interp);
            o["eye"] = Vec3Json(eye);
            keys.push_back(std::move(o));
        }
        nlohmann::json cuts = nlohmann::json::array();
        for (size_t i = 0; i + 1 < frames.size(); ++i)
            if (frames[i + 1] == frames[i] + 1 && frames[i] >= start && frames[i] <= end) cuts.push_back(frames[i]);
        promise->Resolve({{"keys", std::move(keys)}, {"cuts", std::move(cuts)}, {"count", d.SelectedCameraTrack().camera.size()}});
        return true;
    }

    // -----------------------------------------------------------------------
    // studio_set_camera_keys
    // -----------------------------------------------------------------------
    if (cameraSet) {
        const nlohmann::json* keysJson = args.contains("keys") ? &args["keys"] : nullptr;
        if (!keysJson || !keysJson->is_array() || keysJson->empty()) {
            promise->Reject("keys must be a non-empty array");
            return true;
        }
        if (keysJson->size() > kMaxKeys) {
            promise->Reject("Too many keys (max 20000)");
            return true;
        }
        std::string mode, error;
        if (!ParseMode(args, mode, error)) { promise->Reject(error); return true; }
        size_t changed = 0;
        if (!McpWriteCameraKeys(*keysJson, mode, changed, error)) {
            promise->Reject(error);
            return true;
        }
        promise->Resolve({{"changed_keys", changed},
                          {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()}});
        return true;
    }

    // -----------------------------------------------------------------------
    // studio_set_light
    // -----------------------------------------------------------------------
    if (lightSet) {
        const bool del = args.value("delete", false);
        const bool create = args.contains("create");
        const bool hasIndex = args.contains("index");
        std::string error;
        if ((int)create + (int)del > 1 || (del && hasIndex == false && !create)) {
            promise->Reject("Use index (existing light), create, or delete with index");
            return true;
        }
        if (!create && !hasIndex) {
            promise->Reject("studio_set_light requires 'index' or 'create'");
            return true;
        }

        int index = hasIndex ? args["index"].get<int>() : -1;
        if (hasIndex && !create && (index < 0 || index >= (int)d.lights.size())) {
            promise->Reject("Light index out of range: " + std::to_string(index));
            return true;
        }

        // validate props before touching the list
        nlohmann::json props = args.contains("props") ? args["props"] : nlohmann::json::object();
        if (!props.is_object()) {
            promise->Reject("props must be an object");
            return true;
        }
        std::string name;
        bool hasName = false;
        if (args.contains("name") && args["name"].is_string()) {
            name = args["name"].get<std::string>();
            hasName = true;
        }
        if (props.contains("name")) {
            if (!props["name"].is_string()) { promise->Reject("props.name must be a string"); return true; }
            name = props["name"].get<std::string>();
            hasName = true;
        }
        int targetModel = -2;  // -2 = not given, -1 = performer, >= 0 = model index
        if (props.contains("target_model")) {
            if (!props["target_model"].is_number_integer()) {
                promise->Reject("props.target_model must be an integer model index (-1 = performer)");
                return true;
            }
            targetModel = props["target_model"].get<int>();
            if (targetModel < -1 || targetModel >= (int)d.models.size()) {
                promise->Reject("props.target_model out of range: " + std::to_string(targetModel));
                return true;
            }
        }
        // each non-keyed prop is validated by name; invalid enum values are rejected by the Parse* helpers below
        {
            LightKind tmpKind = LightKind::Point;
            AimMode tmpAim = AimMode::Manual;
            TargetPart tmpPart = TargetPart::Centre;
            ShadowType tmpShadow = ShadowType::Hard;
            FalloffType tmpFalloff = FalloffType::None;
            bool tmpBool = false;
            float tmpFloat = 0;
            DirectX::XMFLOAT3 tmpVec3{};
            for (auto it = props.begin(); it != props.end(); ++it) {
                const std::string& k = it.key();
                if (k == "name" || k == "target_model") continue;
                if (k == "enabled" || k == "vmd_link" || k == "affect_diffuse" || k == "affect_specular" ||
                    k == "viewport_visible") {
                    if (!ReadBool(it.value(), tmpBool)) { promise->Reject(k + " must be a boolean"); return true; }
                } else if (k == "rim_strength" || k == "sway_phase" || k == "shadow_softness" ||
                           k == "shadow_density") {
                    if (!it.value().is_number()) { promise->Reject(k + " must be a number"); return true; }
                    tmpFloat = it.value().get<float>();
                } else if (k == "rim_color" || k == "sky_zenith" || k == "sky_horizon" || k == "ground_color" ||
                           k == "shadow_color") {
                    if (!ReadVec3(it.value(), tmpVec3)) { promise->Reject(k + " must be [r,g,b]"); return true; }
                } else if (k == "aim_mode") {
                    if (!it.value().is_string() || !ParseAimMode(ToLowerAscii(it.value().get<std::string>()), tmpAim)) {
                        promise->Reject("aim_mode must be manual, target or sway");
                        return true;
                    }
                } else if (k == "target_part") {
                    if (!it.value().is_string() || !ParseTargetPart(ToLowerAscii(it.value().get<std::string>()), tmpPart)) {
                        promise->Reject("target_part must be centre or head");
                        return true;
                    }
                } else if (k == "shadow") {
                    if (!it.value().is_string() || !ParseShadowType(ToLowerAscii(it.value().get<std::string>()), tmpShadow)) {
                        promise->Reject("shadow must be nocast, hard or soft");
                        return true;
                    }
                } else if (k == "falloff") {
                    if (!it.value().is_string() || !ParseFalloffType(ToLowerAscii(it.value().get<std::string>()), tmpFalloff)) {
                        promise->Reject("falloff must be none, linear or inverse_square");
                        return true;
                    }
                } else {
                    promise->Reject("unknown light prop: " + k);
                    return true;
                }
            }
        }

        nlohmann::json base = args.contains("base") ? args["base"] : nlohmann::json::object();
        if (!base.is_object()) {
            promise->Reject("base must be an object");
            return true;
        }
        nlohmann::json keysJson = args.contains("keys") ? args["keys"] : nlohmann::json::array();
        if (!keysJson.is_array()) {
            promise->Reject("keys must be an array");
            return true;
        }
        if (keysJson.size() > kMaxKeys) {
            promise->Reject("Too many keys (max 20000)");
            return true;
        }
        std::string mode;
        if (!ParseMode(args, mode, error)) { promise->Reject(error); return true; }

        struct NewLightKey {
            int frame = 0;
            nlohmann::json values = nlohmann::json::object();
        };
        std::vector<NewLightKey> parsedKeys;
        parsedKeys.reserve(keysJson.size());
        for (const nlohmann::json& kj : keysJson) {
            if (!kj.is_object()) { promise->Reject("every key must be an object"); return true; }
            NewLightKey nk;
            if (!ReadFrame(kj.contains("frame") ? kj["frame"] : nlohmann::json(), nk.frame, error)) {
                promise->Reject(error);
                return true;
            }
            for (auto it = kj.begin(); it != kj.end(); ++it)
                if (it.key() != "frame") nk.values[it.key()] = it.value();
            // validates the field names/values now, against throwaway values
            LightValues probe;
            if (!ApplyLightValues(nk.values, probe, error)) { promise->Reject(error); return true; }
            parsedKeys.push_back(std::move(nk));
        }
        {
            LightValues probe;
            if (!ApplyLightValues(base, probe, error)) { promise->Reject(error); return true; }
        }

        std::vector<SceneLight> before = d.lights;
        uint32_t uid = 0;
        int outIndex = index;
        if (create) {
            if (!args["create"].is_string()) {
                promise->Reject("create must be a light kind: sun, point, spot, area, ambient, fog_volume or sky");
                return true;
            }
            LightKind kind;
            if (!ParseLightKind(ToLowerAscii(args["create"].get<std::string>()), kind)) {
                promise->Reject("create must be sun, point, spot, area, ambient, fog_volume or sky");
                return true;
            }
            if (kind == LightKind::Sun) {
                for (const SceneLight& l : d.lights)
                    if (l.kind == LightKind::Sun) { promise->Reject("The scene already has a sun light (only one is allowed)"); return true; }
            }
            if (kind == LightKind::Ambient) {
                for (const SceneLight& l : d.lights)
                    if (l.kind == LightKind::Ambient) { promise->Reject("The scene already has an ambient light (only one is allowed)"); return true; }
            }
            if (kind == LightKind::Sky) {
                for (const SceneLight& l : d.lights)
                    if (l.kind == LightKind::Sky) { promise->Reject("The scene already has a sky light (only one is allowed)"); return true; }
            }
            SceneLight l;
            l.uid = d.nextLightUid++;
            l.kind = kind;
            l.enabled = true;
            const DirectX::XMFLOAT3 focus = PerformerFocus(d);
            if (kind == LightKind::Point) {
                l.name = "점광원 " + std::to_string(d.lights.size() + 1);
                l.v.position = {focus.x, focus.y + 40.0f, focus.z - 20.0f};
            } else if (kind == LightKind::Spot) {
                l.name = "스팟 " + std::to_string(d.lights.size() + 1);
                l.v.position = {focus.x, focus.y + 58.0f, focus.z - 18.0f};
                l.v.aim = focus;
                l.v.coneOuter = 0.24f;
                l.v.coneInner = 0.15f;
                l.v.intensity = 2.6f;
            } else if (kind == LightKind::Area) {
                l.name = "면광원 " + std::to_string(d.lights.size() + 1);
                l.v.position = {focus.x, focus.y + 40.0f, focus.z - 20.0f};
                l.v.aim = focus;
                l.v.size = {20.0f, 20.0f};
            } else if (kind == LightKind::Sun) {
                l.name = "메인 조명";
            } else if (kind == LightKind::Sky) {
                l.name = "하늘";
            } else {
                l.name = "환경광";
            }
            if (hasName) l.name = name;
            d.lights.push_back(std::move(l));
            outIndex = (int)d.lights.size() - 1;
            uid = d.lights[(size_t)outIndex].uid;
        } else {
            uid = d.lights[(size_t)index].uid;
        }

        if (del) {
            const uint32_t delUid = d.lights[(size_t)index].uid;
            std::vector<SceneLight> after = before;
            std::erase_if(after, [delUid](const SceneLight& l) { return l.uid == delUid; });
            if (d.selectedLightUid == delUid) d.selectedLightUid = 0;
            d.history.Push(std::make_unique<LightsCommand>(d, "AI: studio_set_light", before, std::move(after)));
            ++d.projectVersion;
            d.rowsKey = ~0ull;
            promise->Resolve({{"changed_keys", 0},
                              {"undo", d.history.UndoName()},
                              {"index", -1},
                              {"uid", delUid}});
            return true;
        }

        SceneLight& light = d.lights[(size_t)outIndex];
        // non-keyed properties
        for (auto it = props.begin(); it != props.end(); ++it) {
            const std::string& k = it.key();
            if (k == "name") light.name = it.value().get<std::string>();
            else if (k == "enabled") ReadBool(it.value(), light.enabled);
            else if (k == "vmd_link") ReadBool(it.value(), light.vmdLink);
            else if (k == "rim_strength") light.rimStrength = it.value().get<float>();
            else if (k == "rim_color") ReadVec3(it.value(), light.rimColor);
            else if (k == "aim_mode") ParseAimMode(ToLowerAscii(it.value().get<std::string>()), light.aimMode);
            else if (k == "target_model") light.targetUid = it.value().get<int>() < 0 ? 0u : d.models[(size_t)it.value().get<int>()]->uid;
            else if (k == "target_part") ParseTargetPart(ToLowerAscii(it.value().get<std::string>()), light.targetPart);
            else if (k == "sway_phase") light.swayPhase = it.value().get<float>();
            else if (k == "sky_zenith") ReadVec3(it.value(), light.skyZenith);
            else if (k == "sky_horizon") ReadVec3(it.value(), light.skyHorizon);
            else if (k == "ground_color") ReadVec3(it.value(), light.groundColor);
            else if (k == "shadow") ParseShadowType(ToLowerAscii(it.value().get<std::string>()), light.shadow);
            else if (k == "shadow_softness") light.shadowSoftness = it.value().get<float>();
            else if (k == "shadow_density") light.shadowDensity = it.value().get<float>();
            else if (k == "shadow_color") ReadVec3(it.value(), light.shadowColor);
            else if (k == "falloff") ParseFalloffType(ToLowerAscii(it.value().get<std::string>()), light.falloff);
            else if (k == "affect_diffuse") ReadBool(it.value(), light.affectDiffuse);
            else if (k == "affect_specular") ReadBool(it.value(), light.affectSpecular);
            else if (k == "viewport_visible") ReadBool(it.value(), light.viewportVisible);
        }
        if (!ApplyLightValues(base, light.v, error)) { promise->Reject(error); return true; }
        // Missing key values evaluate against the keys as they were before this call (with the new base values).
        const SceneLight sampleLight = light;

        // keys
        if (mode == "replace_all") {
            light.keys.clear();
        } else if (mode == "replace_range" && !parsedKeys.empty()) {
            int lo = std::numeric_limits<int>::max(), hi = std::numeric_limits<int>::min();
            for (const NewLightKey& k : parsedKeys) {
                lo = std::min(lo, k.frame);
                hi = std::max(hi, k.frame);
            }
            light.keys.erase(std::remove_if(light.keys.begin(), light.keys.end(),
                                            [&](const LightKey& k) { return k.frame >= lo && k.frame <= hi; }),
                             light.keys.end());
        }
        size_t changed = 0;
        for (const NewLightKey& nk : parsedKeys) {
            LightValues v = SampleLightValues(sampleLight, nk.frame);
            if (!ApplyLightValues(nk.values, v, error)) { promise->Reject(error); return true; }
            UpsertKey(light.keys, LightKey{nk.frame, v});
            ++changed;
        }

        std::vector<SceneLight> after = d.lights;
        // a delete was handled above; creation / property / key edits are all one LightsCommand over the whole list
        if (before != after) {
            d.history.Push(std::make_unique<LightsCommand>(d, "AI: studio_set_light", before, std::move(after)));
            ++d.projectVersion;
            d.rowsKey = ~0ull;
        }
        promise->Resolve({{"changed_keys", changed},
                          {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()},
                          {"index", outIndex},
                          {"uid", uid}});
        return true;
    }

    // -----------------------------------------------------------------------
    // studio_get_bone_keys
    // -----------------------------------------------------------------------
    if (boneGet) {
        const int modelIndex = args.value("model", -1);
        if (modelIndex < 0 || modelIndex >= (int)d.models.size()) {
            promise->Reject("Model index out of range: " + std::to_string(modelIndex));
            return true;
        }
        StudioModel& m = *d.models[(size_t)modelIndex];
        if (!m.pmx) {
            promise->Reject("The model has no PMX data");
            return true;
        }
        int start = args.value("start", 0);
        int end = args.contains("end") ? args["end"].get<int>() : std::numeric_limits<int>::max();
        const bool wantMorphs = args.value("morphs", false);

        std::vector<std::string> boneNames;
        if (args.contains("bones")) {
            if (!args["bones"].is_array()) {
                promise->Reject("bones must be an array of names");
                return true;
            }
            for (const nlohmann::json& b : args["bones"]) {
                if (!b.is_string()) { promise->Reject("bones must be an array of names"); return true; }
                const std::string name = b.get<std::string>();
                if (m.pmx->FindBone(name) < 0) {
                    promise->Reject("Bone not found: " + name);
                    return true;
                }
                boneNames.push_back(name);
            }
        } else {
            for (const auto& [name, keys] : m.motion.bones)
                if (!keys.empty()) boneNames.push_back(name);
        }

        size_t total = 0;
        bool truncated = false;
        nlohmann::json bones = nlohmann::json::object();
        for (const std::string& name : boneNames) {
            const auto it = m.motion.bones.find(name);
            nlohmann::json arr = nlohmann::json::array();
            if (it != m.motion.bones.end()) {
                for (const BoneKf& k : it->second) {
                    if (k.frame < start || k.frame > end) continue;
                    if (total >= kMaxKeys) { truncated = true; break; }
                    float ex, ey, ez;
                    QuatToEuler(k.r, ex, ey, ez);
                    nlohmann::json o;
                    o["frame"] = k.frame;
                    o["translate"] = Vec3Json(k.t);
                    o["rotate_deg"] = nlohmann::json::array(
                        {DirectX::XMConvertToDegrees(ex), DirectX::XMConvertToDegrees(ey), DirectX::XMConvertToDegrees(ez)});
                    o["quat"] = QuatJson(k.r);
                    o["curve"] = BoneCurveToJson(k.interp);
                    arr.push_back(std::move(o));
                    ++total;
                }
            }
            bones[name] = std::move(arr);
            if (truncated) break;
        }

        nlohmann::json morphs = nlohmann::json::object();
        if (wantMorphs && !truncated) {
            std::vector<std::string> morphNames;
            for (const auto& [name, keys] : m.motion.morphs)
                if (!keys.empty()) morphNames.push_back(name);
            for (const std::string& name : morphNames) {
                const auto it = m.motion.morphs.find(name);
                nlohmann::json arr = nlohmann::json::array();
                if (it != m.motion.morphs.end()) {
                    for (const MorphKf& k : it->second) {
                        if (k.frame < start || k.frame > end) continue;
                        if (total >= kMaxKeys) { truncated = true; break; }
                        arr.push_back({{"frame", k.frame}, {"weight", k.weight}});
                        ++total;
                    }
                }
                morphs[name] = std::move(arr);
                if (truncated) break;
            }
        }

        nlohmann::json out;
        out["bones"] = std::move(bones);
        if (wantMorphs) out["morphs"] = std::move(morphs);
        out["truncated"] = truncated;
        promise->Resolve(std::move(out));
        return true;
    }

    // -----------------------------------------------------------------------
    // studio_set_bone_keys
    // -----------------------------------------------------------------------
    if (boneSet) {
        const int modelIndex = args.value("model", -1);
        if (modelIndex < 0 || modelIndex >= (int)d.models.size()) {
            promise->Reject("Model index out of range: " + std::to_string(modelIndex));
            return true;
        }
        StudioModel& m = *d.models[(size_t)modelIndex];
        if (!m.pmx) {
            promise->Reject("The model has no PMX data");
            return true;
        }
        std::string mode, error;
        if (!ParseMode(args, mode, error)) { promise->Reject(error); return true; }
        nlohmann::json bonesJson = args.contains("bones") ? args["bones"] : nlohmann::json::object();
        nlohmann::json morphsJson = args.contains("morphs") ? args["morphs"] : nlohmann::json::object();
        if (!bonesJson.is_object() || !morphsJson.is_object()) {
            promise->Reject("bones and morphs must be objects keyed by track name");
            return true;
        }
        if (bonesJson.empty() && morphsJson.empty()) {
            promise->Reject("bones or morphs must list at least one track");
            return true;
        }

        struct NewBoneKey {
            int frame = 0;
            bool hasTranslate = false, hasRotateDeg = false, hasQuat = false;
            DirectX::XMFLOAT3 translate{};
            DirectX::XMFLOAT4 quat{0, 0, 0, 1};
            nlohmann::json curve;
        };
        std::map<std::string, std::vector<NewBoneKey>> boneKeys;
        size_t total = 0;
        for (auto ti = bonesJson.begin(); ti != bonesJson.end(); ++ti) {
            const std::string& name = ti.key();
            if (m.pmx->FindBone(name) < 0) {
                promise->Reject("Bone not found: " + name);
                return true;
            }
            if (!ti.value().is_array()) { promise->Reject("bones['" + name + "'] must be an array"); return true; }
            std::vector<NewBoneKey> list;
            for (const nlohmann::json& kj : ti.value()) {
                if (!kj.is_object()) { promise->Reject("every key must be an object"); return true; }
                NewBoneKey nk;
                if (!ReadFrame(kj.contains("frame") ? kj["frame"] : nlohmann::json(), nk.frame, error)) {
                    promise->Reject(error);
                    return true;
                }
                if (kj.contains("translate")) {
                    if (!ReadVec3(kj["translate"], nk.translate)) { promise->Reject("translate must be [x,y,z]"); return true; }
                    nk.hasTranslate = true;
                }
                if (kj.contains("rotate_deg")) {
                    float r[3];
                    if (!ReadVec3(kj["rotate_deg"], r)) { promise->Reject("rotate_deg must be [x,y,z]"); return true; }
                    nk.quat = EulerToQuat(r[0], r[1], r[2]);
                    nk.hasRotateDeg = true;
                }
                if (kj.contains("quat")) {
                    if (nk.hasRotateDeg) { promise->Reject("give rotate_deg or quat, not both"); return true; }
                    if (!kj["quat"].is_array() || kj["quat"].size() != 4) { promise->Reject("quat must be [x,y,z,w]"); return true; }
                    DirectX::XMFLOAT4 q;
                    for (int i = 0; i < 4; ++i) {
                        if (!kj["quat"][i].is_number()) { promise->Reject("quat must be [x,y,z,w]"); return true; }
                        (&q.x)[i] = kj["quat"][i].get<float>();
                    }
                    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                    if (len < 1e-6f) { promise->Reject("quat must not be zero length"); return true; }
                    q.x /= len; q.y /= len; q.z /= len; q.w /= len;
                    nk.quat = q;
                    nk.hasQuat = true;
                }
                if (kj.contains("curve")) {
                    uint8_t tmp[64];
                    FillLinearInterp(tmp);
                    if (!ApplyBoneCurve(kj["curve"], tmp, error)) { promise->Reject(error); return true; }
                    nk.curve = kj["curve"];
                }
                list.push_back(std::move(nk));
                if (++total > kMaxKeys) { promise->Reject("Too many keys (max 20000)"); return true; }
            }
            boneKeys[name] = std::move(list);
        }

        struct NewMorphKey { int frame = 0; float weight = 0; };
        std::map<std::string, std::vector<NewMorphKey>> morphKeys;
        for (auto ti = morphsJson.begin(); ti != morphsJson.end(); ++ti) {
            const std::string& name = ti.key();
            if (m.pmx->FindMorph(name) < 0) {
                promise->Reject("Morph not found: " + name);
                return true;
            }
            if (!ti.value().is_array()) { promise->Reject("morphs['" + name + "'] must be an array"); return true; }
            std::vector<NewMorphKey> list;
            for (const nlohmann::json& kj : ti.value()) {
                if (!kj.is_object()) { promise->Reject("every key must be an object"); return true; }
                NewMorphKey nk;
                if (!ReadFrame(kj.contains("frame") ? kj["frame"] : nlohmann::json(), nk.frame, error)) {
                    promise->Reject(error);
                    return true;
                }
                if (!kj.contains("weight") || !kj["weight"].is_number()) {
                    promise->Reject("morph keys require a numeric weight");
                    return true;
                }
                nk.weight = kj["weight"].get<float>();
                list.push_back(nk);
                if (++total > kMaxKeys) { promise->Reject("Too many keys (max 20000)"); return true; }
            }
            morphKeys[name] = std::move(list);
        }

        std::vector<TrackState> before;
        for (const auto& [name, list] : boneKeys) before.push_back(CaptureTrack(d, modelIndex, RowKind::Bone, name));
        for (const auto& [name, list] : morphKeys) before.push_back(CaptureTrack(d, modelIndex, RowKind::Morph, name));
        // Missing values evaluate against the tracks as they were before this call.
        std::map<std::string, std::vector<BoneKf>> originalBones;
        for (const auto& [name, list] : boneKeys) {
            const auto it = m.motion.bones.find(name);
            if (it != m.motion.bones.end()) originalBones[name] = it->second;
        }

        size_t changed = 0;
        for (auto& [name, list] : boneKeys) {
            std::vector<BoneKf>& track = m.motion.bones[name];
            if (mode == "replace_all") {
                changed += track.size();
                track.clear();
            } else if (mode == "replace_range" && !list.empty()) {
                int lo = std::numeric_limits<int>::max(), hi = std::numeric_limits<int>::min();
                for (const NewBoneKey& nk : list) {
                    lo = std::min(lo, nk.frame);
                    hi = std::max(hi, nk.frame);
                }
                const size_t oldSize = track.size();
                track.erase(std::remove_if(track.begin(), track.end(),
                                           [&](const BoneKf& k) { return k.frame >= lo && k.frame <= hi; }),
                            track.end());
                changed += oldSize - track.size();
            }
            for (const NewBoneKey& nk : list) {
                BoneKf base;
                const auto orig = originalBones.find(name);
                if (orig != originalBones.end() && !orig->second.empty()) base = SampleBone(orig->second, nk.frame);
                else { base.t = {0, 0, 0}; base.r = {0, 0, 0, 1}; }
                BoneKf k = base;
                k.frame = nk.frame;
                if (nk.hasTranslate) k.t = nk.translate;
                if (nk.hasRotateDeg || nk.hasQuat) k.r = nk.quat;
                if (!ApplyBoneCurve(nk.curve, k.interp, error)) { promise->Reject(error); return true; }
                UpsertKey(track, k);
                ++changed;
            }
        }
        for (auto& [name, list] : morphKeys) {
            std::vector<MorphKf>& track = m.motion.morphs[name];
            if (mode == "replace_all") {
                changed += track.size();
                track.clear();
            } else if (mode == "replace_range" && !list.empty()) {
                int lo = std::numeric_limits<int>::max(), hi = std::numeric_limits<int>::min();
                for (const NewMorphKey& nk : list) {
                    lo = std::min(lo, nk.frame);
                    hi = std::max(hi, nk.frame);
                }
                const size_t oldSize = track.size();
                track.erase(std::remove_if(track.begin(), track.end(),
                                           [&](const MorphKf& k) { return k.frame >= lo && k.frame <= hi; }),
                            track.end());
                changed += oldSize - track.size();
            }
            for (const NewMorphKey& nk : list) {
                UpsertKey(track, MorphKf{nk.frame, nk.weight});
                ++changed;
            }
        }

        if (changed > 0) {
            StudioPushTrackEdit("AI: studio_set_bone_keys", before);
            m.motionVersion++;  // re-bind even when the resulting track snapshot equals the old one
        }
        promise->Resolve({{"changed_keys", changed},
                          {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()}});
        return true;
    }

    // -----------------------------------------------------------------------
    // studio_delete_keys
    // -----------------------------------------------------------------------
    {
        const std::string target = ToLowerAscii(args.value("target", std::string()));
        if (!args.contains("start") || !args.contains("end") || !args["start"].is_number_integer() ||
            !args["end"].is_number_integer()) {
            promise->Reject("start and end are required integers");
            return true;
        }
        const int start = std::min(args["start"].get<int>(), args["end"].get<int>());
        const int end = std::max(args["start"].get<int>(), args["end"].get<int>());

        std::vector<TrackState> before;
        size_t changed = 0;
        auto inRange = [&](int f) { return f >= start && f <= end; };

        if (target == "camera") {
            before.push_back(CaptureTrack(d, -1, RowKind::Camera, ""));
            const size_t old = d.SelectedCameraTrack().camera.size();
            d.SelectedCameraTrack().camera.erase(std::remove_if(d.SelectedCameraTrack().camera.begin(), d.SelectedCameraTrack().camera.end(),
                                                 [&](const CameraKf& k) { return inRange(k.frame); }),
                                  d.SelectedCameraTrack().camera.end());
            changed = old - d.SelectedCameraTrack().camera.size();
        } else if (target == "light") {
            const int index = args.value("index", -1);
            if (index < 0 || index >= (int)d.lights.size()) {
                promise->Reject("Light index out of range: " + std::to_string(index));
                return true;
            }
            const std::string name = LightTrackName(d.lights[(size_t)index].uid);
            before.push_back(CaptureTrack(d, -1, RowKind::SceneLight, name));
            std::vector<LightKey>& keys = d.lights[(size_t)index].keys;
            const size_t old = keys.size();
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const LightKey& k) { return inRange(k.frame); }),
                       keys.end());
            changed = old - keys.size();
        } else if (target == "bone" || target == "morph") {
            const int modelIndex = args.value("model", -1);
            if (modelIndex < 0 || modelIndex >= (int)d.models.size()) {
                promise->Reject("Model index out of range: " + std::to_string(modelIndex));
                return true;
            }
            StudioModel& m = *d.models[(size_t)modelIndex];
            if (!m.pmx) {
                promise->Reject("The model has no PMX data");
                return true;
            }
            const bool bone = target == "bone";
            const char* listKey = bone ? "bones" : "morphs";
            std::vector<std::string> names;
            if (args.contains(listKey)) {
                if (!args[listKey].is_array()) {
                    promise->Reject(std::string(listKey) + " must be an array of names");
                    return true;
                }
                for (const nlohmann::json& n : args[listKey]) {
                    if (!n.is_string()) {
                        promise->Reject(std::string(listKey) + " must be an array of names");
                        return true;
                    }
                    names.push_back(n.get<std::string>());
                }
            } else if (bone) {
                for (const auto& [name, keys] : m.motion.bones) names.push_back(name);
            } else {
                for (const auto& [name, keys] : m.motion.morphs) names.push_back(name);
            }
            for (const std::string& name : names) {
                const bool exists = bone ? m.pmx->FindBone(name) >= 0 : m.pmx->FindMorph(name) >= 0;
                if (!exists) {
                    promise->Reject(std::string(bone ? "Bone not found: " : "Morph not found: ") + name);
                    return true;
                }
                before.push_back(CaptureTrack(d, modelIndex, bone ? RowKind::Bone : RowKind::Morph, name));
                if (bone) {
                    auto it = m.motion.bones.find(name);
                    if (it == m.motion.bones.end()) continue;
                    const size_t old = it->second.size();
                    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                                    [&](const BoneKf& k) { return inRange(k.frame); }),
                                     it->second.end());
                    changed += old - it->second.size();
                } else {
                    auto it = m.motion.morphs.find(name);
                    if (it == m.motion.morphs.end()) continue;
                    const size_t old = it->second.size();
                    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                                    [&](const MorphKf& k) { return inRange(k.frame); }),
                                     it->second.end());
                    changed += old - it->second.size();
                }
            }
        } else {
            promise->Reject("target must be camera, light, bone or morph");
            return true;
        }

        if (changed > 0) StudioPushTrackEdit("AI: studio_delete_keys", before);
        promise->Resolve({{"changed_keys", changed},
                          {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()}});
        return true;
    }
}

} // namespace mmdx
