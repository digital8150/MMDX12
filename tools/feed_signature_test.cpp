// Signed update feeds (core/FeedSignature.h). The vectors came from `openssl dgst -sha256 -sigopt rsa_padding_mode:pss
// -sigopt rsa_pss_saltlen:32 -sign` (the command the nightly workflow uses) with a throwaway key whose private half was
// deleted. Also the dotted version order a nightly channel depends on.
#include <cstdio>
#include <string>

#include "core/FeedSignature.h"
#include "core/NetUtil.h"

using namespace mmdx::net;

static const char* kModulus =
    "oaUcCQJ0kafVGe9f/nv1BotpIBXeaNaBGNGAZWWh9kRTtyH0dE23QMsO6h8gYD6TZ2ggXyw/dqEUrsRuSbIA7zPw+eO5Z2KWe5OZu5eFVzTuShYL"
    "6zcDafM1bad5usTyaKDL0XeDx6mI5YcNySPKqgPRep0qgICu7osxqyMm3BR/Ck3x2TF/COqIdkmqMBMl4iRr20SVuj/aO0wTjODGpW94m78QWiAM"
    "BzJeejwkm+gw/sMI3vS7Mpl+Tkn6E8tlFaekOtqh8hO25jHQ92nLIGW/HK+ubOoV0LEXGBuR6Zbz2E7HWvqglZcuvYtkF9+Uj7d6B2uqpYzwb0MH"
    "iHNuJ+h9ZV7z8mwgh+iakqSTlImRB8s4g5mSzykDJnctE6nkDn6Nh7reScX5FChJhQMr992TjtGSnJxj8YIrOwFjeSipHKhIk0aTZrnTdovPoTGP"
    "9AKrNCCmVd5h1tU3LQhLxImUDkpwU4IbEEaGaxBseDWnIHkFkaACXIotK0DBSag1";
static const char* kSignature =
    "oJijqP034fVxPkr4AZh5ll/Y8G+s54+04jNAT3PijiQvHTUEzcwZW5iBBcfMszaEsyAn6tzMD87SdE+AezoFJXBxcKXU/pJMZfFWNR2vUXwyiurL"
    "UaQToF/2jUlR62XH8b0t4PbnhSpaSTJtkBWrbreLqvIzcRwj7mKmr0D8JFKF7J3dKLRN2P+6SIDRof4KDLjODbZ1MqNnwnFJAN4sIOGB1ZX58kWR"
    "q/01EGIdfFXll9sFiKG4QRIwfo9bXnWKKLogttOEMK8cd1NSmpyN6rFI+lCyCZ6OvdoRRYwVeOjQLFO6cd6Jn6idx6EO7yEipQ7jt8/XyNwLimZn"
    "KAfEmwlgV4jRbsUmoZAXQCGKWtT40KwvQ9V7lx/l94FwUTeBVL8iBnBqiy50j7s6JMdBkUmEHILnGYBBqsGu+RppYcJvfcn4OKRv7Ca4LHZ+dXRX"
    "ps3E4adRtfIcUfNHklkvHSwEQy+oa/vVfxbAzLYeJluR/TwmWbJ1KFzXjZMiF+43";
static const char* kPayloadB64 = "eyJ2ZXJzaW9uIjoiMy40LjAtbmlnaHRseS43IiwidXJsIjoiaHR0cHM6Ly9leGFtcGxlLmNvbS9hLnppcCJ9";
static const char* kPayload = "{\"version\":\"3.4.0-nightly.7\",\"url\":\"https://example.com/a.zip\"}";
// The same payload with another url: what a tampered server would send.
static const char* kTamperedB64 = "eyJ2ZXJzaW9uIjoiMy40LjAtbmlnaHRseS43IiwidXJsIjoiaHR0cHM6Ly9ldmlsLmV4YW1wbGUvYS56aXAifQ==";
// A real 3072-bit modulus that is not the signer's: a signature check against it must fail.
static const char* kOtherModulus =
    "tG/O+zvNyVa1IT5RT9IFx/G+pq2sfp2rfzWCRMH9mWZBOCOO4HmyD3YpOt+PifdSuNOJBG2N1BUVIBh0w45V846goGrRDDSHx9hcNjNI6g8+DGYj"
    "REXU3PSfyK7bhWfxOff3wpQW+5AgkhAwCIYh9UdZk5loudvgnp7KCfyen+Mc/U379MQRpUHu7Vpj8NdU7xO1WlA3QDSBt4R7U1S9HyZS6oRQgAXs"
    "vIFDbOgLLsflH0QRBYbsizGpncIxaQjX1jghJ1weD0SMAZC7imHjDWGp6aFDm4wauoE1+mZLlYEtO5ZvyjpTccPwWy2cz4v/jETuyYqnwrsdA/oz"
    "J+SZG4OFOHZP63j314QkdiDSeZ1eENvmKGGA/T6RwmFv0YSGgraByYaWzxdLyOix3/NpHvEJ7I4frAB/Alb4AkRhNm2uq9rRJvaR0en/qbiqfikZ"
    "ebGpVf2f+7w52SYUUbD5nvzqM7gzrNmQ1LobTn3KBLoll3l7yCM1G1muuyGrMgY7";

static int g_failed = 0;

static void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failed;
}

static std::string Feed(const std::string& signedFeed, const std::string& signature) {
    return "{\"version\":\"9.9.9\",\"signedFeed\":\"" + signedFeed + "\",\"signature\":\"" + signature + "\"}";
}

static bool Refused(const std::string& body, const char* modulus = kModulus) {
    std::string payload, error;
    return !OpenSignedFeed(body, modulus, payload, error) && payload.empty() && !error.empty();
}

int main() {
    std::string payload, error;
    Check(OpenSignedFeed(Feed(kPayloadB64, kSignature), kModulus, payload, error) && payload == kPayload,
          "a signed feed opens, and only its signed bytes come out");

    Check(Refused(Feed(kTamperedB64, kSignature)), "a changed payload is refused");
    std::string flipped = kSignature;
    flipped[10] = flipped[10] == 'A' ? 'B' : 'A';
    Check(Refused(Feed(kPayloadB64, flipped)), "a changed signature is refused");
    Check(Refused(Feed(kPayloadB64, kSignature), kOtherModulus), "another key's signature is refused");
    Check(Refused(Feed(kPayloadB64, "")), "an empty signature is refused");
    Check(Refused(Feed("", kSignature)), "an empty payload is refused");
    Check(Refused(Feed(kPayloadB64, std::string(kSignature).substr(0, 200))), "a short signature is refused");
    Check(Refused("{\"version\":\"9.9.9\",\"url\":\"https://evil.example/a.zip\"}"), "an unsigned feed is refused");
    Check(Refused("{\"signedFeed\":1,\"signature\":2}"), "non-string fields are refused");
    Check(Refused("not json"), "a body that is not JSON is refused");
    Check(Refused("[]"), "a body that is not an object is refused");
    Check(Refused(Feed(kPayloadB64, kSignature), "AQAB"), "a tiny public key is refused");

    // The order the nightly channel relies on.
    Check(CompareVersions("3.4.0-nightly.10", "3.4.0-nightly.9") > 0, "nightly.10 is newer than nightly.9");
    Check(CompareVersions("3.5.0-nightly.1", "3.4.0-nightly.99") > 0, "a higher base wins over the build number");
    Check(CompareVersions("3.4.0", "3.4.0-nightly.1") <= 0, "a pack's minAppVersion 3.4.0 fits a 3.4.0 nightly");
    Check(CompareVersions("3.5.0", "3.4.0-nightly.1") > 0, "a pack's minAppVersion 3.5.0 does not");

    std::printf(g_failed ? "FAILED\n" : "ALL PASS\n");
    return g_failed ? 1 : 0;
}
