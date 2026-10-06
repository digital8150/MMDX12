// Offscreen thumbnail rendering for the library cards (callback of ThumbnailCache).
#include "app/App.h"
#include "app/Lighting.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <ShlObj.h>
#include <algorithm>
#include <cmath>

namespace mmdx {

std::filesystem::path App::ThumbnailCacheDir() const {
    PWSTR local = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        dir = std::filesystem::path(local) / L"MMDX12" / L"thumbs";
    else
        dir = ExecutableDir() / L"cache" / L"thumbs";
    if (local) CoTaskMemFree(local);
    return dir;
}

bool App::RenderThumbnail(ThumbnailKind kind, std::vector<LoadedModelCpu>& models, ImageRGBA8& out) {
    using namespace DirectX;
    std::vector<std::unique_ptr<ModelInstance>> instances;
    std::vector<std::unique_ptr<GpuModel>> gpus;
    {
        UploadBatch batch(ctx_);
        for (LoadedModelCpu& m : models) {
            if (!m.pmx) continue;
            auto inst = std::make_unique<ModelInstance>(m.pmx);
            inst->UpdatePose();
            auto gpu = renderer_.CreateModel(batch, *m.pmx, m.textures,
                                             kind == ThumbnailKind::Stage ? ModelRole::Stage : ModelRole::Character);
            if (!gpu) continue;
            instances.push_back(std::move(inst));
            gpus.push_back(std::move(gpu));
        }
        batch.Submit();
    }
    if (gpus.empty()) return false;

    FrameView view;
    XMFLOAT3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i < gpus.size(); ++i) {
        gpus[i]->UpdateSkinning(0, instances[i]->SkinMatrices());
        gpus[i]->UpdateMorphs(0, instances[i]->VertexMorphDeltas(), instances[i]->MorphVersion());
        gpus[i]->UpdateMaterials(0, instances[i]->MaterialMul(), instances[i]->MaterialAdd(), instances[i]->MaterialVersion());
        view.models.push_back(gpus[i].get());
        const XMFLOAT3 a = gpus[i]->BoundsMin(), b = gpus[i]->BoundsMax();
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
    }
    BuildLighting(LightingPreset::Studio, 0.0, {0, 10, 0}, view.light);
    view.light.direction = {-0.35f, -0.8f, 0.75f};  // a touch more frontal for portraits

    uint32_t w = 0, h = 0;
    XMVECTOR eye, target;
    if (kind == ThumbnailKind::Character) {
        // Portrait: head and shoulders, 3:4, framed on the head bone when the model has one
        // (mesh bounds are thrown off by long hair, wings and props).
        w = 384;
        h = 512;
        const float height = std::max(hi.y - lo.y, 1.0f);
        float cx = (lo.x + hi.x) * 0.5f, cz = std::min((lo.z + hi.z) * 0.5f, lo.z + 3.0f);
        float headY = hi.y - height * 0.08f;
        const int headBone = instances[0]->Model().FindBone("\xE9\xA0\xAD");  // 頭
        if (headBone >= 0) {
            const XMFLOAT3 hp = instances[0]->BoneWorldPosition(headBone);
            cx = hp.x;
            cz = hp.z;
            headY = hp.y;
        }
        const float standing = std::max(headY - lo.y, 1.0f);
        const float frameH = std::clamp(standing * 0.44f, 3.0f, 40.0f);
        const float cy = headY - frameH * 0.09f;
        view.camera.fovYRadians = XMConvertToRadians(22.0f);
        const float dist = frameH * 0.5f / std::tan(view.camera.fovYRadians * 0.5f) + 2.0f;
        target = XMVectorSet(cx, cy, cz, 1);
        eye = XMVectorSet(cx, cy + dist * 0.05f, cz - dist, 1);
        view.camera.nearZ = std::max(0.1f, dist * 0.05f);
        view.camera.farZ = dist * 20.0f;
    } else {
        // Stage: the audience's view of where the performer stands.
        w = 512;
        h = 320;
        view.camera.fovYRadians = XMConvertToRadians(42.0f);
        target = XMVectorSet(0, 9, 0, 1);
        eye = XMVectorSet(0, 20, -62, 1);
        view.camera.nearZ = 0.5f;
        view.camera.farZ = 4000.0f;
    }
    XMStoreFloat4x4(&view.camera.view, XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat3(&view.camera.eye, eye);
    view.cameraCut = true;

    const bool ok = renderer_.RenderToImage(view, w, h, out);
    for (auto& g : gpus) g->Destroy();  // the GPU is idle: RenderToImage waited
    return ok;
}

} // namespace mmdx
