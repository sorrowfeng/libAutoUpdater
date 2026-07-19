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

void testUrlUtilitiesPreserveUriComponentSemantics() {
    const auto repeated = autoupdater::util::parseAbsoluteUrl("https://updates.example.test/a//b");
    LAU_REQUIRE(repeated);
    LAU_REQUIRE(repeated.value().canonical == "https://updates.example.test/a//b");

    const auto emptyBeforeParent = autoupdater::util::parseAbsoluteUrl("https://updates.example.test/a//../b");
    LAU_REQUIRE(emptyBeforeParent);
    LAU_REQUIRE(emptyBeforeParent.value().canonical == "https://updates.example.test/a/b");

    const auto base =
        autoupdater::util::parseAbsoluteUrl("https://updates.example.test/a/b/manifest.json?old=1/2?3&x=y");
    LAU_REQUIRE(base);
    LAU_REQUIRE(autoupdater::util::resolveUrlReference(base.value(), "").value().canonical == base.value().canonical);
    LAU_REQUIRE(autoupdater::util::resolveUrlReference(base.value(), "?new=2/3?4&x=y").value().canonical ==
                "https://updates.example.test/a/b/manifest.json?new=2/3?4&x=y");
    LAU_REQUIRE(autoupdater::util::resolveUrlReference(base.value(), "next.json?new=2").value().canonical ==
                "https://updates.example.test/a/b/next.json?new=2");
    LAU_REQUIRE(!autoupdater::util::resolveUrlReference(base.value(), "#fragment"));
    LAU_REQUIRE(!autoupdater::util::resolveUrlReference(base.value(), "next.json#fragment"));

#ifdef _WIN32
    const auto fileBase = autoupdater::util::parseAbsoluteUrl("file:///C:/updates/manifest.json");
#else
    const auto fileBase = autoupdater::util::parseAbsoluteUrl("file:///tmp/updates/manifest.json");
#endif
    LAU_REQUIRE(fileBase);
    LAU_REQUIRE(autoupdater::util::resolveUrlReference(fileBase.value(), "").value().canonical ==
                fileBase.value().canonical);
    LAU_REQUIRE(!autoupdater::util::resolveUrlReference(fileBase.value(), "?signature=1"));
    LAU_REQUIRE(!autoupdater::util::resolveUrlReference(fileBase.value(), "?"));
#ifdef _WIN32
    const auto resolvedFile = autoupdater::util::resolveUrlReference(fileBase.value(), "/D:/payload.bin");
    LAU_REQUIRE(!autoupdater::util::resolveUrlReference(fileBase.value(), "/tmp/payload.bin"));
    LAU_REQUIRE(!autoupdater::util::resolveUrlReference(fileBase.value(), "../.."));
#else
    const auto resolvedFile = autoupdater::util::resolveUrlReference(fileBase.value(), "/var/tmp/payload.bin");
#endif
    LAU_REQUIRE(resolvedFile);
    const auto reparsedFile = autoupdater::util::parseAbsoluteUrl(resolvedFile.value().canonical);
    LAU_REQUIRE(reparsedFile);
    LAU_REQUIRE(reparsedFile.value().canonical == resolvedFile.value().canonical);

    const auto signature = autoupdater::util::appendUrlPathSuffix(base.value(), ".sig");
    LAU_REQUIRE(signature);
    LAU_REQUIRE(signature.value().canonical == "https://updates.example.test/a/b/manifest.json.sig?old=1/2?3&x=y");

    const auto directory = autoupdater::util::parseAbsoluteUrl("https://updates.example.test/artifacts/");
    LAU_REQUIRE(directory);
    const auto artifact = autoupdater::util::appendEncodedPath(directory.value(), "dir/a b?#%.bin");
    LAU_REQUIRE(artifact);
    LAU_REQUIRE(artifact.value().canonical == "https://updates.example.test/artifacts/dir/a%20b%3F%23%25.bin");

    for (const char character : std::string("\"<>[]^`{|}")) {
        requireRejectedUrl(std::string("https://updates.example.test/a") + character + "b");
        requireRejectedUrl(std::string("https://updates.example.test/a?x=") + character);
    }
    const auto encodedReserved =
        autoupdater::util::parseAbsoluteUrl("https://updates.example.test/a%7Cb?x=%5Bvalue%5D");
    LAU_REQUIRE(encodedReserved);
    LAU_REQUIRE(encodedReserved.value().canonical == "https://updates.example.test/a%7Cb?x=%5Bvalue%5D");
}
