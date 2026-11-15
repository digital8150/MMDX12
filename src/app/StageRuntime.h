#pragma once
// MMDX stage runtime (docs/stage_format.md, docs/stage_ecosystem.md §3): one loaded stage kept together with the parts
// it was loaded into, so play mode and the studio evaluate it every frame with `EvaluateStage` and hand the result to
// the renderer:
//   - `lights.<name>.<field>` assignments go onto a copy of the manifest's lights, which is what `BuildSceneLighting`
//     resolves (the light rig with its cues; play mode's "스테이지 조명").
//   - `materials.<rule>.<field>` assignments become `GpuModel::SetPbrOverrides` for the materials the rule matches.
// Screen / pack / environment targets are ignored until their milestones (no warnings).
//
// The rule -> material binding is resolved once (at load, `MatchStageMaterialRules`); per frame the evaluator plus a diff
// runs and GpuModel::SetPbrOverrides skips ring entries that already hold the values.
#include "asset/StageEval.h"
#include "asset/StageManifest.h"
#include "asset/StageMaterials.h"
#include "render/GpuModel.h"
#include "render/RenderTypes.h"
#include "render/WorldTypes.h"
#include "studio/SceneLight.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mmdx {

// S6: one `packs[]` entry of a stage at one frame: the registry id its folder is registered under (StagePackId) and its
// parameter values (the manifest's initial values with the `packs.<index>.params.<key>` targets replayed in assignment
// order). `index` is the entry's position in manifest.packs (the binding index). ResolveStagePacks fills it.
struct StagePackState {
    int index = 0;
    std::string id;                        // "stage:<stage id>/<folder>"
    StagePackRole role = StagePackRole::Surface;
    std::map<std::string, float> params;
};

// One loaded stage part: its GPU model plus the rule -> material binding of its materials.
struct StageRuntimePart {
    int part = 0;                    // the part's index in StageManifest::models order
    GpuModel* gpu = nullptr;         // not owned (the scene / the studio document owns it)
    StageMaterialBinding binding;    // parallel to the manifest's `materials`; empty = no override for this part
    // S6: the surface pack this part was assigned at load (id + per-material class overrides). Empty = the part keeps
    // the default shading; resolved once (ResolveStageSurfacePack), the parameters follow the frame.
    StageSurfacePack surfacePack;
};

// S5: one screen after the evaluator's `screens.<id>.<field>` targets (and the studio project's overrides) are
// applied.
struct StageScreenState {
    std::string id;
    StageScreenSource source = StageScreenSource::None;
    std::string file;      // image / video file as the manifest / target writes it (resolved by the caller)
    std::string camera;    // stage camera name, or "uid:<n>" for a studio camera
    // S6: the screen's program pack (a bundled stage pack id, StagePackId) and its parameter values this frame. Empty
    // pack = no program content (the screen is black).
    std::string pack;
    std::map<std::string, float> params;
    uint32_t width = 1920, height = 1080;
    float brightness = 1.0f;
    bool enabled = true;
};

class StageRuntime {
public:
    // `manifest` must outlive the runtime. `variantId` empty = the manifest's default variant.
    void Init(std::shared_ptr<const StageManifest> manifest, std::string variantId, std::vector<StageRuntimePart> parts);

    bool Valid() const { return manifest_ != nullptr; }
    const StageManifest* Manifest() const { return manifest_.get(); }
    const std::string& Variant() const { return variant_; }
    // The stage has lights: play mode's lighting picker defaults to "스테이지 조명" for it.
    bool HasLights() const {
        return manifest_ && (!manifest_->lights.empty() || (!gltfLights_.empty() && manifest_->gltfLights));
    }
    bool Empty() const { return !manifest_; }

    // Evaluates the stage at `timeSeconds` (song time) for GPU frame `frame` and applies the results: the light targets
    // into Lights(), the material targets into the parts' GPU models (only when a value changed). `signals` feeds the
    // manifest's audio.* drivers (nullptr = skipped).
    const std::vector<studio::SceneLight>& Apply(double timeSeconds, uint64_t frame,
                                                 const StageSignals* signals = nullptr);

    // S2a: the parts' glTF punctual lights (asset/StageLights.h PartGltfLights), appended to the rig when the manifest
    // opts in with "gltfLights": true. Called once at load; empty = none.
    void SetGltfLights(std::vector<studio::SceneLight> lights) { gltfLights_ = std::move(lights); }

    // S9: the stage has an `environment.fog` block: it turns the volumetric haze on and sets its density / colour /
    // anisotropy / height falloff while the stage is loaded (the user's volumetric switch can still turn it off).
    bool HasFog() const { return manifest_ && manifest_->environment.fog.present; }

    // The rig at the last Apply (the manifest's lights with the targets applied, plus the glTF lights when enabled;
    // empty when the stage has none).
    const std::vector<studio::SceneLight>& Lights() const { return lights_; }

    // S5: the stage's screens at the last Apply (screens.<id>.<field> targets applied). Empty when it has none.
    const std::vector<StageScreenState>& Screens() const { return screens_; }

    // S6: the bundled packs at the last Apply (`packs[].params` + the packs.<index>.params.<key> targets). Empty when
    // the stage bundles none.
    const std::vector<StagePackState>& Packs() const { return packs_; }

private:
    void ApplyMaterials(const std::vector<StageMaterialRule>& rules, uint64_t frame);

    std::shared_ptr<const StageManifest> manifest_;
    std::string variant_;   // empty = the manifest's default variant
    std::vector<StageRuntimePart> parts_;
    std::vector<studio::SceneLight> lights_;
    std::vector<studio::SceneLight> gltfLights_;   // S2a KHR_lights_punctual of the parts (manifest.gltfLights)
    std::vector<StageScreenState> screens_;        // S5: the screens at the last Apply
    std::vector<StagePackState> packs_;            // S6: the bundled packs at the last Apply
    uint64_t appliedFrame_ = ~0ull;   // the frame `lights_` / the material targets were last applied for
};

// Replays one `lights.<name>.<field>` assignment onto a scene light (the value is already remapped / scaled). False
// for a field a binding cannot drive. The studio and the runtime share it: a stage's rig lights in the studio are
// ordinary scene lights, tagged with the stage they came from (SceneLight::stageUid / stageLightName).
bool ApplyStageLightField(studio::SceneLight& l, const std::string& field, const StageValue& v);
// The manifest light fields any binding drives for the light called `name` (`lights.<name>.<field>`), i.e. the fields
// the studio shows read-only on that light.
std::set<std::string> StageDrivenLightFields(const StageManifest& manifest, const std::string& name);

// Applies one part's resolved rule values to its GPU model (`GpuModel::SetPbrOverrides`, changed values only). The
// studio and play mode share it; a manifest's rules come from `ResolvedStageMaterialRules(manifest, eval)`.
void ApplyStageMaterialList(GpuModel& gpu, int part, const StageMaterialBinding& binding,
                            const std::vector<StageMaterialRule>& rules, uint64_t frame);

// ---- S5 screens ---------------------------------------------------------------------------------------------------
// The manifest's screens with `eval`'s screen targets replayed in assignment order (brightness / enabled / source /
// file / camera) plus (S6) the program pack they draw with and that pack's parameters for this frame. `edit` (optional)
// may replace a screen's values first (the studio's project overrides by id).
std::vector<StageScreenState> ResolveStageScreens(const StageManifest& manifest, const StageEvalResult& eval,
                                                  const std::function<void(StageScreenState&)>& edit = {});

// ---- S6 bundled packs ---------------------------------------------------------------------------------------------
// Every `packs[]` entry with this frame's parameters. Empty when the stage bundles no packs (zero cost).
std::vector<StagePackState> ResolveStagePacks(const StageManifest& manifest, const StageEvalResult& eval);

// Registers every `packs[]` entry with the pack registry as "stage:<stage id>/<folder>" (once per stage folder: the
// registry returns an already registered pack as is). A broken pack is one [W] and the stage keeps loading. Call once
// per loaded stage (play mode: the scene load, studio: adding / opening a stage), before the parts' packs are applied.
void RegisterStagePacks(const StageManifest& manifest);

// S6 surface packs: the pack a stage part's materials are shaded with at load (the FIRST `packs[]` entry of role
// "surface" whose `materials` globs match one of the part's materials), plus the per-material class overrides the pack
// needs (StageSurfacePack): the materials the pack claims are left out (the pack's own class rules,
// ShaderPack::ClassifyStage), every other material of the part is `kMaterialPackOff` (the default shading).
StageSurfacePack ResolveStageSurfacePack(const StageManifest& manifest, const GpuModel& part);
// Applies the surface pack of one part at this frame: the pack of `surface.index` in `packs` (its parameters follow
// `packs.<index>.params.<key>`). GpuModel::SetShaderPack is a no-op when nothing changed, so call it every frame. No
// packs / no pack for the part = nothing to do.
void ApplyStageSurfacePack(const std::vector<StagePackState>& packs, const StageRuntimePart& part);

// S6 world packs: one WorldInstance per `packs[]` entry of role "world", placed at its socket (the socket's node bone's
// world matrix, times the socket's explicit offset; resolved after the parts are posed) and evaluated at `time`.
// `resolveNode` answers a node / bone world matrix (empty name = not resolvable, the socket's explicit transform is used
// on its own). `uidBase` keeps the instances apart from the host's own worlds: each gets uidBase + pack index (the
// studio adds the stage entity's uid * 256 to the base so two stages never share a uid).
void AppendStageWorldPacks(const StageManifest& manifest, const std::vector<StagePackState>& packs, double time,
                           bool exact, const std::shared_ptr<const WorldColliderTrack>& colliders, uint32_t uidBase,
                           const std::function<bool(const std::string& node, DirectX::XMFLOAT4X4& out)>& resolveNode,
                           std::vector<WorldInstance>& out);
// The uid base play mode / the studio use for a stage's world packs (the host's own worlds keep their low uids).
inline constexpr uint32_t kStageWorldUidBase = 0x40000000u;

// A stage camera as a render camera: looks at `target` from `position`, vertical fov in degrees.
CameraParams StageCameraView(const StageCamera& camera, float nearZ = 0.5f, float farZ = 4000.0f);

// S5: the ScreenViews of one stage's screens: each screen's materials are resolved on the stage's parts (the screen's
// own `materials` globs plus every material rule whose `screen` names it). One ScreenView per (screen, part): a screen
// whose globs hit two parts shows the same source twice (each part keeps its own texture).
// `resolveFile` turns a manifest-relative file into an absolute path ("" = not resolved); `resolveCamera` resolves a
// camera name (stage cameras are resolved by the caller, so the studio can answer "uid:<n>").
std::vector<ScreenView> BuildStageScreenViews(
    const StageManifest& manifest, const std::vector<StageScreenState>& screens,
    const std::vector<StageRuntimePart>& parts,
    const std::function<std::filesystem::path(const std::string&)>& resolveFile,
    const std::function<bool(const std::string&, CameraParams&)>& resolveCamera);

// Studio: one stage part in the per-frame material pass (the studio's light rig is copied into the scene lights when
// the stage is added, so only the material targets run there).
struct StageModelOverridePart {
    int part = 0;                              // the part's index in the manifest's models
    GpuModel* gpu = nullptr;
    const StageMaterialBinding* binding = nullptr;
};
// Applies one manifest's material targets at `timeSeconds` to `parts` (studio per-frame pass).
void ApplyStageMaterialTargets(const StageManifest& manifest, const std::string& variant, double timeSeconds,
                               uint64_t frame, const std::vector<StageModelOverridePart>& parts,
                               const StageSignals* signals = nullptr);

// The variant a stage shows: `cliVariant` (--stage-variant) wins, then the saved per-stage choice, then the manifest's
// default variant (an empty manifest default = the first variant / the plain defaults).
std::string ResolveStageVariant(const std::string& manifestDefault, const std::string& saved,
                                const std::string& cliVariant);

} // namespace mmdx
