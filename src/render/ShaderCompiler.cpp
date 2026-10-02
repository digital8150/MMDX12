#include "render/RenderPass.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <d3dcompiler.h>
#include <dxcapi.h>

namespace mmdx {

ComPtr<ID3DBlob> CompileShaderDxc(const std::filesystem::path& file, const char* entry, const char* target,
                                  const ShaderDefines& defines) {
    // dxcompiler.dll ships next to the exe; load it (and the compiler instances) once.
    static struct Dxc {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        ComPtr<IDxcIncludeHandler> include;
        bool ok = false;
        Dxc() {
            HMODULE dxc = LoadLibraryW((ExecutableDir() / L"dxcompiler.dll").c_str());
            if (!dxc) dxc = LoadLibraryW(L"dxcompiler.dll");
            if (!dxc) {
                LOG_ERROR("dxcompiler.dll not found: shader model 6 shaders unavailable");
                return;
            }
            auto create = (DxcCreateInstanceProc)GetProcAddress(dxc, "DxcCreateInstance");
            if (!create) return;
            if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return;
            if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return;
            if (FAILED(utils->CreateDefaultIncludeHandler(&include))) return;
            ok = true;
        }
    } dxc;
    if (!dxc.ok) return {};

    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(dxc.utils->LoadFile(file.c_str(), nullptr, &source))) {
        LOG_ERROR("shader %s: cannot read", PathToUtf8(file).c_str());
        return {};
    }

    std::vector<std::wstring> keep;
    keep.reserve(12 + defines.size() * 2);
    keep.push_back(file.filename().wstring());  // input file (for messages)
    keep.push_back(L"-E");
    keep.push_back(Utf8ToWide(entry));
    keep.push_back(L"-T");
    keep.push_back(Utf8ToWide(target));
    keep.push_back(L"-I");
    keep.push_back(file.parent_path().wstring());
    keep.push_back(L"-HV");
    keep.push_back(L"2018");
    for (const auto& d : defines) {
        keep.push_back(L"-D");
        keep.push_back(Utf8ToWide(d.first) + L"=" + Utf8ToWide(d.second));
    }
#ifdef _DEBUG
    keep.push_back(L"-Od");
    keep.push_back(L"-Zi");
    keep.push_back(L"-Qembed_debug");
#else
    keep.push_back(L"-O3");
#endif
    std::vector<const wchar_t*> args;
    args.reserve(keep.size());
    for (const auto& s : keep) args.push_back(s.c_str());

    DxcBuffer buf{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP};
    ComPtr<IDxcResult> result;
    HRESULT hr = dxc.compiler->Compile(&buf, args.data(), (UINT32)args.size(), dxc.include.Get(),
                                       IID_PPV_ARGS(&result));
    ComPtr<IDxcBlobUtf8> errs;
    const char* text = "";
    SIZE_T len = 0;
    if (result) {
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
        if (errs) {
            text = errs->GetStringPointer();
            len = errs->GetStringLength();
        }
    }
    HRESULT status = E_FAIL;
    if (!result || FAILED(hr) || FAILED(result->GetStatus(&status)) || FAILED(status)) {
        LOG_ERROR("shader %s:%s failed: %.*s", PathToUtf8(file).c_str(), entry, (int)len, text);
        return {};
    }
    if (len > 0) LOG_WARN("shader %s:%s failed: %.*s", PathToUtf8(file).c_str(), entry, (int)len, text);

    ComPtr<IDxcBlob> obj;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj) return {};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(obj->GetBufferSize(), &blob))) return {};
    memcpy(blob->GetBufferPointer(), obj->GetBufferPointer(), obj->GetBufferSize());
    return blob;
}


ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const char* entry, const char* target,
                               const ShaderDefines& defines) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& d : defines) macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(file.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                    flags, 0, &code, &errors);
    if (FAILED(hr)) {
        const char* msg = errors ? (const char*)errors->GetBufferPointer() : "";
        SIZE_T len = errors ? errors->GetBufferSize() : 0;
        LOG_ERROR("shader %s:%s failed: %.*s", PathToUtf8(file).c_str(), entry, (int)len, msg);
        return {};
    }
    return code;
}

} // namespace mmdx
