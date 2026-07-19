#include "TestCommon.h"

#include "UrlPolicy.h"
#include "util/UrlUtil.h"

#include <string>
#include <vector>

namespace {

autoupdater::Config networkConfig(std::string manifestUrl, std::vector<std::string> scopes) {
    autoupdater::Config config;
    config.manifestUrl = std::move(manifestUrl);
    config.security.allowedBaseUrls = std::move(scopes);
    return config;
}

void requireRejectedUrl(const std::string& url) {
    auto parsed = autoupdater::util::parseAbsoluteUrl(url);
    LAU_REQUIRE(!parsed);
    LAU_REQUIRE(parsed.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

} // namespace

void testUrlPolicyFailsClosedAndRejectsMalformedUrls() {
    auto noScopes = networkConfig("https://updates.example.test/releases/manifest.json", {});
    auto noScopesPolicy = autoupdater::UrlPolicy::fromConfig(noScopes);
    LAU_REQUIRE(!noScopesPolicy);
    LAU_REQUIRE(noScopesPolicy.error().code == autoupdater::ErrorCode::InvalidConfig);

    const std::vector<std::string> malformed = {
        "https://user@updates.example.test/releases/manifest.json",
        "https://updates.example.test/releases/manifest.json#fragment",
        "https://updates.example.test/releases\\manifest.json",
        "https://updates.example.test:/releases/manifest.json",
        "https://updates.example.test:0/releases/manifest.json",
        "https://updates.example.test:65536/releases/manifest.json",
        "https://updates.example.test:not-a-port/releases/manifest.json",
        "https://updates.example.test/releases/%",
        "https://updates.example.test/releases/%GG",
        std::string("https://updates.example.test/releases/") + u8"中文.json",
    };
    for (const auto& url : malformed) {
        requireRejectedUrl(url);
    }

#ifdef _WIN32
    const std::string localManifest = "file:///C:/updates/manifest.json";
    const std::string localSibling = "file:///C:/outside/payload.bin";
#else
    const std::string localManifest = "file:///tmp/updates/manifest.json";
    const std::string localSibling = "file:///tmp/outside/payload.bin";
#endif
    autoupdater::Config localConfig;
    localConfig.manifestUrl = localManifest;
    auto disabledLocal = autoupdater::UrlPolicy::fromConfig(localConfig);
    LAU_REQUIRE(!disabledLocal);
    LAU_REQUIRE(disabledLocal.error().code == autoupdater::ErrorCode::InvalidConfig);

    localConfig.security.allowLocalFileUrls = true;
    auto enabledLocal = autoupdater::UrlPolicy::fromConfig(localConfig);
    LAU_REQUIRE(enabledLocal);
    LAU_REQUIRE(enabledLocal.value().authorize(localManifest));
    auto outsideLocalRoot = enabledLocal.value().authorize(localSibling);
    LAU_REQUIRE(!outsideLocalRoot);
    LAU_REQUIRE(outsideLocalRoot.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

void testUrlPolicyCanonicalizesAndEnforcesScopeBoundaries() {
    auto config = networkConfig("HTTPS://UPDATES.EXAMPLE.TEST:443/releases/v1/./manifest.json",
                                {"https://updates.example.test:443/releases/"});
    auto policy = autoupdater::UrlPolicy::fromConfig(config);
    LAU_REQUIRE(policy);
    LAU_REQUIRE(policy.value().initialUrl().canonical == "https://updates.example.test/releases/v1/manifest.json");

    auto defaultPortEquivalent = policy.value().authorize("https://UPDATES.EXAMPLE.TEST:443/releases/v1/manifest.json");
    LAU_REQUIRE(defaultPortEquivalent);
    LAU_REQUIRE(defaultPortEquivalent.value().canonical == "https://updates.example.test/releases/v1/manifest.json");

    auto literalDotSegment = policy.value().authorize("https://updates.example.test/releases/v1/../manifest.json");
    LAU_REQUIRE(literalDotSegment);
    LAU_REQUIRE(literalDotSegment.value().canonical == "https://updates.example.test/releases/manifest.json");

    for (const auto& outside : {
             "https://updates.example.test.evil/releases/manifest.json",
             "https://updates.example.test/releases-evil/manifest.json",
             "https://updates.example.test/releases/../admin/manifest.json",
             "https://updates.example.test:8443/releases/manifest.json",
         }) {
        auto result = policy.value().authorize(outside);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    }

    for (const auto& encodedPathAttack : {
             "https://updates.example.test/releases/%2e%2e/admin/manifest.json",
             "https://updates.example.test/releases/%2E/manifest.json",
             "https://updates.example.test/releases/encoded%2Fslash.json",
             "https://updates.example.test/releases/encoded%5cbackslash.json",
         }) {
        requireRejectedUrl(encodedPathAttack);
    }

    auto httpDefaultPort = autoupdater::util::parseAbsoluteUrl("HTTP://PUBLIC.EXAMPLE.TEST:80/a");
    LAU_REQUIRE(httpDefaultPort);
    LAU_REQUIRE(httpDefaultPort.value().canonical == "http://public.example.test/a");
}

void testUrlPolicyRejectsLocalAndAmbiguousAddressLiterals() {
    const std::vector<std::string> rejectedHosts = {
        "https://localhost/update",
        "https://LOCALHOST./update",
        "https://service.localhost/update",
        "https://127.0.0.1/update",
        "https://10.1.2.3/update",
        "https://172.16.0.1/update",
        "https://172.31.255.255/update",
        "https://192.168.1.1/update",
        "https://169.254.169.254/latest/meta-data",
        "https://100.64.0.1/update",
        "https://2130706433/update",
        "https://127.1/update",
        "https://0177.0.0.1/update",
        "https://0x7f000001/update",
        "https://[::1]/update",
        "https://[::]/update",
        "https://[fc00::1]/update",
        "https://[fe80::1]/update",
        "https://[::ffff:127.0.0.1]/update",
    };
    for (const auto& url : rejectedHosts) {
        requireRejectedUrl(url);
    }

    auto publicIpv4 = autoupdater::util::parseAbsoluteUrl("https://8.8.8.8/update");
    LAU_REQUIRE(publicIpv4);
    auto publicIpv6 = autoupdater::util::parseAbsoluteUrl("https://[2606:4700:4700::1111]/update");
    LAU_REQUIRE(publicIpv6);
}
