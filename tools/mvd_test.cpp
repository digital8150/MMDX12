#include "asset/VmdMotion.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
using namespace mmdx;
static DirectX::XMFLOAT3 Eye(const VmdCameraKey& k) {
    const auto r = DirectX::XMMatrixRotationRollPitchYaw(-k.rotation.x, -k.rotation.y, -k.rotation.z);
    DirectX::XMFLOAT3 eye;
    DirectX::XMStoreFloat3(
        &eye, DirectX::XMVectorAdd(DirectX::XMLoadFloat3(&k.target),
                                   DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 0, k.distance, 0), r)));
    return eye;
}
static void PrintCamera(const char* label, const VmdCameraKey& k) {
    const auto e = Eye(k);
    printf("%s frame=%u target=(%.4f %.4f %.4f) distance=%.4f rotation=(%.4f %.4f %.4f) fovExact=%.4f perspective=%d "
           "eye=(%.4f %.4f %.4f)\n",
           label, k.frame, k.target.x, k.target.y, k.target.z, k.distance, k.rotation.x, k.rotation.y, k.rotation.z,
           k.fovExact, k.perspective, e.x, e.y, e.z);
}
struct Writer {
    std::vector<unsigned char> b;
    template <class T> void put(T x) {
        auto p = (unsigned char*)&x;
        b.insert(b.end(), p, p + sizeof(x));
    }
    void raw(const void* p, size_t n) {
        auto q = (const unsigned char*)p;
        b.insert(b.end(), q, q + n);
    }
    void str(const char* s, bool wide) {
        int n = (int)strlen(s);
        put<int>(wide ? n * 2 : n);
        for (int i = 0; i < n; i++) {
            put<unsigned char>(s[i]);
            if (wide)
                put<unsigned char>(0);
        }
    }
    void section(int type, int minor, int id, int sz, int n, int extra) {
        put<unsigned char>(type);
        put<unsigned char>(minor);
        put<int>(id);
        put<int>(sz);
        put<int>(n);
        put<int>(extra);
    }
};
int main(int argc, char** argv) {
    if (argc > 1) {
        std::filesystem::path p = argv[1];
        auto load = [](const auto& f) {
            VmdMotion m;
            std::string e;
            if (!LoadMvd(f, m, &e)) {
                printf("FAIL %s\n", e.c_str());
                return false;
            }
            printf("bones %zu morphs %zu cameras %zu lights %zu ik %zu\n", m.boneKeys.size(), m.morphKeys.size(),
                   m.cameraKeys.size(), m.lightKeys.size(), m.ikKeys.size());
            if (!m.cameraKeys.empty()) {
                PrintCamera("first", m.cameraKeys.front());
                PrintCamera("last", m.cameraKeys.back());
            }
            return true;
        };
        if (std::filesystem::is_directory(p)) {
            for (auto& e : std::filesystem::recursive_directory_iterator(p))
                if (e.path().extension() == L".mvd")
                    load(e.path());
            return 0;
        }
        return load(p) ? 0 : 1;
    }
    auto path = std::filesystem::temp_directory_path() / "mmdx_mvd_test.mvd";
    bool ok = true;
    for (bool wide : {false, true}) {
        Writer w;
        char magic[30] = {};
        memcpy(magic, "Motion Vector Data file", 23);
        w.raw(magic, 30);
        w.put<float>(1);
        w.put<unsigned char>(wide ? 0 : 1);
        w.str("model", wide);
        w.str("", wide);
        w.put<float>(60);
        w.str("", wide);
        w.section(0, 0, 0, 0, 2, 0);
        w.put<int>(1);
        w.str("bone", wide);
        w.put<int>(2);
        w.str("morph", wide);
        w.section(0x10, 0, 1, 56, 2, 2);
        w.put<unsigned char>(0);
        w.put<unsigned char>(0);
        for (int layer = 0; layer < 2; layer++) {
            w.put<int>(layer);
            w.put<uint64_t>(60);
            for (float f : {1.f, 2.f, 3.f, 0.f, 0.f, 0.f, 1.f})
                w.put(f);
            for (int j = 0; j < 16; j++) {
                const unsigned char curve[] = {0, 0, 255, 255};
                w.put<unsigned char>(j < 4 ? curve[j] : 10 + j);
            }
        }
        w.section(0x20, 0, 2, 16, 1, 0);
        w.put<uint64_t>(30);
        w.put<float>(.75f);
        w.put<int>(0);
        w.section(0x60, wide ? 3 : 0, 0, wide ? 64 : 61, 1, 0);
        w.put<int>(0);
        w.put<uint64_t>(60);
        for (float f : {10.f, 1.f, 2.f, 3.f, .1f, .2f, .3f, 0.7853981634f})
            w.put(f);
        w.put<unsigned char>(1);
        if (wide) {
            w.put<unsigned char>(0);
            w.put<unsigned char>(0);
            w.put<unsigned char>(0);
        }
        for (int j = 0; j < 16; j++) {
            const unsigned char curve[] = {0, 0, 255, 255};
            w.put<unsigned char>(j < 4 ? curve[j] : (j == 5 ? 250 : 30 + j));
        }
        w.section(0x68, 2, 0, 32, 1, 0);
        w.put<uint64_t>(0);
        w.put<unsigned char>(1);
        w.put<unsigned char>(0);
        for (int j = 0; j < 22; j++)
            w.put<unsigned char>(0);
        w.section(0x70, 0, 0, 33, 1, 0);
        w.put<uint64_t>(90);
        for (float f : {1.f, 2.f, 3.f, .5f, .6f, .7f})
            w.put(f);
        w.put<unsigned char>(1);
        w.section(0x40, 0, 0, 25, 1, 8);
        w.put<int>(1);
        w.put<int>(1);
        w.put<uint64_t>(120);
        w.put<unsigned char>(1);
        for (int j = 0; j < 15; j++)
            w.put<unsigned char>(0);
        w.put<unsigned char>(1);
        w.section(0x99, 0, 0, 4, 1, 0);
        w.put<int>(123);
        w.put<unsigned char>(0xff);
        w.put<unsigned char>(0);
        for (size_t n = 0; n <= w.b.size(); n++) {
            std::ofstream f(path, std::ios::binary);
            f.write((char*)w.b.data(), n);
            f.close();
            VmdMotion m;
            std::string err;
            bool loaded = LoadVmd(path, m, &err);
            if (n == w.b.size()) {
                VmdProbe p;
                ok &= loaded && ProbeVmd(path, p, &err) && m.modelName == "model" && m.boneKeys.size() == 1 &&
                      m.boneKeys[0].frame == 30 && m.boneKeys[0].translation.x == 1 && m.boneKeys[0].interp[0] == 20 &&
                      m.boneKeys[0].interp[4] == 20 && m.boneKeys[0].interp[8] == 107 && m.morphKeys.size() == 1 &&
                      m.morphKeys[0].frame == 15 && m.cameraKeys.size() == 1 && m.cameraKeys[0].distance == -10 &&
                      std::fabs(Eye(m.cameraKeys[0]).x - 1) < 1e-4f && std::fabs(Eye(m.cameraKeys[0]).y - 2) < 1e-4f &&
                      std::fabs(Eye(m.cameraKeys[0]).z - 3) < 1e-4f && m.cameraKeys[0].perspective == !wide &&
                      m.cameraKeys[0].fovDeg == 45 && std::fabs(m.cameraKeys[0].fovExact - 45.f) < 1e-4f &&
                      m.cameraKeys[0].interp[0] == 20 && m.cameraKeys[0].interp[1] == 107 &&
                      m.cameraKeys[0].interp[14] == 127 && m.lightKeys.size() == 1 && m.lightKeys[0].frame == 45 &&
                      m.ikKeys.size() == 1 && m.ikKeys[0].frame == 60 && m.ikKeys[0].ikStates[0].first == "bone" &&
                      p.boneNames.size() == 1;
            }
        }
    }
    std::filesystem::remove(path);
    printf("mvd_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
