#include "studio/FileDialog.h"
#include <ShObjIdl.h>
#include <thread>
#include <wrl/client.h>

namespace mmdx::studio {

namespace {
using Microsoft::WRL::ComPtr;

std::filesystem::path RunDialog(HWND owner, bool save, const std::vector<FileFilter>& filters,
                                const std::wstring& defaultName, const std::wstring& defaultExt,
                                const std::filesystem::path& initialDir) {
    std::filesystem::path result;
    std::thread t([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        {
            ComPtr<IFileDialog> dlg;
            HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))
                              : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
            if (SUCCEEDED(hr)) {
                std::vector<COMDLG_FILTERSPEC> specs;
                for (const FileFilter& f : filters) specs.push_back({f.name.c_str(), f.pattern.c_str()});
                if (!specs.empty()) dlg->SetFileTypes((UINT)specs.size(), specs.data());
                DWORD opts = 0;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
                if (!defaultName.empty()) dlg->SetFileName(defaultName.c_str());
                if (!defaultExt.empty()) dlg->SetDefaultExtension(defaultExt.c_str());
                if (!initialDir.empty()) {
                    ComPtr<IShellItem> folder;
                    if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&folder))))
                        dlg->SetFolder(folder.Get());
                }
                if (SUCCEEDED(dlg->Show(owner))) {
                    ComPtr<IShellItem> item;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        result = path;
                        CoTaskMemFree(path);
                    }
                }
            }
        }
        CoUninitialize();
    });
    // Keep pumping the owner's messages while waiting: the dialog sends WM_ENABLE etc. to its owner
    // synchronously, which would deadlock against a plain join().
    HANDLE h = (HANDLE)t.native_handle();
    while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    t.join();
    return result;
}
} // namespace

std::filesystem::path OpenFileDialog(HWND owner, const std::vector<FileFilter>& filters,
                                     const std::filesystem::path& initialDir) {
    return RunDialog(owner, false, filters, {}, {}, initialDir);
}

std::filesystem::path SaveFileDialog(HWND owner, const std::vector<FileFilter>& filters, const std::wstring& defaultName,
                                     const std::wstring& defaultExtension, const std::filesystem::path& initialDir) {
    return RunDialog(owner, true, filters, defaultName, defaultExtension, initialDir);
}

} // namespace mmdx::studio
