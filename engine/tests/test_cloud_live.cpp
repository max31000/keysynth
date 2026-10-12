// [cloud-live]: real HTTPS (JUCE/WinINet) against a running API. Hidden; needs KS_CLOUD_API (default production
// base) and KS_CLOUD_TOKEN (a device token). Works against scripts/dev_cloud_server.py too:
//   $env:KS_CLOUD_API="http://127.0.0.1:8765/keysynth"; $env:KS_CLOUD_TOKEN="ksd_..."; ks-tests "[cloud-live]"

#include "cloud/CloudClient.h"
#include "cloud/HttpTransport.h"
#include "samples/Manifest.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace ks;

namespace {
std::string env(const char* n) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    const char* v = std::getenv(n);
    return v ? v : "";
}
} // namespace

TEST_CASE("cloud live: devices/me + manifest over the real transport", "[.cloud-live]") {
    const std::string token = env("KS_CLOUD_TOKEN");
    std::string base = env("KS_CLOUD_API");
    if (base.empty()) base = cloud::kDefaultApiBase;
    if (token.empty()) SKIP("KS_CLOUD_TOKEN not set");
    auto http = cloud::makeJuceHttpTransport();
    cloud::CloudClient c(http, base, std::filesystem::temp_directory_path() / "ks-cloud-live.json");
    const auto me = c.checkToken(token);
    INFO("status " << me.status << " error " << me.error);
    REQUIRE(me.error.empty());
    CHECK_FALSE(me.info.accountId.empty());
    cloud::HttpRequest req;
    req.url = base + "/api/samples/manifest";
    req.headers.emplace_back("Authorization", "Bearer " + token);
    const auto r = http->send(req);
    INFO("manifest status " << r.status);
    REQUIRE((r.status == 200 || r.status == 503));
    if (r.status == 200) CHECK(samples::Manifest::parse(r.body).has_value());
}
