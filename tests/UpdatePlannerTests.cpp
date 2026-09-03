#include "TestCommon.h"

#include "UpdatePlanner.h"

#include <algorithm>
#include <array>
#include <chrono>
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

const autoupdater::util::UtcInstant& plannerNow() {
    static const auto instant = autoupdater::util::parseRfc3339("2026-07-19T12:00:00Z").value();
    return instant;
}

} // namespace

void testUpdatePlannerCreatesOperations() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();
    envelope.manifest.files.push_back({"bin/app.exe", "", "newhash", 7});
    envelope.manifest.remove.push_back("old.dll");

    autoupdater::LocalSnapshot snapshot;
    snapshot.files.push_back({"bin/app.exe", true, "oldhash", 7});

    auto decision = autoupdater::planUpdate(config, envelope, snapshot, std::nullopt, plannerNow());
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
    const auto reservedFile = autoupdater::planUpdate(config, reservedFileEnvelope, {}, std::nullopt, plannerNow());
    LAU_REQUIRE(!reservedFile);
    LAU_REQUIRE(reservedFile.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

    auto reservedRemoveEnvelope = plannerEnvelope();
    reservedRemoveEnvelope.manifest.remove.push_back(".autoupdater/journal/terminal.json");
    const auto reservedRemove = autoupdater::planUpdate(config, reservedRemoveEnvelope, {}, std::nullopt, plannerNow());
    LAU_REQUIRE(!reservedRemove);
    LAU_REQUIRE(reservedRemove.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

void testUpdatePlannerPercentEncodesArtifactPaths() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();
    const std::string artifactPath = u8"资源/My App#100%.bin";
    envelope.manifest.files.push_back({artifactPath, "", "newhash", 9});

    autoupdater::LocalSnapshot snapshot;
    auto decision = autoupdater::planUpdate(config, envelope, snapshot, std::nullopt, plannerNow());
    if (!decision) {
        throw std::runtime_error(decision.error().message);
    }
    LAU_REQUIRE(decision.value().downloads.size() == 1);
    LAU_REQUIRE(decision.value().downloads[0].url == "https://updates.example.test/releases/1.1.0/artifacts/"
                                                     "%E8%B5%84%E6%BA%90/My%20App%23100%25.bin");
}

void testUpdatePlannerEnforcesRfc3339ExpiryBoundary() {
    const auto now = autoupdater::util::parseRfc3339("2026-07-19T12:00:00Z").value();
    auto config = plannerConfig();

    auto valid = plannerEnvelope();
    valid.manifest.releaseDate = "2026-07-18T12:00:00Z";
    valid.manifest.publishedAt = "2026-07-19T11:00:00-01:00";
    valid.manifest.expiresAt = "2026-07-19T12:00:00.000000001Z";
    const auto beforeBoundary = autoupdater::planUpdate(config, valid, {}, std::nullopt, now);
    LAU_REQUIRE(beforeBoundary);

    valid.manifest.expiresAt = "2026-07-19T14:00:00+02:00";
    const auto atBoundary = autoupdater::planUpdate(config, valid, {}, std::nullopt, now);
    LAU_REQUIRE(!atBoundary);
    LAU_REQUIRE(atBoundary.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

    valid.manifest.expiresAt = "2026-07-19T11:59:59.999999999Z";
    const auto afterBoundary = autoupdater::planUpdate(config, valid, {}, std::nullopt, now);
    LAU_REQUIRE(!afterBoundary);
    LAU_REQUIRE(afterBoundary.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

    config.security.rejectExpiredManifest = false;
    valid.manifest.expiresAt = "2000-01-01T00:00:00Z";
    const auto policyDisabled = autoupdater::planUpdate(config, valid, {}, std::nullopt, now);
    LAU_REQUIRE(policyDisabled);

    valid.manifest.expiresAt = "not-a-time";
    const auto malformedExpiry = autoupdater::planUpdate(config, valid, {}, std::nullopt, now);
    LAU_REQUIRE(!malformedExpiry);
    LAU_REQUIRE(malformedExpiry.error().code == autoupdater::ErrorCode::ManifestParseFailed);

    valid.manifest.expiresAt.clear();
    valid.manifest.releaseDate = "2026-02-30T00:00:00Z";
    const auto malformedMetadata = autoupdater::planUpdate(config, valid, {}, std::nullopt, now);
    LAU_REQUIRE(!malformedMetadata);
    LAU_REQUIRE(malformedMetadata.error().code == autoupdater::ErrorCode::ManifestParseFailed);
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

        const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
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
    const auto signaturePolicyDisabled = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
    LAU_REQUIRE(!signaturePolicyDisabled);
    LAU_REQUIRE(signaturePolicyDisabled.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

    config.security.requireManifestSignature = true;
    envelope.releaseManifestSignatureVerified = false;
    const auto releaseSignatureUnverified = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
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

        const auto rejected = autoupdater::planUpdate(config, envelope, {}, lastAccepted, plannerNow());
        LAU_REQUIRE(!rejected);
        LAU_REQUIRE(rejected.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);

        config.security.rejectDowngrade = false;
        config.security.requireManifestSignature = true;
        envelope.manifest.allowDowngrade = true;
        envelope.releaseManifestSignatureVerified = true;
        const auto accepted = autoupdater::planUpdate(config, envelope, {}, lastAccepted, plannerNow());
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

    const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
    LAU_REQUIRE(!decision);
    LAU_REQUIRE(decision.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

void testUpdatePlannerPreservesNormalUpgradeAndSameVersionSemantics() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();

    envelope.releaseManifestSignatureVerified = false;
    envelope.manifest.allowDowngrade = false;
    config.security.rejectDowngrade = true;
    const auto upgrade = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
    LAU_REQUIRE(upgrade);
    LAU_REQUIRE(upgrade.value().checkResult.updateAvailable);

    config.security.rejectDowngrade = false;
    config.security.requireManifestSignature = true;
    envelope.manifest.version = config.currentVersion;
    envelope.manifest.allowDowngrade = true;
    envelope.releaseManifestSignatureVerified = true;
    const auto sameVersion = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
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

        const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
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
    requireRejectedAtCurrentVersion({{"objects/app.bin", "bin/app.exe", "same-hash", 4}}, {"bin/app.exe"});
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
        autoupdater::planUpdate(config, snapshotEnvelope, snapshot, std::nullopt, plannerNow());
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

    const auto decision = autoupdater::planUpdate(config, envelope, {}, std::nullopt, plannerNow());
    LAU_REQUIRE(decision);
    LAU_REQUIRE(decision.value().checkResult.updateAvailable);
    LAU_REQUIRE(decision.value().operations.size() == 4);
    LAU_REQUIRE(!decision.value().downloads.empty());
    LAU_REQUIRE(decision.value().operations[0].target == "a");
    LAU_REQUIRE(decision.value().operations[1].target == "ab");
    LAU_REQUIRE(decision.value().operations[2].target == "shared/first.bin");
    LAU_REQUIRE(decision.value().operations[3].target == "shared/second.bin");
}

void testUpdatePlannerIndexesLargeSnapshots() {
    auto config = plannerConfig();
    auto envelope = plannerEnvelope();
    autoupdater::LocalSnapshot snapshot;
    constexpr std::size_t kFileCount = 10000;
    const std::string hash(64, 'a');
    const std::string pathPrefix = "assets/" + std::string(96, 'a') + "/managed-file-";
    envelope.manifest.files.reserve(kFileCount);
    snapshot.files.reserve(kFileCount);

    for (std::size_t index = 0; index < kFileCount; ++index) {
        auto suffix = std::to_string(index);
        suffix.insert(suffix.begin(), 5 - suffix.size(), '0');
        const auto path = pathPrefix + suffix + ".bin";
        envelope.manifest.files.push_back({path, "", hash, 64});
        snapshot.files.push_back({path, true, hash, 64});
    }
    std::reverse(snapshot.files.begin(), snapshot.files.end());
    snapshot.files[kFileCount / 2].sha256 = std::string(64, 'b');
    const auto changedPath = snapshot.files[kFileCount / 2].path;

    const auto started = std::chrono::steady_clock::now();
    const auto decision = autoupdater::planUpdate(config, envelope, snapshot, std::nullopt, plannerNow());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    LAU_REQUIRE(decision);
    LAU_REQUIRE(decision.value().checkResult.updateAvailable);
    LAU_REQUIRE(decision.value().downloads.size() == 1);
    LAU_REQUIRE(decision.value().operations.size() == 1);
    LAU_REQUIRE(decision.value().operations.front().target == changedPath);
    // This is deliberately generous for debug/sanitizer CI while still
    // catching the former reversed-vector O(n^2) lookup at the supported
    // 10,000-entry manifest limit.
    LAU_REQUIRE(elapsed < std::chrono::seconds(5));

    auto normalizedEnvelope = plannerEnvelope();
    normalizedEnvelope.manifest.files.push_back({"objects/app.bin", "Bin/App.exe", hash, 64});
    autoupdater::LocalSnapshot normalizedSnapshot;
    normalizedSnapshot.files.push_back({"bin/app.EXE", true, hash, 64});
    const auto normalized =
        autoupdater::planUpdate(config, normalizedEnvelope, normalizedSnapshot, std::nullopt, plannerNow());
    LAU_REQUIRE(normalized);
    LAU_REQUIRE(normalized.value().downloads.empty());
    LAU_REQUIRE(normalized.value().operations.empty());

    autoupdater::LocalSnapshot invalidSnapshot;
    invalidSnapshot.files.push_back({"Bin\\App.exe", true, hash, 64});
    const auto invalid =
        autoupdater::planUpdate(config, normalizedEnvelope, invalidSnapshot, std::nullopt, plannerNow());
    LAU_REQUIRE(!invalid);
    LAU_REQUIRE(invalid.error().code == autoupdater::ErrorCode::PathTraversalRejected);
}
