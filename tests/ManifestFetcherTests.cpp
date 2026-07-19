#include "TestCommon.h"

#include "ManifestFetcher.h"
#include "NetworkTestCommon.h"
#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class CountingSignatureVerifier final : public autoupdater::ISignatureVerifier {
  public:
    CountingSignatureVerifier() = default;

    explicit CountingSignatureVerifier(std::vector<bool> scriptedResults)
        : scriptedResults_(std::move(scriptedResults)) {}

    autoupdater::Result<void> verify(std::string_view data, std::string_view signature,
                                     std::string_view publicKey) noexcept override {
        const auto callIndex = static_cast<std::size_t>(calls);
        ++calls;
        verifiedData.emplace_back(data);
        verifiedSignatures.emplace_back(signature);
        verifiedPublicKeys.emplace_back(publicKey);
        if (callIndex < scriptedResults_.size() && !scriptedResults_[callIndex]) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::ManifestSignatureInvalid, "Scripted signature rejection"});
        }
        return autoupdater::Result<void>::ok();
    }

    int calls = 0;
    std::vector<std::string> verifiedData;
    std::vector<std::string> verifiedSignatures;
    std::vector<std::string> verifiedPublicKeys;

  private:
    std::vector<bool> scriptedResults_;
};

autoupdater::Config manifestConfig(const std::string& manifestUrl) {
    autoupdater::Config config;
    config.appId = "com.example.app";
    config.channel = "stable";
    config.platform = "windows";
    config.arch = "x64";
    config.manifestUrl = manifestUrl;
    config.security.allowedBaseUrls = {"https://updates.example.test/"};
    return config;
}

std::string releaseManifest(std::string baseUrl = {}) {
    std::string baseField;
    if (!baseUrl.empty()) {
        baseField = "\n      \"baseUrl\": \"" + baseUrl + "\",";
    }
    return R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "platform": "windows",
      "arch": "x64",
      "version": "1.2.0",)json" +
           baseField + R"json(
      "files": []
    })json";
}

autoupdater::Result<autoupdater::ManifestEnvelope> fetch(const autoupdater::Config& config,
                                                         autoupdater::test::ScriptedNetworkClient& network,
                                                         CountingSignatureVerifier& verifier) {
    auto hash = autoupdater::createDefaultHashProvider();
    autoupdater::CancellationToken cancel;
    return autoupdater::fetchAndVerifyManifest(config, network, *hash, verifier, cancel);
}

} // namespace

void testManifestFetcherRoutesIndexManifest() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    const std::string releaseUrl = "https://updates.example.test/releases/1.2.0/windows-x64/manifest.json";

    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "targets": [
        {
          "platform": "linux",
          "arch": "x64",
          "manifestUrl": "https://updates.example.test/releases/1.2.0/linux-x64/manifest.json"
        },
        {
          "platform": "windows",
          "arch": "x64",
          "manifestUrl": "https://updates.example.test/releases/1.2.0/windows-x64/manifest.json"
        }
      ]
    })json"));
    network.queueText(indexUrl + ".sig", autoupdater::test::textResponse("index-signature"));
    network.queueText(releaseUrl, autoupdater::test::textResponse(
                                      releaseManifest("https://updates.example.test/releases/1.2.0/windows-x64/")));
    network.queueText(releaseUrl + ".sig", autoupdater::test::textResponse("release-signature"));

    auto config = manifestConfig(indexUrl);
    config.security.requireManifestSignature = true;
    config.security.publicKeyPem = "fake-key";
    CountingSignatureVerifier verifier;

    auto envelope = fetch(config, network, verifier);
    LAU_REQUIRE(envelope);
    LAU_REQUIRE(envelope.value().manifest.version.toString() == "1.2.0");
    LAU_REQUIRE(envelope.value().manifest.platform == "windows");
    LAU_REQUIRE(envelope.value().releaseManifestSignatureVerified);
    LAU_REQUIRE(verifier.calls == 2);
    LAU_REQUIRE(verifier.verifiedData.back() == envelope.value().rawBytes);
    LAU_REQUIRE(verifier.verifiedPublicKeys == std::vector<std::string>({"fake-key", "fake-key"}));
    LAU_REQUIRE(network.textRequests ==
                std::vector<std::string>({indexUrl, indexUrl + ".sig", releaseUrl, releaseUrl + ".sig"}));
}

void testManifestFetcherRanksWildcardIndexTargets() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    const std::string exactUrl = "https://updates.example.test/releases/exact/manifest.json";
    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "targets": [
        {"manifestUrl": "releases/global/manifest.json"},
        {"platform": "windows", "manifestUrl": "releases/windows/manifest.json"},
        {"arch": "x64", "manifestUrl": "releases/x64/manifest.json"},
        {"platform": "windows", "arch": "x64", "manifestUrl": "releases/exact/manifest.json"}
      ]
    })json"));
    network.queueText(exactUrl, autoupdater::test::textResponse(releaseManifest()));
    auto config = manifestConfig(indexUrl);
    CountingSignatureVerifier verifier;

    const auto envelope = fetch(config, network, verifier);

    LAU_REQUIRE(envelope);
    LAU_REQUIRE(network.textRequests == std::vector<std::string>({indexUrl, exactUrl}));

    const std::string platformUrl = "https://updates.example.test/releases/windows/manifest.json";
    autoupdater::test::ScriptedNetworkClient fallbackNetwork;
    fallbackNetwork.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "targets": [
        {"manifestUrl": "releases/global/manifest.json"},
        {"platform": "windows", "manifestUrl": "releases/windows/manifest.json"}
      ]
    })json"));
    fallbackNetwork.queueText(platformUrl, autoupdater::test::textResponse(releaseManifest()));
    CountingSignatureVerifier fallbackVerifier;

    const auto fallbackEnvelope = fetch(config, fallbackNetwork, fallbackVerifier);

    LAU_REQUIRE(fallbackEnvelope);
    LAU_REQUIRE(fallbackNetwork.textRequests == std::vector<std::string>({indexUrl, platformUrl}));

    const std::string archUrl = "https://updates.example.test/releases/x64/manifest.json";
    autoupdater::test::ScriptedNetworkClient archNetwork;
    archNetwork.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "targets": [
        {"manifestUrl": "releases/global/manifest.json"},
        {"arch": "x64", "manifestUrl": "releases/x64/manifest.json"}
      ]
    })json"));
    archNetwork.queueText(archUrl, autoupdater::test::textResponse(releaseManifest()));
    CountingSignatureVerifier archVerifier;

    const auto archEnvelope = fetch(config, archNetwork, archVerifier);

    LAU_REQUIRE(archEnvelope);
    LAU_REQUIRE(archNetwork.textRequests == std::vector<std::string>({indexUrl, archUrl}));

    const std::string globalUrl = "https://updates.example.test/releases/global/manifest.json";
    autoupdater::test::ScriptedNetworkClient globalNetwork;
    globalNetwork.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "targets": [{"manifestUrl": "releases/global/manifest.json"}]
    })json"));
    globalNetwork.queueText(globalUrl, autoupdater::test::textResponse(releaseManifest()));
    CountingSignatureVerifier globalVerifier;

    const auto globalEnvelope = fetch(config, globalNetwork, globalVerifier);

    LAU_REQUIRE(globalEnvelope);
    LAU_REQUIRE(globalNetwork.textRequests == std::vector<std::string>({indexUrl, globalUrl}));
}

void testManifestFetcherRejectsAmbiguousIndexTargets() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    auto config = manifestConfig(indexUrl);

    for (const auto& indexDocument : {
             R"json({
               "schemaVersion": 1,
               "targets": [
                 {"platform": "windows", "manifestUrl": "windows/manifest.json"},
                 {"arch": "x64", "manifestUrl": "x64/manifest.json"}
               ]
             })json",
             R"json({
               "schemaVersion": 1,
               "targets": [
                 {"platform": "windows", "arch": "x64", "manifestUrl": "first.json"},
                 {"platform": "windows", "arch": "x64", "manifestUrl": "second.json"}
               ]
             })json",
             R"json({
               "schemaVersion": 1,
               "targets": [
                 {"manifestUrl": "first.json"},
                 {"platform": "", "arch": "", "manifestUrl": "second.json"}
               ]
             })json",
             R"json({
               "schemaVersion": 1,
               "targets": [
                 {"platform": "windows", "arch": "x64", "manifestUrl": ""}
               ]
             })json"}) {
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(indexUrl, autoupdater::test::textResponse(indexDocument));
        CountingSignatureVerifier verifier;

        const auto envelope = fetch(config, network, verifier);

        LAU_REQUIRE(!envelope);
        LAU_REQUIRE(envelope.error().code == autoupdater::ErrorCode::ManifestParseFailed);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({indexUrl}));
    }
}

void testManifestFetcherRequiresConcreteClientRouteDimensions() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    const std::string indexDocument = R"json({
      "schemaVersion": 1,
      "targets": [{"manifestUrl": "fallback/manifest.json"}]
    })json";
    for (const bool clearPlatform : {false, true}) {
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(indexUrl, autoupdater::test::textResponse(indexDocument));
        auto config = manifestConfig(indexUrl);
        if (clearPlatform) {
            config.platform.clear();
        } else {
            config.arch.clear();
        }
        CountingSignatureVerifier verifier;

        const auto envelope = fetch(config, network, verifier);

        LAU_REQUIRE(!envelope);
        LAU_REQUIRE(envelope.error().code == autoupdater::ErrorCode::InvalidConfig);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({indexUrl}));
    }
}

void testManifestFetcherRejectsInvalidReleaseBehindSignedIndex() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    const std::string releaseUrl = "https://updates.example.test/releases/1.2.0/windows-x64/manifest.json";
    const std::string indexDocument = R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "targets": [
        {
          "platform": "windows",
          "arch": "x64",
          "manifestUrl": "https://updates.example.test/releases/1.2.0/windows-x64/manifest.json"
        }
      ]
    })json";
    const auto releaseDocument = releaseManifest("https://updates.example.test/releases/1.2.0/windows-x64/");

    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(indexUrl, autoupdater::test::textResponse(indexDocument));
    network.queueText(indexUrl + ".sig", autoupdater::test::textResponse("index-signature"));
    network.queueText(releaseUrl, autoupdater::test::textResponse(releaseDocument));
    network.queueText(releaseUrl + ".sig", autoupdater::test::textResponse("release-signature"));

    auto config = manifestConfig(indexUrl);
    config.security.requireManifestSignature = true;
    config.security.publicKeyPem = "fake-key";
    CountingSignatureVerifier verifier({true, false});

    const auto envelope = fetch(config, network, verifier);
    LAU_REQUIRE(!envelope);
    LAU_REQUIRE(envelope.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);
    LAU_REQUIRE(verifier.calls == 2);
    LAU_REQUIRE(verifier.verifiedData == std::vector<std::string>({indexDocument, releaseDocument}));
    LAU_REQUIRE(verifier.verifiedSignatures == std::vector<std::string>({"index-signature", "release-signature"}));
    LAU_REQUIRE(network.textRequests ==
                std::vector<std::string>({indexUrl, indexUrl + ".sig", releaseUrl, releaseUrl + ".sig"}));
}

void testManifestFetcherRejectsDisallowedIndexTarget() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "targets": [
        {
          "platform": "windows",
          "arch": "x64",
          "manifestUrl": "https://evil.example.test/manifest.json"
        }
      ]
    })json"));

    auto config = manifestConfig(indexUrl);
    CountingSignatureVerifier verifier;
    auto envelope = fetch(config, network, verifier);
    LAU_REQUIRE(!envelope);
    LAU_REQUIRE(envelope.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(network.textRequests == std::vector<std::string>({indexUrl}));
}

void testManifestFetcherRejectsAllowedBaseUrlPrefixBypass() {
    const std::string indexUrl = "https://updates.example.test/index.json";
    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(indexUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "targets": [
        {
          "platform": "windows",
          "arch": "x64",
          "manifestUrl": "https://updates.example.test.evil/manifest.json"
        }
      ]
    })json"));

    auto config = manifestConfig(indexUrl);
    CountingSignatureVerifier verifier;
    auto envelope = fetch(config, network, verifier);
    LAU_REQUIRE(!envelope);
    LAU_REQUIRE(envelope.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(network.textRequests == std::vector<std::string>({indexUrl}));
}

void testManifestFetcherRejectsInitialUrlBeforeNetwork() {
    auto config = manifestConfig("https://evil.example.test/manifest.json");
    autoupdater::test::ScriptedNetworkClient network;
    CountingSignatureVerifier verifier;

    auto envelope = fetch(config, network, verifier);
    LAU_REQUIRE(!envelope);
    LAU_REQUIRE(envelope.error().code == autoupdater::ErrorCode::InvalidConfig);
    LAU_REQUIRE(network.textRequests.empty());
}

void testManifestFetcherResolvesIndexTargetFromEffectiveUrl() {
    const std::string initialIndex = "https://updates.example.test/channels/latest/index.json";
    const std::string effectiveIndex = "https://updates.example.test/channels/stable/index.json";
    const std::string releaseUrl = "https://updates.example.test/channels/stable/release/manifest.json";

    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(initialIndex, autoupdater::test::redirectResponse(302, "../stable/index.json"));
    network.queueText(effectiveIndex, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "targets": [
        {
          "platform": "windows",
          "arch": "x64",
          "manifestUrl": "release/manifest.json"
        }
      ]
    })json"));
    network.queueText(releaseUrl, autoupdater::test::textResponse(releaseManifest()));

    auto config = manifestConfig(initialIndex);
    CountingSignatureVerifier verifier;
    auto envelope = fetch(config, network, verifier);
    LAU_REQUIRE(envelope);
    LAU_REQUIRE(envelope.value().sourceUrl == releaseUrl);
    LAU_REQUIRE(envelope.value().artifactBaseUrl == "https://updates.example.test/channels/stable/release/");
    LAU_REQUIRE(network.textRequests == std::vector<std::string>({initialIndex, effectiveIndex, releaseUrl}));
}

void testManifestFetcherResolvesSignaturesFromEffectiveUrl() {
    const std::string initial = "https://updates.example.test/latest/manifest.json";
    const std::string effective = "https://updates.example.test/releases/1.2.0/manifest.json";

    {
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(initial, autoupdater::test::redirectResponse(302, "../releases/1.2.0/manifest.json"));
        network.queueText(effective, autoupdater::test::textResponse(releaseManifest()));
        network.queueText(effective + ".sig", autoupdater::test::textResponse("default-signature"));
        auto config = manifestConfig(initial);
        config.security.requireManifestSignature = true;
        config.security.publicKeyPem = "fake-key";
        CountingSignatureVerifier verifier;
        auto envelope = fetch(config, network, verifier);
        LAU_REQUIRE(envelope);
        LAU_REQUIRE(envelope.value().releaseManifestSignatureVerified);
        LAU_REQUIRE(verifier.calls == 1);
        LAU_REQUIRE(verifier.verifiedSignatures == std::vector<std::string>({"default-signature"}));
        LAU_REQUIRE(verifier.verifiedPublicKeys == std::vector<std::string>({"fake-key"}));
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({initial, effective, effective + ".sig"}));
    }

    {
        const std::string relativeSignature = "https://updates.example.test/releases/1.2.0/signatures/current.sig";
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(initial, autoupdater::test::redirectResponse(302, "../releases/1.2.0/manifest.json"));
        network.queueText(effective, autoupdater::test::textResponse(releaseManifest()));
        network.queueText(relativeSignature, autoupdater::test::textResponse("relative-signature"));
        auto config = manifestConfig(initial);
        config.security.requireManifestSignature = true;
        config.security.manifestSignatureUrl = "signatures/current.sig";
        config.security.publicKeyPem = "fake-key";
        CountingSignatureVerifier verifier;
        auto envelope = fetch(config, network, verifier);
        LAU_REQUIRE(envelope);
        LAU_REQUIRE(envelope.value().releaseManifestSignatureVerified);
        LAU_REQUIRE(verifier.calls == 1);
        LAU_REQUIRE(verifier.verifiedSignatures == std::vector<std::string>({"relative-signature"}));
        LAU_REQUIRE(verifier.verifiedPublicKeys == std::vector<std::string>({"fake-key"}));
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({initial, effective, relativeSignature}));
    }
}

void testManifestFetcherKeepsQueriesInTheirUriComponent() {
    const std::string manifestUrl = "https://updates.example.test/releases/manifest.json?token=manifest";

    {
        const std::string signatureUrl = "https://updates.example.test/releases/manifest.json.sig?token=manifest";
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(manifestUrl, autoupdater::test::textResponse(releaseManifest()));
        network.queueText(signatureUrl, autoupdater::test::textResponse("signature"));
        auto config = manifestConfig(manifestUrl);
        config.security.requireManifestSignature = true;
        config.security.publicKeyPem = "fake-key";
        CountingSignatureVerifier verifier;

        const auto envelope = fetch(config, network, verifier);

        LAU_REQUIRE(envelope);
        LAU_REQUIRE(envelope.value().artifactBaseUrl == "https://updates.example.test/releases/");
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({manifestUrl, signatureUrl}));
    }

    {
        const std::string signatureUrl = "https://updates.example.test/releases/manifest.json?signature=one/two?three";
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(manifestUrl, autoupdater::test::textResponse(releaseManifest()));
        network.queueText(signatureUrl, autoupdater::test::textResponse("signature"));
        auto config = manifestConfig(manifestUrl);
        config.security.requireManifestSignature = true;
        config.security.manifestSignatureUrl = "?signature=one/two?three";
        config.security.publicKeyPem = "fake-key";
        CountingSignatureVerifier verifier;

        const auto envelope = fetch(config, network, verifier);

        LAU_REQUIRE(envelope);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({manifestUrl, signatureUrl}));
    }
}

void testManifestFetcherResolvesEmptyAndRelativeArtifactBases() {
    const std::string manifestUrl = "https://updates.example.test/releases/1.2.0/manifest.json";

    {
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(manifestUrl, autoupdater::test::textResponse(releaseManifest()));
        auto config = manifestConfig(manifestUrl);
        CountingSignatureVerifier verifier;
        auto envelope = fetch(config, network, verifier);
        LAU_REQUIRE(envelope);
        LAU_REQUIRE(!envelope.value().releaseManifestSignatureVerified);
        LAU_REQUIRE(verifier.calls == 0);
        LAU_REQUIRE(envelope.value().artifactBaseUrl == "https://updates.example.test/releases/1.2.0/");
    }

    {
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(manifestUrl, autoupdater::test::textResponse(releaseManifest("artifacts")));
        auto config = manifestConfig(manifestUrl);
        CountingSignatureVerifier verifier;
        auto envelope = fetch(config, network, verifier);
        LAU_REQUIRE(envelope);
        LAU_REQUIRE(!envelope.value().releaseManifestSignatureVerified);
        LAU_REQUIRE(verifier.calls == 0);
        LAU_REQUIRE(envelope.value().artifactBaseUrl == "https://updates.example.test/releases/1.2.0/artifacts/");
    }
}
