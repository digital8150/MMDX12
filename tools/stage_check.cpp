// stage_check: validates an MMDX stage folder (stage.json + its model parts). Prints the manifest it read, every
// problem and a texture / file size budget. Exit code 0 = ok, 1 = problems, 2 = usage.
//
// Usage: stage_check <stage dir> [--skip-models]
//        stage_check --selftest
//
// Checks: the JSON schema and its semantics (asset/StageManifest.h's validator: ids, references, binding targets,
// cue tracks, pack roles, screen sources), that every declared file exists, that every model part loads through
// LoadModelFile, that material names / globs in "materials" and "screens" match a material of the parts, and that
// variants / cues only name known params. --skip-models skips the model load (fast schema check); --selftest runs
// the evaluator and validator unit checks on built-in JSON.
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/StageEval.h"
#include "asset/StageManifest.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using mmdx::StageManifest;
using mmdx::StageParam;

int g_problems = 0;

void Problem(const std::string& what) {
    std::printf("  [problem] %s\n", what.c_str());
    ++g_problems;
}

void Info(const std::string& what) { std::printf("  %s\n", what.c_str()); }

std::string Hex(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

// ---- manifest report -------------------------------------------------------------------------------------------------

void ReportManifest(const StageManifest& m) {
    const std::string lang = mmdx::StageLanguage();
    Info("id: " + m.id + "  version: " + m.version + "  name: \"" + m.name.Get(lang) + "\"");
    if (!m.minAppVersion.empty()) Info("minAppVersion: " + m.minAppVersion);
    if (!m.license.empty()) Info("license: " + m.license);
    if (!m.authors.empty()) {
        std::string a;
        for (const auto& au : m.authors) a += (a.empty() ? "" : ", ") + au.name;
        Info("authors: " + a);
    }
    if (!m.tags.empty()) {
        std::string t;
        for (const auto& tag : m.tags) t += (t.empty() ? "" : ", ") + tag;
        Info("tags: " + t);
    }
    {
        std::string parts;
        for (const auto& p : m.models) parts += (parts.empty() ? "" : ", ") + p.file + " (" + mmdx::StagePartRoleName(p.role) + ")";
        Info("models: " + parts);
    }
    Info("materials: " + std::to_string(m.materials.size()) + " rules, lights: " + std::to_string(m.lights.size()) +
         (m.gltfLights ? " (+ the parts' glTF lights: \"gltfLights\": true)" : "") +
         ", sockets: " + std::to_string(m.sockets.size()) + ", cameras: " + std::to_string(m.cameras.size()) +
         ", screens: " + std::to_string(m.screens.size()) + ", packs: " + std::to_string(m.packs.size()));
    Info("params: " + std::to_string(m.params.size()) + ", variants: " + std::to_string(m.variants.size()) +
         ", bindings: " + std::to_string(m.bindings.size()) + ", cue tracks: " + std::to_string(m.cues.size()));
    for (const auto& p : m.models) {
        std::string field;
        for (const auto& l : m.lights)
            if (l.name == p.name) field = " (same name as a light)";
        (void)field;
    }
    for (const auto& v : m.variants)
        Info("variant: " + v.id + " \"" + v.label.Get(lang) + "\" (" + std::to_string(v.values.size()) + " values)" +
             ((!m.defaultVariant.empty() && m.defaultVariant == v.id) ? " [default]" : ""));
    if (m.defaultVariant.empty() && !m.variants.empty()) Info("variant: the first one is the default");
}

// ---- model loading + material names ----------------------------------------------------------------------------------

struct LoadedParts {
    bool ok = true;
    std::set<std::string> materials;   // exact names of every part
    uint64_t vertices = 0, indices = 0;
};

LoadedParts LoadParts(const StageManifest& m, bool load) {
    LoadedParts out;
    for (const auto& part : m.models) {
        const fs::path path = (m.dir / mmdx::Utf8ToPath(part.file)).lexically_normal();
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            Problem("models: " + part.file + ": missing file");
            out.ok = false;
            continue;
        }
        if (!load) continue;
        mmdx::PmxModel model;
        std::string err;
        if (!mmdx::LoadModelFile(path, mmdx::ModelRole::Stage, model, &err)) {
            Problem("models: " + part.file + ": does not load: " + err);
            out.ok = false;
            continue;
        }
        out.vertices += model.vertices.size();
        out.indices += model.indices.size();
        for (const auto& mat : model.materials) out.materials.insert(mat.name);
        Info("model: " + part.file + " -> " + std::to_string(model.vertices.size()) + " vertices, " +
             std::to_string(model.materials.size()) + " materials");
        // S2a: KHR_lights_punctual. The manifest takes them as part of the rig only with "gltfLights": true, so the
        // report says whether they are used or ignored.
        if (!model.importedLights.empty())
            Info("glTF lights: " + std::to_string(model.importedLights.size()) + " in " + part.file +
                 (m.gltfLights ? " (in the rig: \"gltfLights\": true)" : " (ignored: no \"gltfLights\")"));
        if (model.vertices.empty() || model.indices.empty()) {
            Problem("models: " + part.file + ": no geometry");
            out.ok = false;
        }
    }
    return out;
}

// Every "materials" / "screens" pattern that matches no part material is a problem: the override would do nothing.
void CheckMaterialNames(const StageManifest& m, const std::set<std::string>& materials) {
    auto anyMatch = [&](const std::string& pattern) {
        if (mmdx::StageGlobIsPattern(pattern)) {
            for (const auto& n : materials)
                if (mmdx::StageGlobMatch(pattern, n)) return true;
            return false;
        }
        return materials.count(pattern) > 0;
    };
    for (const auto& r : m.materials)
        if (!anyMatch(r.match)) Problem("materials: \"" + r.match + "\" matches no material of the parts");
    for (const auto& s : m.screens)
        for (const auto& pattern : s.materials)
            if (!anyMatch(pattern)) Problem("screens: " + s.id + ": \"" + pattern + "\" matches no material of the parts");
    for (const auto& p : m.packs)
        if (p.role == mmdx::StagePackRole::Surface)
            for (const auto& pattern : p.materials)
                if (!anyMatch(pattern)) Problem("packs: " + p.folder + ": \"" + pattern + "\" matches no material of the parts");
}

// ---- files / budget --------------------------------------------------------------------------------------------------

void CheckFiles(const StageManifest& m) {
    // declared files exist
    for (const auto& s : m.screens)
        if (s.source == mmdx::StageScreenSource::Image || s.source == mmdx::StageScreenSource::Video) {
            std::error_code ec;
            if (!fs::is_regular_file(m.dir / mmdx::Utf8ToPath(s.file), ec))
                Problem("screens: " + s.id + ": missing file " + s.file);
        }
    for (const auto& p : m.packs) {
        std::error_code ec;
        if (!fs::is_regular_file(m.dir / mmdx::Utf8ToPath(p.folder) / L"pack.json", ec))
            Problem("packs: " + p.folder + ": no pack.json");
    }
    // S7b: a light's gobo must be a bundled image (png / jpg; the renderer decodes anything, the format rule keeps a
    // stage's assets reviewable).
    for (const auto& l : m.lights) {
        if (l.gobo.empty()) continue;
        const fs::path file = m.dir / mmdx::Utf8ToPath(l.gobo);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            Problem("lights: " + l.name + ": missing gobo file " + l.gobo);
            continue;
        }
        const std::string ext = mmdx::ToLowerAscii(mmdx::PathToUtf8(file.extension()));
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
            Problem("lights: " + l.name + ": gobo file must be a .png / .jpg image (" + l.gobo + ")");
    }
    // S9b-3: a sky light's image must be a bundled panorama (an HDRI, an 8-bit image or a DDS cubemap). The encoding is
    // validated by the manifest.
    for (const auto& l : m.lights) {
        if (l.skyMap.empty()) continue;
        const fs::path file = m.dir / mmdx::Utf8ToPath(l.skyMap);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            Problem("lights: " + l.name + ": missing sky map file " + l.skyMap);
            continue;
        }
        const std::string ext = mmdx::ToLowerAscii(mmdx::PathToUtf8(file.extension()));
        if (ext != ".hdr" && ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".dds")
            Problem("lights: " + l.name + ": sky map file must be a .hdr / .png / .jpg / .dds image (" + l.skyMap + ")");
    }
    // budget: every file under the folder, grouped by extension
    std::map<std::string, std::pair<uint64_t, uint32_t>> byExt;   // ext -> (bytes, count)
    uint64_t total = 0;
    uint32_t files = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(m.dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        const std::string ext = mmdx::ToLowerAscii(mmdx::PathToUtf8(it->path().extension()));
        const uint64_t size = (uint64_t)fs::file_size(it->path(), e2);
        if (e2) continue;
        auto& slot = byExt[ext.empty() ? "(none)" : ext];
        slot.first += size;
        ++slot.second;
        total += size;
        ++files;
    }
    char buf[128];
    for (const auto& [ext, v] : byExt) {
        std::snprintf(buf, sizeof(buf), "%.1f MB in %u file(s)", v.first / (1024.0 * 1024.0), v.second);
        Info("files " + ext + ": " + buf);
    }
    std::snprintf(buf, sizeof(buf), "%.1f MB in %u file(s)", total / (1024.0 * 1024.0), files);
    Info("total: " + std::string(buf));
    // The gallery's zip budget (docs/stage_ecosystem.md S12 mirrors the shader pack rules): warn well before it.
    if (total > 512ull * 1024 * 1024) Problem("total size " + std::string(buf) + " is over 512 MB");
    else if (total > 256ull * 1024 * 1024) Info("note: over 256 MB, downloads get slow");
}

// ---- self test -------------------------------------------------------------------------------------------------------

int SelfTest() {
    int failures = 0;
    auto check = [&](bool ok, const char* what, const std::string& detail = {}) {
        std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
        if (!ok) ++failures;
    };

    const std::string json = R"({
      "format": "mmdx-stage", "formatVersion": 1,
      "id": "selftest", "version": "1.0.0", "name": {"ko": "셀프 테스트", "en": "Self test"},
      "models": [{"file": "room.glb", "role": "floor"}],
      "materials": [{"match": "cove*", "baseColor": [0.5, 0.6, 0.7, 1.0], "roughness": 0.4},
                    {"match": "glass", "transmission": 1.0, "ior": 1.5}],
      "lights": [
        {"kind": "sun", "name": "key", "values": {"direction": [-0.3, -1, 0.2], "color": [1, 0.9, 0.8], "intensity": 1.2}},
        {"kind": "spot", "name": "fill", "values": {"position": [0, 40, 0], "aim": [0, 0, 0], "intensity": 2.0, "coneOuter": 0.3, "coneInner": 0.2}},
        {"kind": "spot", "name": "up", "values": {"intensity": 1.0}}],
      "sockets": [{"name": "world", "position": [0, 0, 0]}, {"name": "node_socket", "node": "Box"}],
      "cameras": [{"name": "wide", "position": [0, 20, -60], "target": [0, 10, 0], "fov": 40}],
      "screens": [{"id": "led", "materials": ["led*"], "source": "image", "file": "led.png", "width": 512, "height": 256}],
      "environment": {"fog": {"density": 0.03, "color": [0.5, 0.6, 0.7], "anisotropy": 0.5},
                      "fogVolumes": [{"id": "beam", "shape": "box", "position": [0, 20, 0],
                                      "extent": [30, 20, 30], "density": 2.0, "color": [0.8, 0.9, 1.0], "edge": 0.4},
                                     {"shape": "sphere", "extent": [20, 20, 20], "density": 0.5}]},
      "packs": [{"folder": "packs/floor", "role": "surface", "materials": ["cove*"], "params": {"tint": 0.5}},
                {"folder": "packs/haze", "role": "world", "socket": "world"}],
      "groups": [{"id": "look", "label": {"en": "Look"}}],
      "params": [
        {"key": "coveColor", "type": "color", "label": {"en": "Cove colour"}, "group": "look", "default": [0.5, 0.6, 0.7, 1.0]},
        {"key": "keyIntensity", "type": "float", "default": 1.0, "min": 0.0, "max": 4.0},
        {"key": "fillLevel", "type": "float", "default": 0.5, "min": 0.0, "max": 1.0},
        {"key": "mode", "type": "enum", "default": "warm",
         "options": [{"value": "warm", "label": {"en": "Warm"}}, {"value": "cool"}]}],
      "variants": [{"id": "studio", "label": {"en": "Studio"}, "values": {"coveColor": [0.8, 0.8, 0.82, 1.0]}},
                   {"id": "night", "values": {"coveColor": [0.05, 0.06, 0.09, 1.0]}}],
      "defaultVariant": "studio",
      "bindings": [
        {"param": "coveColor", "target": "materials.cove*.baseColor"},
        {"param": "keyIntensity", "target": "lights.key.intensity"},
        {"param": "fillLevel", "target": "lights.fill.intensity", "scale": 2.0, "offset": 0.25},
        {"param": "fillLevel", "target": "lights.up.intensity", "remap": [0.0, 1.0, 4.0, 1.0]}],
      "cues": [{"param": "keyIntensity", "keys": [{"frame": 0, "value": 1.0}, {"frame": 60, "value": 3.0}]}]
    })";
    StageManifest m;
    std::string error;
    const bool parsed = mmdx::ParseStageManifest(json, fs::path("selftest"), m, error);
    check(parsed, "manifest parses", error);
    const auto errors = mmdx::ValidateStageManifest(m);
    check(errors.empty(), "manifest validates", errors.empty() ? "" : errors.front());

    // glob
    check(mmdx::StageGlobMatch("cove*", "Cove_Center"), "glob: case-insensitive prefix");
    check(!mmdx::StageGlobMatch("cove", "cove_wall"), "glob: exact pattern does not match a longer name");
    check(mmdx::StageGlobMatch("*.*", "a.b"), "glob: two wildcards");
    check(mmdx::StageGlobMatch("Led?1", "led01"), "glob: '?' matches one character");
    check(mmdx::StageGlobMatch("*", ""), "glob: '*' matches the empty string");

    // defaults + default variant
    auto r = mmdx::EvaluateStage(m, "", {}, 0.0);
    check(r.warnings.empty(), "no evaluator warnings", r.warnings.empty() ? "" : r.warnings.front());
    const auto& defColor = r.params.at("coveColor");
    check(defColor.type == mmdx::StageValue::Type::Color && defColor.x == 0.8f && defColor.z == 0.82f,
          "variant overrides the default colour");
    // cue at 0 s = frame 0 = 1.0, so keyIntensity stays 1.0
    check(r.params.at("keyIntensity").x == 1.0f, "cue at t=0");
    auto r2 = mmdx::EvaluateStage(m, "", {}, 1.0);   // frame 30: half way 1 -> 3
    check(std::abs(r2.params.at("keyIntensity").x - 2.0f) < 1e-4f, "cue is linear in time", Hex(r2.params.at("keyIntensity").x));
    auto r3 = mmdx::EvaluateStage(m, "night", {}, 100.0);
    check(r3.params.at("coveColor").x == 0.05f, "chosen variant");
    // bindings in order; scale / offset; remap
    const auto findTarget = [](const mmdx::StageEvalResult& res, const std::string& target) -> const mmdx::StageAssignment* {
        for (const auto& a : res.targets)
            if (a.target == target) return &a;
        return nullptr;
    };
    const auto* col = findTarget(r, "materials.cove*.baseColor");
    check(col && col->value.type == mmdx::StageValue::Type::Color && col->value.x == 0.8f, "colour assignment");
    const auto* key = findTarget(r, "lights.key.intensity");
    check(key && key->value.x == 1.0f, "light intensity assignment");
    const auto* fill = findTarget(r, "lights.fill.intensity");
    check(fill && std::abs(fill->value.x - 1.25f) < 1e-4f, "scale + offset", fill ? Hex(fill->value.x) : "");
    const auto* up = findTarget(r, "lights.up.intensity");
    check(up && std::abs(up->value.x - 2.5f) < 1e-4f, "remap", up ? Hex(up->value.x) : "");
    check(r.targets.size() == 4, "every binding produced an assignment");
    // overrides beat cues
    mmdx::StageParamValues overrides;
    overrides["keyIntensity"] = mmdx::StageValue::Number(4.0f);
    auto r4 = mmdx::EvaluateStage(m, "", overrides, 1.0);
    check(r4.params.at("keyIntensity").x == 4.0f, "overrides beat cues");
    // clamps
    overrides["keyIntensity"] = mmdx::StageValue::Number(99.0f);
    check(mmdx::EvaluateStage(m, "", overrides, 0.0).params.at("keyIntensity").x == 4.0f, "overrides clamp to max");
    // unknown references warn, never crash
    mmdx::StageParamValues bad;
    bad["nope"] = mmdx::StageValue::Number(1.0f);
    auto r5 = mmdx::EvaluateStage(m, "nosuchvariant", bad, 0.0);
    check(r5.warnings.size() == 2, "unknown override + unknown variant warn", std::to_string(r5.warnings.size()));
    check(r5.params.at("coveColor").x == 0.5f, "an unknown variant falls back to the defaults");

    // S7b: a gobo target nests one level ("lights.<name>.gobo.<field>"): owner plus field split at the last dot
    {
        const std::string goboJson = R"({
          "format": "mmdx-stage", "formatVersion": 1, "id": "gobotest", "version": "1", "name": "Gobo test",
          "models": [{"file": "x.glb"}],
          "lights": [{"kind": "spot", "name": "ring",
                      "values": {"position": [0, 100, 0], "aim": [0, 0, 0]},
                      "gobo": {"file": "g/ring.png", "rotation": 10, "scale": 1.5, "spin": 30, "focus": 0.5}}],
          "params": [{"key": "spin", "type": "float", "default": 45.0, "min": 0.0, "max": 180.0}],
          "bindings": [{"param": "spin", "target": "lights.ring.gobo.spin"}]
        })";
        StageManifest gm;
        check(mmdx::ParseStageManifest(goboJson, fs::path("gobotest"), gm, error), "gobo manifest parses", error);
        check(mmdx::ValidateStageManifest(gm).empty(), "gobo manifest validates",
              mmdx::ValidateStageManifest(gm).empty() ? "" : mmdx::ValidateStageManifest(gm).front());
        check(gm.lights.size() == 1 && gm.lights[0].gobo == "g/ring.png" && gm.lights[0].goboSpin == 30.0f &&
                  gm.lights[0].goboFocus == 0.5f,
              "gobo block parsed");
        const mmdx::StageTargetSpec gs = mmdx::ResolveStageTarget(gm, "lights.ring.gobo.spin");
        check(gs.ok && gs.owner == "ring" && gs.field == "gobo.spin" && gs.type == mmdx::StageTargetSpec::ValueType::Float,
              "gobo target splits owner / field");
        check(!mmdx::ResolveStageTarget(gm, "lights.ring.gobo.nope").ok, "unknown gobo field rejected");
        const auto gr = mmdx::EvaluateStage(gm, "", {}, 0.0);
        const auto* spin = findTarget(gr, "lights.ring.gobo.spin");
        check(spin && std::abs(spin->value.x - 45.0f) < 1e-4f, "gobo spin binding assignment");
    }

    // S9b-1: a sky light's values and its nested targets ("lights.<name>.sky.<field>"), and the ranges stage_check adds
    {
        const std::string skyJson = R"({
          "format": "mmdx-stage", "formatVersion": 1, "id": "skytest", "version": "1", "name": "Sky test",
          "models": [{"file": "x.glb"}],
          "lights": [{"kind": "sky", "name": "sky",
                      "values": {"skyTimeOfDay": 6.5, "skyTurbidity": 4.5, "skyHaze": 0.4, "skyNorthDeg": -20.0,
                                 "skyGround": [0.3, 0.3, 0.28], "skyIntensity": 1.2, "skySunIntensity": 0.9,
                                 "skyMaxElevationDeg": 55.0}}],
          "params": [{"key": "tod", "type": "float", "default": 12.0, "min": 0.0, "max": 24.0}],
          "bindings": [{"param": "tod", "target": "lights.sky.sky.timeOfDay"}]
        })";
        StageManifest sm;
        check(mmdx::ParseStageManifest(skyJson, fs::path("skytest"), sm, error), "sky manifest parses", error);
        check(mmdx::ValidateStageManifest(sm).empty(), "sky manifest validates",
              mmdx::ValidateStageManifest(sm).empty() ? "" : mmdx::ValidateStageManifest(sm).front());
        check(sm.lights.size() == 1 && sm.lights[0].kind == mmdx::studio::LightKind::Sky &&
                  sm.lights[0].v.skyTimeOfDay == 6.5f && sm.lights[0].v.skyTurbidity == 4.5f &&
                  sm.lights[0].v.skyHaze == 0.4f && sm.lights[0].v.skySunIntensity == 0.9f,
              "sky values parsed");
        const mmdx::StageTargetSpec ss = mmdx::ResolveStageTarget(sm, "lights.sky.sky.timeOfDay");
        check(ss.ok && ss.owner == "sky" && ss.field == "sky.timeOfDay" &&
                  ss.type == mmdx::StageTargetSpec::ValueType::Float,
              "sky target splits owner / field");
        check(!mmdx::ResolveStageTarget(sm, "lights.sky.sky.nope").ok, "unknown sky field rejected");
        mmdx::StageManifest badSky = sm;
        badSky.lights[0].v.skyTurbidity = 30.0f;
        check(!mmdx::ValidateStageManifest(badSky).empty(), "turbidity out of range rejected");
        mmdx::StageManifest twoSkies = sm;
        twoSkies.lights.push_back(sm.lights[0]);
        check(!mmdx::ValidateStageManifest(twoSkies).empty(), "two sky lights rejected");
    }

    // S9b-3: a sky light's image (base keys beside the light, its two values in "values") and its nested targets
    {
        const std::string skyMapJson = R"({
          "format": "mmdx-stage", "formatVersion": 1, "id": "skymaptest", "version": "1", "name": "Sky map test",
          "models": [{"file": "x.glb"}],
          "lights": [{"kind": "sky", "name": "sky", "skyMap": "sky/test_sky.hdr", "skyMapEncoding": "rgbt",
                      "skyMapMirror": true,
                      "values": {"skySunIntensity": 0.0, "skyMapRotationDeg": 15.0, "skyMapIntensity": 2.0}}],
          "params": [{"key": "rot", "type": "float", "default": 30.0, "min": -180.0, "max": 180.0}],
          "bindings": [{"param": "rot", "target": "lights.sky.sky.mapRotation"}]
        })";
        StageManifest mm;
        check(mmdx::ParseStageManifest(skyMapJson, fs::path("skymaptest"), mm, error), "sky map manifest parses", error);
        check(mmdx::ValidateStageManifest(mm).empty(), "sky map manifest validates",
              mmdx::ValidateStageManifest(mm).empty() ? "" : mmdx::ValidateStageManifest(mm).front());
        check(mm.lights.size() == 1 && mm.lights[0].skyMap == "sky/test_sky.hdr" &&
                  mm.lights[0].skyMapEncoding == "rgbt" && mm.lights[0].skyMapMirror &&
                  mm.lights[0].v.skyMapIntensity == 2.0f && mm.lights[0].v.skyMapRotationDeg == 15.0f,
              "sky map keys parsed");
        const mmdx::StageTargetSpec ms = mmdx::ResolveStageTarget(mm, "lights.sky.sky.mapRotation");
        check(ms.ok && ms.owner == "sky" && ms.field == "sky.mapRotation" &&
                  ms.type == mmdx::StageTargetSpec::ValueType::Float,
              "sky map target splits owner / field");
        check(!mmdx::ResolveStageTarget(mm, "lights.sky.sky.mapNope").ok, "unknown sky map field rejected");
        mmdx::StageManifest badEnc = mm;
        badEnc.lights[0].skyMapEncoding = "hdri";
        check(!mmdx::ValidateStageManifest(badEnc).empty(), "an unknown sky map encoding rejected");
        mmdx::StageManifest badMap = mm;
        badMap.lights[0].skyMap = "../escape.hdr";
        check(!mmdx::ValidateStageManifest(badMap).empty(), "a sky map path outside the stage rejected");
        mmdx::StageManifest badMapInt = mm;
        badMapInt.lights[0].v.skyMapIntensity = -1.0f;
        check(!mmdx::ValidateStageManifest(badMapInt).empty(), "a negative sky map intensity rejected");
    }

    // S9b-4: the cloud layer on the physical sky (its seven values in "values") and its nested targets
    {
        const std::string cloudJson = R"({
          "format": "mmdx-stage", "formatVersion": 1, "id": "cloudtest", "version": "1", "name": "Cloud test",
          "models": [{"file": "x.glb"}],
          "lights": [{"kind": "sky", "name": "sky",
                      "values": {"cloudCoverage": 0.5, "cloudOpacity": 0.8, "cloudSoftness": 0.3,
                                 "cloudHeightKm": 3.0, "cloudScaleKm": 6.0, "cloudWindDeg": 90.0,
                                 "cloudWindSpeed": 25.0}}],
          "params": [{"key": "cov", "type": "float", "default": 0.4, "min": 0.0, "max": 1.0}],
          "bindings": [{"param": "cov", "target": "lights.sky.sky.cloudCoverage"}]
        })";
        StageManifest cm;
        check(mmdx::ParseStageManifest(cloudJson, fs::path("cloudtest"), cm, error), "cloud manifest parses", error);
        check(mmdx::ValidateStageManifest(cm).empty(), "cloud manifest validates",
              mmdx::ValidateStageManifest(cm).empty() ? "" : mmdx::ValidateStageManifest(cm).front());
        check(cm.lights.size() == 1 && cm.lights[0].kind == mmdx::studio::LightKind::Sky &&
                  cm.lights[0].v.cloudCoverage == 0.5f && cm.lights[0].v.cloudOpacity == 0.8f &&
                  cm.lights[0].v.cloudSoftness == 0.3f && cm.lights[0].v.cloudHeightKm == 3.0f &&
                  cm.lights[0].v.cloudScaleKm == 6.0f && cm.lights[0].v.cloudWindDeg == 90.0f &&
                  cm.lights[0].v.cloudWindSpeed == 25.0f,
              "cloud values parsed");
        const mmdx::StageTargetSpec cs = mmdx::ResolveStageTarget(cm, "lights.sky.sky.cloudCoverage");
        check(cs.ok && cs.owner == "sky" && cs.field == "sky.cloudCoverage" &&
                  cs.type == mmdx::StageTargetSpec::ValueType::Float,
              "cloud target splits owner / field");
        check(!mmdx::ResolveStageTarget(cm, "lights.sky.sky.cloudNope").ok, "unknown cloud field rejected");
        mmdx::StageManifest badCov = cm;
        badCov.lights[0].v.cloudCoverage = 1.5f;
        check(!mmdx::ValidateStageManifest(badCov).empty(), "cloud coverage above 1 rejected");
        mmdx::StageManifest badSpeed = cm;
        badSpeed.lights[0].v.cloudWindSpeed = 200.0f;
        check(!mmdx::ValidateStageManifest(badSpeed).empty(), "a cloud wind speed above 100 rejected");
    }

    // enum -> screen source (string target)
    const std::string enumJson = R"({
      "format": "mmdx-stage", "formatVersion": 1, "id": "enumtest", "version": "1", "name": "Enum test",
      "models": [{"file": "x.glb"}],
      "screens": [{"id": "led", "materials": ["led"], "source": "none"}],
      "params": [{"key": "src", "type": "enum", "default": "image",
                  "options": [{"value": "none"}, {"value": "image"}, {"value": "video"}]}],
      "bindings": [{"param": "src", "target": "screens.led.source"}]
    })";
    StageManifest em;
    check(mmdx::ParseStageManifest(enumJson, fs::path("enumtest"), em, error), "enum manifest parses", error);
    check(mmdx::ValidateStageManifest(em).empty(), "enum manifest validates");
    auto er = mmdx::EvaluateStage(em, "", {}, 0.0);
    const auto* src = findTarget(er, "screens.led.source");
    check(src && src->value.type == mmdx::StageValue::Type::Text && src->value.text == "image",
          "enum -> string target uses the option value", src ? src->value.text : "(none)");

    // S2a: the "gltfLights" flag is parsed (off by default) and a stage that opts in keeps validating unchanged.
    {
        StageManifest on;
        std::string onError;
        std::string json2 = json;
        json2.insert(json2.find("\"models\""), "\"gltfLights\": true, ");
        check(mmdx::ParseStageManifest(json2, fs::path("selftest"), on, onError), "gltfLights parses", onError);
        check(on.gltfLights, "gltfLights: true is read");
        check(!m.gltfLights, "gltfLights defaults to off");
        check(mmdx::ValidateStageManifest(on).empty(), "gltfLights: true validates");
    }

    // L0 drivers: the shapes, blends, amount, clamp, and audio drivers skipped without signals
    {
        const std::string driverJson = R"({
          "format": "mmdx-stage", "formatVersion": 1, "id": "drivertest", "version": "1", "name": "Driver test",
          "models": [{"file": "x.glb"}],
          "params": [
            {"key": "beam", "type": "float", "default": 0.8, "min": 0.0, "max": 1.0},
            {"key": "glow", "type": "float", "default": 2.0, "min": 0.0, "max": 10.0},
            {"key": "spin", "type": "float", "default": 0.0, "min": -100.0, "max": 100.0},
            {"key": "tint", "type": "color", "default": [0.5, 0.4, 1.0, 0.7]},
            {"key": "react", "type": "float", "default": 0.5, "min": 0.0, "max": 1.0},
            {"key": "steps", "type": "int", "default": 2, "min": 0, "max": 4}],
          "drivers": [
            {"param": "beam", "input": "audio.beat", "shape": "pulse", "power": 2, "min": 0.25, "max": 1.0},
            {"param": "glow", "input": "time", "shape": "sine", "every": 2.0, "min": 1.0, "max": 3.0, "blend": "set"},
            {"param": "spin", "input": "time", "shape": "ramp", "every": 0.5, "min": 0.0, "max": 10.0, "blend": "add"},
            {"param": "tint", "input": "audio.energy", "min": 0.0, "max": 1.0, "blend": "mul", "amount": "react"},
            {"param": "steps", "input": "time", "shape": "saw", "every": 1.0, "min": 0.0, "max": 10.0, "blend": "set"}]
        })";
        StageManifest dm;
        check(mmdx::ParseStageManifest(driverJson, fs::path("drivertest"), dm, error), "driver manifest parses", error);
        const auto dErrors = mmdx::ValidateStageManifest(dm);
        check(dErrors.empty(), "driver manifest validates", dErrors.empty() ? "" : dErrors.front());
        check(dm.drivers.size() == 5 && dm.drivers[0].hasPower && !dm.drivers[3].hasPower &&
                  dm.drivers[1].shape == mmdx::StageDriverShape::Sine && dm.drivers[3].blend == mmdx::StageDriverBlend::Mul,
              "drivers parsed");
        check(mmdx::StageUsesAudio(dm), "StageUsesAudio: an audio.* driver");
        const auto approx = [](float a, float b) { return std::abs(a - b) < 1e-4f; };

        mmdx::StageSignals sig;
        sig.audioValid = true;
        sig.beatCount = 3.5;   // half way through a beat: pulse = (1 - 0.5)^2 = 0.25
        sig.energy = 0.5f;
        // t = 0.5 s: sine every 2 s -> f = 0.25 -> 0.5; ramp every 0.5 s -> u = 1 -> 10 added; saw f = 0.5 -> 5
        auto dr = mmdx::EvaluateStage(dm, "", {}, 0.5, {}, &sig);
        check(approx(dr.params.at("beam").x, 0.25f + 0.75f * 0.25f), "pulse driver", Hex(dr.params.at("beam").x));
        check(approx(dr.params.at("glow").x, 2.0f), "sine driver (set)", Hex(dr.params.at("glow").x));
        check(approx(dr.params.at("spin").x, 10.0f), "ramp driver (add, unwrapped)", Hex(dr.params.at("spin").x));
        check(dr.params.at("steps").x == 4.0f, "int driver clamps and rounds", Hex(dr.params.at("steps").x));
        // colour mul at energy 0.5, amount react 0.5: rgb * lerp(1, 0.5, 0.5) = rgb * 0.75, alpha kept
        const auto& tint = dr.params.at("tint");
        check(approx(tint.x, 0.375f) && approx(tint.y, 0.3f) && approx(tint.z, 0.75f) && approx(tint.w, 0.7f),
              "colour mul with amount", Hex(tint.x) + " " + Hex(tint.w));
        // the amount param's own override is read before the drivers
        mmdx::StageParamValues full;
        full["react"] = mmdx::StageValue::Number(1.0f);
        auto dr2 = mmdx::EvaluateStage(dm, "", full, 0.5, {}, &sig);
        check(approx(dr2.params.at("tint").x, 0.25f), "amount 1 = the full driven value", Hex(dr2.params.at("tint").x));
        // no signals: audio drivers keep the base, time drivers still apply
        auto dr3 = mmdx::EvaluateStage(dm, "", {}, 0.5);
        check(approx(dr3.params.at("beam").x, 0.8f) && approx(dr3.params.at("tint").x, 0.5f),
              "audio drivers skipped without signals", Hex(dr3.params.at("beam").x));
        check(approx(dr3.params.at("glow").x, 2.0f), "time drivers apply without signals");
        mmdx::StageSignals invalid = sig;
        invalid.audioValid = false;
        check(approx(mmdx::EvaluateStage(dm, "", {}, 0.5, {}, &invalid).params.at("beam").x, 0.8f),
              "audio drivers skipped while the analysis is not valid");
        // a driver's result clamps to the param's range (spin: ramp at t = 30 s adds 600 -> 100)
        check(approx(mmdx::EvaluateStage(dm, "", {}, 30.0, {}, &sig).params.at("spin").x, 100.0f), "driver clamps to max");
        // a negative beat count (before the first beat) still wraps into the cycle
        sig.beatCount = -0.25;   // f = 0.75 -> pulse (0.25)^2
        check(approx(mmdx::EvaluateStage(dm, "", {}, 0.5, {}, &sig).params.at("beam").x, 0.25f + 0.75f * 0.0625f),
              "pulse before the first beat");
        // square + duty, level power
        mmdx::StageDriver sq;
        sq.input = mmdx::StageDriverInput::Bar;
        sq.shape = mmdx::StageDriverShape::Square;
        sq.duty = 0.25f;
        sig.barCount = 2.2;
        double sv = -1;
        check(mmdx::SampleStageDriver(sq, 0.0, &sig, sv) && sv == 1.0, "square on within the duty");
        sig.barCount = 2.3;
        check(mmdx::SampleStageDriver(sq, 0.0, &sig, sv) && sv == 0.0, "square off after the duty");
        mmdx::StageDriver lvl;
        lvl.input = mmdx::StageDriverInput::Onset;
        sig.onset = 0.5f;
        check(mmdx::SampleStageDriver(lvl, 0.0, &sig, sv) && approx((float)sv, 0.5f), "level input: power 1 by default");
        lvl.hasPower = true;
        lvl.power = 2.0f;
        check(mmdx::SampleStageDriver(lvl, 0.0, &sig, sv) && approx((float)sv, 0.25f), "level input: explicit power");

        // validation
        const auto rejects = [&](const char* what, auto mutate) {
            StageManifest b = dm;
            mutate(b);
            check(!mmdx::ValidateStageManifest(b).empty(), what);
        };
        rejects("driver: unknown param", [](StageManifest& b) { b.drivers[0].param = "nope"; });
        rejects("driver: colour with set", [](StageManifest& b) { b.drivers[3].blend = mmdx::StageDriverBlend::Set; });
        rejects("driver: colour with add", [](StageManifest& b) { b.drivers[3].blend = mmdx::StageDriverBlend::Add; });
        rejects("driver: every <= 0", [](StageManifest& b) { b.drivers[1].every = 0.0f; });
        rejects("driver: power <= 0", [](StageManifest& b) { b.drivers[0].power = -1.0f; });
        rejects("driver: unknown amount param", [](StageManifest& b) { b.drivers[3].amount = "nope"; });
        rejects("driver: amount param driven", [](StageManifest& b) { b.drivers[3].amount = "beam"; });
        rejects("driver: bool param", [](StageManifest& b) {
            StageParam p;
            p.key = "flag";
            p.type = mmdx::StageParamType::Bool;
            b.params.push_back(p);
            b.drivers[0].param = "flag";
        });
        StageManifest pm;
        std::string perr;
        std::string badJson = driverJson;
        badJson.replace(badJson.find("\"shape\": \"pulse\""), 16, "\"shape\": \"blip\"");
        check(!mmdx::ParseStageManifest(badJson, fs::path("drivertest"), pm, perr), "driver: unknown shape is a parse problem",
              perr);
        badJson = driverJson;
        badJson.replace(badJson.find("\"every\": 2.0"), 12, "\"every\": \"2\"");
        check(!mmdx::ParseStageManifest(badJson, fs::path("drivertest"), pm, perr), "driver: a wrong-typed field is a parse problem",
              perr);
        badJson = driverJson;
        badJson.replace(badJson.find("\"input\": \"audio.beat\""), 21, "\"input\": \"audio.bass\"");
        check(!mmdx::ParseStageManifest(badJson, fs::path("drivertest"), pm, perr), "driver: unknown input is a parse problem",
              perr);
        check(!mmdx::StageUsesAudio(m), "StageUsesAudio: no drivers");
    }

    // binding gain: out = (remap(value) * scale + offset) * the gain param's value after drivers
    {
        const std::string gainJson = R"({
          "format": "mmdx-stage", "formatVersion": 1, "id": "gaintest", "version": "1", "name": "Gain test",
          "models": [{"file": "x.glb"}],
          "lights": [{"kind": "spot", "name": "beam_01", "values": {"intensity": 1.0, "color": [1, 1, 1]}}],
          "params": [
            {"key": "beamChase1", "type": "float", "default": 0.5, "min": 0.0, "max": 1.0},
            {"key": "beamLevel", "type": "float", "default": 1.0, "min": 0.0, "max": 1.0},
            {"key": "beamTint", "type": "color", "default": [0.5, 1.0, 0.25, 0.6]},
            {"key": "on", "type": "bool", "default": true}],
          "drivers": [{"param": "beamLevel", "input": "time", "shape": "saw", "every": 1.0, "min": 0.0, "max": 1.0}],
          "bindings": [
            {"param": "beamChase1", "target": "lights.beam_01.intensity", "scale": 3.6, "gain": "beamLevel"},
            {"param": "beamTint", "target": "lights.beam_01.color", "gain": "beamLevel"},
            {"param": "on", "target": "lights.beam_01.enabled"}]
        })";
        StageManifest gm;
        check(mmdx::ParseStageManifest(gainJson, fs::path("gaintest"), gm, error), "gain manifest parses", error);
        const auto gErrors = mmdx::ValidateStageManifest(gm);
        check(gErrors.empty(), "gain manifest validates", gErrors.empty() ? "" : gErrors.front());
        check(gm.bindings.size() == 3 && gm.bindings[0].gain == "beamLevel" && gm.bindings[2].gain.empty(),
              "gain parsed");
        // t = 0.25 s: saw -> beamLevel 0.25; intensity 0.5 * 3.6 * 0.25 = 0.45; colour rgb * 0.25, alpha kept
        const auto gr = mmdx::EvaluateStage(gm, "", {}, 0.25);
        check(gr.warnings.empty(), "gain: no warnings", gr.warnings.empty() ? "" : gr.warnings.front());
        const auto* gi = findTarget(gr, "lights.beam_01.intensity");
        check(gi && std::abs(gi->value.x - 0.45f) < 1e-4f, "gain on a number target (driven gain)",
              gi ? Hex(gi->value.x) : "");
        const auto* gc = findTarget(gr, "lights.beam_01.color");
        check(gc && std::abs(gc->value.x - 0.125f) < 1e-4f && std::abs(gc->value.z - 0.0625f) < 1e-4f &&
                  std::abs(gc->value.w - 0.6f) < 1e-4f,
              "gain on a colour target keeps alpha", gc ? Hex(gc->value.x) : "");
        StageManifest badGain = gm;
        badGain.bindings[0].gain = "nope";
        check(!mmdx::ValidateStageManifest(badGain).empty(), "gain: unknown param rejected");
        badGain = gm;
        badGain.bindings[0].gain = "beamTint";
        check(!mmdx::ValidateStageManifest(badGain).empty(), "gain: a colour gain param rejected");
        badGain = gm;
        badGain.bindings[2].gain = "beamLevel";
        check(!mmdx::ValidateStageManifest(badGain).empty(), "gain: a bool target with a gain rejected");
    }

    // validation catches author errors
    StageManifest bad2 = m;
    bad2.bindings.push_back({"fillLevel", "lights.nosuch.intensity", 1, 0, std::nullopt});
    bad2.bindings.push_back({"fillLevel", "lights.key.nosuchfield", 1, 0, std::nullopt});
    bad2.bindings.push_back({"keyIntensity", "materials.nosuchrule.roughness", 1, 0, std::nullopt});
    bad2.bindings.push_back({"noparam", "lights.key.intensity", 1, 0, std::nullopt});
    const auto badErrors = mmdx::ValidateStageManifest(bad2);
    check(badErrors.size() >= 4, "validation rejects unknown targets / fields / rules / params",
          std::to_string(badErrors.size()));
    StageManifest badColor = m;
    badColor.bindings.push_back({"coveColor", "lights.key.intensity", 1, 0, std::nullopt});
    check(!mmdx::ValidateStageManifest(badColor).empty(), "validation rejects a colour param on a number target");
    StageManifest bad3 = m;
    bad3.variants.push_back({"broken", {}, {{"nosuchparam", mmdx::StageValue::Number(1)}}});
    check(!mmdx::ValidateStageManifest(bad3).empty(), "validation rejects a variant value of an unknown param");
    StageManifest bad4 = m;
    bad4.id = "Not Valid!";
    check(!mmdx::ValidateStageManifest(bad4).empty(), "validation rejects a bad id");
    StageManifest bad5 = m;
    StageParam duplicate;
    duplicate.key = "mode";
    duplicate.type = mmdx::StageParamType::Float;
    bad5.params.push_back(duplicate);
    check(!mmdx::ValidateStageManifest(bad5).empty(), "validation rejects a repeated param key");

    // clone independence: the parsed manifest holds what the JSON says
    check(m.lights.size() == 3 && m.lights[1].kind == mmdx::studio::LightKind::Spot, "lights parsed with kinds");
    check(m.screens.size() == 1 && m.screens[0].width == 512, "screens parsed");
    check(m.environment.fog.present && m.environment.fog.density == 0.03f, "fog parsed");
    check(m.environment.fogVolumes.size() == 2 && !m.environment.fogVolumes[0].sphere &&
              m.environment.fogVolumes[1].sphere && m.environment.fogVolumes[0].density == 2.0f &&
              m.environment.fogVolumes[0].id == "beam" && m.environment.fogVolumes[0].extent.y == 20.0f,
          "environment.fogVolumes parsed (shape, extent, density, id)");
    check(m.packs.size() == 2 && m.packs[1].role == mmdx::StagePackRole::World, "packs parsed");
    check(m.cameras.size() == 1 && m.cameras[0].fov == 40.0f, "cameras parsed");
    check(m.sockets.size() == 2 && m.sockets[1].node == "Box", "sockets parsed");
    check(m.models.size() == 1 && m.models[0].role == mmdx::StagePartRole::Floor && m.models[0].name == "room",
          "model parts parsed (name from the file stem)");

    // an unparsable / foreign file is rejected with a message
    StageManifest other;
    check(!mmdx::ParseStageManifest(R"({"format":"pack","formatVersion":1})", fs::path("x"), other, error),
          "a foreign format is rejected");
    check(!error.empty(), "the rejection explains itself", error);
    check(!mmdx::ParseStageManifest("{not json", fs::path("x"), other, error), "bad JSON is rejected");

    std::printf("\nstage_check selftest: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: stage_check <stage dir> [--skip-models]\n       stage_check --selftest\n");
        return 2;
    }
    if (std::strcmp(argv[1], "--selftest") == 0) return SelfTest();
    bool loadModels = true;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--skip-models") == 0) loadModels = false;
        else {
            std::printf("unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    const fs::path dir = mmdx::Utf8ToPath(argv[1]).lexically_normal();
    const fs::path manifestFile = fs::is_directory(dir) ? dir / mmdx::Utf8ToPath(mmdx::kStageManifestName) : dir;
    std::printf("stage_check: %s\n", mmdx::PathToUtf8(manifestFile).c_str());
    StageManifest m;
    std::string error;
    if (!mmdx::LoadStageManifest(manifestFile, m, error)) {
        std::printf("  [problem] %s\n", error.c_str());
        std::printf("\nPROBLEMS\n");
        return 1;
    }
    std::printf("manifest: ok\n");
    ReportManifest(m);
    for (const auto& e : mmdx::ValidateStageManifest(m)) Problem(e);

    // the evaluator must never warn on a valid manifest; a default variant / defaults pass is the cheapest probe
    const auto eval = mmdx::EvaluateStage(m, "", {}, 0.0);
    for (const auto& w : eval.warnings) Problem("evaluator: " + w);

    const LoadedParts parts = LoadParts(m, loadModels);
    if (loadModels && parts.ok && !parts.materials.empty()) CheckMaterialNames(m, parts.materials);
    if (loadModels && parts.ok) {
        Info("vertices: " + std::to_string(parts.vertices) + ", indices: " + std::to_string(parts.indices) +
             ", material names: " + std::to_string(parts.materials.size()));
    }
    CheckFiles(m);

    std::printf(g_problems == 0 ? "\nOK\n" : "\nPROBLEMS\n");
    return g_problems == 0 ? 0 : 1;
}
