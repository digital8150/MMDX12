// stage_material_test : a synthetic self test of the MMDX stage material rules (asset/StageMaterials.h) on PBR records,
// focused on the emissive fields (issue #30): a rule that sets only `emissiveStrength` must scale the material's own
// emission (an emissive map's factor, an sRGB constant, an HDR constant), leave a material that does not emit dark,
// and remember the own emission (`ext.emissiveOwn`) for the runtime binding. A rule with a colour keeps its old
// behaviour. No files are needed. Exit code 0 = pass. Style: like tools/raymat_test.cpp.
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"
#include "asset/StageManifest.h"
#include "asset/StageMaterials.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mmdx;

namespace {

int g_fails = 0;
void Expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}
bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

float EncodeSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

const PbrSlot& Emissive(const PbrMaterial& m) { return m.slots[(int)PbrSlotId::Emissive]; }

// Materials: 0 "tex" = emissive map x factor 1 (a glTF emitter), 1 "const" = sRGB constant 0.5 linear, 2 "hdr" = HDR
// constant 15 (KHR_materials_emissive_strength 15), 3 "dark" = a record that does not emit, 4 "pmx" = no record.
void MakeModel(PmxModel& pmx, PbrModelData& pbr) {
    const char* names[] = {"tex", "const", "hdr", "dark", "pmx"};
    pmx.materials.clear();
    for (const char* n : names) {
        PmxMaterial m;
        m.name = n;
        pmx.materials.push_back(m);
    }
    pbr.materials.assign(pmx.materials.size(), PbrMaterial{});
    for (int i = 0; i < 4; ++i) pbr.materials[i].valid = true;

    PbrMaterial& tex = pbr.materials[0];
    tex.emissiveEnable = true;
    PbrSlot& te = tex.slots[(int)PbrSlotId::Emissive];
    te.from = 1;
    te.tex.kind = PbrTexKind::File;
    te.applyScale = 1;
    te.hasValue = true;
    te.value[0] = te.value[1] = te.value[2] = 1.0f;

    PbrMaterial& c = pbr.materials[1];
    c.emissiveEnable = true;
    PbrSlot& ce = c.slots[(int)PbrSlotId::Emissive];
    ce.hasValue = true;
    ce.value[0] = ce.value[1] = ce.value[2] = EncodeSrgb(0.5f);

    PbrMaterial& h = pbr.materials[2];
    h.emissiveEnable = true;
    h.ext.emissiveStrength = 15.0f;
    PbrSlot& he = h.slots[(int)PbrSlotId::Emissive];
    he.hasValue = true;
    he.value[0] = 15.0f;
    he.value[1] = 7.5f;
    he.value[2] = 0.0f;
    pbr.materialsWithRecord = 4;
}

void Apply(const std::vector<StageMaterialRule>& rules, PmxModel& pmx, PbrModelData& pbr) {
    ApplyStageMaterialRules(rules, MatchStageMaterialRules(rules, pmx), pmx, pbr);
}

void TestStrengthOnly() {
    PmxModel pmx;
    PbrModelData pbr;
    MakeModel(pmx, pbr);
    StageMaterialRule r;
    r.match = "*";
    r.emissiveStrength = 4.0f;
    Apply({r}, pmx, pbr);

    const PbrMaterial& tex = pbr.materials[0];
    Expect(tex.emissiveEnable, "strength only: an emissive map keeps glowing");
    Expect(Emissive(tex).applyScale == 1 && Near(Emissive(tex).value[0], 4.0f), "strength only: map factor x 4");
    Expect(tex.ext.emissiveStrength > 1.0f, "strength only: a map scaled above 1 is HDR");
    Expect(tex.ext.emissiveOwn.w > 0.5f && Near(tex.ext.emissiveOwn.x, 1.0f), "own emission of the map = factor 1");

    const PbrMaterial& c = pbr.materials[1];
    Expect(c.emissiveEnable, "strength only: an sRGB constant keeps glowing");
    Expect(c.ext.emissiveStrength > 1.0f && Near(Emissive(c).value[0], 2.0f),
           "strength only: constant 0.5 linear x 4 = HDR linear 2");
    Expect(Near(c.ext.emissiveOwn.x, 0.5f), "own emission of the constant = its linear colour");

    const PbrMaterial& h = pbr.materials[2];
    Expect(h.emissiveEnable && Near(Emissive(h).value[0], 60.0f) && Near(Emissive(h).value[1], 30.0f),
           "strength only: HDR constant 15 / 7.5 x 4");
    Expect(Near(h.ext.emissiveOwn.x, 15.0f), "own emission of the HDR constant = its linear value");

    Expect(!pbr.materials[3].emissiveEnable, "strength only: a record that does not emit stays dark");
    Expect(pbr.materials[3].ext.emissiveOwn.w == 0.0f, "a record that does not emit has no own emission");
    Expect(!pbr.materials[4].emissiveEnable, "strength only: a PMX material gets a dark stage record");
}

void TestStrengthBelowOne() {
    PmxModel pmx;
    PbrModelData pbr;
    MakeModel(pmx, pbr);
    StageMaterialRule r;
    r.match = "hdr";
    r.emissiveStrength = 0.5f;
    Apply({r}, pmx, pbr);
    const PbrMaterial& h = pbr.materials[2];
    Expect(h.ext.emissiveStrength > 1.0f && Near(Emissive(h).value[0], 7.5f),
           "strength 0.5 on HDR 15: still HDR (7.5), not clamped to 1");
    StageMaterialRule off;
    off.match = "const";
    off.emissiveStrength = 0.0f;
    Apply({off}, pmx, pbr);
    Expect(!pbr.materials[1].emissiveEnable, "strength 0 turns the emission off");
}

void TestColourRule() {
    PmxModel pmx;
    PbrModelData pbr;
    MakeModel(pmx, pbr);
    StageMaterialRule r;
    r.match = "dark";
    r.emissive = DirectX::XMFLOAT3{1.0f, 0.0f, 0.0f};
    Apply({r}, pmx, pbr);
    const PbrMaterial& d = pbr.materials[3];
    Expect(d.emissiveEnable && Near(Emissive(d).value[0], 1.0f) && Near(Emissive(d).value[1], 0.0f) &&
               d.ext.emissiveStrength <= 1.0f,
           "colour rule: sRGB constant red, not HDR");
    StageMaterialRule s;
    s.match = "tex";
    s.emissive = DirectX::XMFLOAT3{0.0f, 1.0f, 0.0f};
    s.emissiveStrength = 3.0f;
    Apply({s}, pmx, pbr);
    const PbrMaterial& t = pbr.materials[0];
    Expect(Near(Emissive(t).value[0], 0.0f) && Near(Emissive(t).value[1], 3.0f), "colour + strength on a map: factor");
}

} // namespace

// A Ray-MMD exponent map (EMISSIVE_MAP_APPLY_SCALE 2: map ^ value): a strength-only rule keeps the exponent and scales
// the result (applyScale 3, strength in the slot's scale), instead of rewriting it as a linear factor.
void TestExponentMap() {
    PmxModel pmx;
    PbrModelData pbr;
    MakeModel(pmx, pbr);
    PbrSlot& te = pbr.materials[0].slots[(int)PbrSlotId::Emissive];
    te.applyScale = 2;
    te.value[0] = te.value[1] = te.value[2] = 2.0f;
    StageMaterialRule r;
    r.match = "tex";
    r.emissiveStrength = 3.0f;
    Apply({r}, pmx, pbr);
    const PbrMaterial& t = pbr.materials[0];
    const PbrSlot& e = Emissive(t);
    Expect(t.emissiveEnable && e.applyScale == 3 && Near(e.value[0], 2.0f) && e.hasScale && Near(e.scale, 3.0f) &&
               t.ext.emissiveStrength > 1.0f,
           "strength only on an exponent map: exponent kept, x3, HDR");
    r.emissiveStrength = 0.5f;
    Apply({r}, pmx, pbr);
    Expect(Emissive(pbr.materials[0]).applyScale == 3 && Near(Emissive(pbr.materials[0]).value[0], 2.0f) &&
               Near(Emissive(pbr.materials[0]).scale, 0.5f),
           "a second strength rule replaces the scale, the exponent stays");
}

// Applying strength rules twice to one record scales its original emission both times (4 then 0.5 = 0.25, not 2),
// and strength 0 followed by 1 brings the emission back.
void TestReapply() {
    PmxModel pmx;
    PbrModelData pbr;
    MakeModel(pmx, pbr);
    StageMaterialRule r;
    r.match = "hdr";
    r.emissiveStrength = 4.0f;
    Apply({r}, pmx, pbr);
    r.emissiveStrength = 0.5f;
    Apply({r}, pmx, pbr);
    Expect(Near(Emissive(pbr.materials[2]).value[0], 7.5f), "re-applied strength scales the original (15 x 0.5)");
    r.emissiveStrength = 0.0f;
    Apply({r}, pmx, pbr);
    r.emissiveStrength = 1.0f;
    Apply({r}, pmx, pbr);
    Expect(pbr.materials[2].emissiveEnable && Near(Emissive(pbr.materials[2]).value[0], 15.0f),
           "strength 0 then 1 restores the original emission");
}

int main() {
    TestStrengthOnly();
    TestReapply();
    TestExponentMap();
    TestStrengthBelowOne();
    TestColourRule();
    printf("%s (%d failures)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
    return g_fails ? 1 : 0;
}
