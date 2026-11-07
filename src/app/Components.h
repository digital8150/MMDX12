#pragma once
// Optional components: the files that ship in the FULL release zip but not in the "-lite" one
// (v2.0.0, P0.3). A lite install fetches them on demand from the update object store.
//
// THIS LIST IS THE SINGLE SOURCE OF TRUTH. tools/package_release.ps1 strips exactly these paths
// from the lite stage and carries a copy of the same list (see the comment there); keep the two
// in sync. Paths are forward-slash, relative to the install root (the folder holding MMDX12.exe),
// exactly like Updater.h manifest paths.
//
// The app fetches a component from the manifest of the INSTALLED version, whose hash the signed
// feed names (see updater::ComponentFetcher, src/app/Updater.*): the bytes always match this exe.
#include <filesystem>
#include <string>

#include "core/I18n.h"
#include "core/TextUtil.h"

namespace mmdx::components {

struct Def {
    const char* id;            // stable id: "upscaler:dlss", "fonts:cjk", ...
    int upscalerKind;          // UpscalerKind (1 DLSS, 2 FSR, 3 XeSS), 0 for non-upscaler components
    const char* label;         // display label (Korean source string for Tr(), or a brand name)
    const char* const* files;  // install-root-relative forward-slash paths
    int fileCount;
};

inline constexpr const char* kDlssFiles[] = {"nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"};
inline constexpr const char* kFsrFiles[] = {"amd_fidelityfx_loader_dx12.dll",
                                            "amd_fidelityfx_upscaler_dx12.dll"};
inline constexpr const char* kXessFiles[] = {"libxess.dll"};
inline constexpr const char* kCjkFontFiles[] = {
    "assets/fonts/NotoSansCJKjp-Regular.otf", "assets/fonts/NotoSansCJKjp-Bold.otf",
    "assets/fonts/NotoSansCJKsc-Regular.otf", "assets/fonts/NotoSansCJKsc-Bold.otf"};

inline constexpr Def kDlss = {"upscaler:dlss", 1, "NVIDIA DLSS", kDlssFiles, 3};
inline constexpr Def kFsr = {"upscaler:fsr", 2, "AMD FSR", kFsrFiles, 2};
inline constexpr Def kXess = {"upscaler:xess", 3, "Intel XeSS", kXessFiles, 1};
inline constexpr Def kCjk = {"fonts:cjk", 0, TrKey("고품질 CJK 글꼴"), kCjkFontFiles, 4};

inline const Def* const kAll[] = {&kDlss, &kFsr, &kXess, &kCjk};
inline constexpr int kAllCount = (int)(sizeof(kAll) / sizeof(kAll[0]));

inline const Def* Find(const std::string& id) {
    for (const Def* d : kAll)
        if (id == d->id) return d;
    return nullptr;
}

// The component that provides upscaler `kind` (UpscalerKind), or null.
inline const Def* ForUpscaler(int kind) {
    for (const Def* d : kAll)
        if (d->upscalerKind == kind) return d;
    return nullptr;
}

// Absolute path of file `i` of the component next to the running exe.
inline std::filesystem::path FilePath(const Def& d, int i) {
    return ExecutableDir() / Utf8ToPath(d.files[i]);
}

// True when every file of the component is present next to the exe.
inline bool Present(const Def& d) {
    std::error_code ec;
    for (int i = 0; i < d.fileCount; ++i)
        if (!std::filesystem::exists(FilePath(d, i), ec)) return false;
    return true;
}

// True when `relPath` (forward-slash, as in a manifest) is one of the optional component files.
inline bool IsComponentPath(const std::string& relPath) {
    for (const Def* d : kAll)
        for (int i = 0; i < d->fileCount; ++i)
            if (relPath == d->files[i]) return true;
    return false;
}

} // namespace mmdx::components
