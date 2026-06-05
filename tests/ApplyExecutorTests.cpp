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
