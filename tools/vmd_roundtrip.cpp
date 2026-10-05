#include "asset/VmdMotion.h"
#include "asset/VpdFile.h"
#include "asset/BinaryReader.h"
#include "core/TextUtil.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>

namespace {

using mmdx::VmdMotion;

bool CompareMotion(const VmdMotion& a, const VmdMotion& b, std::string& diff) {
    if (a.modelName != b.modelName) {
        diff = "modelName";
        return false;
    }
    if (a.boneKeys.size() != b.boneKeys.size()) {
        diff = "boneKeys.size()";
        return false;
    }
    for (size_t i = 0; i < a.boneKeys.size(); ++i) {
        const mmdx::VmdBoneKey& x = a.boneKeys[i];
        const mmdx::VmdBoneKey& y = b.boneKeys[i];
        if (x.boneName != y.boneName) { diff = "boneKeys[" + std::to_string(i) + "].boneName"; return false; }
        if (memcmp(&x.frame, &y.frame, sizeof(x.frame)) != 0) { diff = "boneKeys[" + std::to_string(i) + "].frame"; return false; }
        if (memcmp(&x.translation, &y.translation, sizeof(x.translation)) != 0) { diff = "boneKeys[" + std::to_string(i) + "].translation"; return false; }
        if (memcmp(&x.rotation, &y.rotation, sizeof(x.rotation)) != 0) { diff = "boneKeys[" + std::to_string(i) + "].rotation"; return false; }
        if (memcmp(x.interp, y.interp, sizeof(x.interp)) != 0) { diff = "boneKeys[" + std::to_string(i) + "].interp"; return false; }
    }
    if (a.morphKeys.size() != b.morphKeys.size()) {
        diff = "morphKeys.size()";
        return false;
    }
    for (size_t i = 0; i < a.morphKeys.size(); ++i) {
        const mmdx::VmdMorphKey& x = a.morphKeys[i];
        const mmdx::VmdMorphKey& y = b.morphKeys[i];
        if (x.morphName != y.morphName) { diff = "morphKeys[" + std::to_string(i) + "].morphName"; return false; }
        if (memcmp(&x.frame, &y.frame, sizeof(x.frame)) != 0) { diff = "morphKeys[" + std::to_string(i) + "].frame"; return false; }
        if (memcmp(&x.weight, &y.weight, sizeof(x.weight)) != 0) { diff = "morphKeys[" + std::to_string(i) + "].weight"; return false; }
    }
    if (a.cameraKeys.size() != b.cameraKeys.size()) {
        diff = "cameraKeys.size()";
        return false;
    }
    for (size_t i = 0; i < a.cameraKeys.size(); ++i) {
        const mmdx::VmdCameraKey& x = a.cameraKeys[i];
        const mmdx::VmdCameraKey& y = b.cameraKeys[i];
        if (memcmp(&x.frame, &y.frame, sizeof(x.frame)) != 0) { diff = "cameraKeys[" + std::to_string(i) + "].frame"; return false; }
        if (memcmp(&x.distance, &y.distance, sizeof(x.distance)) != 0) { diff = "cameraKeys[" + std::to_string(i) + "].distance"; return false; }
        if (memcmp(&x.target, &y.target, sizeof(x.target)) != 0) { diff = "cameraKeys[" + std::to_string(i) + "].target"; return false; }
        if (memcmp(&x.rotation, &y.rotation, sizeof(x.rotation)) != 0) { diff = "cameraKeys[" + std::to_string(i) + "].rotation"; return false; }
        if (memcmp(x.interp, y.interp, sizeof(x.interp)) != 0) { diff = "cameraKeys[" + std::to_string(i) + "].interp"; return false; }
        if (memcmp(&x.fovDeg, &y.fovDeg, sizeof(x.fovDeg)) != 0) { diff = "cameraKeys[" + std::to_string(i) + "].fovDeg"; return false; }
        if (x.perspective != y.perspective) { diff = "cameraKeys[" + std::to_string(i) + "].perspective"; return false; }
    }
    if (a.lightKeys.size() != b.lightKeys.size()) {
        diff = "lightKeys.size()";
        return false;
    }
    for (size_t i = 0; i < a.lightKeys.size(); ++i) {
        const mmdx::VmdLightKey& x = a.lightKeys[i];
        const mmdx::VmdLightKey& y = b.lightKeys[i];
        if (memcmp(&x.frame, &y.frame, sizeof(x.frame)) != 0) { diff = "lightKeys[" + std::to_string(i) + "].frame"; return false; }
        if (memcmp(&x.color, &y.color, sizeof(x.color)) != 0) { diff = "lightKeys[" + std::to_string(i) + "].color"; return false; }
        if (memcmp(&x.direction, &y.direction, sizeof(x.direction)) != 0) { diff = "lightKeys[" + std::to_string(i) + "].direction"; return false; }
    }
    if (a.shadowKeys.size() != b.shadowKeys.size()) {
        diff = "shadowKeys.size()";
        return false;
    }
    for (size_t i = 0; i < a.shadowKeys.size(); ++i) {
        const mmdx::VmdShadowKey& x = a.shadowKeys[i];
        const mmdx::VmdShadowKey& y = b.shadowKeys[i];
        if (memcmp(&x.frame, &y.frame, sizeof(x.frame)) != 0) { diff = "shadowKeys[" + std::to_string(i) + "].frame"; return false; }
        if (memcmp(&x.mode, &y.mode, sizeof(x.mode)) != 0) { diff = "shadowKeys[" + std::to_string(i) + "].mode"; return false; }
        if (memcmp(&x.distance, &y.distance, sizeof(x.distance)) != 0) { diff = "shadowKeys[" + std::to_string(i) + "].distance"; return false; }
    }
    if (a.ikKeys.size() != b.ikKeys.size()) {
        diff = "ikKeys.size()";
        return false;
    }
    for (size_t i = 0; i < a.ikKeys.size(); ++i) {
        const mmdx::VmdIkKey& x = a.ikKeys[i];
        const mmdx::VmdIkKey& y = b.ikKeys[i];
        if (memcmp(&x.frame, &y.frame, sizeof(x.frame)) != 0) { diff = "ikKeys[" + std::to_string(i) + "].frame"; return false; }
        if (x.visible != y.visible) { diff = "ikKeys[" + std::to_string(i) + "].visible"; return false; }
        if (x.ikStates != y.ikStates) { diff = "ikKeys[" + std::to_string(i) + "].ikStates"; return false; }
    }
    if (a.maxFrame != b.maxFrame) {
        diff = "maxFrame";
        return false;
    }
    return true;
}

void CollectVmdFiles(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (mmdx::ToLowerAscii(entry.path().extension().string()) == ".vmd") {
            out.push_back(entry.path());
        }
    }
}

int RunRoundtrip(const std::vector<std::filesystem::path>& inputs) {
    std::vector<std::filesystem::path> vmdPaths;
    for (const std::filesystem::path& input : inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(input, ec)) {
            CollectVmdFiles(input, vmdPaths);
        } else {
            vmdPaths.push_back(input);
        }
    }

    int fails = 0;
    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
    for (const std::filesystem::path& p : vmdPaths) {
        const std::string rel = mmdx::PathToUtf8(p);
        VmdMotion original;
        std::string error;
        if (!mmdx::LoadVmd(p, original, &error)) {
            printf("SKIP %s: %s\n", rel.c_str(), error.c_str());
            continue;
        }
        const std::filesystem::path tmp = tmpDir / (p.filename().wstring() + L".vmd_roundtrip.tmp");
        VmdMotion reloaded;
        bool ok = true;
        std::string failReason;
        std::error_code ec;
        if (!mmdx::SaveVmd(tmp, original, &error)) {
            ok = false;
            failReason = "save failed: " + error;
        }
        if (ok && !mmdx::LoadVmd(tmp, reloaded, &error)) {
            ok = false;
            failReason = "reload failed: " + error;
        }
        if (ok) {
            std::string diff;
            if (!CompareMotion(original, reloaded, diff)) {
                ok = false;
                failReason = diff;
            }
        }
        const bool bytesIdentical2 = [&p, &tmp]() {
            std::vector<uint8_t> a;
            std::vector<uint8_t> b;
            std::string err;
            if (!mmdx::ReadWholeFile(p, a, &err) || !mmdx::ReadWholeFile(tmp, b, &err)) return false;
            return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
        }();
        if (ok) {
            printf("OK %s bones=%d morphs=%d cams=%d bytes-identical=%d\n", rel.c_str(),
                   static_cast<int>(original.boneKeys.size()),
                   static_cast<int>(original.morphKeys.size()),
                   static_cast<int>(original.cameraKeys.size()), bytesIdentical2 ? 1 : 0);
        } else {
            printf("FAIL %s bones=%d morphs=%d cams=%d bytes-identical=%d: %s\n", rel.c_str(),
                   static_cast<int>(original.boneKeys.size()),
                   static_cast<int>(original.morphKeys.size()),
                   static_cast<int>(original.cameraKeys.size()), bytesIdentical2 ? 1 : 0,
                   failReason.c_str());
            ++fails;
        }
        std::filesystem::remove(tmp, ec);
    }

    printf("total=%d pass=%d fail=%d\n",
           static_cast<int>(vmdPaths.size()),
           static_cast<int>(vmdPaths.size()) - fails, fails);
    return 0;
}

int RunVpdSelftest() {
    mmdx::VpdPose pose;
    pose.modelName = "初音ミク";
    pose.bones.push_back({"センター", {0.123456f, -1.5f, 3.14159f}, {0.0f, 0.7071068f, 0.0f, 0.7071068f}});
    pose.bones.push_back({"右腕", {1.0f, 2.0f, 3.0f}, {0.2588190f, 0.0f, 0.0f, 0.9659258f}});
    pose.morphs.push_back({"まばたき", 0.75f});

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / L"mmdx_vpd_selftest.tmp";
    std::string error;
    if (!mmdx::SaveVpd(tmp, pose, &error)) {
        printf("VPD FAIL save: %s\n", error.c_str());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return 1;
    }

    std::vector<uint8_t> raw;
    if (!mmdx::ReadWholeFile(tmp, raw, &error)) {
        printf("VPD FAIL read-back: %s\n", error.c_str());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return 1;
    }
    const std::string rawText(reinterpret_cast<const char*>(raw.data()), raw.size());
    if (rawText.find("\r\n") == std::string::npos ||
        rawText.rfind("Vocaloid Pose Data file", 0) != 0) {
        printf("VPD FAIL raw bytes: expected CRLF and \"Vocaloid Pose Data file\" header\n");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return 1;
    }

    mmdx::VpdPose loaded;
    if (!mmdx::LoadVpd(tmp, loaded, &error)) {
        printf("VPD FAIL load: %s\n", error.c_str());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return 1;
    }

    std::string reason;
    bool ok = true;
    if (loaded.modelName != pose.modelName) {
        ok = false;
        reason = "modelName mismatch";
    }
    if (ok && loaded.bones.size() != pose.bones.size()) {
        ok = false;
        reason = "bones count mismatch";
    }
    for (size_t i = 0; ok && i < pose.bones.size(); ++i) {
        const mmdx::VpdBone& x = pose.bones[i];
        const mmdx::VpdBone& y = loaded.bones[i];
        const float tol = 1e-6f;
        if (x.name != y.name ||
            fabsf(y.translation.x - x.translation.x) > tol || fabsf(y.translation.y - x.translation.y) > tol ||
            fabsf(y.translation.z - x.translation.z) > tol ||
            fabsf(y.rotation.x - x.rotation.x) > tol || fabsf(y.rotation.y - x.rotation.y) > tol ||
            fabsf(y.rotation.z - x.rotation.z) > tol || fabsf(y.rotation.w - x.rotation.w) > tol) {
            ok = false;
            reason = "bone mismatch at " + std::to_string(i);
        }
    }
    if (ok && loaded.morphs.size() != pose.morphs.size()) {
        ok = false;
        reason = "morphs count mismatch";
    }
    for (size_t i = 0; ok && i < pose.morphs.size(); ++i) {
        const mmdx::VpdMorph& x = pose.morphs[i];
        const mmdx::VpdMorph& y = loaded.morphs[i];
        if (x.name != y.name || fabsf(y.weight - x.weight) > 1e-6f) {
            ok = false;
            reason = "morph mismatch at " + std::to_string(i);
        }
    }

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (ok) {
        printf("VPD OK\n");
        return 0;
    }
    printf("VPD FAIL %s\n", reason.c_str());
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::filesystem::path> inputs;
    bool vpdSelftest = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--vpd-selftest") vpdSelftest = true;
        else inputs.push_back(arg);
    }

    if (vpdSelftest) return RunVpdSelftest();
    if (inputs.empty()) {
        fprintf(stderr, "usage: vmd_roundtrip <file.vmd | directory> [--vpd-selftest]\n");
        return 2;
    }
    return RunRoundtrip(inputs);
}
