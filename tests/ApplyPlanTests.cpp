#include "TestCommon.h"

#include "libAutoUpdater/ApplyPlan.h"

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
