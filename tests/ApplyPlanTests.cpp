#include "TestCommon.h"

#include "libAutoUpdater/ApplyPlan.h"
#include "util/PathUtil.h"

#include <filesystem>

void testApplyPlanRoundTrip() {
    autoupdater::ApplyPlan plan;
    plan.appId = "com.example.app";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "1.1.0";
    plan.releaseId = "1.1.0+1";
    plan.manifestSha256 = "abc";
    plan.installDir = "install";
    plan.stagingDir = "install/.autoupdater/staging/1.1.0";
    plan.backupDir = "install/.autoupdater/backup/1.0.0-to-1.1.0";
    plan.restartCommand = {"install/app"};
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/app", "bin/app", "hash", 4});
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
    plan.manifestSha256 = "abc";
    plan.installDir = root / std::filesystem::u8path(u8"安装目录");
    plan.stagingDir = root / std::filesystem::u8path(u8"暂存目录");
    plan.backupDir = root / std::filesystem::u8path(u8"备份目录");
    plan.restartCommand = {autoupdater::util::pathToUtf8(plan.installDir / std::filesystem::u8path(u8"应用.exe"))};
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, u8"资源/应用.txt", u8"资源/应用.txt", "hash", 4});

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
