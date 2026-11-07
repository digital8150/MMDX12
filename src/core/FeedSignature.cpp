#include "core/FeedSignature.h"

#include <Windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <json.hpp>
#include "core/TextUtil.h"

namespace mmdx::net {
namespace {

constexpr size_t kMinModulusBytes = 2048 / 8;

bool VerifyRsaPssSha256(std::string_view message, const std::vector<uint8_t>& signature,
                        const std::vector<uint8_t>& modulus) {
    if (modulus.size() < kMinModulusBytes || signature.size() != modulus.size()) return false;

    const uint8_t exponent[3] = {1, 0, 1};
    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = (ULONG)modulus.size() * 8;
    header.cbPublicExp = sizeof(exponent);
    header.cbModulus = (ULONG)modulus.size();
    std::vector<uint8_t> blob(sizeof(header) + sizeof(exponent) + modulus.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), exponent, sizeof(exponent));
    std::memcpy(blob.data() + sizeof(header) + sizeof(exponent), modulus.data(), modulus.size());

    BCRYPT_ALG_HANDLE rsa = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&rsa, BCRYPT_RSA_ALGORITHM, nullptr, 0))) return false;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptImportKeyPair(rsa, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0))) {
        uint8_t digest[32];
        BCRYPT_ALG_HANDLE sha = nullptr;
        if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            const bool hashed = BCRYPT_SUCCESS(BCryptHash(sha, nullptr, 0, (PUCHAR)message.data(), (ULONG)message.size(),
                                                          digest, sizeof(digest)));
            BCryptCloseAlgorithmProvider(sha, 0);
            if (hashed) {
                BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, 32};
                ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, &padding, digest, sizeof(digest), (PUCHAR)signature.data(),
                                                          (ULONG)signature.size(), BCRYPT_PAD_PSS));
            }
        }
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(rsa, 0);
    return ok;
}

}  // namespace

bool OpenSignedFeed(std::string_view body, const char* modulusBase64, std::string& payload, std::string& error) {
    payload.clear();
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "invalid feed response";
        return false;
    }
    const auto signedFeed = j.find("signedFeed");
    const auto signature = j.find("signature");
    if (signedFeed == j.end() || signature == j.end() || !signedFeed->is_string() || !signature->is_string()) {
        error = "feed is not signed";
        return false;
    }
    const std::vector<uint8_t> bytes = Base64Decode(signedFeed->get<std::string>());
    const std::string_view message((const char*)bytes.data(), bytes.size());
    if (!VerifyRsaPssSha256(message, Base64Decode(signature->get<std::string>()), Base64Decode(modulusBase64))) {
        error = "feed signature is not valid";
        return false;
    }
    payload.assign(message);
    return true;
}

}  // namespace mmdx::net
