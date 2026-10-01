#include <Windows.h>
#include <shellapi.h>
#include <cstdio>

#include "app/App.h"

#pragma comment(lib, "shell32.lib")

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    mmdx::AppOptions opt = mmdx::ParseCommandLine(argc, argv);
    LocalFree(argv);
    mmdx::App app;
    return app.Run(inst, opt);
}
