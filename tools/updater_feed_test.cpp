// What FetchFeed accepts (app/Updater.cpp), checked in a test build against its own key: CMakeLists.txt sets
// MMDX12_FEED_TEST_KEY for this target to the public half of a throwaway key whose private half was deleted (the vectors
// came from tools/sign_feed.py). tools/update_e2e.py and tools/components_e2e.py run the whole flow against the real exe.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/Updater.h"
#include "core/Log.h"

using namespace mmdx::updater;
namespace fs = std::filesystem;

static const char* kGoodPayload =
    "eyJ2ZXJzaW9uIjoiOS45LjkiLCJ1cmwiOiJodHRwczovL2V4YW1wbGUuY29tL2EuemlwIiwic2l6ZSI6Nywic2hhMjU2IjoiYWFhYWFhYWFhYW"
    "FhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYSIsInBhZ2UiOiJodHRwczovL2V4YW1wbGUuY29t"
    "LyIsIm1hbmlmZXN0IjoiaHR0cHM6Ly9leGFtcGxlLmNvbS9tYW5pZmVzdHMvOS45LjkuanNvbiIsIm1hbmlmZXN0U2hhMjU2IjoiYmJiYmJiYm"
    "JiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYiIsIm1hbmlmZXN0cyI6eyI5LjkuOSI6ImJi"
    "YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmIiLCIzLjMuMCI6IkNDQ0NDQ0NDQ0"
    "NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0MifSwibm90ZXMiOnsiZW4iOiJzaWduZWQgbm90"
    "ZSJ9fQ==";
static const char* kGoodSignature =
    "Zp+0J8TZZ3V56+/KDMUGFJkCe4aitT0QfX5Xu5kPPMTbocyVDJm3nR1t2oGIewV8b3uYZ8rDnX+sZpz7ege/wLJujQtYzC/Q/7/mclncY7f85I"
    "8EHATgSDTQ1AHMXuLh7lC/DJnepKMkctbUyVavJIKtlfcO7+RYLuMqevKcQv8UaNnsBrBnqcdGzj+GuDP5ljydYDWzGd7Msx+OmjOpjb2R9T/h"
    "wdRqaAjmtiQmXBvDmiPI9bJOmLq9fbv4UtqW4zgRAMFQmkGwekJ9NhfgYSyzurXctgOxeQtUZT3OG2a8upBjtmkKI2ca/hYG0JfYVu2Kv9IoD0"
    "eTpHkK6O8hFA==";
static const char* kNoManifestPayload =
    "eyJ2ZXJzaW9uIjoiOS45LjkiLCJ1cmwiOiJodHRwczovL2V4YW1wbGUuY29tL2EuemlwIiwic2l6ZSI6Nywic2hhMjU2IjoiYWFhYWFhYWFhYW"
    "FhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYSIsInBhZ2UiOiJodHRwczovL2V4YW1wbGUuY29t"
    "LyIsIm5vdGVzIjp7ImVuIjoic2lnbmVkIG5vdGUifX0=";
static const char* kNoManifestSignature =
    "rvwhn0Tk6MO2P/Lmam5LtA+4VjP68C1TEwPAcykjpemZk+jd11IjyVlqKIX7e4XgE4XtlViDQ8p3Zk+lYkLz9xIOC2ZsRmPXQVccJ0/CchhxPu"
    "iolH1PxJ0eQPdI6vLVBXKlETItC+7xf9X1ILL/2eOLKyNVwvehJmO6UJ//6zM2hYvDYo+aW/O7c4zw4ELrc1s0Q5ZwWSl39vxQcujds6mvIGBQ"
    "Sd7NTH8Qo8Nwk/MOgu6uX8kTpW5tfuDZLZNrtaGDJoeEFjAEpW6/CdUFMH9ddX5bbQHFbcMpoTQCQFe2nQxUlILlaGCjy4YFWfdTA+Eg0KKka2"
    "XtfqtQsF6lmw==";
static const char* kNoZipHashPayload =
    "eyJ2ZXJzaW9uIjoiOS45LjkiLCJ1cmwiOiJodHRwczovL2V4YW1wbGUuY29tL2EuemlwIiwic2l6ZSI6NywicGFnZSI6Imh0dHBzOi8vZXhhbX"
    "BsZS5jb20vIiwibWFuaWZlc3QiOiJodHRwczovL2V4YW1wbGUuY29tL21hbmlmZXN0cy85LjkuOS5qc29uIiwibWFuaWZlc3RTaGEyNTYiOiJi"
    "YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiIiwibWFuaWZlc3RzIjp7IjkuOS"
    "45IjoiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYiIsIjMuMy4wIjoiQ0ND"
    "Q0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQyJ9LCJub3RlcyI6eyJlbiI6InNpZ2"
    "5lZCBub3RlIn19";
static const char* kNoZipHashSignature =
    "muu9qnfPI6U4dMLJ9ct2t32tF4PQsRiEthgfxGPtIY9/5tZ3Qy6aTP4QQQtbqfv93DeEy/3I5L5y9kvBjsFO06SVl4nUmgP9SB1IwfemqWkWWK"
    "VXv9Snz1lQde1ttY4/XYn7T6XhiqYYbDxrUlZ7xixY2uWo0FUjMsbhF78gH50LhQzeXFHF79guSFBF17k1N37gq5lva0F/z94E0Dvn8QooIvFw"
    "Kh1eqa2Y2HyXRG2ZnA0LBOUhboqkcYyVfUx259mWCv87iMoviWC3eUTRpXTepjbW3uRR6uR5JK+FHD1i2ly2I3B/kAYNR9RP6VFYkNo09cJZ1H"
    "Y6NzwXMmK5eA==";
static const char* kShortZipHashPayload =
    "eyJ2ZXJzaW9uIjoiOS45LjkiLCJ1cmwiOiJodHRwczovL2V4YW1wbGUuY29tL2EuemlwIiwic2l6ZSI6Nywic2hhMjU2IjoiYWJjIiwicGFnZS"
    "I6Imh0dHBzOi8vZXhhbXBsZS5jb20vIiwibWFuaWZlc3QiOiJodHRwczovL2V4YW1wbGUuY29tL21hbmlmZXN0cy85LjkuOS5qc29uIiwibWFu"
    "aWZlc3RTaGEyNTYiOiJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiIiwibW"
    "FuaWZlc3RzIjp7IjkuOS45IjoiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJi"
    "YiIsIjMuMy4wIjoiQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQyJ9LCJub3"
    "RlcyI6eyJlbiI6InNpZ25lZCBub3RlIn19";
static const char* kShortZipHashSignature =
    "JIxMtKJy0fUDYkWlCTWzbc2bczXoXiFiMzWY212/BnwdSkXM1OCerHBNODMDH7H1uV7DAeTCVxpLFJmIP4OHEHltYG7+Vj0OCIjlAOhr2pYbd3"
    "hc02JjR2uINTPm3U/cIn1UhGOtYdh/6ea1/OF2VKI4HRwZK9yXzxCnOhQ99b1DQOq0T55rr4Lt89oWak6hcGWEmLUl0N1LFcyshWgdn8Ct+0o2"
    "3oBdKU/GEUEZFWaJuqcvnuC5h7kbhtG+0tXNd1wQtkvVPXVBDk0hmuDC1/HgaMQmGTV4BLDG5iVDyrJUZSb3uiO7s78WwvBrvxQCOaRTbndRUI"
    "QUtqbwc0vHsA==";
static const char* kNoManifestHashPayload =
    "eyJ2ZXJzaW9uIjoiOS45LjkiLCJ1cmwiOiJodHRwczovL2V4YW1wbGUuY29tL2EuemlwIiwic2l6ZSI6Nywic2hhMjU2IjoiYWFhYWFhYWFhYW"
    "FhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYSIsInBhZ2UiOiJodHRwczovL2V4YW1wbGUuY29t"
    "LyIsIm1hbmlmZXN0IjoiaHR0cHM6Ly9leGFtcGxlLmNvbS9tYW5pZmVzdHMvOS45LjkuanNvbiIsIm1hbmlmZXN0cyI6eyI5LjkuOSI6ImJiYm"
    "JiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmIiLCIzLjMuMCI6IkNDQ0NDQ0NDQ0ND"
    "Q0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0MifSwibm90ZXMiOnsiZW4iOiJzaWduZWQgbm90ZS"
    "J9fQ==";
static const char* kNoManifestHashSignature =
    "spAObu5ysA1T9eDyZP0Ovl6+sufI4qJ9lbRzo7bBkz6+wVIVeHJeXZ3NDDjMembf0xh4HVU6gqttNBJE1NgvSK/QD2XEAHlv7WgOIN4q7gXKlx"
    "cYWA//Uznb0buG8CQV1slaa+LYKw1kDByL6AGS2TfjSmogS93JSyeuVEKrHOmmkguGR9QRqjHckOkbKTTFJBtp7/gwRnOFQXQxjF/tYe4CVFo7"
    "0gDJ4gkXCiWv06Yh+NWoZWBjGvWy+7rdL1KYl/LWj07xDr9R3sKeSWx7eju7KMNDZDjCrHA241GCw6XrZ7dGlYgdGWNCSKwU0V9h/ZMdPb9mPK"
    "akkKRpHMTW5A==";
static const char* kNoVersionPayload =
    "eyJ1cmwiOiJodHRwczovL2V4YW1wbGUuY29tL2EuemlwIiwic2l6ZSI6Nywic2hhMjU2IjoiYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYW"
    "FhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYSIsInBhZ2UiOiJodHRwczovL2V4YW1wbGUuY29tLyIsIm1hbmlmZXN0IjoiaHR0"
    "cHM6Ly9leGFtcGxlLmNvbS9tYW5pZmVzdHMvOS45LjkuanNvbiIsIm1hbmlmZXN0U2hhMjU2IjoiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYm"
    "JiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYiIsIm1hbmlmZXN0cyI6eyI5LjkuOSI6ImJiYmJiYmJiYmJiYmJiYmJiYmJi"
    "YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmIiLCIzLjMuMCI6IkNDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0"
    "NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0MifSwibm90ZXMiOnsiZW4iOiJzaWduZWQgbm90ZSJ9fQ==";
static const char* kNoVersionSignature =
    "fQUDvroE5kInAmZKTQwptCWqwGFqQqmESGvfWz3h36mzYlTqYmMvcmUyrcHJHb4AtH1aCrbE3tPd9gFCK8+UEMQoR3MEqlKB6VDVIBO5diIXD2"
    "RR/A7dKHgJR/yaf8nZmcKaUCwQ+XNmjefM5xRHuwv3peXrYI9HaNekcu+/HqX0sFjIhnGUIW/aUL+YgmXq5HsHkfrUvRVKDKUaKMgg8dxKcOZI"
    "+u72YrOYnWmjboxs5GdOwWlqDBfRJIMqBH32g1io4c7Sjp8k06d7pYRrRNbI/5iTtUj7pVfXfSYjPa8tDTCGCepBQ5xIo1uM3JA0sJR63Z2a3T"
    "To7JiNH2Uj4g==";
static const char* kNoUrlPayload =
    "eyJ2ZXJzaW9uIjoiOS45LjkiLCJzaXplIjo3LCJzaGEyNTYiOiJhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYW"
    "FhYWFhYWFhYWFhYWFhYWFhYWFhIiwicGFnZSI6Imh0dHBzOi8vZXhhbXBsZS5jb20vIiwibWFuaWZlc3QiOiJodHRwczovL2V4YW1wbGUuY29t"
    "L21hbmlmZXN0cy85LjkuOS5qc29uIiwibWFuaWZlc3RTaGEyNTYiOiJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYm"
    "JiYmJiYmJiYmJiYmJiYmJiYmJiYmJiIiwibWFuaWZlc3RzIjp7IjkuOS45IjoiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJi"
    "YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYiIsIjMuMy4wIjoiQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0"
    "NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQyJ9LCJub3RlcyI6eyJlbiI6InNpZ25lZCBub3RlIn19";
static const char* kNoUrlSignature =
    "imZ5f4S3jFoFVwjXwHFjA/okrmjANd9rdBQ3w9vUw82QpwkKn8tIPhuQQ1fzqwFX2iLrqg4yBInKgOfqNMUwjwtbF+JmGHGV7RyfdnnoZwPsqf"
    "QLaEe2NDBklk70UMHG2uuTE/qM1ZwXznd9jk7uBc8KOYGOvz8riqFfrat9Uv7RWNxy8LeXeJGidXTCebWbnHntAKgJHbdM1PbsriqtAMj0mAru"
    "D5ZF5UX3q5DbNHxwJZJs1bPX81EopCbGMygq8QclN8hf66PRL6a2g3B8TdbhoKgStALlOf/afoUn2tMoabk0TKG697tkBLSyLWspzmXEmOh/oG"
    "ZsgXY6vbraQQ==";
static const char* kNotJsonPayload =
    "bm90IGpzb24=";
static const char* kNotJsonSignature =
    "xGrbE/hvXve480B1OItEyGdIpOungYVP9YzEi69HPgzkhrvRlxf3cMybElMD4fEDPrfTYXq6RhtTh4qt2yrT7gJaUvqgxJc0MbrkZVoqMRJCA8"
    "TVOErRnMSgEuWeWhjSW+1Vf5G8zJj0aPTwHhvf8PvbCxK4Pnom4MgjtWu2CQaYU+BV6LyL58u8zr/jRpLvBsMK1c+78M9v8RgJoid5fEStJ/oM"
    "taKcRlgyhJ7DG5j1Gzv56oVoQahkJZwFfR/SlEwmhXItsG38spjzpLAg4/fsygGiYLldkQd/KAAkLnBuJRKarkHbzGRGRxUJ+VNSv/CmlENxeD"
    "kGCIem2fBj8w==";

static int g_failed = 0;

static void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failed;
}

static Feed Fetch(const std::string& body) {
    const fs::path file = fs::temp_directory_path() / "mmdx12_updater_feed_test.json";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << body;
    Feed feed = FetchFeed(file.string());
    std::error_code ec;
    fs::remove(file, ec);
    return feed;
}

// `outer` = the plain fields a server is free to change.
static std::string Body(const char* payload, const char* signature, const std::string& outer = "\"version\":\"1.0.0\"") {
    return "{" + outer + ",\"signedFeed\":\"" + payload + "\",\"signature\":\"" + signature + "\"}";
}

static bool Refused(const std::string& body, const char* why) {
    const Feed feed = Fetch(body);
    const bool refused = !feed.ok && feed.error.find(why) != std::string::npos;
    if (!refused) std::printf("  got ok=%d error='%s'\n", (int)feed.ok, feed.error.c_str());
    return refused;
}

int main() {
    Feed f = Fetch(Body(kGoodPayload, kGoodSignature));
    Check(f.ok && f.version == "9.9.9" && f.url == "https://example.com/a.zip" && f.size == 7, "a signed feed is read");
    Check(f.sha256 == std::string(64, 'a') && f.manifestSha256 == std::string(64, 'b'), "its hashes are read");
    Check(f.manifests.size() == 2 && f.manifests["9.9.9"] == std::string(64, 'b') && f.manifests["3.3.0"] == std::string(64, 'c'),
          "its manifests map is read, in lower case");
    Check(f.notes["en"] == "signed note", "its notes are read");

    f = Fetch(Body(kGoodPayload, kGoodSignature,
                   "\"version\":\"99.0.0\",\"url\":\"https://evil.example/a.zip\",\"sha256\":\"" + std::string(64, 'e') +
                       "\",\"manifests\":{\"9.9.9\":\"" + std::string(64, 'e') + "\"}"));
    Check(f.ok && f.version == "9.9.9" && f.url == "https://example.com/a.zip" && f.sha256 == std::string(64, 'a') &&
              f.manifests["9.9.9"] == std::string(64, 'b'),
          "fields outside the signed bytes are ignored");

    Check(Fetch(Body(kNoManifestPayload, kNoManifestSignature)).ok, "a signed feed without a delta manifest is read");

    Check(Refused("{\"version\":\"9.9.9\",\"url\":\"https://evil.example/a.zip\",\"sha256\":\"" + std::string(64, 'e') + "\"}",
                  "not signed"),
          "an unsigned feed is refused");
    Check(Refused(Body(kNoManifestPayload, kGoodSignature), "signature"), "another feed's signature is refused");
    Check(Refused(Body(kNoZipHashPayload, kNoZipHashSignature), "zip hash"), "a signed feed without a zip hash is refused");
    Check(Refused(Body(kShortZipHashPayload, kShortZipHashSignature), "zip hash"), "a signed feed with a broken zip hash is refused");
    Check(Refused(Body(kNoManifestHashPayload, kNoManifestHashSignature), "manifest hash"),
          "a signed feed with a manifest but no hash for it is refused");
    Check(Refused(Body(kNoVersionPayload, kNoVersionSignature), "version"), "a signed feed without a version is refused");
    Check(Refused(Body(kNoUrlPayload, kNoUrlSignature), "url"), "a signed feed without a url is refused");
    Check(Refused(Body(kNotJsonPayload, kNotJsonSignature), "invalid feed"), "signed bytes that are not JSON are refused");

    std::printf(g_failed ? "FAILED\n" : "ALL PASS\n");
    return g_failed ? 1 : 0;
}
