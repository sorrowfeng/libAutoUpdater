#include "ApplyPlanWriter.h"

#include "util/PathUtil.h"

#include <sstream>

namespace autoupdater {

namespace {

std::string safeVersionForPath(const Version& version) {
    auto text = version.toString();
    for (auto& ch : text) {
        if (ch == '+' || ch == '/' || ch == '\\' || ch == ':') {
            ch = '_';
        }
    }
    return text;
}

} // namespace

Result<WrittenApplyPlan> writeApplyPlan(const Config& config, const ManifestEnvelope& envelope,
                                        const UpdateDecision& decision, IFileSystem& fileSystem) {
    const auto stateRoot = util::defaultStagingRoot(config.installDir);
    const auto backupDir =
        stateRoot / "backup" /
        (safeVersionForPath(config.currentVersion) + "-to-" + safeVersionForPath(envelope.manifest.version));

    ApplyPlan plan;
    plan.schemaVersion = 1;
    plan.appId = config.appId.empty() ? envelope.manifest.appId : config.appId;
    plan.fromVersion = config.currentVersion.toString();
    plan.toVersion = envelope.manifest.version.toString();
    plan.releaseId = envelope.manifest.releaseId;
    plan.manifestSha256 = envelope.sha256;
    plan.installDir = config.installDir;
    plan.stagingDir = config.tempDir;
    plan.backupDir = backupDir;
    plan.restartCommand = config.restartCommand;
    plan.operations = decision.operations;

    const auto planPath = config.tempDir / "apply-plan.json";
    auto write = fileSystem.writeText(planPath, plan.toJson());
    if (!write) {
        return Result<WrittenApplyPlan>::fail(write.error());
    }

    WrittenApplyPlan written;
    written.plan = std::move(plan);
    written.path = planPath;
    return Result<WrittenApplyPlan>::ok(std::move(written));
}

} // namespace autoupdater
