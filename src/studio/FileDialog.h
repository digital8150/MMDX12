#pragma once
// Native open/save dialogs (IFileOpenDialog / IFileSaveDialog). Each call runs the dialog on its own
// STA thread and blocks until it closes, so it works whatever COM mode the calling thread is in.
#include <Windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx::studio {

struct FileFilter {
    std::wstring name;     // "VMD motion"
    std::wstring pattern;  // "*.vmd"
};

// Empty path when cancelled. `defaultName` is the suggested file name (save) without directory.
std::filesystem::path OpenFileDialog(HWND owner, const std::vector<FileFilter>& filters,
                                     const std::filesystem::path& initialDir = {});
std::filesystem::path SaveFileDialog(HWND owner, const std::vector<FileFilter>& filters, const std::wstring& defaultName,
                                     const std::wstring& defaultExtension, const std::filesystem::path& initialDir = {});

} // namespace mmdx::studio
