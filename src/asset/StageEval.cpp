// The MMDX stage evaluator (see StageEval.h): pure, deterministic, no I/O.
// Precedence per param: defaults < the chosen variant < cue tracks (at `time`) < the caller's overrides < drivers.
#include "asset/StageEval.h"

#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

StageValue LerpValue(const StageValue& a, const StageValue& b, float t) {
    StageValue out = a;
    out.x = Lerp(a.x, b.x, t);
    out.y = Lerp(a.y, b.y, t);
    out.z = Lerp(a.z, b.z, t);
    out.w = Lerp(a.w, b.w, t);
    return out;
}

// The remapped / scaled / offset value of a number or the xyz (and w) of a colour.
StageValue MapValue(const StageValue& in, const std::optional<std::array<float, 4>>& remap, float scale,
                    float offset) {
    StageValue out = in;
    auto map = [&](float v) {
        if (remap) {
            const float inMin = (*remap)[0], inMax = (*remap)[1], outMin = (*remap)[2], outMax = (*remap)[3];
            const float t = (v - inMin) / (inMax - inMin);
            v = outMin + (outMax - outMin) * t;
        }
        return v * scale + offset;
    };
    out.x = map(in.x);
    if (in.type == StageValue::Type::Color) {
        out.y = map(in.y);
        out.z = map(in.z);
        out.w = map(in.w);
    }
    return out;
}

// Does `spec` accept a value of this param type (shared shape with validation)?
bool Fits(const StageParam& param, StageTargetSpec::ValueType target) {
    using VT = StageTargetSpec::ValueType;
    switch (target) {
    case VT::Color: return param.type == StageParamType::Color;
    case VT::String: return param.type == StageParamType::Enum;
    case VT::Vec3: return param.type == StageParamType::Color || param.type == StageParamType::Float ||
                          param.type == StageParamType::Int || param.type == StageParamType::Bool;
    default: return param.type != StageParamType::Color;
    }
}

} // namespace

bool SampleStageCue(const StageCue& cue, const StageParam& param, double timeSeconds, StageValue& out) {
    if (cue.keys.empty()) return false;
    const double frame = std::max(0.0, timeSeconds) * 30.0;
    // the last key at or before `frame`; nothing before it -> the first key (as MMD holds its first key)
    if (frame <= (double)cue.keys.front().frame) {
        out = cue.keys.front().value;
        return true;
    }
    if (frame >= (double)cue.keys.back().frame) {
        out = cue.keys.back().value;
        return true;
    }
    const auto next = std::upper_bound(cue.keys.begin(), cue.keys.end(), frame,
                                       [](double f, const StageCueKey& k) { return f < (double)k.frame; });
    const StageCueKey& a = *(next - 1);
    const StageCueKey& b = *next;
    if (cue.stepped || a.frame == b.frame) {
        out = a.value;
        return true;
    }
    // Linear on numbers and colours; a stepped-only type (int / bool / enum, stepped colour) holds its key.
    if (param.type == StageParamType::Int || param.type == StageParamType::Bool ||
        param.type == StageParamType::Enum || a.value.type == StageValue::Type::Text ||
        b.value.type == StageValue::Type::Text) {
        out = a.value;
        return true;
    }
    if (a.value.type != b.value.type) {
        out = a.value;
        return true;
    }
    const float t = (float)((frame - (double)a.frame) / (double)(b.frame - a.frame));
    out = LerpValue(a.value, b.value, t);
    if (param.type == StageParamType::Enum) out = a.value;
    return true;
}

StageValue ApplyStageBinding(const StageBinding& binding, const StageValue& in) {
    return MapValue(in, binding.remap, binding.scale, binding.offset);
}

bool SampleStageDriver(const StageDriver& driver, double timeSeconds, const StageSignals* signals, double& out) {
    const bool audio = StageDriverIsAudio(driver.input);
    if (audio && (!signals || !signals->audioValid)) return false;
    if (StageDriverIsLevel(driver.input)) {
        const float raw = driver.input == StageDriverInput::Onset    ? signals->onset
                          : driver.input == StageDriverInput::Energy ? signals->energy
                                                                     : signals->section;
        const double v = std::clamp((double)raw, 0.0, 1.0);
        out = driver.hasPower ? std::pow(v, (double)driver.power) : v;
        return true;
    }
    const double count = driver.input == StageDriverInput::Beat  ? signals->beatCount
                         : driver.input == StageDriverInput::Bar ? signals->barCount
                                                                 : std::max(0.0, timeSeconds);
    const double u = count / (double)driver.every + (double)driver.phase;
    const double f = u - std::floor(u);
    switch (driver.shape) {
    case StageDriverShape::Saw: out = f; break;
    case StageDriverShape::Pulse: out = std::pow(1.0 - f, (double)driver.power); break;
    case StageDriverShape::Sine: out = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * f); break;
    case StageDriverShape::Triangle: out = 1.0 - std::abs(2.0 * f - 1.0); break;
    case StageDriverShape::Square: out = f < (double)driver.duty ? 1.0 : 0.0; break;
    case StageDriverShape::Ramp: out = u; break;
    }
    return true;
}

StageEvalResult EvaluateStage(const StageManifest& manifest, const std::string& variantId,
                              const StageParamValues& overrides, double timeSeconds,
                              const std::set<std::string>& skipCues, const StageSignals* signals) {
    StageEvalResult result;
    const StageVariant* variant = variantId.empty() ? manifest.DefaultVariant() : manifest.FindVariant(variantId);
    if (!variantId.empty() && !variant)
        result.warnings.push_back("unknown variant \"" + variantId + "\" (the defaults are used)");

    // 1. defaults, then the variant
    for (const StageParam& p : manifest.params) result.params[p.key] = p.DefaultValue();
    if (variant)
        for (const auto& [key, value] : variant->values) {
            const StageParam* p = manifest.FindParam(key);
            if (p) result.params[key] = ClampStageParamValue(*p, value);
        }

    // 2. cue tracks at `time` (a param the caller keyed itself keeps its override)
    for (const StageCue& cue : manifest.cues) {
        const StageParam* p = manifest.FindParam(cue.param);
        if (!p) {
            result.warnings.push_back("cue track for unknown param \"" + cue.param + "\"");
            continue;
        }
        if (skipCues.count(cue.param)) continue;
        StageValue value;
        if (SampleStageCue(cue, *p, timeSeconds, value)) result.params[cue.param] = ClampStageParamValue(*p, value);
    }

    // 3. caller overrides
    for (const auto& [key, value] : overrides) {
        const StageParam* p = manifest.FindParam(key);
        if (!p) {
            result.warnings.push_back("override for unknown param \"" + key + "\"");
            continue;
        }
        result.params[key] = ClampStageParamValue(*p, value);
    }

    // 4. drivers, in list order, each on the previous result; `amount` reads the value before any driver
    if (!manifest.drivers.empty()) {
        const StageParamValues before = result.params;
        for (const StageDriver& driver : manifest.drivers) {
            const StageParam* p = manifest.FindParam(driver.param);
            if (!p) {
                result.warnings.push_back("driver for unknown param \"" + driver.param + "\"");
                continue;
            }
            const bool color = p->type == StageParamType::Color;
            if (p->type == StageParamType::Bool || p->type == StageParamType::Enum ||
                (color && driver.blend != StageDriverBlend::Mul))
                continue;   // a validation error; never applied
            double s = 0.0;
            if (!SampleStageDriver(driver, timeSeconds, signals, s)) continue;
            const float v = (float)((double)driver.min + ((double)driver.max - (double)driver.min) * s);
            StageValue& slot = result.params[driver.param];
            const StageValue base = slot;
            StageValue driven = base;
            if (color) {
                driven.x *= v;
                driven.y *= v;
                driven.z *= v;
            } else if (driver.blend == StageDriverBlend::Set) {
                driven.x = v;
            } else if (driver.blend == StageDriverBlend::Add) {
                driven.x = base.x + v;
            } else {
                driven.x = base.x * v;
            }
            if (!driver.amount.empty()) {
                const auto a = before.find(driver.amount);
                const float t = a != before.end() ? std::clamp(a->second.x, 0.0f, 1.0f) : 1.0f;
                driven = LerpValue(base, driven, t);
                driven.w = base.w;
            }
            slot = ClampStageParamValue(*p, driven);
        }
    }

    // 5. bindings -> target assignments
    for (const StageBinding& binding : manifest.bindings) {
        const StageParam* param = manifest.FindParam(binding.param);
        if (!param) {
            result.warnings.push_back("binding for unknown param \"" + binding.param + "\"");
            continue;
        }
        const StageTargetSpec spec = ResolveStageTarget(manifest, binding.target);
        if (!spec.ok) {
            result.warnings.push_back("binding " + binding.param + " -> " + binding.target + ": " + spec.error);
            continue;
        }
        if (!Fits(*param, spec.type)) {
            result.warnings.push_back("binding " + binding.param + " -> " + binding.target +
                                      ": the value type does not match the target");
            continue;
        }
        const auto it = result.params.find(binding.param);
        if (it == result.params.end()) continue;
        StageValue mapped = ApplyStageBinding(binding, it->second);
        if (!binding.gain.empty()) {
            // the gain param's resolved value (after drivers): numbers x g, colour / vec3 rgb (xyz) x g, alpha kept
            const StageParam* gainParam = manifest.FindParam(binding.gain);
            const auto g = result.params.find(binding.gain);
            if (!gainParam || g == result.params.end() ||
                (gainParam->type != StageParamType::Float && gainParam->type != StageParamType::Int) ||
                spec.type == StageTargetSpec::ValueType::Bool || spec.type == StageTargetSpec::ValueType::String) {
                result.warnings.push_back("binding " + binding.param + " -> " + binding.target + ": bad gain \"" +
                                          binding.gain + "\" (ignored)");
            } else {
                const float gv = g->second.x;
                mapped.x *= gv;
                if (mapped.type == StageValue::Type::Color) {
                    mapped.y *= gv;
                    mapped.z *= gv;
                }
            }
        }
        StageAssignment assignment;
        assignment.target = binding.target;
        assignment.value = mapped;
        if (spec.type == StageTargetSpec::ValueType::String && param->type == StageParamType::Enum) {
            // an enum target takes the option's "value" string, not its index
            if (!param->options.empty()) {
                const int index = std::clamp((int)std::lround(mapped.x), 0, (int)param->options.size() - 1);
                assignment.value = StageValue::Text(param->options[(size_t)index].value);
            }
        }
        result.targets.push_back(std::move(assignment));
    }
    return result;
}

} // namespace mmdx
