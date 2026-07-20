#include "ApplyPlanWriter.h"

#include "ApplyTransactionReceipt.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
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

Result<std::filesystem::path> resolvedPath(const std::filesystem::path& path, const char* description) {
    std::error_code error;
    auto resolved = std::filesystem::absolute(path, error);
    if (!error) {
        resolved = std::filesystem::weakly_canonical(resolved, error);
    }
    if (error) {
        return Result<std::filesystem::path>::fail(
            {ErrorCode::InvalidConfig, std::string("Failed to resolve ") + description});
    }
    return Result<std::filesystem::path>::ok(std::move(resolved));
}

bool isSameOrDescendant(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    if (relative == ".") {
        return true;
    }
    const auto component = relative.begin();
    return component != relative.end() && *component != "..";
}

Result<void> validateRollbackRequestDirectory(const Config& config, const PendingUpdate& pending,
                                              const std::string& transactionId) {
    try {
        auto installDir = resolvedPath(config.installDir, "the install directory for rollback");
        if (!installDir) {
            return Result<void>::fail(installDir.error());
        }
        auto requestDir = resolvedPath(config.tempDir, "the rollback request directory");
        if (!requestDir) {
            return Result<void>::fail(requestDir.error());
        }
        const auto privateStaging = (installDir.value() / ".autoupdater" / "staging").lexically_normal();
        auto sourceBackup = resolvedPath(pending.backupDir, "the forward rollback backup directory");
        if (!sourceBackup) {
            return Result<void>::fail(sourceBackup.error());
        }
        auto undoBackup = resolvedPath(util::defaultStagingRoot(config.installDir) / "backup" / "rollback" /
                                           util::pathFromUtf8(transactionId),
                                       "the rollback undo directory");
        if (!undoBackup) {
            return Result<void>::fail(undoBackup.error());
        }

        if (isSameOrDescendant(requestDir.value(), installDir.value()) &&
            !isSameOrDescendant(requestDir.value(), privateStaging)) {
            return Result<void>::fail(
                {ErrorCode::InvalidConfig,
                 "Rollback requests inside the install directory must use private updater staging"});
        }
        if (isSameOrDescendant(requestDir.value(), sourceBackup.value()) ||
            isSameOrDescendant(requestDir.value(), undoBackup.value())) {
            return Result<void>::fail(
                {ErrorCode::InvalidConfig, "Rollback request directory cannot overlap rollback backup evidence"});
        }
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail({ErrorCode::InvalidConfig, "Failed to validate the rollback request directory"});
    }
}

Result<void> verifyPublishedPlan(IRootedDirectory& root, const std::string& fileName,
                                 const RootedFileMetadata& preparedMetadata, const std::string& contents) {
    auto opened = root.openRegularFile(fileName, RootedFileOpenMode::ReadOnly);
    if (!opened) {
        return Result<void>::fail(opened.error());
    }
    if (!opened.value().exists()) {
        return Result<void>::fail({ErrorCode::FileSystemError, "Published apply plan is missing"});
    }
    const auto failOpened = [&](Error error) {
        auto closed = opened.value().file->close();
        if (!closed) {
            error.message += "; failed to close published apply plan: " + closed.error().message;
        }
        return Result<void>::fail(std::move(error));
    };
    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        return failOpened(metadata.error());
    }
    if (metadata.value().identity != preparedMetadata.identity || metadata.value().size != contents.size()) {
        return failOpened({ErrorCode::SecurityPolicyViolation, "Published apply plan identity or size changed"});
    }
    auto rewound = opened.value().file->seek(0);
    if (!rewound) {
        return failOpened(rewound.error());
    }
    std::array<char, 64 * 1024> buffer{};
    std::size_t offset = 0;
    while (offset < contents.size()) {
        auto read = opened.value().file->read(buffer.data(), std::min(buffer.size(), contents.size() - offset));
        if (!read) {
            return failOpened(read.error());
        }
        if (read.value() == 0 || std::memcmp(buffer.data(), contents.data() + offset, read.value()) != 0) {
            return failOpened({ErrorCode::SecurityPolicyViolation, "Published apply plan contents changed"});
        }
        offset += read.value();
    }
    auto finalMetadata = opened.value().file->metadata();
    if (!finalMetadata) {
        return failOpened(finalMetadata.error());
    }
    if (finalMetadata.value().identity != metadata.value().identity ||
        finalMetadata.value().size != metadata.value().size) {
        return failOpened({ErrorCode::SecurityPolicyViolation, "Published apply plan changed while being read"});
    }
    return opened.value().file->close();
}

Result<WrittenApplyPlan> writePlanFile(ApplyPlan plan, const std::filesystem::path& directory,
                                       const std::string& fileName, const ResourceLimits& limits,
                                       IFileSystem& fileSystem) {
    const auto json = plan.toJson();
    if (json.size() > limits.maxApplyPlanBytes) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ResourceLimitExceeded, "Generated apply plan exceeds its byte limit"});
    }
    // Keep the producer and the external updater on the same resource and
    // schema contract. In particular, planning can merge independently
    // bounded manifest arrays into one operations array.
    auto validatedPlan = ApplyPlan::parse(json, limits);
    if (!validatedPlan) {
        return Result<WrittenApplyPlan>::fail(validatedPlan.error());
    }

    auto root = fileSystem.openRoot(directory, RootAccess::ReadWrite, true);
    if (!root) {
        return Result<WrittenApplyPlan>::fail(root.error());
    }
    auto existing = root.value()->openRegularFile(fileName, RootedFileOpenMode::ReadOnly);
    if (!existing) {
        return Result<WrittenApplyPlan>::fail(existing.error());
    }
    RootedEntryExpectation expectation = RootedEntryExpectation::missing();
    if (existing.value().exists()) {
        auto metadata = existing.value().file->metadata();
        if (!metadata) {
            auto error = metadata.error();
            auto closed = existing.value().file->close();
            if (!closed) {
                error.message += "; failed to close apply-plan target: " + closed.error().message;
            }
            return Result<WrittenApplyPlan>::fail(std::move(error));
        }
        expectation = RootedEntryExpectation::matching(metadata.value());
        auto closed = existing.value().file->close();
        if (!closed) {
            return Result<WrittenApplyPlan>::fail(closed.error());
        }
    }
    auto temporary = root.value()->createAtomicReplacement(fileName);
    if (!temporary) {
        return Result<WrittenApplyPlan>::fail(temporary.error());
    }
    auto write = temporary.value()->file().write(json.data(), json.size());
    if (!write) {
        auto error = write.error();
        auto discarded = temporary.value()->discard();
        if (!discarded) {
            error.message += "; failed to discard incomplete apply plan: " + discarded.error().message;
        }
        return Result<WrittenApplyPlan>::fail(std::move(error));
    }
    auto preparedMetadata = temporary.value()->file().metadata();
    if (!preparedMetadata) {
        auto error = preparedMetadata.error();
        auto discarded = temporary.value()->discard();
        if (!discarded) {
            error.message += "; failed to discard invalid apply plan: " + discarded.error().message;
        }
        return Result<WrittenApplyPlan>::fail(std::move(error));
    }
    auto committed = temporary.value()->commit(expectation);
    auto discarded = temporary.value()->discard();
    const auto publication = temporary.value()->publishStatus();
    if (!committed) {
        auto error = committed.error();
        if (!discarded) {
            error.message += "; failed to finish apply-plan cleanup: " + discarded.error().message;
            return Result<WrittenApplyPlan>::fail(std::move(error));
        }
        if (publication.publication == RootedPublication::NotPublished || !publication.namespaceDurable ||
            !publication.failureCanBeReconciled) {
            return Result<WrittenApplyPlan>::fail(std::move(error));
        }
        temporary.value().reset();
        auto reconciled = verifyPublishedPlan(*root.value(), fileName, preparedMetadata.value(), json);
        if (!reconciled) {
            error.message += "; failed to reconcile apply-plan publication: " + reconciled.error().message;
            return Result<WrittenApplyPlan>::fail(std::move(error));
        }
    } else {
        if (!discarded) {
            return Result<WrittenApplyPlan>::fail(discarded.error());
        }
        temporary.value().reset();
        auto verified = verifyPublishedPlan(*root.value(), fileName, preparedMetadata.value(), json);
        if (!verified) {
            return Result<WrittenApplyPlan>::fail(verified.error());
        }
    }

    WrittenApplyPlan written;
    written.plan = std::move(plan);
    written.path = directory / util::pathFromUtf8(fileName);
    written.digest = util::sha256Bytes(json);
    return Result<WrittenApplyPlan>::ok(std::move(written));
}

} // namespace

Result<WrittenApplyPlan> writeApplyPlan(const Config& config, const ManifestEnvelope& envelope,
                                        const UpdateDecision& decision, IFileSystem& fileSystem) {
    const auto stateRoot = util::defaultStagingRoot(config.installDir);
    const auto backupDir =
        stateRoot / "backup" /
        (safeVersionForPath(config.currentVersion) + "-to-" + safeVersionForPath(envelope.manifest.version)) /
        util::pathFromUtf8(envelope.sha256);

    ApplyPlan plan;
    plan.schemaVersion = 2;
    plan.intent = ApplyPlanIntent::Install;
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

    return writePlanFile(std::move(plan), config.tempDir, "apply-plan.json", config.resources, fileSystem);
}

Result<WrittenApplyPlan> writeRollbackRequestPlan(const Config& config, const PendingUpdate& pending,
                                                  IFileSystem& fileSystem) {
    if (!pending.applyPlanDigest.empty() && !util::isLowerHexSha256(pending.applyPlanDigest)) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ApplyFailed, "Pending update has an invalid forward apply-plan digest"});
    }
    if (pending.backupDir.empty()) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ApplyFailed, "Pending update has no forward rollback backup root"});
    }
    if (config.currentVersion != pending.version) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ApplyFailed, "The running version does not match the pending applied version"});
    }
    auto terminal = loadTerminalApplyTransaction(fileSystem, config.installDir);
    if (!terminal) {
        return Result<WrittenApplyPlan>::fail(terminal.error());
    }
    if (!terminal.value()) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ApplyFailed, "The pending update has no completed apply transaction"});
    }
    if (!pending.applyPlanDigest.empty() && terminal.value()->planDigest != pending.applyPlanDigest) {
        return Result<WrittenApplyPlan>::fail(
            {ErrorCode::ApplyFailed, "The latest completed transaction does not match the pending update"});
    }
    auto validRequestDirectory = validateRollbackRequestDirectory(config, pending, terminal.value()->transactionId);
    if (!validRequestDirectory) {
        return Result<WrittenApplyPlan>::fail(validRequestDirectory.error());
    }

    ApplyPlan plan;
    plan.schemaVersion = 2;
    plan.intent = ApplyPlanIntent::Rollback;
    // Legacy pending state did not persist the digest. Binding the request to
    // the exact terminal receipt is safe because the external updater also
    // validates the immutable source snapshot's version, release and roots.
    plan.rollbackOf = ApplyTransactionReference{terminal.value()->transactionId, terminal.value()->planDigest};
    plan.appId = config.appId;
    plan.fromVersion = pending.version.toString();
    plan.releaseId = pending.releaseId;
    plan.installDir = config.installDir;
    plan.stagingDir = pending.backupDir;
    plan.backupDir = util::defaultStagingRoot(config.installDir) / "backup" / "rollback" /
                     util::pathFromUtf8(terminal.value()->transactionId);
    // The external updater derives both operations and the restart command
    // from the terminal-bound immutable forward transaction snapshot while
    // holding the install lock. The request itself cannot introduce either.
    plan.restartCommand.clear();
    plan.operations.clear();

    return writePlanFile(std::move(plan), config.tempDir, "rollback-plan.json", config.resources, fileSystem);
}

} // namespace autoupdater
