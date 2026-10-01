#include "render/RenderPass.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(file.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
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
