#include "core/SystemShutdown.h"

#include <windows.h>
#include <reason.h>
#include <winreg.h>

namespace mmdx {

namespace {

// The privilege exists on every normal account but starts disabled in the process token.
bool EnableShutdownPrivilege(DWORD& lastError) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        lastError = GetLastError();
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) != FALSE;
    if (ok) {
        // AdjustTokenPrivileges can return TRUE without assigning: GetLastError tells them apart.
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        ok = GetLastError() == ERROR_SUCCESS;
    }
    if (!ok) lastError = GetLastError();
    CloseHandle(token);
    return ok;
}

}  // namespace

bool RequestSystemShutdown(std::string* error) {
    DWORD code = ERROR_SUCCESS;
    if (EnableShutdownPrivilege(code)) {
        code = InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_POWEROFF,
                                 SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    }
    if (code == ERROR_SUCCESS) return true;
    if (error) *error = "Windows error " + std::to_string((unsigned long)code);
    return false;
}

} // namespace mmdx
