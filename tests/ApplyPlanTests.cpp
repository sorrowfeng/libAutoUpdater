#include "TestCommon.h"

#include "libAutoUpdater/ApplyPlan.h"
#include "util/PathUtil.h"

#include <filesystem>
#include <utility>
#include <vector>

namespace {

const std::string kSha256(64, 'a');
const std::string kManifestSha256(64, 'b');

autoupdater::ApplyPlan applyPlanWithOperations(std::vector<autoupdater::ApplyOperation> operations) {
    autoupdater::ApplyPlan plan;
    plan.installDir = "install";
    plan.stagingDir = "staging";
    plan.backupDir = "backup";
    plan.operations = std::move(operations);
    return plan;
}

void requireApplyPlanTargetConflict(std::vector<autoupdater::ApplyOperation> operations) {
    const auto parsed = autoupdater::ApplyPlan::parse(applyPlanWithOperations(std::move(operations)).toJson());
    LAU_REQUIRE(!parsed);
    LAU_REQUIRE(parsed.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

} // namespace

void testApplyPlanRoundTrip() {
    autoupdater::ApplyPlan plan;
    plan.appId = "com.example.app";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "1.1.0";
    plan.releaseId = "1.1.0+1";
    plan.manifestSha256 = kManifestSha256;
    plan.installDir = "install";
    plan.stagingDir = "install/.autoupdater/staging/1.1.0";
    plan.backupDir = "install/.autoupdater/backup/1.0.0-to-1.1.0";
    plan.restartCommand = {"install/app"};
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/app", "bin/app", kSha256, 4});
    plan.operations.push_back({autoupdater::ApplyOperationType::Remove, "", "old.dll", "", 0});

    auto parsed = autoupdater::ApplyPlan::parse(plan.toJson());
    LAU_REQUIRE(parsed);
    LAU_REQUIRE(parsed.value().operations.size() == 2);
    LAU_REQUIRE(parsed.value().operations[0].type == autoupdater::ApplyOperationType::Replace);
    LAU_REQUIRE(parsed.value().operations[1].type == autoupdater::ApplyOperationType::Remove);
}

void testApplyPlanRoundTripPreservesUnicodePaths() {
    const auto root = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"libAutoUpdater-中文路径");

    autoupdater::ApplyPlan plan;
    plan.appId = "com.example.app";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "2.0.0";
    plan.releaseId = "unicode-paths";
    plan.manifestSha256 = kManifestSha256;
    plan.installDir = root / std::filesystem::u8path(u8"安装目录");
    plan.stagingDir = root / std::filesystem::u8path(u8"暂存目录");
    plan.backupDir = root / std::filesystem::u8path(u8"备份目录");
    plan.restartCommand = {autoupdater::util::pathToUtf8(plan.installDir / std::filesystem::u8path(u8"应用.exe"))};
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, u8"资源/应用.txt", u8"资源/应用.txt", kSha256, 4});

    const auto json = plan.toJson();
    LAU_REQUIRE(json.find(u8"安装目录") != std::string::npos);
    LAU_REQUIRE(json.find(u8"资源/应用.txt") != std::string::npos);

    auto parsed = autoupdater::ApplyPlan::parse(json);
    LAU_REQUIRE(parsed);
    LAU_REQUIRE(autoupdater::util::pathToUtf8(parsed.value().installDir) ==
                autoupdater::util::pathToUtf8(plan.installDir));
    LAU_REQUIRE(autoupdater::util::pathToUtf8(parsed.value().stagingDir) ==
                autoupdater::util::pathToUtf8(plan.stagingDir));
    LAU_REQUIRE(autoupdater::util::pathToUtf8(parsed.value().backupDir) ==
                autoupdater::util::pathToUtf8(plan.backupDir));
    LAU_REQUIRE(parsed.value().operations.size() == 1);
    LAU_REQUIRE(parsed.value().operations[0].target == u8"资源/应用.txt");
}

void testApplyPlanRollbackContract() {
    const std::string transactionId(64, '1');
    const std::string planDigest(64, 'a');

    autoupdater::ApplyPlan rollback;
    rollback.intent = autoupdater::ApplyPlanIntent::Rollback;
    rollback.rollbackOf = autoupdater::ApplyTransactionReference{transactionId, planDigest};
    rollback.installDir = "install";
    rollback.stagingDir = "install/.autoupdater/backup/forward";
    rollback.backupDir = "install/.autoupdater/backup/rollback/transaction";
    rollback.operations.emplace_back(autoupdater::ApplyOperationType::Replace, "bin/app", "bin/app",
                                     std::string(64, 'b'), 4, 0751U);
    rollback.operations[0].precondition = autoupdater::ApplyOperationPrecondition{true, 5, std::string(64, 'c'), 0755U};

    auto parsed = autoupdater::ApplyPlan::parse(rollback.toJson());
    LAU_REQUIRE(parsed);
    LAU_REQUIRE(parsed.value().schemaVersion == 2);
    LAU_REQUIRE(parsed.value().intent == autoupdater::ApplyPlanIntent::Rollback);
    LAU_REQUIRE(parsed.value().rollbackOf.has_value());
    LAU_REQUIRE(parsed.value().rollbackOf->transactionId == transactionId);
    LAU_REQUIRE(parsed.value().rollbackOf->planDigest == planDigest);
    LAU_REQUIRE(parsed.value().operations[0].permissions.has_value());
    LAU_REQUIRE(*parsed.value().operations[0].permissions == 0751U);
    LAU_REQUIRE(parsed.value().operations[0].precondition.has_value());
    LAU_REQUIRE(parsed.value().operations[0].precondition->exists);
    LAU_REQUIRE(parsed.value().operations[0].precondition->size == 5);
    LAU_REQUIRE(parsed.value().operations[0].precondition->sha256 == std::string(64, 'c'));
    LAU_REQUIRE(parsed.value().operations[0].precondition->permissions == 0755U);

    auto missingReference = rollback;
    missingReference.rollbackOf.reset();
    LAU_REQUIRE(!autoupdater::ApplyPlan::parse(missingReference.toJson()));

    auto installWithReference = rollback;
    installWithReference.intent = autoupdater::ApplyPlanIntent::Install;
    LAU_REQUIRE(!autoupdater::ApplyPlan::parse(installWithReference.toJson()));

    auto reservedTarget = rollback;
    reservedTarget.operations[0].target = "AUTOUP~1/staging/rollback-plan.json";
    LAU_REQUIRE(!autoupdater::ApplyPlan::parse(reservedTarget.toJson()));

    const auto legacy = autoupdater::ApplyPlan::parse(R"json({
      "schemaVersion": 1,
      "installDir": "install",
      "stagingDir": "staging",
      "backupDir": "backup",
      "operations": []
    })json");
    LAU_REQUIRE(legacy);
    LAU_REQUIRE(legacy.value().intent == autoupdater::ApplyPlanIntent::Install);
    LAU_REQUIRE(!legacy.value().rollbackOf.has_value());
}

void testApplyPlanRejectsConflictingManagedTargets() {
    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Replace, "objects/first.bin", "bin/app.exe", kSha256, 4},
        {autoupdater::ApplyOperationType::Replace, "objects/second.bin", "bin/app.exe", kSha256, 4},
    });

    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Remove, "", "Bin/App.exe", "", 0},
        {autoupdater::ApplyOperationType::Remove, "", "bin/app.EXE", "", 0},
    });

    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Remove, "", "obsolete.dll", "", 0},
        {autoupdater::ApplyOperationType::Remove, "", "obsolete.dll", "", 0},
    });

    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Replace, "objects/app.bin", "bin/app.exe", kSha256, 4},
        {autoupdater::ApplyOperationType::Remove, "", "bin/app.exe", "", 0},
    });

    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Remove, "", "bin", "", 0},
        {autoupdater::ApplyOperationType::Remove, "", "bin/app.exe", "", 0},
    });
    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Remove, "", "bin/app.exe", "", 0},
        {autoupdater::ApplyOperationType::Remove, "", "bin", "", 0},
    });

    requireApplyPlanTargetConflict({
        {autoupdater::ApplyOperationType::Remove, "", ".AUToupdater/journal/active.json", "", 0},
    });
}

void testApplyPlanAllowsSharedSourceForDistinctManagedTargets() {
    const auto parsed = autoupdater::ApplyPlan::parse(
        applyPlanWithOperations(
            {
                {autoupdater::ApplyOperationType::Replace, "objects/shared.bin", "bin/first.exe", kSha256, 9},
                {autoupdater::ApplyOperationType::Replace, "objects/shared.bin", "bin/second.exe", kSha256, 9},
                {autoupdater::ApplyOperationType::Replace, "objects/shared.bin", "a", kSha256, 9},
                {autoupdater::ApplyOperationType::Replace, "objects/shared.bin", "ab", kSha256, 9},
                {autoupdater::ApplyOperationType::Replace, "objects/shared.bin", "shared/first", kSha256, 9},
                {autoupdater::ApplyOperationType::Replace, "objects/shared.bin", "shared/second", kSha256, 9},
            })
            .toJson());

    LAU_REQUIRE(parsed);
    LAU_REQUIRE(parsed.value().schemaVersion == 2);
    LAU_REQUIRE(parsed.value().operations.size() == 6);
    LAU_REQUIRE(parsed.value().operations[0].source == parsed.value().operations[1].source);
    LAU_REQUIRE(parsed.value().operations[0].target != parsed.value().operations[1].target);
}
