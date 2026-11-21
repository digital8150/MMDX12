#include "asset/StageMaterials.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace mmdx {

namespace {

// A linear stage colour as the sRGB-encoded constant the shader decodes (PbrAlbedo / PbrEmissive do
// SrgbToLinear(saturate(value)) on a slot without a texture).
float EncodeSrgb(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

DirectX::XMFLOAT3 EncodeSrgb(const DirectX::XMFLOAT3& c) {
    return {EncodeSrgb(c.x), EncodeSrgb(c.y), EncodeSrgb(c.z)};
}

bool SlotHasTexture(const PbrSlot& slot) {
    return slot.tex.kind != PbrTexKind::None && slot.from != 0;
}

float DecodeSrgb(float encoded) {
    encoded = std::clamp(encoded, 0.0f, 1.0f);
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

// The record's own emission as a linear colour, the way PbrEmissive reads it: the emissive map's linear factor
// (applyScale 1; a map without a factor = 1), or the constant (linear when the record is HDR, else sRGB-encoded).
// w = 0 when the record does not emit.
DirectX::XMFLOAT4 RecordEmissiveLinear(const PbrMaterial& m) {
    if (!m.emissiveEnable) return {0.0f, 0.0f, 0.0f, 0.0f};
    const PbrSlot& em = m.slots[(int)PbrSlotId::Emissive];
    if (SlotHasTexture(em)) {
        if (em.applyScale == 1 && em.hasValue)
            return {std::max(0.0f, em.value[0]), std::max(0.0f, em.value[1]), std::max(0.0f, em.value[2]), 1.0f};
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
    if (m.ext.emissiveStrength > 1.0f)
        return {std::max(0.0f, em.value[0]), std::max(0.0f, em.value[1]), std::max(0.0f, em.value[2]), 1.0f};
    return {DecodeSrgb(em.value[0]), DecodeSrgb(em.value[1]), DecodeSrgb(em.value[2]), 1.0f};
}

// Writes one assignment into a rule's field (the evaluator already checked that the param type fits the target).
void ApplyAssignment(StageMaterialRule& rule, const std::string& field, const StageValue& value) {
    if (field == "baseColor") {
        rule.baseColor = value.AsColor();
    } else if (field == "roughness") {
        rule.roughness = value.AsNumber();
    } else if (field == "metallic") {
        rule.metallic = value.AsNumber();
    } else if (field == "emissive") {
        rule.emissive = DirectX::XMFLOAT3{value.x, value.y, value.z};
    } else if (field == "emissiveStrength") {
        rule.emissiveStrength = value.AsNumber();
    } else if (field == "transmission") {
        rule.transmission = value.AsNumber();
    } else if (field == "ior") {
        rule.ior = value.AsNumber();
    } else if (field == "alpha") {
        rule.alpha = value.AsNumber();
    } else if (field == "mirror") {
        rule.mirror = value.AsNumber() > 0.5f;
    } else if (field == "castShadow") {
        rule.castShadow = value.AsNumber() > 0.5f;
    } else if (field == "receiveShadow") {
        rule.receiveShadow = value.AsNumber() > 0.5f;
    } else if (field == "screen") {
        rule.screen = value.text;
    }
}

// The record a material without PBR data gets: its PMX material through the native colour model. The albedo samples
// the PMX diffuse map (APPLY_DIFFUSE, like every imported / Ray albedo map); a material without a map keeps its diffuse
// colour as the constant. Smoothness / metalness are neutral (0.5 / 0), emissive off.
PbrMaterial MakeStageRecord(const PmxMaterial& pm) {
    PbrMaterial m;
    m.valid = true;
    m.shadingModel = 0;   // DEFAULT
    PbrSlot& albedo = m.slots[(int)PbrSlotId::Albedo];
    if (pm.textureIndex >= 0) {
        albedo.from = 3;
        albedo.tex.kind = PbrTexKind::PmxDiffuse;
        albedo.tex.pmxIndex = pm.textureIndex;
        m.albedoApplyDiffuse = 1;
    } else {
        albedo.from = 0;
        albedo.hasValue = true;
        albedo.value[0] = pm.diffuse.x;
        albedo.value[1] = pm.diffuse.y;
        albedo.value[2] = pm.diffuse.z;
    }
    PbrSlot& rough = m.slots[(int)PbrSlotId::Smoothness];
    rough.from = 0;
    rough.type = 2;   // linear (perceptual) roughness: r = value^2
    rough.hasValue = true;
    rough.value[0] = 0.5f;
    PbrSlot& metal = m.slots[(int)PbrSlotId::Metalness];
    metal.from = 0;
    metal.hasValue = true;
    metal.value[0] = 0.0f;
    return m;
}

// One material's PBR record from one rule: the rule's values on top of whatever record the material already has (an
// imported glTF record keeps its maps; a material with none gets the stage record above).
void ApplyRuleToRecord(const StageMaterialRule& rule, PbrMaterial& m, PmxMaterial& pm) {
    if (rule.baseColor) {
        const DirectX::XMFLOAT4 c = *rule.baseColor;
        const float alpha = std::clamp(c.w, 0.0f, 1.0f);
        pm.diffuse = {c.x, c.y, c.z, alpha};
        PbrSlot& albedo = m.slots[(int)PbrSlotId::Albedo];
        if (SlotHasTexture(albedo)) {
            // the map is tinted by the material colour (ALBEDO_MAP_APPLY_DIFFUSE reads pm.diffuse above)
            m.albedoApplyDiffuse = 1;
            albedo.applyScale = 0;
        } else {
            albedo.from = 0;
            albedo.hasValue = true;
            albedo.applyScale = 0;
            albedo.value[0] = EncodeSrgb(c.x);
            albedo.value[1] = EncodeSrgb(c.y);
            albedo.value[2] = EncodeSrgb(c.z);
            m.albedoApplyDiffuse = 0;
        }
    }
    if (rule.roughness) {
        // The shader reads a SMOOTHNESS map without a per-run factor, so an authored roughness replaces the map.
        PbrSlot& rough = m.slots[(int)PbrSlotId::Smoothness];
        rough = PbrSlot{};
        rough.from = 0;
        rough.type = 2;   // linear roughness (glTF / stage convention)
        rough.hasValue = true;
        rough.value[0] = std::clamp(*rule.roughness, 0.0f, 1.0f);
    }
    if (rule.metallic) {
        PbrSlot& metal = m.slots[(int)PbrSlotId::Metalness];
        metal = PbrSlot{};
        metal.from = 0;
        metal.hasValue = true;
        metal.value[0] = std::clamp(*rule.metallic, 0.0f, 1.0f);
    }
    // The colour the rule emits: its own colour, or (a rule with only a strength) the material's own emission, so a
    // strength-only rule scales what the material already emits. A material that emits nothing stays dark then.
    std::optional<DirectX::XMFLOAT3> base;
    if (rule.emissive) base = *rule.emissive;
    else if (rule.emissiveStrength && m.ext.emissiveOwn.w > 0.0f)
        base = DirectX::XMFLOAT3{m.ext.emissiveOwn.x, m.ext.emissiveOwn.y, m.ext.emissiveOwn.z};
    if (base) {
        const float strength = rule.emissiveStrength ? std::max(0.0f, *rule.emissiveStrength) : 1.0f;
        const DirectX::XMFLOAT3 e{base->x * strength, base->y * strength, base->z * strength};
        const float peak = std::max(e.x, std::max(e.y, e.z));
        m.emissiveEnable = peak > 0.0f;
        // > 1 marks the record HDR (GpuModel), so a strong panel is not clamped: a strength above 1, or a colour that
        // is already above 1 (an imported KHR_materials_emissive_strength emitter scaled by a strength-only rule)
        const bool hdr = strength > 1.0f || peak > 1.0f;
        m.ext.emissiveStrength = hdr ? std::max(strength, peak) : strength;
        PbrSlot& em = m.slots[(int)PbrSlotId::Emissive];
        if (SlotHasTexture(em)) {
            em.applyScale = 1;   // linear factor on the decoded map
            em.hasValue = true;
            em.value[0] = e.x;
            em.value[1] = e.y;
            em.value[2] = e.z;
        } else {
            em.from = 0;
            em.applyScale = 0;
            em.hasValue = true;
            // An HDR emissive is read as a linear colour by PbrEmissive; the plain case keeps the sRGB constant every
            // other record uses.
            if (hdr) {
                em.value[0] = e.x;
                em.value[1] = e.y;
                em.value[2] = e.z;
            } else {
                em.value[0] = EncodeSrgb(e.x);
                em.value[1] = EncodeSrgb(e.y);
                em.value[2] = EncodeSrgb(e.z);
            }
        }
    }
    if (rule.alpha) {
        const float a = std::clamp(*rule.alpha, 0.0f, 1.0f);
        pm.diffuse.w = a;
    }
    // S2a: the glTF extensions a stage rule can set (docs/stage_format.md "materials"). S2b: a transmission > 0 makes
    // the material a dielectric in every render path (raster / RT Fresnel alpha, RT / PT / GI refraction).
    if (rule.transmission) m.ext.transmission = std::clamp(*rule.transmission, 0.0f, 1.0f);
    if (rule.ior) m.ext.ior = std::clamp(*rule.ior, 1.0f, 3.0f);
    // S10: a planar mirror. The record carries the flag so the GPU record (GpuModel::Create) and the ray-traced
    // geometry (RtScene) see it; the material keeps its own albedo / roughness (tint / blur).
    if (rule.mirror) m.ext.mirror = *rule.mirror;
    // castShadow / receiveShadow are the PMX material flags (see ApplyStageMaterialRules).
}

} // namespace

std::vector<StageMaterialRule> ResolvedStageMaterialRules(const StageManifest& manifest, const StageEvalResult& eval) {
    std::vector<StageMaterialRule> rules = manifest.materials;
    for (const StageAssignment& a : eval.targets) {
        const StageTargetSpec spec = ResolveStageTarget(manifest, a.target);
        if (!spec.ok || spec.kind != StageTargetSpec::Kind::Materials) continue;   // light / screen / ... targets
        for (StageMaterialRule& r : rules)
            if (r.match == spec.owner) {
                ApplyAssignment(r, spec.field, a.value);
                break;
            }
    }
    return rules;
}

StageMaterialBinding MatchStageMaterialRules(const std::vector<StageMaterialRule>& rules, const PmxModel& pmx) {
    StageMaterialBinding binding(rules.size());
    for (size_t r = 0; r < rules.size(); ++r) {
        const std::string& pattern = rules[r].match;
        for (size_t i = 0; i < pmx.materials.size(); ++i) {
            const PmxMaterial& m = pmx.materials[i];
            if (StageGlobMatch(pattern, m.name) ||
                (!m.nameEn.empty() && StageGlobMatch(pattern, m.nameEn)))
                binding[r].push_back(i);
        }
    }
    return binding;
}

StageMaterialRule StageEffectiveMaterialValues(const std::vector<StageMaterialRule>& rules,
                                               const StageMaterialBinding& binding, size_t material) {
    StageMaterialRule out;
    for (size_t r = 0; r < rules.size() && r < binding.size(); ++r) {
        if (std::find(binding[r].begin(), binding[r].end(), material) == binding[r].end()) continue;
        const StageMaterialRule& src = rules[r];
        if (!out.baseColor && src.baseColor) out.baseColor = src.baseColor;
        if (!out.roughness && src.roughness) out.roughness = src.roughness;
        if (!out.metallic && src.metallic) out.metallic = src.metallic;
        if (!out.emissive && src.emissive) out.emissive = src.emissive;
        if (!out.emissiveStrength && src.emissiveStrength) out.emissiveStrength = src.emissiveStrength;
        if (!out.transmission && src.transmission) out.transmission = src.transmission;
        if (!out.ior && src.ior) out.ior = src.ior;
        if (!out.alpha && src.alpha) out.alpha = src.alpha;
        if (!out.mirror && src.mirror) out.mirror = src.mirror;
        if (!out.screen && src.screen) out.screen = src.screen;
        if (!out.castShadow && src.castShadow) out.castShadow = src.castShadow;
        if (!out.receiveShadow && src.receiveShadow) out.receiveShadow = src.receiveShadow;
    }
    return out;
}

bool StageRuleSetsPbr(const StageMaterialRule& rule) {
    // S10: a mirror needs a record too: the shaders take the mirror branch inside the PBR path (the flag itself
    // reaches the material constants from the record in GpuModel::Create).
    return rule.baseColor || rule.roughness || rule.metallic || rule.emissive || rule.emissiveStrength ||
           rule.transmission || rule.ior || rule.mirror;
}

void ApplyStageMaterialRules(const std::vector<StageMaterialRule>& rules, const StageMaterialBinding& binding,
                             PmxModel& pmx, PbrModelData& pbr) {
    if (rules.empty() || pmx.materials.empty()) return;
    // An imported part (glTF / GLB) carries its PBR data on the PmxModel: build the records it would get with the PBR
    // material source, so a stage's own materials are the source of truth here (an MMDX stage is authored in PBR).
    if (pbr.materials.empty() && !pmx.importedPbr.empty()) BuildImportedPbrModelData(pmx, pbr);
    if (pbr.materials.size() < pmx.materials.size()) pbr.materials.resize(pmx.materials.size());

    int records = pbr.materialsWithRecord;
    for (size_t i = 0; i < pmx.materials.size(); ++i) {
        // every record keeps its own emission: a binding may give a material a strength later, at run time
        if (i < pbr.materials.size() && pbr.materials[i].valid)
            pbr.materials[i].ext.emissiveOwn = RecordEmissiveLinear(pbr.materials[i]);
        const StageMaterialRule v = StageEffectiveMaterialValues(rules, binding, i);
        const bool matched = v.baseColor || v.roughness || v.metallic || v.emissive || v.emissiveStrength ||
                             v.transmission || v.ior || v.alpha || v.mirror || v.screen || v.castShadow ||
                             v.receiveShadow;
        if (!matched) continue;
        PmxMaterial& pm = pmx.materials[i];
        if (StageRuleSetsPbr(v) && !pbr.materials[i].valid) {
            pbr.materials[i] = MakeStageRecord(pm);
            ++records;
        }
        if (pbr.materials[i].valid) ApplyRuleToRecord(v, pbr.materials[i], pm);
        else if (v.baseColor) pm.diffuse = *v.baseColor;
        else if (v.alpha) pm.diffuse.w = std::clamp(*v.alpha, 0.0f, 1.0f);
        if (v.castShadow || v.receiveShadow) {
            if (v.castShadow) {
                pm.flags = (uint8_t)(*v.castShadow ? (pm.flags | PmxMat_CastShadow)
                                                   : (pm.flags & ~(PmxMat_CastShadow | PmxMat_GroundShadow)));
            }
            if (v.receiveShadow) {
                pm.flags = (uint8_t)(*v.receiveShadow ? (pm.flags | PmxMat_ReceiveShadow)
                                                      : (pm.flags & ~PmxMat_ReceiveShadow));
            }
        }
    }
    pbr.materialsWithRecord = records;
}

void ApplyStageMaterialRules(const StageManifest& manifest, const std::string& variant,
                             const StageParamValues& overrides, double timeSeconds,
                             const StageMaterialBinding& binding, PmxModel& pmx, PbrModelData& pbr) {
    if (manifest.materials.empty()) return;
    const StageEvalResult eval = EvaluateStage(manifest, variant, overrides, timeSeconds);
    ApplyStageMaterialRules(ResolvedStageMaterialRules(manifest, eval), binding, pmx, pbr);
}

} // namespace mmdx
