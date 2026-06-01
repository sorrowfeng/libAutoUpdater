#include "TestCommon.h"

#include "ManifestFetcher.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

#include <map>

namespace {

class FakeNetworkClient final : public autoupdater::INetworkClient {
public:
    std::map<std::string, std::string> texts;

    autoupdater::Result<std::string> getText(
        const std::string& url,
        const autoupdater::NetworkOptions&,
        autoupdater::CancellationToken&) noexcept override {
        const auto it = texts.find(url);
        if (it == texts.end()) {
            return autoupdater::Result<std::string>::fail({autoupdater::ErrorCode::NetworkError, "missing url: " + url});
        }
        return autoupdater::Result<std::string>::ok(it->second);
    }

    autoupdater::Result<autoupdater::DownloadResult> downloadToFile(
        const std::string&,
        const std::filesystem::path&,
        const autoupdater::NetworkOptions&,
        const std::optional<autoupdater::DownloadResumeInfo>&,
        autoupdater::ProgressCallback,
        autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::DownloadResult>::fail({autoupdater::ErrorCode::DownloadFailed, "not used"});
    }
};

class CountingSignatureVerifier final : public autoupdater::ISignatureVerifier {
public:
    int calls = 0;

    autoupdater::Result<void> verify(std::string_view,
                                     std::string_view,
                                     std::string_view) noexcept override {
        ++calls;
        return autoupdater::Result<void>::ok();
    }
};

} // namespace

void testManifestFetcherRoutesIndexManifest() {
    const std::string indexUrl = "https://updates.example.com/index.json";
    const std::string releaseUrl = "https://updates.example.com/releases/1.2.0/windows-x64/manifest.json";

    FakeNetworkClient network;
    network.texts[indexUrl] = R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "targets": [
        {
          "platform": "linux",
          "arch": "x64",
          "manifestUrl": "https://updates.example.com/releases/1.2.0/linux-x64/manifest.json"
        },
        {
          "platform": "windows",
          "arch": "x64",
          "manifestUrl": "https://updates.example.com/releases/1.2.0/windows-x64/manifest.json"
        }
      ]
    })json";
    network.texts[indexUrl + ".sig"] = "index-signature";
    network.texts[releaseUrl] = R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "platform": "windows",
      "arch": "x64",
      "version": "1.2.0",
      "baseUrl": "https://updates.example.com/releases/1.2.0/windows-x64/",
      "files": []
    })json";
    network.texts[releaseUrl + ".sig"] = "release-signature";

    auto hash = autoupdater::createDefaultHashProvider();
    CountingSignatureVerifier verifier;
    autoupdater::CancellationToken cancel;
    autoupdater::Config config;
    config.appId = "com.example.app";
    config.channel = "stable";
    config.platform = "windows";
    config.arch = "x64";
    config.manifestUrl = indexUrl;
    config.security.requireManifestSignature = true;
    config.security.publicKeyPem = "fake-key";

    auto envelope = autoupdater::fetchAndVerifyManifest(config, network, *hash, verifier, cancel);
    LAU_REQUIRE(envelope);
    LAU_REQUIRE(envelope.value().manifest.version.toString() == "1.2.0");
    LAU_REQUIRE(envelope.value().manifest.platform == "windows");
    LAU_REQUIRE(verifier.calls == 2);
}

