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

    const auto json = plan.toJson();
    if (json.size() > config.resources.maxApplyPlanBytes) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ResourceLimitExceeded, "Generated apply plan exceeds its byte limit"});
    }
    // Keep the producer and the external updater on the same resource and
    // schema contract. In particular, planning can merge independently
    // bounded manifest arrays into one operations array.
    auto validatedPlan = ApplyPlan::parse(json, config.resources);
    if (!validatedPlan) {
        return Result<WrittenApplyPlan>::fail(validatedPlan.error());
    }

    const auto planPath = config.tempDir / "apply-plan.json";
    auto root = fileSystem.openRoot(config.tempDir, RootAccess::ReadWrite, true);
    if (!root) {
        return Result<WrittenApplyPlan>::fail(root.error());
    }
    auto existing = root.value()->openRegularFile("apply-plan.json", RootedFileOpenMode::ReadOnly);
    if (!existing) {
        return Result<WrittenApplyPlan>::fail(existing.error());
    }
    RootedEntryExpectation expectation = RootedEntryExpectation::missing();
    if (existing.value().exists()) {
        auto metadata = existing.value().file->metadata();
        if (!metadata) {
            return Result<WrittenApplyPlan>::fail(metadata.error());
        }
        expectation = RootedEntryExpectation::matching(metadata.value());
    }
    existing.value().file.reset();
    auto temporary = root.value()->createAtomicReplacement("apply-plan.json");
    if (!temporary) {
        return Result<WrittenApplyPlan>::fail(temporary.error());
    }
    auto write = temporary.value()->file().write(json.data(), json.size());
    if (!write) {
        return Result<WrittenApplyPlan>::fail(write.error());
    }
    auto committed = temporary.value()->commit(expectation);
    if (!committed) {
        return Result<WrittenApplyPlan>::fail(committed.error());
    }

    WrittenApplyPlan written;
    written.plan = std::move(plan);
    written.path = planPath;
    return Result<WrittenApplyPlan>::ok(std::move(written));
}

} // namespace autoupdater
