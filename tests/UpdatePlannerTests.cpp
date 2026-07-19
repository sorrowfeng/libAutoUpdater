#include "TestCommon.h"

#include "UpdatePlanner.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

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

    auto reservedFileEnvelope = plannerEnvelope();
    reservedFileEnvelope.manifest.files.push_back(
        {"artifacts/app.exe", ".AUToupdater/staging/rollback-plan.json", "newhash", 7});
    const auto reservedFile =
        autoupdater::planUpdate(config, reservedFileEnvelope, {}, std::nullopt);
    LAU_REQUIRE(!reservedFile);
    LAU_REQUIRE(reservedFile.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

    auto reservedRemoveEnvelope = plannerEnvelope();
    reservedRemoveEnvelope.manifest.remove.push_back(".autoupdater/journal/terminal.json");
    const auto reservedRemove =
        autoupdater::planUpdate(config, reservedRemoveEnvelope, {}, std::nullopt);
    LAU_REQUIRE(!reservedRemove);
    LAU_REQUIRE(reservedRemove.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
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

void testUpdatePlannerRequiresVerifiedLocalDowngradeAuthorization() {
    struct DowngradeCase {
        bool remoteAllows = false;
        bool signatureVerified = false;
        bool localAllows = false;
        bool accepted = false;
    };
    constexpr std::array<DowngradeCase, 8> cases{{
        {false, false, false, false},
        {false, false, true, false},
        {false, true, false, false},
        {false, true, true, false},
        {true, false, false, false},
        {true, false, true, false},
        {true, true, false, false},
        {true, true, true, true},
    }};

    for (const auto& testCase : cases) {
        auto config = plannerConfig();
        config.currentVersion = autoupdater::Version::parse("2.0.0").value();
        config.security.rejectDowngrade = !testCase.localAllows;
        config.security.requireManifestSignature = testCase.signatureVerified;

        auto envelope = plannerEnvelope();
        envelope.manifest.version = autoupdater::Version::parse("1.5.0").value();
        envelope.manifest.allowDowngrade = testCase.remoteAllows;
        envelope.releaseManifestSignatureVerified = testCase.signatureVerified;

        const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
        if (testCase.accepted) {
            LAU_REQUIRE(decision);
            LAU_REQUIRE(decision.value().checkResult.updateAvailable);
        } else {
            LAU_REQUIRE(!decision);
            LAU_REQUIRE(decision.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        }
    }

    auto config = plannerConfig();
    config.currentVersion = autoupdater::Version::parse("2.0.0").value();
    config.security.rejectDowngrade = false;
    auto envelope = plannerEnvelope();
    envelope.manifest.version = autoupdater::Version::parse("1.5.0").value();
    envelope.manifest.allowDowngrade = true;
    envelope.releaseManifestSignatureVerified = true;

    config.security.requireManifestSignature = false;
    const auto signaturePolicyDisabled = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
    LAU_REQUIRE(!signaturePolicyDisabled);
    LAU_REQUIRE(signaturePolicyDisabled.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

    config.security.requireManifestSignature = true;
    envelope.releaseManifestSignatureVerified = false;
    const auto releaseSignatureUnverified = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
    LAU_REQUIRE(!releaseSignatureUnverified);
    LAU_REQUIRE(releaseSignatureUnverified.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

void testUpdatePlannerUsesHighestDowngradeBaseline() {
    struct BaselineCase {
        const char* currentVersion = nullptr;
        const char* lastAcceptedVersion = nullptr;
        const char* remoteVersion = nullptr;
    };
    constexpr std::array<BaselineCase, 2> cases{{
        {"3.0.0", "2.0.0", "2.5.0"},
        {"1.0.0", "3.0.0", "2.0.0"},
    }};

    for (const auto& testCase : cases) {
        auto config = plannerConfig();
        config.currentVersion = autoupdater::Version::parse(testCase.currentVersion).value();
        const auto lastAccepted = autoupdater::Version::parse(testCase.lastAcceptedVersion).value();

        auto envelope = plannerEnvelope();
        envelope.manifest.version = autoupdater::Version::parse(testCase.remoteVersion).value();

        const auto rejected = autoupdater::planUpdate(config, envelope, {}, lastAccepted);
        LAU_REQUIRE(!rejected);
        LAU_REQUIRE(rejected.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

        config.security.rejectDowngrade = false;
        config.security.requireManifestSignature = true;
        envelope.manifest.allowDowngrade = true;
        envelope.releaseManifestSignatureVerified = true;
        const auto accepted = autoupdater::planUpdate(config, envelope, {}, lastAccepted);
        LAU_REQUIRE(accepted);
        LAU_REQUIRE(accepted.value().checkResult.updateAvailable);
    }
}

void testUpdatePlannerRejectsUnauthorizedDowngradeBeforeReinstallDecision() {
    auto config = plannerConfig();
    config.currentVersion = autoupdater::Version::parse("2.0.0").value();
    config.security.rejectDowngrade = false;
    config.security.requireManifestSignature = true;

    auto envelope = plannerEnvelope();
    envelope.manifest.version = autoupdater::Version::parse("1.5.0").value();
    envelope.manifest.minVersion = autoupdater::Version::parse("3.0.0").value();
    envelope.manifest.allowDowngrade = true;
    envelope.releaseManifestSignatureVerified = false;

    const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
    LAU_REQUIRE(!decision);
    LAU_REQUIRE(decision.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

void testUpdatePlannerPreservesNormalUpgradeAndSameVersionSemantics() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();

    envelope.releaseManifestSignatureVerified = false;
    envelope.manifest.allowDowngrade = false;
    config.security.rejectDowngrade = true;
    const auto upgrade = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
    LAU_REQUIRE(upgrade);
    LAU_REQUIRE(upgrade.value().checkResult.updateAvailable);

    config.security.rejectDowngrade = false;
    config.security.requireManifestSignature = true;
    envelope.manifest.version = config.currentVersion;
    envelope.manifest.allowDowngrade = true;
    envelope.releaseManifestSignatureVerified = true;
    const auto sameVersion = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
    LAU_REQUIRE(sameVersion);
    LAU_REQUIRE(!sameVersion.value().checkResult.updateAvailable);
}

void testUpdatePlannerRejectsProgrammaticManagedTargetConflictsEarly() {
    const auto config = plannerConfig();
    const auto requireRejectedAtCurrentVersion = [&](std::vector<autoupdater::ManifestFile> files,
                                                     std::vector<std::string> remove = {}) {
        auto envelope = plannerEnvelope();
        envelope.manifest.version = config.currentVersion;
        envelope.manifest.files = std::move(files);
        envelope.manifest.remove = std::move(remove);

        const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
        LAU_REQUIRE(!decision);
        LAU_REQUIRE(decision.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    };

    requireRejectedAtCurrentVersion({
        {"objects/first.bin", "bin/app.exe", "same-hash", 4},
        {"objects/second.bin", "bin/app.exe", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({
        {"objects/first.bin", "Bin/App.exe", "same-hash", 4},
        {"objects/second.bin", "bin/app.EXE", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({}, {"obsolete.dll", "obsolete.dll"});
    requireRejectedAtCurrentVersion(
        {{"objects/app.bin", "bin/app.exe", "same-hash", 4}}, {"bin/app.exe"});
    requireRejectedAtCurrentVersion({
        {"bin/app.exe", "", "same-hash", 4},
        {"objects/app.bin", "bin/app.exe", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({
        {"objects/directory", "bin", "same-hash", 4},
        {"objects/child.bin", "bin/app.exe", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({
        {"objects/child.bin", "bin/app.exe", "same-hash", 4},
        {"objects/directory", "bin", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({
        {"objects/payload.bin", ".autoupdater/staging/payload.bin", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({
        {".autoupdater/staging/payload.bin", "", "same-hash", 4},
    });
    requireRejectedAtCurrentVersion({}, {".autoupdater/journal/active.json"});

    auto snapshotEnvelope = plannerEnvelope();
    snapshotEnvelope.manifest.files = {
        {"objects/first.bin", "bin/app.exe", "same-hash", 4},
        {"objects/second.bin", "bin/app.exe", "same-hash", 4},
    };
    autoupdater::LocalSnapshot snapshot;
    snapshot.files.push_back({"bin/app.exe", true, "same-hash", 4});

    const auto snapshotDecision =
        autoupdater::planUpdate(config, snapshotEnvelope, snapshot, std::nullopt);
    LAU_REQUIRE(!snapshotDecision);
    LAU_REQUIRE(snapshotDecision.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

void testUpdatePlannerAllowsSharedSourceForDistinctManagedTargets() {
    const auto config = plannerConfig();
    auto envelope = plannerEnvelope();
    envelope.manifest.files = {
        {"objects/shared.bin", "a", "same-hash", 9},
        {"objects/shared.bin", "ab", "same-hash", 9},
        {"objects/shared.bin", "shared/first.bin", "same-hash", 9},
        {"objects/shared.bin", "shared/second.bin", "same-hash", 9},
    };

    const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt);
    LAU_REQUIRE(decision);
    LAU_REQUIRE(decision.value().checkResult.updateAvailable);
    LAU_REQUIRE(decision.value().operations.size() == 4);
    LAU_REQUIRE(!decision.value().downloads.empty());
    LAU_REQUIRE(decision.value().operations[0].target == "a");
    LAU_REQUIRE(decision.value().operations[1].target == "ab");
    LAU_REQUIRE(decision.value().operations[2].target == "shared/first.bin");
    LAU_REQUIRE(decision.value().operations[3].target == "shared/second.bin");
}
