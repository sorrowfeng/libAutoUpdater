#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "ApplyJournal.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "util/Json.h"
#include "util/PathUtil.h"

#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace {

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

void testApplyExecutorUsesSafeAtomicJournalName() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-journal-name-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    autoupdater::ApplyPlan plan;
    plan.installDir = root / "install";
    plan.stagingDir = root / "staging";
    plan.backupDir = root / "backup";
    plan.toVersion = "2.0.0";
    plan.manifestSha256 = "manifest-digest";

    const std::vector<std::string> untrustedReleaseIds = {
        "",
        "../escaped",
        "..\\escaped-windows",
        (root / "absolute-journal").string(),
        "\\\\server\\share\\journal",
        std::string("nul\0suffix", 10),
        std::string(4096, 'a'),
    };

    for (const auto& releaseId : untrustedReleaseIds) {
        plan.releaseId = releaseId;
        const auto digest = autoupdater::updater::applyPlanDigest(plan);
        LAU_REQUIRE(digest);
        LAU_REQUIRE(digest.value().size() == 64);
        for (std::size_t i = 0; i < 64; ++i) {
            const auto value = digest.value()[i];
            LAU_REQUIRE((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
        }
    }

    plan.releaseId = "same-release";
    const auto emptyPlanName = autoupdater::updater::applyPlanDigest(plan);
    LAU_REQUIRE(emptyPlanName);
    plan.operations.push_back({autoupdater::ApplyOperationType::Remove, "", "obsolete.txt", "", 0});
    const auto changedPlanName = autoupdater::updater::applyPlanDigest(plan);
    LAU_REQUIRE(changedPlanName);
    LAU_REQUIRE(emptyPlanName.value() != changedPlanName.value());
    plan.operations.clear();

    plan.releaseId = "../../../escaped";
    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(result);
    LAU_REQUIRE(!std::filesystem::exists(root / "escaped.json"));

    const auto absoluteJournal = root / "absolute-journal.json";
    writeFile(absoluteJournal, "sentinel");
    plan.releaseId = (root / "absolute-journal").string();
    plan.toVersion = "2.0.0\"\nwith-control";
    result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(result);
    LAU_REQUIRE(readFile(absoluteJournal) == "sentinel");

    const auto journalDir = plan.installDir / ".autoupdater" / "journal";
    std::size_t journalCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(journalDir)) {
        LAU_REQUIRE(entry.path().extension() != ".tmp");
        LAU_REQUIRE(entry.path().parent_path() == journalDir);
        ++journalCount;
    }
    LAU_REQUIRE(journalCount == 5);

    const auto terminal = autoupdater::updater::parseActiveTransaction(readFile(journalDir / "terminal.json"));
    LAU_REQUIRE(terminal);
    const auto planDigest = autoupdater::updater::applyPlanDigest(plan);
    LAU_REQUIRE(planDigest);
    LAU_REQUIRE(terminal.value().planDigest == planDigest.value());
    LAU_REQUIRE(terminal.value().transactionId != terminal.value().planDigest);

    const auto summary = autoupdater::updater::parseApplyJournalSummary(
        readFile(journalDir / (terminal.value().transactionId + ".json")));
    LAU_REQUIRE(summary);
    LAU_REQUIRE(summary.value().fileState == autoupdater::updater::JournalFileState::Complete);
    LAU_REQUIRE(summary.value().restartState == autoupdater::updater::JournalRestartState::NotRequested);
    const auto snapshot =
        autoupdater::ApplyPlan::parse(readFile(journalDir / (terminal.value().transactionId + ".plan.json")));
    LAU_REQUIRE(snapshot);
    LAU_REQUIRE(snapshot.value().toVersion == plan.toVersion);

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorRequiresWritableJournal() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-journal-required-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / "install";
    const auto staging = root / "staging";
    const auto backup = root / "backup";
    writeFile(install / "bin/app.txt", "old");
    writeFile(staging / "bin/app.txt", "new");
    writeFile(install / ".autoupdater" / "journal", "not-a-directory");

    auto hash = autoupdater::createDefaultHashProvider();
    const auto expectedHash = hash->sha256Bytes("new");
    LAU_REQUIRE(expectedHash);

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "journal-required";
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "bin/app.txt", "bin/app.txt", expectedHash.value(), 3});

    const auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::FileSystemError);
    LAU_REQUIRE(readFile(install / "bin/app.txt") == "old");

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorRollsBackCurrentFailedOperation() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-apply-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / "install";
    const auto staging = root / "staging";
    const auto backup = root / "backup";

    writeFile(install / "bin/a.txt", "old-a");
    writeFile(install / "bin/b.txt", "old-b");
    writeFile(staging / "bin/a.txt", "new-a");
    writeFile(staging / "bin/b.txt", "bad-b");

    auto hash = autoupdater::createDefaultHashProvider();
    auto hashA = hash->sha256Bytes("new-a");
    LAU_REQUIRE(hashA);

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "test";
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/a.txt", "bin/a.txt", hashA.value(), 5});
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "bin/b.txt", "bin/b.txt", "not-the-real-hash", 5});

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(readFile(install / "bin/a.txt") == "old-a");
    LAU_REQUIRE(readFile(install / "bin/b.txt") == "old-b");

    const auto limitedInstall = root / "limited-install";
    const auto limitedStaging = root / "limited-staging";
    const auto limitedBackup = root / "limited-backup";
    writeFile(limitedInstall / "large.txt", "large");
    writeFile(limitedStaging / "large.txt", "new");
    auto smallHash = hash->sha256Bytes("new");
    LAU_REQUIRE(smallHash);
    autoupdater::ApplyPlan limitedPlan;
    limitedPlan.installDir = limitedInstall;
    limitedPlan.stagingDir = limitedStaging;
    limitedPlan.backupDir = limitedBackup;
    limitedPlan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "large.txt", "large.txt", smallHash.value(), 3});
    autoupdater::updater::ApplyExecutorDependencies dependencies;
    dependencies.fileSystem = autoupdater::createDefaultFileSystem();
    dependencies.hashProvider = hash;
    dependencies.processLauncher = autoupdater::createDefaultProcessLauncher();
    dependencies.limits.maxArtifactBytes = 4;
    dependencies.limits.maxTotalArtifactBytes = 8;
    result = autoupdater::updater::executeApplyPlanWithDependencies(limitedPlan, std::move(dependencies));
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    LAU_REQUIRE(readFile(limitedInstall / "large.txt") == "large");
    LAU_REQUIRE(!std::filesystem::exists(limitedBackup / "large.txt"));

    const auto aggregateInstall = root / "aggregate-install";
    const auto aggregateStaging = root / "aggregate-staging";
    const auto aggregateBackup = root / "aggregate-backup";
    for (const auto* name : {"a.txt", "b.txt"}) {
        writeFile(aggregateInstall / name, "four");
        writeFile(aggregateStaging / name, "new");
    }
    autoupdater::ApplyPlan aggregatePlan;
    aggregatePlan.installDir = aggregateInstall;
    aggregatePlan.stagingDir = aggregateStaging;
    aggregatePlan.backupDir = aggregateBackup;
    aggregatePlan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "a.txt", "a.txt", smallHash.value(), 3});
    aggregatePlan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "b.txt", "b.txt", smallHash.value(), 3});
    autoupdater::updater::ApplyExecutorDependencies aggregateDependencies;
    aggregateDependencies.fileSystem = autoupdater::createDefaultFileSystem();
    aggregateDependencies.hashProvider = hash;
    aggregateDependencies.processLauncher = autoupdater::createDefaultProcessLauncher();
    aggregateDependencies.limits.maxArtifactBytes = 4;
    aggregateDependencies.limits.maxTotalArtifactBytes = 7;
    result = autoupdater::updater::executeApplyPlanWithDependencies(aggregatePlan, std::move(aggregateDependencies));
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    LAU_REQUIRE(readFile(aggregateInstall / "a.txt") == "four");
    LAU_REQUIRE(readFile(aggregateInstall / "b.txt") == "four");

    const auto boundaryInstall = root / "boundary-install";
    const auto boundaryStaging = root / "boundary-staging";
    const auto boundaryBackup = root / "boundary-backup";
    for (const auto* name : {"a.txt", "b.txt"}) {
        writeFile(boundaryInstall / name, "four");
        writeFile(boundaryStaging / name, "new");
    }
    auto boundaryPlan = aggregatePlan;
    boundaryPlan.installDir = boundaryInstall;
    boundaryPlan.stagingDir = boundaryStaging;
    boundaryPlan.backupDir = boundaryBackup;
    autoupdater::updater::ApplyExecutorDependencies boundaryDependencies;
    boundaryDependencies.fileSystem = autoupdater::createDefaultFileSystem();
    boundaryDependencies.hashProvider = hash;
    boundaryDependencies.processLauncher = autoupdater::createDefaultProcessLauncher();
    boundaryDependencies.limits.maxArtifactBytes = 4;
    boundaryDependencies.limits.maxTotalArtifactBytes = 8;
    result = autoupdater::updater::executeApplyPlanWithDependencies(boundaryPlan, std::move(boundaryDependencies));
    LAU_REQUIRE(result);
    LAU_REQUIRE(readFile(boundaryInstall / "a.txt") == "new");
    LAU_REQUIRE(readFile(boundaryInstall / "b.txt") == "new");

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorRejectsExistingLock() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-lock-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / "install";
    std::filesystem::create_directories(install / ".autoupdater" / "update.lock", ec);

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = root / "staging";
    plan.backupDir = root / "backup";
    plan.releaseId = "lock-test";

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyFailed);

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorReplacesFilesInUnicodeDirectory() {
    const auto root =
        std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"libAutoUpdater-应用测试-中文路径");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / std::filesystem::u8path(u8"安装目录");
    const auto staging = root / std::filesystem::u8path(u8"暂存目录");
    const auto backup = root / std::filesystem::u8path(u8"备份目录");
    const auto managedPath = u8"资源/应用.txt";

    writeFile(autoupdater::util::safeJoin(install, managedPath).value(), "version 1\n");
    writeFile(autoupdater::util::safeJoin(staging, managedPath).value(), "version 2\n");

    auto hash = autoupdater::createDefaultHashProvider();
    auto hashNew = hash->sha256Bytes("version 2\n");
    LAU_REQUIRE(hashNew);

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "unicode-apply-test";
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, managedPath, managedPath, hashNew.value(), 10});

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(result);
    LAU_REQUIRE(readFile(autoupdater::util::safeJoin(install, managedPath).value()) == "version 2\n");
    LAU_REQUIRE(readFile(autoupdater::util::safeJoin(backup, managedPath).value()) == "version 1\n");

    std::filesystem::remove_all(root, ec);
}
