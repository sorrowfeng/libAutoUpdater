#include "TestCommon.h"

#include "UpdatePlanner.h"

#include <string>

namespace {

autoupdater::Config plannerConfig() {
    autoupdater::Config config;
    config.appId = "com.example.app";
    config.channel = "stable";
    config.platform = "windows";
    config.arch = "x64";
    config.installDir = "install";
    config.tempDir = "install/.autoupdater/staging/1.1.0";
    config.currentVersion = autoupdater::Version::parse("1.0.0").value();
    config.manifestUrl = "https://updates.example.test/releases/1.1.0/manifest.json";
    config.security.allowedBaseUrls = {"https://updates.example.test/releases/"};
    return config;
}

autoupdater::ManifestEnvelope plannerEnvelope() {
    autoupdater::ManifestEnvelope envelope;
    envelope.sha256 = "manifest";
    envelope.sourceUrl = "https://updates.example.test/releases/1.1.0/manifest.json";
    envelope.artifactBaseUrl = "https://updates.example.test/releases/1.1.0/artifacts/";
    envelope.manifest.appId = "com.example.app";
    envelope.manifest.channel = "stable";
    envelope.manifest.platform = "windows";
    envelope.manifest.arch = "x64";
    envelope.manifest.version = autoupdater::Version::parse("1.1.0").value();
    envelope.manifest.baseUrl = "artifacts";
    return envelope;
}

} // namespace

void testUpdatePlannerCreatesOperations() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();
    envelope.manifest.files.push_back({"bin/app.exe", "", "newhash", 7});
    envelope.manifest.remove.push_back("old.dll");

    autoupdater::LocalSnapshot snapshot;
    snapshot.files.push_back({"bin/app.exe", true, "oldhash", 7});

    auto decision = autoupdater::planUpdate(config, envelope, snapshot, std::nullopt);
    LAU_REQUIRE(decision);
    LAU_REQUIRE(decision.value().checkResult.updateAvailable);
    LAU_REQUIRE(decision.value().downloads.size() == 1);
    LAU_REQUIRE(decision.value().downloads[0].url ==
                "https://updates.example.test/releases/1.1.0/artifacts/bin/app.exe");
    LAU_REQUIRE(decision.value().operations.size() == 2);
    LAU_REQUIRE(decision.value().operations[0].type == autoupdater::ApplyOperationType::Replace);
    LAU_REQUIRE(decision.value().operations[1].type == autoupdater::ApplyOperationType::Remove);
}

void testUpdatePlannerPercentEncodesArtifactPaths() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();
    const std::string artifactPath = u8"资源/My App#100%.bin";
    envelope.manifest.files.push_back({artifactPath, "", "newhash", 9});

    autoupdater::LocalSnapshot snapshot;
    auto decision = autoupdater::planUpdate(config, envelope, snapshot, std::nullopt);
    if (!decision) {
        throw std::runtime_error(decision.error().message);
    }
    LAU_REQUIRE(decision.value().downloads.size() == 1);
    LAU_REQUIRE(decision.value().downloads[0].url == "https://updates.example.test/releases/1.1.0/artifacts/"
                                                     "%E8%B5%84%E6%BA%90/My%20App%23100%25.bin");
}
