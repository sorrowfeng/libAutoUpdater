#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"

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
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/b.txt", "bin/b.txt", "not-the-real-hash", 5});

    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(readFile(install / "bin/a.txt") == "old-a");
    LAU_REQUIRE(readFile(install / "bin/b.txt") == "old-b");

    std::filesystem::remove_all(root, ec);
}

