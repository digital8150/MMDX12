#pragma once
// Small formatting helpers shared by the screens.
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/I18n.h"

namespace mmdx::ui {

inline std::string Thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

inline std::string MinSec(double seconds) {
    if (seconds < 0) seconds = 0;
    const int t = (int)(seconds + 0.5);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

} // namespace mmdx::ui
