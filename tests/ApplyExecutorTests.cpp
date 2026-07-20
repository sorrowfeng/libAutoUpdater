#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "ApplyJournal.h"
#include "ProcessWait.h"
#include "RootedCommitFault.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "util/Json.h"
#include "util/PathUtil.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
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

autoupdater::updater::ActiveTransaction readTerminalTransaction(const std::filesystem::path& installDir) {
    const auto terminal = autoupdater::updater::parseActiveTransaction(
        readFile(installDir / ".autoupdater" / "journal" / "terminal.json"));
    LAU_REQUIRE(terminal);
    return terminal.value();
}

autoupdater::updater::ApplyExecutorDependencies
applyDependencies(const std::shared_ptr<autoupdater::IHashProvider>& hashProvider) {
    autoupdater::updater::ApplyExecutorDependencies dependencies;
    dependencies.fileSystem = autoupdater::createDefaultFileSystem();
    dependencies.hashProvider = hashProvider;
    dependencies.processLauncher = autoupdater::createDefaultProcessLauncher();
    return dependencies;
}

class FailingProcessLauncher final : public autoupdater::IProcessLauncher {
  public:
    autoupdater::Result<void> launch(const autoupdater::ProcessLaunchRequest&) noexcept override {
        ++launchCalls_;
        return autoupdater::Result<void>::fail(
            {autoupdater::ErrorCode::ApplyLaunchFailed,
             "Injected restart launch failure: https://example.test/?token=AU022_JOURNAL_SECRET"});
    }

    int launchCalls() const noexcept {
        return launchCalls_;
    }

  private:
    int launchCalls_ = 0;
};

struct PublicRollbackScenario {
    std::filesystem::path root;
    std::filesystem::path installDir;
    std::filesystem::path stagingDir;
    std::filesystem::path forwardBackupDir;
    std::filesystem::path rollbackUndoDir;
    std::shared_ptr<autoupdater::IHashProvider> hashProvider;
    autoupdater::ApplyPlan forwardPlan;
    autoupdater::ApplyPlan rollbackRequest;
    autoupdater::updater::ActiveTransaction forwardTerminal;
};

PublicRollbackScenario completePublicRollbackSource(const std::filesystem::path& root, bool legacySchema = false) {
    constexpr std::string_view kOldReplaced = "old-replaced";
    constexpr std::string_view kNewReplaced = "new-replaced";
    constexpr std::string_view kNewAdded = "new-added";
    constexpr std::string_view kOldRemoved = "old-removed";

    PublicRollbackScenario scenario;
    scenario.root = root;
    scenario.installDir = root / "install";
    scenario.stagingDir = root / "staging";
    scenario.forwardBackupDir = root / "forward-backup";
    scenario.hashProvider = autoupdater::createDefaultHashProvider();

    writeFile(scenario.installDir / "bin/replaced.txt", std::string(kOldReplaced));
    writeFile(scenario.installDir / "bin/removed.txt", std::string(kOldRemoved));
    writeFile(scenario.stagingDir / "bin/replaced.txt", std::string(kNewReplaced));
    writeFile(scenario.stagingDir / "bin/added.txt", std::string(kNewAdded));

#ifndef _WIN32
    constexpr auto kReplacedMode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                   std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
    constexpr auto kRemovedMode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                  std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec;
    std::error_code permissionsError;
    std::filesystem::permissions(scenario.installDir / "bin/replaced.txt", kReplacedMode,
                                 std::filesystem::perm_options::replace, permissionsError);
    LAU_REQUIRE(!permissionsError);
    std::filesystem::permissions(scenario.installDir / "bin/removed.txt", kRemovedMode,
                                 std::filesystem::perm_options::replace, permissionsError);
    LAU_REQUIRE(!permissionsError);
#endif

    const auto replacedHash = scenario.hashProvider->sha256Bytes(std::string(kNewReplaced));
    const auto addedHash = scenario.hashProvider->sha256Bytes(std::string(kNewAdded));
    LAU_REQUIRE(replacedHash);
    LAU_REQUIRE(addedHash);

    scenario.forwardPlan.appId = "com.example.public-rollback";
    scenario.forwardPlan.schemaVersion = legacySchema ? 1 : 2;
    scenario.forwardPlan.fromVersion = "1.0.0";
    scenario.forwardPlan.toVersion = "2.0.0";
    scenario.forwardPlan.releaseId = "release-2";
    scenario.forwardPlan.manifestSha256 = std::string(64, 'd');
    scenario.forwardPlan.installDir = scenario.installDir;
    scenario.forwardPlan.stagingDir = scenario.stagingDir;
    scenario.forwardPlan.backupDir = scenario.forwardBackupDir;
    scenario.forwardPlan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/replaced.txt",
                                               "bin/replaced.txt", replacedHash.value(), kNewReplaced.size()});
    scenario.forwardPlan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/added.txt",
                                               "bin/added.txt", addedHash.value(), kNewAdded.size()});
    scenario.forwardPlan.operations.push_back({autoupdater::ApplyOperationType::Remove, "", "bin/removed.txt", "", 0});
    scenario.forwardPlan.operations.push_back(
        {autoupdater::ApplyOperationType::Remove, "", "bin/already-missing.txt", "", 0});

    const auto applied = autoupdater::updater::executeApplyPlan(scenario.forwardPlan);
    LAU_REQUIRE(applied);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == kNewReplaced);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/added.txt") == kNewAdded);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/removed.txt"));
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/already-missing.txt"));

    scenario.forwardTerminal = readTerminalTransaction(scenario.installDir);
    scenario.rollbackUndoDir = autoupdater::util::defaultStagingRoot(scenario.installDir) / "backup" / "rollback" /
                               autoupdater::util::pathFromUtf8(scenario.forwardTerminal.transactionId);

    scenario.rollbackRequest.intent = autoupdater::ApplyPlanIntent::Rollback;
    scenario.rollbackRequest.rollbackOf = autoupdater::ApplyTransactionReference{
        scenario.forwardTerminal.transactionId,
        scenario.forwardTerminal.planDigest,
    };
    scenario.rollbackRequest.appId = scenario.forwardPlan.appId;
    scenario.rollbackRequest.fromVersion = scenario.forwardPlan.toVersion;
    scenario.rollbackRequest.releaseId = scenario.forwardPlan.releaseId;
    scenario.rollbackRequest.installDir = scenario.installDir;
    scenario.rollbackRequest.stagingDir = scenario.forwardBackupDir;
    scenario.rollbackRequest.backupDir = scenario.rollbackUndoDir;
    return scenario;
}

} // namespace

void testApplyExecutorUsesSafeAtomicJournalName() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-journal-name-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const std::string legacyTransactionId(64, 'a');
    const autoupdater::ApplyOperation legacyOperation{autoupdater::ApplyOperationType::Remove, "", "obsolete.txt", "",
                                                      0};
    std::string legacyIdentityMaterial = legacyTransactionId;
    const auto appendLegacy = [&legacyIdentityMaterial](std::string_view value) {
        legacyIdentityMaterial.push_back('\0');
        legacyIdentityMaterial.append(value.data(), value.size());
    };
    appendLegacy("0");
    appendLegacy("remove");
    appendLegacy(legacyOperation.source);
    appendLegacy(legacyOperation.target);
    appendLegacy(legacyOperation.sha256);
    appendLegacy("0");
    auto identityHash = autoupdater::createDefaultHashProvider();
    const auto legacyExpectedId = identityHash->sha256Bytes(legacyIdentityMaterial);
    const auto legacyActualId = autoupdater::updater::applyOperationId(legacyTransactionId, 0, legacyOperation);
    LAU_REQUIRE(legacyExpectedId);
    LAU_REQUIRE(legacyActualId);
    LAU_REQUIRE(legacyActualId.value() == legacyExpectedId.value());

    autoupdater::ApplyPlan plan;
    plan.installDir = root / "install";
    plan.stagingDir = root / "staging";
    plan.backupDir = root / "backup";
    plan.toVersion = "2.0.0";
    plan.manifestSha256 = std::string(64, 'd');

    auto reservedTargetPlan = plan;
    reservedTargetPlan.operations.push_back(
        {autoupdater::ApplyOperationType::Remove, "", "AUTOUP~1/journal/terminal.json", "", 0});
    const auto reservedTarget = autoupdater::updater::executeApplyPlan(reservedTargetPlan);
    LAU_REQUIRE(!reservedTarget);
    LAU_REQUIRE(reservedTarget.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(!std::filesystem::exists(plan.installDir));

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
    plan.toVersion = "2.0.0";
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
    LAU_REQUIRE(terminal.value().completedAt.has_value());

    const auto summary = autoupdater::updater::parseApplyJournalSummary(
        readFile(journalDir / (terminal.value().transactionId + ".json")));
    LAU_REQUIRE(summary);
    LAU_REQUIRE(summary.value().fileState == autoupdater::updater::JournalFileState::Complete);
    LAU_REQUIRE(summary.value().restartState == autoupdater::updater::JournalRestartState::NotRequested);
    LAU_REQUIRE(summary.value().completedAt == terminal.value().completedAt);
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
        {autoupdater::ApplyOperationType::Replace, "bin/b.txt", "bin/b.txt", std::string(64, '0'), 5});

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().phase == autoupdater::ErrorPhase::Apply);
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

void testApplyExecutorExecutesOperationFreePublicRollback() {
    constexpr std::string_view kOldReplaced = "old-replaced";
    constexpr std::string_view kNewReplaced = "new-replaced";
    constexpr std::string_view kNewAdded = "new-added";
    constexpr std::string_view kOldRemoved = "old-removed";

    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-public-rollback-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto scenario = completePublicRollbackSource(root);

    writeFile(scenario.installDir / "bin/replaced.txt", "unexpected-third-state");
    const auto rejectedStaleTarget = autoupdater::updater::executeApplyPlan(scenario.rollbackRequest);
    LAU_REQUIRE(!rejectedStaleTarget);
    LAU_REQUIRE(rejectedStaleTarget.error().phase == autoupdater::ErrorPhase::Rollback);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / ".autoupdater" / "journal" / "active.json"));
    LAU_REQUIRE(readTerminalTransaction(scenario.installDir).transactionId == scenario.forwardTerminal.transactionId);
    writeFile(scenario.installDir / "bin/replaced.txt", std::string(kNewReplaced));

    writeFile(scenario.forwardBackupDir / "bin/replaced.txt", "tampered-backup");
    const auto rejectedBackup = autoupdater::updater::executeApplyPlan(scenario.rollbackRequest);
    LAU_REQUIRE(!rejectedBackup);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / ".autoupdater" / "journal" / "active.json"));
    writeFile(scenario.forwardBackupDir / "bin/replaced.txt", std::string(kOldReplaced));

    bool racedAfterTerminalValidation = false;
    autoupdater::updater::ApplyExecutionHooks raceHooks;
    raceHooks.checkpoint = [&](std::string_view boundary, std::size_t) {
        if (!racedAfterTerminalValidation && boundary == "journal.plan.after") {
            racedAfterTerminalValidation = true;
            writeFile(scenario.installDir / "bin/replaced.txt", "concurrent-third-state");
        }
        return autoupdater::updater::ApplyFaultAction::Continue;
    };
    const auto rejectedRace = autoupdater::updater::executeApplyPlanWithDependencies(
        scenario.rollbackRequest, applyDependencies(scenario.hashProvider), raceHooks);
    LAU_REQUIRE(!rejectedRace);
    LAU_REQUIRE(racedAfterTerminalValidation);
    LAU_REQUIRE(rejectedRace.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / ".autoupdater" / "journal" / "active.json"));
    LAU_REQUIRE(readTerminalTransaction(scenario.installDir).transactionId == scenario.forwardTerminal.transactionId);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == "concurrent-third-state");
    writeFile(scenario.installDir / "bin/replaced.txt", std::string(kNewReplaced));

    auto operationSmuggling = scenario.rollbackRequest;
    operationSmuggling.operations.emplace_back(autoupdater::ApplyOperationType::Remove, "", "bin/replaced.txt", "", 0);
    const auto rejectedSmuggling = autoupdater::updater::executeApplyPlan(operationSmuggling);
    LAU_REQUIRE(!rejectedSmuggling);
    LAU_REQUIRE(readTerminalTransaction(scenario.installDir).transactionId == scenario.forwardTerminal.transactionId);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == kNewReplaced);

    auto restartSmuggling = scenario.rollbackRequest;
    restartSmuggling.restartCommand = {"untrusted-restart"};
    const auto rejectedRestart = autoupdater::updater::executeApplyPlan(restartSmuggling);
    LAU_REQUIRE(!rejectedRestart);
    LAU_REQUIRE(readTerminalTransaction(scenario.installDir).transactionId == scenario.forwardTerminal.transactionId);

    const auto rolledBack = autoupdater::updater::executeApplyPlan(scenario.rollbackRequest);
    LAU_REQUIRE(rolledBack);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == kOldReplaced);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/added.txt"));
    LAU_REQUIRE(readFile(scenario.installDir / "bin/removed.txt") == kOldRemoved);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/already-missing.txt"));

    LAU_REQUIRE(readFile(scenario.forwardBackupDir / "bin/replaced.txt") == kOldReplaced);
    LAU_REQUIRE(readFile(scenario.forwardBackupDir / "bin/removed.txt") == kOldRemoved);
    LAU_REQUIRE(!std::filesystem::exists(scenario.forwardBackupDir / "bin/added.txt"));
    LAU_REQUIRE(readFile(scenario.rollbackUndoDir / "bin/replaced.txt") == kNewReplaced);
    LAU_REQUIRE(readFile(scenario.rollbackUndoDir / "bin/added.txt") == kNewAdded);
    LAU_REQUIRE(!std::filesystem::exists(scenario.rollbackUndoDir / "bin/removed.txt"));

    const auto rollbackTerminal = readTerminalTransaction(scenario.installDir);
    LAU_REQUIRE(rollbackTerminal.transactionId != scenario.forwardTerminal.transactionId);
    const auto rollbackSnapshot = autoupdater::ApplyPlan::parse(
        readFile(scenario.installDir / ".autoupdater" / "journal" / (rollbackTerminal.transactionId + ".plan.json")));
    LAU_REQUIRE(rollbackSnapshot);
    LAU_REQUIRE(rollbackSnapshot.value().intent == autoupdater::ApplyPlanIntent::Rollback);
    LAU_REQUIRE(rollbackSnapshot.value().rollbackOf.has_value());
    LAU_REQUIRE(rollbackSnapshot.value().rollbackOf->transactionId == scenario.forwardTerminal.transactionId);
    LAU_REQUIRE(rollbackSnapshot.value().rollbackOf->planDigest == scenario.forwardTerminal.planDigest);
    LAU_REQUIRE(rollbackSnapshot.value().backupDir == scenario.rollbackUndoDir);
    LAU_REQUIRE(rollbackSnapshot.value().operations.size() == 3);
    LAU_REQUIRE(rollbackSnapshot.value().operations[0].type == autoupdater::ApplyOperationType::Replace);
    LAU_REQUIRE(rollbackSnapshot.value().operations[0].target == "bin/removed.txt");
    LAU_REQUIRE(rollbackSnapshot.value().operations[0].precondition.has_value());
    LAU_REQUIRE(!rollbackSnapshot.value().operations[0].precondition->exists);
    LAU_REQUIRE(rollbackSnapshot.value().operations[1].type == autoupdater::ApplyOperationType::Remove);
    LAU_REQUIRE(rollbackSnapshot.value().operations[1].target == "bin/added.txt");
    LAU_REQUIRE(rollbackSnapshot.value().operations[1].precondition.has_value());
    LAU_REQUIRE(rollbackSnapshot.value().operations[1].precondition->exists);
    LAU_REQUIRE(rollbackSnapshot.value().operations[1].precondition->size == kNewAdded.size());
    LAU_REQUIRE(rollbackSnapshot.value().operations[2].type == autoupdater::ApplyOperationType::Replace);
    LAU_REQUIRE(rollbackSnapshot.value().operations[2].target == "bin/replaced.txt");
    LAU_REQUIRE(rollbackSnapshot.value().operations[2].precondition.has_value());
    LAU_REQUIRE(rollbackSnapshot.value().operations[2].precondition->exists);
    LAU_REQUIRE(rollbackSnapshot.value().operations[2].precondition->size == kNewReplaced.size());

#ifndef _WIN32
    constexpr auto kModeMask = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                               std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                               std::filesystem::perms::group_write | std::filesystem::perms::group_exec |
                               std::filesystem::perms::others_read | std::filesystem::perms::others_write |
                               std::filesystem::perms::others_exec;
    constexpr auto kReplacedMode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                   std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
    constexpr auto kRemovedMode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                  std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec;
    LAU_REQUIRE((std::filesystem::status(scenario.installDir / "bin/replaced.txt").permissions() & kModeMask) ==
                kReplacedMode);
    LAU_REQUIRE((std::filesystem::status(scenario.installDir / "bin/removed.txt").permissions() & kModeMask) ==
                kRemovedMode);
    LAU_REQUIRE(rollbackSnapshot.value().operations[0].permissions.has_value());
    LAU_REQUIRE(*rollbackSnapshot.value().operations[0].permissions == static_cast<std::uint32_t>(kRemovedMode));
    LAU_REQUIRE(rollbackSnapshot.value().operations[2].permissions.has_value());
    LAU_REQUIRE(*rollbackSnapshot.value().operations[2].permissions == static_cast<std::uint32_t>(kReplacedMode));
#endif

    const auto replayed = autoupdater::updater::executeApplyPlan(scenario.rollbackRequest);
    LAU_REQUIRE(replayed);
    LAU_REQUIRE(readTerminalTransaction(scenario.installDir).transactionId == rollbackTerminal.transactionId);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == kOldReplaced);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/added.txt"));
    LAU_REQUIRE(readFile(scenario.installDir / "bin/removed.txt") == kOldRemoved);

    std::filesystem::remove_all(root, ec);

    const auto legacyRoot = std::filesystem::temp_directory_path() / "libAutoUpdater-public-rollback-schema1-test";
    std::filesystem::remove_all(legacyRoot, ec);
    auto legacyScenario = completePublicRollbackSource(legacyRoot, true);
    const auto legacyRollback = autoupdater::updater::executeApplyPlan(legacyScenario.rollbackRequest);
    LAU_REQUIRE(legacyRollback);
    LAU_REQUIRE(readFile(legacyScenario.installDir / "bin/replaced.txt") == kOldReplaced);
    LAU_REQUIRE(!std::filesystem::exists(legacyScenario.installDir / "bin/added.txt"));
    LAU_REQUIRE(readFile(legacyScenario.installDir / "bin/removed.txt") == kOldRemoved);
    std::filesystem::remove_all(legacyRoot, ec);
}

void testApplyExecutorRecoversFailedPublicRollbackOfRollback() {
    constexpr std::string_view kNewReplaced = "new-replaced";
    constexpr std::string_view kNewAdded = "new-added";
    constexpr std::string_view kOldRemoved = "old-removed";

    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-public-rollback-recovery-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto scenario = completePublicRollbackSource(root);

    bool applyFailureInjected = false;
    bool compensatingRollbackFailureInjected = false;
    autoupdater::updater::ApplyExecutionHooks hooks;
    hooks.checkpoint = [&](std::string_view boundary, std::size_t operationIndex) {
        if (!applyFailureInjected && boundary == "remove.after" && operationIndex == 1) {
            applyFailureInjected = true;
            return autoupdater::updater::ApplyFaultAction::Fail;
        }
        if (applyFailureInjected && !compensatingRollbackFailureInjected && boundary == "rollback.replace.before" &&
            operationIndex == 1) {
            compensatingRollbackFailureInjected = true;
            return autoupdater::updater::ApplyFaultAction::Fail;
        }
        return autoupdater::updater::ApplyFaultAction::Continue;
    };

    auto failed = autoupdater::updater::executeApplyPlanWithDependencies(
        scenario.rollbackRequest, applyDependencies(scenario.hashProvider), hooks);
    LAU_REQUIRE(!failed);
    LAU_REQUIRE(applyFailureInjected);
    LAU_REQUIRE(compensatingRollbackFailureInjected);

    const auto journalDir = scenario.installDir / ".autoupdater" / "journal";
    const auto active = autoupdater::updater::parseActiveTransaction(readFile(journalDir / "active.json"));
    LAU_REQUIRE(active);
    LAU_REQUIRE(active.value().transactionId != scenario.forwardTerminal.transactionId);
    const auto failedSummaryPath = journalDir / (active.value().transactionId + ".json");
    const auto failedSummary = autoupdater::updater::parseApplyJournalSummary(readFile(failedSummaryPath));
    LAU_REQUIRE(failedSummary);
    LAU_REQUIRE(failedSummary.value().fileState == autoupdater::updater::JournalFileState::RecoveryFailed);
    LAU_REQUIRE(!failedSummary.value().applyError.empty());
    LAU_REQUIRE(!failedSummary.value().rollbackError.empty());
    const auto failedOperationPath =
        journalDir / (active.value().transactionId + ".ops") / "00000001.json";
    const auto failedOperation =
        autoupdater::updater::parseApplyJournalOperation(readFile(failedOperationPath));
    LAU_REQUIRE(failedOperation);
    LAU_REQUIRE(failedOperation.value().rollbackState == autoupdater::updater::JournalRollbackState::Failed);

    const auto activeSnapshot =
        autoupdater::ApplyPlan::parse(readFile(journalDir / (active.value().transactionId + ".plan.json")));
    LAU_REQUIRE(activeSnapshot);
    LAU_REQUIRE(activeSnapshot.value().intent == autoupdater::ApplyPlanIntent::Rollback);
    LAU_REQUIRE(activeSnapshot.value().rollbackOf.has_value());
    LAU_REQUIRE(activeSnapshot.value().rollbackOf->transactionId == scenario.forwardTerminal.transactionId);
    LAU_REQUIRE(activeSnapshot.value().backupDir == scenario.rollbackUndoDir);
    LAU_REQUIRE(readFile(scenario.rollbackUndoDir / "bin/replaced.txt") == kNewReplaced);
    LAU_REQUIRE(readFile(scenario.rollbackUndoDir / "bin/added.txt") == kNewAdded);

    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == kNewReplaced);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/added.txt"));
    LAU_REQUIRE(readFile(scenario.installDir / "bin/removed.txt") == kOldRemoved);

    auto legacySummaryJson = autoupdater::util::Json::parse(readFile(failedSummaryPath), {});
    LAU_REQUIRE(legacySummaryJson && legacySummaryJson.value().isObject());
    auto legacySummaryObject = legacySummaryJson.value().asObject();
    constexpr const char* kLegacySecret = "AU022_LEGACY_JOURNAL_SECRET";
    for (const auto* field : {"applyError", "rollbackError"}) {
        auto errorObject = legacySummaryObject.at(field).asObject();
        errorObject["message"] = autoupdater::util::Json(
            std::string("https://user:password@example.test/?token=") + kLegacySecret);
        legacySummaryObject[field] = autoupdater::util::Json(std::move(errorObject));
    }
    writeFile(failedSummaryPath, autoupdater::util::Json(std::move(legacySummaryObject)).stringify(2));

    auto legacyOperationJson = autoupdater::util::Json::parse(readFile(failedOperationPath), {});
    LAU_REQUIRE(legacyOperationJson && legacyOperationJson.value().isObject());
    auto legacyOperationObject = legacyOperationJson.value().asObject();
    auto legacyOperationError = legacyOperationObject.at("error").asObject();
    constexpr const char* kLegacyOperationSecret = "AU022_LEGACY_OPERATION_SECRET";
    legacyOperationError["message"] = autoupdater::util::Json(
        std::string("signature=") + kLegacyOperationSecret + "; -----BEGIN PRIVATE KEY-----");
    legacyOperationObject["error"] = autoupdater::util::Json(std::move(legacyOperationError));
    writeFile(failedOperationPath, autoupdater::util::Json(std::move(legacyOperationObject)).stringify(2));

    const auto recoveredInterruptedTransaction = autoupdater::updater::executeApplyPlan(scenario.forwardPlan);
    LAU_REQUIRE(!recoveredInterruptedTransaction);
    LAU_REQUIRE(recoveredInterruptedTransaction.error().phase == autoupdater::ErrorPhase::Recovery);
    LAU_REQUIRE(!std::filesystem::exists(journalDir / "active.json"));
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == kNewReplaced);
    LAU_REQUIRE(readFile(scenario.installDir / "bin/added.txt") == kNewAdded);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/removed.txt"));
    LAU_REQUIRE(readFile(failedSummaryPath).find(kLegacySecret) == std::string::npos);
    LAU_REQUIRE(readFile(failedOperationPath).find(kLegacyOperationSecret) == std::string::npos);

    const auto recovered = autoupdater::updater::executeApplyPlan(scenario.rollbackRequest);
    LAU_REQUIRE(recovered);
    LAU_REQUIRE(!std::filesystem::exists(journalDir / "active.json"));
    LAU_REQUIRE(readFile(scenario.installDir / "bin/replaced.txt") == "old-replaced");
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/added.txt"));
    LAU_REQUIRE(readFile(scenario.installDir / "bin/removed.txt") == kOldRemoved);
    LAU_REQUIRE(!std::filesystem::exists(scenario.installDir / "bin/already-missing.txt"));

    const auto recoveredSummary =
        autoupdater::updater::parseApplyJournalSummary(readFile(journalDir / (active.value().transactionId + ".json")));
    LAU_REQUIRE(recoveredSummary);
    LAU_REQUIRE(recoveredSummary.value().fileState == autoupdater::updater::JournalFileState::RolledBack);
    LAU_REQUIRE(recoveredSummary.value().rollbackError.empty());
    const auto terminalAfterRecovery = readTerminalTransaction(scenario.installDir);
    LAU_REQUIRE(terminalAfterRecovery.transactionId != scenario.forwardTerminal.transactionId);
    const auto terminalPlan =
        autoupdater::ApplyPlan::parse(readFile(journalDir / (terminalAfterRecovery.transactionId + ".plan.json")));
    LAU_REQUIRE(terminalPlan);
    LAU_REQUIRE(terminalPlan.value().intent == autoupdater::ApplyPlanIntent::Rollback);
    LAU_REQUIRE(terminalPlan.value().rollbackOf.has_value());
    LAU_REQUIRE(terminalPlan.value().rollbackOf->transactionId == scenario.rollbackRequest.rollbackOf->transactionId);
    LAU_REQUIRE(terminalPlan.value().rollbackOf->planDigest == scenario.rollbackRequest.rollbackOf->planDigest);

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorReportsRestartFailurePhase() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-restart-diagnostic-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const auto installDir = root / "install";
    const auto stagingDir = root / "staging";
    const auto backupDir = root / "backup";
    writeFile(installDir / "bin/app.txt", "old");
    writeFile(stagingDir / "bin/app.txt", "new");

    auto hashProvider = autoupdater::createDefaultHashProvider();
    const auto newHash = hashProvider->sha256Bytes("new");
    LAU_REQUIRE(newHash);

    autoupdater::ApplyPlan plan;
    plan.schemaVersion = 2;
    plan.appId = "com.example.restart-diagnostic";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "2.0.0";
    plan.releaseId = "release-2";
    plan.manifestSha256 = std::string(64, 'd');
    plan.installDir = installDir;
    plan.stagingDir = stagingDir;
    plan.backupDir = backupDir;
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "bin/app.txt", "bin/app.txt", newHash.value(), 3});
    plan.restartCommand = {"restart-placeholder", "--restored"};

    auto processLauncher = std::make_shared<FailingProcessLauncher>();
    autoupdater::updater::ApplyExecutorDependencies dependencies;
    dependencies.fileSystem = autoupdater::createDefaultFileSystem();
    dependencies.hashProvider = hashProvider;
    dependencies.processLauncher = processLauncher;
    const auto result =
        autoupdater::updater::executeApplyPlanWithDependencies(plan, std::move(dependencies));

    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
    LAU_REQUIRE(result.error().phase == autoupdater::ErrorPhase::Restart);
    LAU_REQUIRE(processLauncher->launchCalls() == 1);
    LAU_REQUIRE(readFile(installDir / "bin/app.txt") == "new");

    const auto terminal = readTerminalTransaction(installDir);
    const auto summary = autoupdater::updater::parseApplyJournalSummary(
        readFile(installDir / ".autoupdater" / "journal" / (terminal.transactionId + ".json")));
    LAU_REQUIRE(summary);
    LAU_REQUIRE(summary.value().fileState == autoupdater::updater::JournalFileState::Complete);
    LAU_REQUIRE(summary.value().restartState == autoupdater::updater::JournalRestartState::Failed);
    LAU_REQUIRE(summary.value().restartError.message == "phase=Restart code=ApplyLaunchFailed");
    LAU_REQUIRE(summary.value().restartError.message.find("AU022_JOURNAL_SECRET") == std::string::npos);

    std::filesystem::remove_all(root, error);
}

void testApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-apply-target-conflicts-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const std::vector<std::vector<autoupdater::ApplyOperation>> conflictingOperations = {
        {
            {autoupdater::ApplyOperationType::Replace, "objects/first.bin", "bin/app.exe", std::string(64, 'a'), 4},
            {autoupdater::ApplyOperationType::Replace, "objects/second.bin", "bin/app.exe", std::string(64, 'a'), 4},
        },
        {
            {autoupdater::ApplyOperationType::Remove, "", "Bin/App.exe", "", 0},
            {autoupdater::ApplyOperationType::Remove, "", "bin/app.EXE", "", 0},
        },
        {
            {autoupdater::ApplyOperationType::Remove, "", "obsolete.dll", "", 0},
            {autoupdater::ApplyOperationType::Remove, "", "obsolete.dll", "", 0},
        },
        {
            {autoupdater::ApplyOperationType::Replace, "objects/app.bin", "bin/app.exe", std::string(64, 'a'), 4},
            {autoupdater::ApplyOperationType::Remove, "", "bin/app.exe", "", 0},
        },
        {
            {autoupdater::ApplyOperationType::Remove, "", "bin", "", 0},
            {autoupdater::ApplyOperationType::Remove, "", "bin/app.exe", "", 0},
        },
        {
            {autoupdater::ApplyOperationType::Remove, "", "bin/app.exe", "", 0},
            {autoupdater::ApplyOperationType::Remove, "", "bin", "", 0},
        },
        {
            {autoupdater::ApplyOperationType::Remove, "", ".autoupdater/journal/active.json", "", 0},
        },
    };

    for (std::size_t index = 0; index < conflictingOperations.size(); ++index) {
        autoupdater::ApplyPlan plan;
        plan.installDir = root / ("install-" + std::to_string(index));
        plan.stagingDir = root / ("staging-" + std::to_string(index));
        plan.backupDir = root / ("backup-" + std::to_string(index));
        plan.operations = conflictingOperations[index];

        const auto result = autoupdater::updater::executeApplyPlan(plan);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(!std::filesystem::exists(plan.installDir));
        LAU_REQUIRE(!std::filesystem::exists(plan.installDir / ".autoupdater" / "journal"));
    }

    LAU_REQUIRE(!std::filesystem::exists(root));
}

void testApplyExecutorValidatesProcessWaitInputs() {
    const auto noWait = autoupdater::updater::waitForProcessExit(0, std::chrono::seconds(0));
    LAU_REQUIRE(noWait);

    const auto negative = autoupdater::updater::waitForProcessExit(0, std::chrono::seconds(-1));
    LAU_REQUIRE(!negative);
    LAU_REQUIRE(negative.error().code == autoupdater::ErrorCode::ApplyFailed);

    const auto overlong = autoupdater::updater::waitForProcessExit(0, autoupdater::detail::kMaximumProcessWaitTimeout +
                                                                          std::chrono::seconds(1));
    LAU_REQUIRE(!overlong);
    LAU_REQUIRE(overlong.error().code == autoupdater::ErrorCode::ApplyFailed);

    const auto invalidPid = autoupdater::updater::waitForProcessExit(
        autoupdater::detail::maximumPlatformProcessId() + 1, std::chrono::seconds(0));
    LAU_REQUIRE(!invalidPid);
    LAU_REQUIRE(invalidPid.error().code == autoupdater::ErrorCode::ApplyFailed);
}

void testApplyExecutorReconcilesPublishedCommitAcknowledgements() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-apply-publish-ack-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    auto hashProvider = autoupdater::createDefaultHashProvider();
    const std::string oldContents = "old-contents";
    const std::string newContents = "new-contents";
    auto newHash = hashProvider->sha256Bytes(newContents);
    LAU_REQUIRE(newHash);

    const std::vector<std::string> faultTargets = {
        "bin/app.txt",
        ".autoupdater/journal/active.json",
    };
    for (std::size_t index = 0; index < faultTargets.size(); ++index) {
        const auto caseRoot = root / std::to_string(index);
        const auto installDir = caseRoot / "install";
        const auto stagingDir = caseRoot / "staging";
        const auto backupDir = caseRoot / "backup";
        writeFile(installDir / "bin/app.txt", oldContents);
        writeFile(stagingDir / "bin/app.txt", newContents);

        autoupdater::ApplyPlan plan;
        plan.schemaVersion = 2;
        plan.appId = "com.example.publish-ack";
        plan.fromVersion = "1.0.0";
        plan.toVersion = "2.0.0";
        plan.releaseId = "release-2";
        plan.manifestSha256 = std::string(64, 'd');
        plan.installDir = installDir;
        plan.stagingDir = stagingDir;
        plan.backupDir = backupDir;
        plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/app.txt", "bin/app.txt",
                                   newHash.value(), newContents.size()});

        auto fault = std::make_shared<autoupdater::test::CommitAcknowledgementFault>();
        fault->target = faultTargets[index];
        fault->reportUnknownPublication = true;
        autoupdater::updater::ApplyExecutorDependencies dependencies;
        dependencies.fileSystem =
            std::make_shared<autoupdater::test::CommitFaultFileSystem>(autoupdater::createDefaultFileSystem(), fault);
        dependencies.hashProvider = hashProvider;
        dependencies.processLauncher = autoupdater::createDefaultProcessLauncher();
        auto applied = autoupdater::updater::executeApplyPlanWithDependencies(plan, std::move(dependencies));
        LAU_REQUIRE(applied);
        LAU_REQUIRE(fault->consumed.load(std::memory_order_acquire));
        LAU_REQUIRE(readFile(installDir / "bin/app.txt") == newContents);
    }
    std::filesystem::remove_all(root, error);
}
