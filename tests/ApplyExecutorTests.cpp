#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "util/PathUtil.h"

#include <filesystem>
#include <fstream>

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

void testApplyExecutorReclaimsStaleLock() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-stale-lock-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / "install";
    // A lock left behind by a crashed updater, owned by a dead process.
    const auto lockDir = install / ".autoupdater" / "update.lock";
    std::filesystem::create_directories(lockDir, ec);
    writeFile(lockDir / "owner.txt", "99999999\n");

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = root / "staging";
    plan.backupDir = root / "backup";
    plan.releaseId = "stale-lock-test";

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE_MSG(result, result ? "" : result.error().message);

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorRejectsDuplicateTargets() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-dup-target-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / "install";
    const auto staging = root / "staging";
    const auto backup = root / "backup";

    writeFile(install / "bin/a.txt", "old");
    writeFile(staging / "bin/a.txt", "new");
    auto hash = autoupdater::createDefaultHashProvider();
    const auto hashNew = hash->sha256Bytes("new").value();

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "dup-target-test";
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/a.txt", "bin/a.txt", hashNew, 3});
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/a.txt", "bin/a.txt", hashNew, 3});

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyFailed);
    LAU_REQUIRE(readFile(install / "bin/a.txt") == "old");

    std::filesystem::remove_all(root, ec);
}

void testApplyExecutorRecoversInterruptedTransaction() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-recovery-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto install = root / "install";
    const auto backup = install / ".autoupdater" / "backup" / "1.0.0-to-1.1.0";

    // State left behind by an updater that crashed mid-transaction: the
    // target was already replaced and the journal still says "applying".
    writeFile(install / "bin/a.txt", "broken-new");
    writeFile(backup / "bin/a.txt", "good-old");
    writeFile(install / ".autoupdater" / "journal" / "crashed.json",
              std::string("{\"state\": \"applying\", \"toVersion\": \"1.1.0\", \"backupDir\": \"") +
                  autoupdater::util::pathToUtf8(backup) +
                  "\", \"applied\": [{\"target\": \"bin/a.txt\", \"backupExists\": true}]}");

    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = install / ".autoupdater" / "staging" / "1.2.0";
    plan.backupDir = install / ".autoupdater" / "backup" / "1.1.0-to-1.2.0";
    plan.releaseId = "recovery-test";

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(result);
    // The interrupted transaction was rolled back before the new one ran.
    LAU_REQUIRE(readFile(install / "bin/a.txt") == "good-old");

    std::filesystem::remove_all(root, ec);
}
