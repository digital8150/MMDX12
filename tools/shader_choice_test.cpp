// shader_choice_test: ShaderChoice::SwitchPack (settings remembered per pack) and the ini round trip.
#include "app/Settings.h"
#include <cstdio>
#include <filesystem>

using namespace mmdx;

static int failures = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++failures;
}

int main() {
    ShaderChoice c;
    c.SwitchPack("hoyo_toon_v2");
    c.params["rampThreshold"] = 0.4f;
    c.textureFolder = "F:/tex/hutao";

    c.SwitchPack("nimble_toon");
    Check(c.pack == "nimble_toon" && c.params.empty() && c.textureFolder.empty(), "a new pack starts clean");
    Check(c.remembered.count("hoyo_toon_v2") == 1, "the previous pack is remembered");
    c.params["rim"] = 1.5f;

    c.SwitchPack("");
    Check(c.pack.empty() && c.params.empty(), "default shading has no params");
    Check(c.remembered.size() == 2, "both packs are remembered while the default shading is selected");

    c.SwitchPack("hoyo_toon_v2");
    Check(c.pack == "hoyo_toon_v2" && c.params.at("rampThreshold") == 0.4f && c.textureFolder == "F:/tex/hutao",
          "coming back restores params and texture folder");
    Check(c.remembered.count("hoyo_toon_v2") == 0 && c.remembered.count("nimble_toon") == 1,
          "the restored pack leaves the memory, the other stays");
    c.SwitchPack("hoyo_toon_v2");
    Check(c.pack == "hoyo_toon_v2" && c.textureFolder == "F:/tex/hutao", "switching to the current pack is a no-op");

    // an untouched pack is not remembered
    ShaderChoice d;
    d.SwitchPack("plain");
    d.SwitchPack("");
    Check(d.remembered.empty(), "a pack with default settings leaves no memo");

    // ini round trip (also with the default shading selected: only memos remain)
    AppSettings s;
    s.SetCharacterShader("model/felix.pmx", c);
    ShaderChoice onlyMemo;
    onlyMemo.remembered["nimble_toon"] = ShaderMemo{{{"rim", 1.5f}}, "F:/tex/nimble"};
    s.SetCharacterShader("model/darko.pmx", onlyMemo);
    s.SetCharacterShader("model/empty.pmx", ShaderChoice{});
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "mmdx12_shader_choice_test.ini";
    Check(s.Save(file), "saved");
    AppSettings t;
    Check(t.Load(file), "loaded");
    const ShaderChoice a = t.CharacterShader("model/felix.pmx");
    Check(a == c, "selected pack + memos survive the ini");
    const ShaderChoice b = t.CharacterShader("model/darko.pmx");
    Check(b.pack.empty() && b.remembered.count("nimble_toon") == 1 &&
              b.remembered.at("nimble_toon").textureFolder == "F:/tex/nimble" &&
              b.remembered.at("nimble_toon").params.at("rim") == 1.5f,
          "a character on the default shading keeps its memos");
    Check(t.characterShaders.count("model/empty.pmx") == 0, "an empty choice is not stored");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
