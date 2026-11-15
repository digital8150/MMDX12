#pragma once
// The MMDX stage evaluator: a pure function of (manifest, variant, param overrides, time). No file system, no
// renderer, no app state, so the studio, play mode, GI and the tools all agree on what a stage shows at time t.
//
//   resolved param values: defaults < variant values < cue tracks < project / caller overrides < drivers
//   (drivers modulate the value the earlier stages resolved; audio.* drivers need `signals`, else they are skipped)
//   targets: every binding turns its param's resolved value into one assignment (path + typed value);
//            several bindings may hit the same target and are emitted in manifest order.
//
// Unknown targets never crash: they are reported by ValidateStageManifest (a validation error) and skipped here
// with a warning. A cue track whose keys are all beyond `time` holds its first key (as MMD does).
#include "asset/StageManifest.h"

#include <json.hpp>
#include <set>
#include <string>
#include <vector>

namespace mmdx {

struct StageAssignment {
    std::string target;   // the binding's target path, verbatim
    StageValue value;     // float / vec3 / colour / bool / string, already remapped and scaled
};

struct StageEvalResult {
    StageParamValues params;              // every declared param, resolved (key order)
    std::vector<StageAssignment> targets; // in binding order
    std::vector<std::string> warnings;    // unknown targets / params; empty for a valid manifest
};

// The song's audio analysis sampled at the frame's audio time (the app fills it from its BeatAnalysis; the drivers'
// audio.* inputs read it).
struct StageSignals {
    bool audioValid = false;
    double beatCount = 0.0;   // beat index + phase, continuous (may be negative before the first beat)
    double barCount = 0.0;    // downbeat index + bar phase, continuous
    float onset = 0, energy = 0, section = 0, bpm = 0;
};

// `variantId` empty = the manifest's default variant. `overrides` cover the variant (project overrides). `time` is
// the song time in seconds; cues are 30 fps frames. Negative time is clamped to 0.
// `skipCues` names params whose manifest cue track is not applied (the studio keyed the param itself or imported its
// cue: the project's keys replace the stage's own light show for it).
// `signals` feeds the audio.* drivers; nullptr (or audioValid false) skips them (time drivers always apply).
StageEvalResult EvaluateStage(const StageManifest& manifest, const std::string& variantId,
                              const StageParamValues& overrides, double timeSeconds,
                              const std::set<std::string>& skipCues = {}, const StageSignals* signals = nullptr);

// One driver's signal before min / max (pulse / saw / sine / triangle / square in 0..1, ramp unbounded, level inputs
// pow(clamp(value, 0, 1), power)). False when the driver needs audio and `signals` has none.
bool SampleStageDriver(const StageDriver& driver, double timeSeconds, const StageSignals* signals, double& out);

// One param value from typed JSON: a number for float / int / bool (0|1) / enum (an option's "value" string, or its
// index), an array of 3..4 numbers for color. False (with `error`) for a type mismatch. Shared by the manifest parser
// and by project-level overrides.
bool ParseStageParamValue(const StageParam& param, const nlohmann::json& j, StageValue& out, std::string& error);
// The same for a JSON that is a plain string / number / bool / array, resolved against `param`'s type.
bool StageParamValueFromJson(const StageParam& param, const nlohmann::json& j, StageValue& out, std::string& error);

// The cue track's value at `time`: stepped = the last key at or before the frame, linear = lerp (colours
// component-wise; int / bool / enum keys hold). Empty -> false.
bool SampleStageCue(const StageCue& cue, const StageParam& param, double timeSeconds, StageValue& out);

// Applies a binding's remap / scale / offset to `in` (used for both number and colour values).
StageValue ApplyStageBinding(const StageBinding& binding, const StageValue& in);

// Inverse of a param's value: used by the studio to turn a UI value into overrides (rounds int / enum, clamps).
StageValue ClampStageParamValue(const StageParam& param, const StageValue& in);

} // namespace mmdx
