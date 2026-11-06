#pragma once
#include <string>

namespace mmdx {

// Asks Windows to power the machine off now. Running applications get the normal end-session messages and nothing is
// forced, so another program's unsaved-work prompt can still hold the shutdown. False = Windows refused; `error` then
// says why.
bool RequestSystemShutdown(std::string* error = nullptr);

} // namespace mmdx
