#include "TestCommon.h"

#include "UpdatePlanner.h"

void testUpdatePlannerCreatesOperations() {
    autoupdater::Config config;
    config.appId = "com.example.app";
    config.channel = "stable";
    config.platform = "windows";
    config.arch = "x64";
    config.installDir = "install";
    config.tempDir = "install/.autoupdater/staging/1.1.0";
    config.currentVersion = autoupdater::Version::parse("1.0.0").value();

    autoupdater::ManifestEnvelope envelope;
    envelope.sha256 = "manifest";
    envelope.manifest.appId = "com.example.app";
    envelope.manifest.channel = "stable";
    envelope.manifest.platform = "windows";
    envelope.manifest.arch = "x64";
    envelope.manifest.version = autoupdater::Version::parse("1.1.0").value();
    envelope.manifest.baseUrl = "file:///release";
    envelope.manifest.files.push_back({"bin/app.exe", "", "newhash", 7});
    envelope.manifest.remove.push_back("old.dll");

    autoupdater::LocalSnapshot snapshot;
    snapshot.files.push_back({"bin/app.exe", true, "oldhash", 7});

    auto decision = autoupdater::planUpdate(config, envelope, snapshot, std::nullopt);
    LAU_REQUIRE(decision);
    LAU_REQUIRE(decision.value().checkResult.updateAvailable);
    LAU_REQUIRE(decision.value().downloads.size() == 1);
    LAU_REQUIRE(decision.value().operations.size() == 2);
    LAU_REQUIRE(decision.value().operations[0].type == autoupdater::ApplyOperationType::Replace);
    LAU_REQUIRE(decision.value().operations[1].type == autoupdater::ApplyOperationType::Remove);
}

