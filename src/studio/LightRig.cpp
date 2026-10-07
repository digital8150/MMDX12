// LightRig helpers: project-file names, spot names, key sampling. See LightRig.h.
#include "studio/LightRig.h"

namespace mmdx::studio {

const char* LightSourceName(LightSource s) {
    switch (s) {
    case LightSource::Preset: return "preset";
    case LightSource::Custom: return "custom";
    case LightSource::VmdTrack: break;
    }
    return "vmd";
}

bool ParseLightSource(const std::string& s, LightSource& out) {
    if (s == "vmd") { out = LightSource::VmdTrack; return true; }
    if (s == "preset") { out = LightSource::Preset; return true; }
    if (s == "custom") { out = LightSource::Custom; return true; }
    return false;
}

const char* SpotModeName(uint8_t mode) {
    switch (mode) {
    case 1: return "center";
    case 2: return "head";
    case 3: return "manual";
    case 0: break;
    }
    return "auto";
}

bool ParseSpotMode(const std::string& s, uint8_t& out) {
    if (s == "auto") { out = 0; return true; }
    if (s == "center") { out = 1; return true; }
    if (s == "head") { out = 2; return true; }
    if (s == "manual") { out = 3; return true; }
    return false;
}

bool LightRig::AddSpot() {
    if (spots.size() >= kMaxRigSpots) return false;
    SpotLight s;
    int n = 1;
    for (;;) {
        bool used = false;
        for (const SpotLight& o : spots)
            if (o.name == "\xEC\x8A\xA4\xED\x8C\x9F " + std::to_string(n)) used = true;  // 스팟
        if (!used) break;
        ++n;
    }
    s.name = "\xEC\x8A\xA4\xED\x8C\x9F " + std::to_string(n);  // 스팟
    s.swingPhase = (float)spots.size() * 1.3f;                 // the concert preset's phase pattern
    s.coneInner = s.coneOuter * 0.6f;
    spots.push_back(std::move(s));
    return true;
}

SpotKf SampleSpotKeys(const SpotLight& s, int frame) {
    SpotKf out;
    out.position = s.position;
    out.aim = s.aim;
    out.color = s.color;
    out.intensity = s.intensity;
    out.coneOuter = s.coneOuter;
    out.frame = frame;
    if (s.keys.empty()) return out;
    if (frame <= s.keys.front().frame) { SpotKf k = s.keys.front(); k.frame = frame; return k; }
    if (frame >= s.keys.back().frame) { SpotKf k = s.keys.back(); k.frame = frame; return k; }
    size_t i = 0;
    while (i + 1 < s.keys.size() && s.keys[i + 1].frame <= frame) ++i;
    if (s.keys[i].frame == frame) return s.keys[i];
    const SpotKf& a = s.keys[i];
    const SpotKf& b = s.keys[i + 1];
    const float t = (float)(frame - a.frame) / (float)(b.frame - a.frame);
    out.position = {a.position.x + (b.position.x - a.position.x) * t, a.position.y + (b.position.y - a.position.y) * t,
                    a.position.z + (b.position.z - a.position.z) * t};
    out.aim = {a.aim.x + (b.aim.x - a.aim.x) * t, a.aim.y + (b.aim.y - a.aim.y) * t,
               a.aim.z + (b.aim.z - a.aim.z) * t};
    out.color = {a.color.x + (b.color.x - a.color.x) * t, a.color.y + (b.color.y - a.color.y) * t,
                 a.color.z + (b.color.z - a.color.z) * t};
    out.intensity = a.intensity + (b.intensity - a.intensity) * t;
    out.coneOuter = a.coneOuter + (b.coneOuter - a.coneOuter) * t;
    return out;
}

} // namespace mmdx::studio
