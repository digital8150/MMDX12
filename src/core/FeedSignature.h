#pragma once
// Signed update feeds: what the app installs must not depend on the update server being honest. The feed carries
// every hash the updater checks, so whoever can change the feed can change the files; a signature made away from the
// server, checked against a public key inside the exe, closes that.
//
// The feed file is the plain feed fields plus
//   "signedFeed": base64 of the feed JSON the app really reads,
//   "signature":  base64 RSA-PSS (SHA-256, 32-byte salt) over exactly those bytes.
// The outer fields are ignored once a key is set: only the signed bytes count.
#include <string>
#include <string_view>

namespace mmdx::net {

// `modulusBase64`: the RSA public key's modulus, big endian (exponent 65537). True = `payload` holds the signed JSON.
// Any other input (not JSON, fields missing, wrong signature) = false + `error`, and `payload` stays empty.
bool OpenSignedFeed(std::string_view body, const char* modulusBase64, std::string& payload, std::string& error);

}  // namespace mmdx::net
