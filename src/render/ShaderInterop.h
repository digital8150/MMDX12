#pragma once
// C++ mirror of the constant-buffer / vertex layouts used by shaders/*.hlsl.
// Keep both sides in sync. HLSL uses `#pragma pack_matrix(row_major)` and mul(v, M), so
// DirectXMath matrices are uploaded as-is (no transpose).
#include <DirectXMath.h>
#include <cstdint>

namespace mmdx {

struct SceneConstants {               // b0, 256-byte aligned slot per frame
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT3 eyePos;      float _pad0;
    DirectX::XMFLOAT3 lightDir;    float _pad1;   // normalized, pointing from light toward scene
    DirectX::XMFLOAT3 lightColor;  float _pad2;
    DirectX::XMFLOAT2 viewportSize; float edgeScale; float _pad3;  // edgeScale = viewportHeight / 1080
};
static_assert(sizeof(SceneConstants) == 256, "SceneConstants layout");

enum MaterialShaderFlags : uint32_t {
    MatFlag_HasTexture = 1u << 0,
    MatFlag_HasToon    = 1u << 1,
    MatFlag_SphereMul  = 1u << 2,
    MatFlag_SphereAdd  = 1u << 3,
};

struct MaterialConstants {            // b1, one 256-byte slot per material
    DirectX::XMFLOAT4 diffuse;
    DirectX::XMFLOAT3 specular;  float specularPower;
    DirectX::XMFLOAT3 ambient;   float edgeSize;
    DirectX::XMFLOAT4 edgeColor;
    uint32_t flags;              // MaterialShaderFlags
    uint32_t _pad[3];
    uint8_t _reserve[256 - 80];
};
static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants layout");

// Vertex buffer slot 0 (static, DEFAULT heap). 60 bytes.
struct GpuVertex {
    float position[3];   // POSITION      R32G32B32_FLOAT
    float normal[3];     // NORMAL        R32G32B32_FLOAT
    float uv[2];         // TEXCOORD0     R32G32_FLOAT
    uint16_t bones[4];   // BLENDINDICES  R16G16B16A16_UINT  (unused slots = 0 with weight 0)
    float weights[4];    // BLENDWEIGHT   R32G32B32A32_FLOAT (sum = 1)
    float edgeScale;     // TEXCOORD1     R32_FLOAT
};
static_assert(sizeof(GpuVertex) == 60, "GpuVertex layout");
// Vertex buffer slot 1 (per frame slot, UPLOAD heap): float3 morph position delta, TEXCOORD2.

} // namespace mmdx
