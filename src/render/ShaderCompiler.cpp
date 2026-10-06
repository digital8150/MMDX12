#include "render/RenderPass.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>

namespace mmdx {

static ComPtr<ID3DBlob> CompileShaderDxcUncached(const std::filesystem::path& file, const char* entry, const char* target,
                                                 const ShaderDefines& defines, std::string* errors) {
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
    if (!dxc.ok) {
        if (errors) *errors = "dxcompiler.dll not found";
        return {};
    }

    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(dxc.utils->LoadFile(file.c_str(), nullptr, &source))) {
        if (errors) *errors = "cannot read " + PathToUtf8(file);
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
        if (errors && len > 0) errors->assign(text, len);
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


static ComPtr<ID3DBlob> CompileShaderUncached(const std::filesystem::path& file, const char* entry, const char* target,
                                              const ShaderDefines& defines, std::string* errors) {
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
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(file.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                    flags, 0, &code, &errorBlob);
    if (FAILED(hr)) {
        const char* msg = errorBlob ? (const char*)errorBlob->GetBufferPointer() : "";
        const SIZE_T len = errorBlob ? errorBlob->GetBufferSize() : 0;
        if (errors && len > 0) errors->assign(msg, len);
        LOG_ERROR("shader %s:%s failed: %.*s", PathToUtf8(file).c_str(), entry, (int)len, msg);
        return {};
    }
    return code;
}

// ---- compiled shader cache -----------------------------------------------------------------------------------------
// Shaders compile at runtime (seconds at every start). The bytecode is cached in <exe>/shader_cache, keyed by a hash of
// every .hlsl/.hlsli in the shader directory (includes included), the entry, target, defines and the compiler DLL's
// timestamp, so any shader edit invalidates it. Failures are never cached.
namespace {
uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::mutex hashMutex;
std::map<std::filesystem::path, uint64_t> hashes;

uint64_t SourceHash(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(hashMutex);
    auto it = hashes.find(dir);
    if (it != hashes.end()) return it->second;
    uint64_t h = 1469598103934665603ull;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto ext = e.path().extension();
        if (e.is_regular_file(ec) && (ext == L".hlsl" || ext == L".hlsli")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const std::string name = PathToUtf8(f.filename());
        h = Fnv(h, name.data(), name.size());
        std::ifstream in(f, std::ios::binary);
        std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        h = Fnv(h, buf.data(), buf.size());
    }
    const auto dll = std::filesystem::last_write_time(ExecutableDir() / L"dxcompiler.dll", ec);
    if (!ec) {
        const auto t = dll.time_since_epoch().count();
        h = Fnv(h, &t, sizeof(t));
    }
    return hashes[dir] = h;
}

std::filesystem::path CachePath(const std::filesystem::path& file, const char* entry, const char* target,
                                const ShaderDefines& defines, bool dxc) {
    uint64_t h = SourceHash(file.parent_path());
    const std::string key = PathToUtf8(file.filename()) + "|" + entry + "|" + target + "|" + (dxc ? "dxc" : "fxc");
    h = Fnv(h, key.data(), key.size());
    for (const auto& d : defines) {
        // a shader pack's surface (quoted include path relative to the shader): its folder's sources are inputs too
        if (d.first == "MMDX_PACK" && d.second.size() > 2) {
            const std::filesystem::path inc = file.parent_path() / Utf8ToPath(d.second.substr(1, d.second.size() - 2));
            const uint64_t ph = SourceHash(inc.parent_path());
            h = Fnv(h, &ph, sizeof(ph));
        }
        h = Fnv(h, d.first.data(), d.first.size());
        h = Fnv(h, "=", 1);
        h = Fnv(h, d.second.data(), d.second.size());
        h = Fnv(h, ";", 1);
    }
#ifdef _DEBUG
    h = Fnv(h, "D", 1);
#endif
    wchar_t name[40];
    swprintf_s(name, L"%016llx.bin", (unsigned long long)h);
    return ExecutableDir() / L"shader_cache" / name;
}

ComPtr<ID3DBlob> CacheLoad(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize n = in.tellg();
    if (n <= 0) return {};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob((SIZE_T)n, &blob))) return {};
    in.seekg(0);
    if (!in.read((char*)blob->GetBufferPointer(), n)) return {};
    return blob;
}

void CacheStore(const std::filesystem::path& path, ID3DBlob* blob) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // unique tmp name: shaders may compile on several threads
    std::filesystem::path tmp = path;
    tmp += L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        out.write((const char*)blob->GetBufferPointer(), (std::streamsize)blob->GetBufferSize());
        if (!out) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

ComPtr<ID3DBlob> Cached(bool dxc, const std::filesystem::path& file, const char* entry, const char* target,
                        const ShaderDefines& defines, std::string* errors) {
    const std::filesystem::path path = CachePath(file, entry, target, defines, dxc);
    if (ComPtr<ID3DBlob> hit = CacheLoad(path)) return hit;
    ComPtr<ID3DBlob> blob = dxc ? CompileShaderDxcUncached(file, entry, target, defines, errors)
                                : CompileShaderUncached(file, entry, target, defines, errors);
    if (blob) CacheStore(path, blob.Get());
    return blob;
}
} // namespace

void ResetShaderSourceHashes() {
    std::lock_guard<std::mutex> lock(hashMutex);
    hashes.clear();
}

ComPtr<ID3DBlob> CompileShaderDxc(const std::filesystem::path& file, const char* entry, const char* target,
                                  const ShaderDefines& defines, std::string* errors) {
    return Cached(true, file, entry, target, defines, errors);
}

ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const char* entry, const char* target,
                               const ShaderDefines& defines, std::string* errors) {
    return Cached(false, file, entry, target, defines, errors);
}

} // namespace mmdx
