#include "ApplyExecutor.h"

#include "ApplyJournal.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace autoupdater::updater {

namespace {

constexpr std::size_t kNoOperation = (std::numeric_limits<std::size_t>::max)();
constexpr std::uint32_t kPortablePermissionMask = 0777U;

struct ObservedFile {
    bool exists = false;
    RootedFileMetadata metadata;
    std::string sha256;
};

Result<void> consumePlanTextBudget(const std::string& value, const ResourceLimits& limits,
                                   std::uint64_t& consumedBytes) {
    if (value.size() > limits.json.maxStringBytes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan string exceeds its byte limit"});
    }
    if (consumedBytes > limits.maxApplyPlanBytes || value.size() > limits.maxApplyPlanBytes - consumedBytes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan exceeds its byte limit"});
    }
    consumedBytes += static_cast<std::uint64_t>(value.size());
    return Result<void>::ok();
}

Result<void> validateApplyPlanResources(const ApplyPlan& plan, const ResourceLimits& limits) {
    if (plan.schemaVersion != 1 && plan.schemaVersion != 2) {
        return Result<void>::fail({ErrorCode::UnsupportedManifestSchema, "Unsupported apply plan schemaVersion"});
    }
    if (plan.schemaVersion == 1 &&
        (plan.intent != ApplyPlanIntent::Install || plan.rollbackOf.has_value())) {
        return Result<void>::fail(
            {ErrorCode::ApplyFailed, "Apply plan schemaVersion 1 cannot contain transaction intent"});
    }
    if (plan.intent == ApplyPlanIntent::Rollback) {
        if (plan.schemaVersion != 2 || !plan.rollbackOf ||
            !util::isLowerHexSha256(plan.rollbackOf->transactionId) ||
            !util::isLowerHexSha256(plan.rollbackOf->planDigest)) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Rollback transaction reference is invalid"});
        }
    } else if (plan.rollbackOf) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Install plans cannot contain rollbackOf"});
    }
    if (plan.operations.size() > limits.json.maxContainerEntries ||
        plan.restartCommand.size() > limits.json.maxContainerEntries) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan entry limit exceeded"});
    }

    std::uint64_t consumedBytes = 1024;
    if (consumedBytes > limits.maxApplyPlanBytes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan exceeds its byte limit"});
    }
    for (const auto& value :
         {plan.appId, plan.fromVersion, plan.toVersion, plan.releaseId, plan.manifestSha256,
           util::pathToUtf8(plan.installDir), util::pathToUtf8(plan.stagingDir), util::pathToUtf8(plan.backupDir)}) {
        auto valid = consumePlanTextBudget(value, limits, consumedBytes);
        if (!valid) {
            return valid;
        }
    }
    if (plan.rollbackOf) {
        for (const auto& value : {plan.rollbackOf->transactionId, plan.rollbackOf->planDigest}) {
            auto valid = consumePlanTextBudget(value, limits, consumedBytes);
            if (!valid) {
                return valid;
            }
        }
    }
    for (const auto& argument : plan.restartCommand) {
        auto valid = consumePlanTextBudget(argument, limits, consumedBytes);
        if (!valid) {
            return valid;
        }
    }

    std::uint64_t totalArtifactBytes = 0;
    for (const auto& operation : plan.operations) {
        auto validTarget = util::validateManagedTargetPath(operation.target);
        if (!validTarget) {
            return validTarget;
        }
        if (operation.type == ApplyOperationType::Replace) {
            auto validSource = util::validateManagedPath(operation.source);
            if (!validSource) {
                return validSource;
            }
        }
        for (const auto& value : {operation.source, operation.target, operation.sha256}) {
            auto valid = consumePlanTextBudget(value, limits, consumedBytes);
            if (!valid) {
                return valid;
            }
        }
        if (operation.size > limits.maxArtifactBytes) {
            return Result<void>::fail(
                {ErrorCode::ResourceLimitExceeded, "Apply operation exceeds the artifact byte limit"});
        }
        if (operation.permissions &&
            (operation.type != ApplyOperationType::Replace || *operation.permissions > kPortablePermissionMask)) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Apply operation permissions are invalid"});
        }
        if (operation.precondition) {
            if (plan.schemaVersion < 2 ||
                (operation.precondition->permissions &&
                 *operation.precondition->permissions > kPortablePermissionMask) ||
                (operation.precondition->exists &&
                 (!util::isLowerHexSha256(operation.precondition->sha256) ||
                  operation.precondition->size > limits.maxArtifactBytes)) ||
                (!operation.precondition->exists &&
                 (operation.precondition->size != 0 || !operation.precondition->sha256.empty() ||
                  operation.precondition->permissions))) {
                return Result<void>::fail({ErrorCode::ApplyFailed, "Apply operation precondition is invalid"});
            }
            auto valid = consumePlanTextBudget(operation.precondition->sha256, limits, consumedBytes);
            if (!valid) {
                return valid;
            }
        }
        if (operation.type == ApplyOperationType::Replace) {
            if (totalArtifactBytes > limits.maxTotalArtifactBytes ||
                operation.size > limits.maxTotalArtifactBytes - totalArtifactBytes) {
                return Result<void>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Apply operations exceed the total artifact byte limit"});
            }
            totalArtifactBytes += operation.size;
        }
    }
    return Result<void>::ok();
}

std::filesystem::perms sanitizedFilePermissions(std::filesystem::perms permissions) {
    constexpr auto allowed = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                             std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                             std::filesystem::perms::group_write | std::filesystem::perms::group_exec |
                             std::filesystem::perms::others_read | std::filesystem::perms::others_write |
                             std::filesystem::perms::others_exec;
    return permissions & allowed;
}

std::filesystem::perms defaultInstalledFilePermissions() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
           std::filesystem::perms::group_read | std::filesystem::perms::others_read;
}

bool permissionsKnown(std::filesystem::perms permissions) {
    return permissions != std::filesystem::perms::unknown;
}

std::uint32_t permissionBits(std::filesystem::perms permissions) {
    return static_cast<std::uint32_t>(sanitizedFilePermissions(permissions));
}

Result<void> copyRootedFile(IRootedFile& source, IRootedFile& destination) {
    auto sourceStart = source.seek(0);
    if (!sourceStart) {
        return sourceStart;
    }
    auto cleared = destination.truncate(0);
    if (!cleared) {
        return cleared;
    }
    auto destinationStart = destination.seek(0);
    if (!destinationStart) {
        return destinationStart;
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        auto read = source.read(buffer.data(), buffer.size());
        if (!read) {
            return Result<void>::fail(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        auto written = destination.write(buffer.data(), read.value());
        if (!written) {
            return written;
        }
    }
    return Result<void>::ok();
}

Result<void> copyPermissions(IRootedFile& source, IRootedFile& destination) {
    auto metadata = source.metadata();
    if (!metadata) {
        return Result<void>::fail(metadata.error());
    }
    return destination.setPermissions(sanitizedFilePermissions(metadata.value().permissions));
}

Result<ObservedFile> observeFile(IRootedDirectory& root, const std::string& path, IHashProvider& hashProvider) {
    auto opened = root.openRegularFile(path, RootedFileOpenMode::ReadOnly);
    if (!opened) {
        return Result<ObservedFile>::fail(opened.error());
    }
    if (!opened.value().exists()) {
        return Result<ObservedFile>::ok({});
    }
    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        return Result<ObservedFile>::fail(metadata.error());
    }
    auto hash = hashProvider.sha256Stream(*opened.value().file);
    if (!hash) {
        return Result<ObservedFile>::fail(hash.error());
    }
    ObservedFile observed;
    observed.exists = true;
    observed.metadata = metadata.value();
    observed.sha256 = hash.value();
    return Result<ObservedFile>::ok(std::move(observed));
}

Result<void> verifyEvidence(IRootedFile& file, std::uint64_t expectedSize, const std::string& expectedHash,
                            IHashProvider& hashProvider, const std::string& context) {
    auto metadata = file.metadata();
    if (!metadata) {
        return Result<void>::fail(metadata.error());
    }
    if (metadata.value().size != expectedSize) {
        return Result<void>::fail({ErrorCode::HashMismatch, context + " size mismatch"});
    }
    auto hash = hashProvider.sha256Stream(file);
    if (!hash) {
        return Result<void>::fail(hash.error());
    }
    if (hash.value() != expectedHash) {
        return Result<void>::fail({ErrorCode::HashMismatch, context + " SHA-256 mismatch"});
    }
    return Result<void>::ok();
}

bool matchesEvidence(const ObservedFile& file, std::uint64_t size, const std::string& hash) {
    return file.exists && file.metadata.size == size && file.sha256 == hash;
}

bool matchesInstalled(const ObservedFile& file, const ApplyOperation& operation) {
    if (operation.type == ApplyOperationType::Remove) {
        return !file.exists;
    }
    return matchesEvidence(file, operation.size, operation.sha256) &&
           (!operation.permissions ||
            (permissionsKnown(file.metadata.permissions) &&
             permissionBits(file.metadata.permissions) == *operation.permissions));
}

bool matchesPrecondition(const ObservedFile& file, const ApplyOperationPrecondition& precondition) {
    if (file.exists != precondition.exists) {
        return false;
    }
    if (!precondition.exists) {
        return true;
    }
    return matchesEvidence(file, precondition.size, precondition.sha256) &&
           (!precondition.permissions ||
            (permissionsKnown(file.metadata.permissions) &&
             permissionBits(file.metadata.permissions) == *precondition.permissions));
}

bool matchesOriginal(const ObservedFile& file, const ApplyJournalOperation& record) {
    if (!record.originalExists) {
        return !file.exists;
    }
    return matchesEvidence(file, record.originalSize, record.originalSha256) &&
           (!record.originalPermissionsKnown ||
            permissionBits(file.metadata.permissions) == record.originalPermissions);
}

Result<void> verifyOriginalEvidence(IRootedFile& file, const ApplyJournalOperation& record, IHashProvider& hashProvider,
                                    const std::string& context) {
    auto verified = verifyEvidence(file, record.originalSize, record.originalSha256, hashProvider, context);
    if (!verified) {
        return verified;
    }
    if (record.originalPermissionsKnown) {
        auto metadata = file.metadata();
        if (!metadata) {
            return Result<void>::fail(metadata.error());
        }
        if (!permissionsKnown(metadata.value().permissions) ||
            permissionBits(metadata.value().permissions) != record.originalPermissions) {
            return Result<void>::fail({ErrorCode::SecurityPolicyViolation, context + " permissions mismatch"});
        }
    }
    return Result<void>::ok();
}

JournalError toJournalError(const Error& error) {
    return {toString(error.code), error.message};
}

Error combinedError(const Error& primary, std::string_view context, const Error& secondary) {
    return {ErrorCode::ApplyFailed, primary.message + "; " + std::string(context) + ": " + secondary.message};
}

Result<void> copyToReplacement(IRootedDirectory& targetRoot, const std::string& target,
                               RootedDirectoryCreationMode directoryMode, IRootedFile& source,
                               const RootedEntryExpectation& expectation, std::uint64_t expectedSize,
                               const std::string& expectedHash, IHashProvider& hashProvider) {
    auto temporary = targetRoot.createAtomicReplacement(target, directoryMode);
    if (!temporary) {
        return Result<void>::fail(temporary.error());
    }
    auto copied = copyRootedFile(source, temporary.value()->file());
    if (!copied) {
        return copied;
    }
    auto permissions = copyPermissions(source, temporary.value()->file());
    if (!permissions) {
        return permissions;
    }
    auto verified = verifyEvidence(temporary.value()->file(), expectedSize, expectedHash, hashProvider,
                                   "Prepared rooted replacement");
    if (!verified) {
        return verified;
    }
    return temporary.value()->commit(expectation);
}

Result<void> ensureBackup(IRootedDirectory& backupRoot, const std::string& target, IRootedFile& source,
                          const ApplyJournalOperation& record, IHashProvider& hashProvider) {
    auto existing = backupRoot.openRegularFile(target, RootedFileOpenMode::ReadOnly);
    if (!existing) {
        return Result<void>::fail(existing.error());
    }
    if (existing.value().exists()) {
        auto verified =
            verifyOriginalEvidence(*existing.value().file, record, hashProvider, "Existing rollback backup");
        if (!verified) {
            return Result<void>::fail(
                {ErrorCode::SecurityPolicyViolation, "Existing rollback backup does not match the journal"});
        }
        return Result<void>::ok();
    }

    auto temporary = backupRoot.createAtomicReplacement(target);
    if (!temporary) {
        return Result<void>::fail(temporary.error());
    }
    auto copied = copyRootedFile(source, temporary.value()->file());
    if (!copied) {
        return copied;
    }
    auto permissions = copyPermissions(source, temporary.value()->file());
    if (!permissions) {
        return permissions;
    }
    auto verified = verifyOriginalEvidence(temporary.value()->file(), record, hashProvider, "Prepared rollback backup");
    if (!verified) {
        return verified;
    }
    auto committed = temporary.value()->commit(RootedEntryExpectation::missing());
    if (!committed) {
        return committed;
    }
    temporary.value().reset();
    auto durable = backupRoot.openRegularFile(target, RootedFileOpenMode::ReadOnly);
    if (!durable) {
        return Result<void>::fail(durable.error());
    }
    if (!durable.value().exists()) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Rollback backup disappeared after commit"});
    }
    return verifyOriginalEvidence(*durable.value().file, record, hashProvider, "Committed rollback backup");
}

bool hasReplaceOperation(const ApplyPlan& plan) {
    for (const auto& operation : plan.operations) {
        if (operation.type == ApplyOperationType::Replace) {
            return true;
        }
    }
    return false;
}

Result<void> launchRestart(const ApplyPlan& plan, IProcessLauncher& launcher) {
    if (plan.restartCommand.empty()) {
        return Result<void>::ok();
    }
    ProcessLaunchRequest request;
    request.executable = plan.restartCommand.front();
    request.workingDirectory = plan.installDir;
    request.detached = true;
    for (std::size_t index = 1; index < plan.restartCommand.size(); ++index) {
        request.arguments.push_back(plan.restartCommand[index]);
    }
    return launcher.launch(request);
}

class ApplyTransaction final {
  public:
    ApplyTransaction(const ApplyPlan& plan, IRootedDirectory& installRoot, IRootedDirectory& backupRoot,
                     IRootedDirectory* stagingRoot, IHashProvider& hashProvider, IProcessLauncher& processLauncher,
                     ResourceLimits limits, const ApplyExecutionHooks& hooks, std::string transactionId,
                     std::string planDigest, ApplyJournalSummary summary = {})
        : plan_(plan), installRoot_(installRoot), backupRoot_(backupRoot), stagingRoot_(stagingRoot),
          hashProvider_(hashProvider), processLauncher_(processLauncher), limits_(std::move(limits)), hooks_(hooks),
          transactionId_(std::move(transactionId)), planDigest_(std::move(planDigest)), journal_(installRoot_, limits_),
          summary_(std::move(summary)) {}

    Result<void> start() {
        const auto planJson = plan_.toJson();
        auto snapshot = journal_.writePlanSnapshot(transactionId_, planDigest_, planJson);
        if (!snapshot) {
            return snapshot;
        }
        auto boundary = checkpoint("journal.plan.after", kNoOperation);
        if (!boundary) {
            return boundary;
        }

        summary_.transactionId = transactionId_;
        summary_.planDigest = planDigest_;
        summary_.fileState = JournalFileState::Prepared;
        summary_.operationCount = plan_.operations.size();
        summary_.restartState =
            plan_.restartCommand.empty() ? JournalRestartState::NotRequested : JournalRestartState::NotAttempted;
        auto prepared = persistSummary("journal.prepared.after");
        if (!prepared) {
            return prepared;
        }

        // Build and durably verify every rollback record before publishing the
        // active pointer. If preparation is interrupted, no install target has
        // been modified and the unique orphan transaction can be ignored.
        for (std::size_t index = 0; index < plan_.operations.size(); ++index) {
            auto operationPrepared = prepareOne(index, plan_.operations[index]);
            if (!operationPrepared) {
                return operationPrepared;
            }
        }
        boundary = checkpoint("journal.preparation_complete.after", kNoOperation);
        if (!boundary) {
            return boundary;
        }

        auto active = journal_.writeActive({transactionId_, planDigest_});
        if (!active) {
            return active;
        }
        active_ = true;
        boundary = checkpoint("journal.active.after", kNoOperation);
        if (!boundary) {
            return failAndRollback(boundary.error());
        }

        summary_.fileState = JournalFileState::Applying;
        auto applying = persistSummary("journal.applying.after");
        if (!applying) {
            return failAndRollback(applying.error());
        }

        for (std::size_t index = 0; index < plan_.operations.size(); ++index) {
            auto applied = applyOne(index, plan_.operations[index]);
            if (!applied) {
                return failAndRollback(applied.error());
            }
        }

        summary_.fileState = JournalFileState::FilesApplied;
        auto filesApplied = persistSummary("journal.files_applied.after");
        if (!filesApplied) {
            return failAndRollback(filesApplied.error());
        }
        return finishRestart();
    }

    Result<void> recover() {
        active_ = true;
        if (summary_.fileState == JournalFileState::Complete) {
            auto verified = verifyAppliedState();
            if (!verified) {
                const Error terminalError{ErrorCode::ApplyFailed,
                                          "Completed file installation did not survive durable reconciliation: " +
                                              verified.error().message};
                return recordTerminalReconciliationFailure(terminalError);
            }
            const auto restartFailure = completedRestartFailure();
            auto cleared = publishTerminalAndClear();
            if (!cleared) {
                return cleared;
            }
            if (restartFailure) {
                return Result<void>::fail(*restartFailure);
            }
            return Result<void>::ok();
        }
        if (summary_.fileState == JournalFileState::RolledBack) {
            auto reconciled = rollbackAll();
            if (!reconciled) {
                return recordRollbackFailure({ErrorCode::ApplyFailed, "Failed to reconcile a completed rollback"},
                                             reconciled.error());
            }
            auto cleared = clearActive();
            if (!cleared) {
                return cleared;
            }
            return Result<void>::fail(
                {ErrorCode::ApplyFailed, "The previous incomplete apply transaction was rolled back"});
        }
        if (summary_.fileState == JournalFileState::FilesApplied) {
            auto verified = verifyAppliedState();
            if (!verified) {
                const Error reconciliationError{ErrorCode::ApplyFailed,
                                                "Recovered file installation did not match its durable journal: " +
                                                    verified.error().message};
                if (restartMayHaveStarted()) {
                    return recordTerminalReconciliationFailure(reconciliationError);
                }
                return recoverByRollback(reconciliationError);
            }
            return finishRestart();
        }

        if (restartMayHaveStarted()) {
            return recordTerminalReconciliationFailure(
                {ErrorCode::ApplyFailed, "Apply journal cannot safely roll back after the restart may have started"});
        }

        const Error interrupted{ErrorCode::ApplyFailed,
                                "Recovered an incomplete apply transaction and restored its prior file state"};
        return recoverByRollback(interrupted);
    }

    Result<void> replayTerminal() {
        if (summary_.fileState != JournalFileState::Complete) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Terminal receipt points to an incomplete transaction"});
        }
        auto verified = verifyAppliedState();
        if (!verified) {
            return Result<void>::fail(
                {ErrorCode::ApplyFailed,
                 "Terminal transaction no longer matches installed files: " + verified.error().message});
        }
        const auto restartFailure = completedRestartFailure();
        if (restartFailure) {
            return Result<void>::fail(*restartFailure);
        }
        return Result<void>::ok();
    }

    Result<std::vector<ApplyJournalOperation>> rollbackSourceRecords() {
        if (summary_.fileState != JournalFileState::Complete) {
            return Result<std::vector<ApplyJournalOperation>>::fail(
                {ErrorCode::ApplyFailed, "Rollback source transaction is not complete"});
        }
        auto verified = verifyAppliedState();
        if (!verified) {
            return Result<std::vector<ApplyJournalOperation>>::fail(verified.error());
        }
        return loadOperationRecords();
    }

  private:
    bool restartMayHaveStarted() const noexcept {
        return summary_.restartState == JournalRestartState::Intent ||
               summary_.restartState == JournalRestartState::Launched ||
               summary_.restartState == JournalRestartState::OutcomeUnknown;
    }

    Result<void> recordTerminalReconciliationFailure(const Error& error) {
        summary_.applyError = toJournalError(error);
        if (summary_.restartState == JournalRestartState::Intent) {
            summary_.restartState = JournalRestartState::OutcomeUnknown;
            summary_.restartError = {toString(ErrorCode::ApplyLaunchFailed),
                                     "Restart intent was interrupted before terminal file reconciliation"};
        }
        auto recorded = persistSummary("journal.terminal_reconciliation_failed.after");
        if (!recorded) {
            return Result<void>::fail(
                combinedError(error, "failed to persist terminal reconciliation failure", recorded.error()));
        }
        // A restarted application may still be running. Keep the active
        // pointer and require operator intervention instead of replacing its
        // files underneath it.
        return Result<void>::fail(error);
    }

    Result<void> recoverByRollback(const Error& interrupted) {
        summary_.fileState = JournalFileState::RollingBack;
        summary_.rollbackError = {};
        if (summary_.applyError.empty()) {
            summary_.applyError = toJournalError(interrupted);
        }
        auto marked = persistSummary("journal.rollback_summary.after");
        if (!marked) {
            return Result<void>::fail(combinedError(interrupted, "failed to persist recovery intent", marked.error()));
        }
        auto rolledBack = rollbackAll();
        if (!rolledBack) {
            return recordRollbackFailure(interrupted, rolledBack.error());
        }
        summary_.fileState = JournalFileState::RolledBack;
        summary_.rollbackError = {};
        auto completed = persistSummary("journal.rolled_back.after");
        if (!completed) {
            return Result<void>::fail(
                combinedError(interrupted, "failed to persist recovered rollback", completed.error()));
        }
        auto cleared = clearActive();
        if (!cleared) {
            return Result<void>::fail(
                combinedError(interrupted, "failed to clear recovered transaction", cleared.error()));
        }
        return Result<void>::fail(interrupted);
    }

    Result<void> checkpoint(std::string_view name, std::size_t operationIndex) const {
        if (!hooks_.checkpoint) {
            return Result<void>::ok();
        }
        try {
            if (hooks_.checkpoint(name, operationIndex) == ApplyFaultAction::Fail) {
                return Result<void>::fail(
                    {ErrorCode::ApplyFailed, "Injected apply failure at transaction boundary: " + std::string(name)});
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail(
                {ErrorCode::ApplyFailed, "Apply transaction checkpoint threw an exception: " + std::string(name)});
        }
    }

    Result<void> persistSummary(std::string_view boundaryName) {
        auto saved = journal_.writeSummary(summary_);
        if (!saved) {
            return saved;
        }
        return checkpoint(boundaryName, kNoOperation);
    }

    Result<void> persistOperation(ApplyJournalOperation& record, std::string_view boundaryName) {
        auto saved = journal_.writeOperation(record);
        if (!saved) {
            return saved;
        }
        return checkpoint(boundaryName, record.index);
    }

    Result<void> failOperation(ApplyJournalOperation& record, const Error& error) {
        record.error = toJournalError(error);
        auto saved = journal_.writeOperation(record);
        if (!saved) {
            return Result<void>::fail(combinedError(error, "failed to record operation error", saved.error()));
        }
        auto boundary = checkpoint("journal.operation_error.after", record.index);
        if (!boundary) {
            return Result<void>::fail(combinedError(error, "operation error checkpoint failed", boundary.error()));
        }
        return Result<void>::fail(error);
    }

    Result<void> prepareOne(std::size_t index, const ApplyOperation& operation) {
        auto operationId = applyOperationId(transactionId_, index, operation);
        if (!operationId) {
            return Result<void>::fail(operationId.error());
        }
        auto original = observeFile(installRoot_, operation.target, hashProvider_);
        if (!original) {
            return Result<void>::fail(original.error());
        }
        if (operation.precondition && !matchesPrecondition(original.value(), *operation.precondition)) {
            return Result<void>::fail(
                {ErrorCode::SecurityPolicyViolation,
                 "Managed target no longer matches the transaction precondition"});
        }

        ApplyJournalOperation record;
        record.transactionId = transactionId_;
        record.index = index;
        record.operationId = operationId.value();
        record.intent = operation.type == ApplyOperationType::Replace ? "replace" : "remove";
        record.originalExists = original.value().exists;
        const bool alreadyApplied = matchesInstalled(original.value(), operation);
        if (record.originalExists) {
            record.originalIdentity = original.value().metadata.identity;
            record.originalSize = original.value().metadata.size;
            record.originalSha256 = original.value().sha256;
            record.originalPermissionsKnown = permissionsKnown(original.value().metadata.permissions);
            if (record.originalPermissionsKnown) {
                record.originalPermissions = permissionBits(original.value().metadata.permissions);
            }
            record.backupState = alreadyApplied ? JournalBackupState::NotRequired : JournalBackupState::Intent;
        } else {
            record.backupState = JournalBackupState::NotRequired;
        }
        if (alreadyApplied) {
            record.applyState = JournalApplyState::Complete;
            record.installedIdentity = original.value().exists ? original.value().metadata.identity : std::string();
            record.rollbackState = JournalRollbackState::NotRequired;
        }
        auto recorded = persistOperation(record, "journal.operation_initial.after");
        if (!recorded) {
            return failOperation(record, recorded.error());
        }

        if (alreadyApplied) {
            return Result<void>::ok();
        }

        if (record.originalExists) {
            if (record.originalSize > limits_.maxArtifactBytes ||
                preparedBackupBytes_ > limits_.maxTotalArtifactBytes ||
                record.originalSize > limits_.maxTotalArtifactBytes - preparedBackupBytes_) {
                return failOperation(record,
                                     {ErrorCode::ResourceLimitExceeded, "Rollback backup exceeds its byte budget"});
            }
            preparedBackupBytes_ += record.originalSize;
            auto boundary = checkpoint("backup.before", index);
            if (!boundary) {
                return failOperation(record, boundary.error());
            }
            auto source = installRoot_.openRegularFile(operation.target, RootedFileOpenMode::ReadOnly);
            if (!source) {
                return failOperation(record, source.error());
            }
            if (!source.value().exists()) {
                return failOperation(record,
                                     {ErrorCode::SecurityPolicyViolation, "Managed target disappeared before backup"});
            }
            auto metadata = source.value().file->metadata();
            if (!metadata) {
                return failOperation(record, metadata.error());
            }
            if (metadata.value().identity != record.originalIdentity) {
                return failOperation(
                    record, {ErrorCode::SecurityPolicyViolation, "Managed target identity changed before backup"});
            }
            auto backup = ensureBackup(backupRoot_, operation.target, *source.value().file, record, hashProvider_);
            if (!backup) {
                return failOperation(record, backup.error());
            }
            boundary = checkpoint("backup.after", index);
            if (!boundary) {
                return failOperation(record, boundary.error());
            }
            record.backupState = JournalBackupState::Durable;
            recorded = persistOperation(record, "journal.backup_durable.after");
            if (!recorded) {
                return failOperation(record, recorded.error());
            }
        }

        return Result<void>::ok();
    }

    Result<void> applyOne(std::size_t index, const ApplyOperation& operation) {
        auto loaded = journal_.readOperation(transactionId_, index);
        if (!loaded) {
            return Result<void>::fail(loaded.error());
        }
        if (!loaded.value()) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Prepared apply operation record is missing"});
        }
        auto record = std::move(*loaded.value());
        auto operationId = applyOperationId(transactionId_, index, operation);
        if (!operationId) {
            return Result<void>::fail(operationId.error());
        }
        if (record.operationId != operationId.value()) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Prepared apply operation identity mismatch"});
        }

        auto original = observeFile(installRoot_, operation.target, hashProvider_);
        if (!original) {
            return failOperation(record, original.error());
        }
        if (!matchesOriginal(original.value(), record) ||
            (record.originalExists && original.value().metadata.identity != record.originalIdentity)) {
            return failOperation(
                record, {ErrorCode::SecurityPolicyViolation, "Managed target changed after transaction preparation"});
        }
        if (record.applyState == JournalApplyState::Complete &&
            record.rollbackState == JournalRollbackState::NotRequired) {
            if (!matchesInstalled(original.value(), operation)) {
                return failOperation(record, {ErrorCode::HashMismatch, "Idempotent apply target changed"});
            }
            return Result<void>::ok();
        }
        if (record.applyState != JournalApplyState::Pending ||
            record.rollbackState != JournalRollbackState::NotStarted ||
            (record.originalExists && record.backupState != JournalBackupState::Durable)) {
            return failOperation(record, {ErrorCode::ApplyFailed, "Prepared apply operation state is inconsistent"});
        }

        const auto expectation = record.originalExists ? RootedEntryExpectation::matching(original.value().metadata)
                                                       : RootedEntryExpectation::missing();
        if (operation.type == ApplyOperationType::Replace) {
            return applyReplacement(record, operation, expectation, original.value());
        }
        return applyRemoval(record, operation, expectation);
    }

    Result<void> applyReplacement(ApplyJournalOperation& record, const ApplyOperation& operation,
                                  const RootedEntryExpectation& expectation, const ObservedFile& original) {
        if (!stagingRoot_) {
            return failOperation(record, {ErrorCode::FileSystemError, "Staging root is unavailable"});
        }
        auto source = stagingRoot_->openRegularFile(operation.source, RootedFileOpenMode::ReadOnly);
        if (!source) {
            return failOperation(record, source.error());
        }
        if (!source.value().exists()) {
            return failOperation(record, {ErrorCode::FileSystemError, "Staged update file is missing"});
        }
        auto sourceVerified =
            verifyEvidence(*source.value().file, operation.size, operation.sha256, hashProvider_, "Staged update file");
        if (!sourceVerified) {
            return failOperation(record, sourceVerified.error());
        }

        auto temporary =
            installRoot_.createAtomicReplacement(operation.target, RootedDirectoryCreationMode::InstalledContent);
        if (!temporary) {
            return failOperation(record, temporary.error());
        }
        auto copied = copyRootedFile(*source.value().file, temporary.value()->file());
        if (!copied) {
            return failOperation(record, copied.error());
        }
        Result<void> permissions = operation.permissions
                                       ? temporary.value()->file().setPermissions(
                                             static_cast<std::filesystem::perms>(*operation.permissions))
                                       : (original.exists ? temporary.value()->file().setPermissions(
                                                                sanitizedFilePermissions(original.metadata.permissions))
                                                          : temporary.value()->file().setPermissions(
                                                                defaultInstalledFilePermissions()));
        if (!permissions) {
            return failOperation(record, permissions.error());
        }
        auto prepared = verifyEvidence(temporary.value()->file(), operation.size, operation.sha256, hashProvider_,
                                       "Prepared installed file");
        if (!prepared) {
            return failOperation(record, prepared.error());
        }

        record.applyState = JournalApplyState::Intent;
        auto intent = persistOperation(record, "journal.apply_intent.after");
        if (!intent) {
            return failOperation(record, intent.error());
        }
        auto boundary = checkpoint("replace.before", record.index);
        if (!boundary) {
            return failOperation(record, boundary.error());
        }
        auto committed = temporary.value()->commit(expectation);
        if (!committed) {
            return failOperation(record, committed.error());
        }
        temporary.value().reset();
        boundary = checkpoint("replace.after", record.index);
        if (!boundary) {
            return failOperation(record, boundary.error());
        }

        auto installed = observeFile(installRoot_, operation.target, hashProvider_);
        if (!installed) {
            return failOperation(record, installed.error());
        }
        if (!matchesInstalled(installed.value(), operation)) {
            return failOperation(record,
                                 {ErrorCode::HashMismatch, "Installed file does not match the signed apply plan"});
        }
        record.installedIdentity = installed.value().metadata.identity;
        record.applyState = JournalApplyState::Complete;
        record.error = {};
        auto completed = persistOperation(record, "journal.apply_complete.after");
        if (!completed) {
            return failOperation(record, completed.error());
        }
        return Result<void>::ok();
    }

    Result<void> applyRemoval(ApplyJournalOperation& record, const ApplyOperation& operation,
                              const RootedEntryExpectation& expectation) {
        record.applyState = JournalApplyState::Intent;
        auto intent = persistOperation(record, "journal.apply_intent.after");
        if (!intent) {
            return failOperation(record, intent.error());
        }
        if (record.originalExists) {
            auto boundary = checkpoint("remove.before", record.index);
            if (!boundary) {
                return failOperation(record, boundary.error());
            }
            auto removed = installRoot_.removeRegularFile(operation.target, expectation);
            if (!removed) {
                return failOperation(record, removed.error());
            }
            boundary = checkpoint("remove.after", record.index);
            if (!boundary) {
                return failOperation(record, boundary.error());
            }
        }
        auto installed = observeFile(installRoot_, operation.target, hashProvider_);
        if (!installed) {
            return failOperation(record, installed.error());
        }
        if (installed.value().exists) {
            return failOperation(record, {ErrorCode::ApplyFailed, "Removed managed target is still present"});
        }
        record.installedIdentity.clear();
        record.applyState = JournalApplyState::Complete;
        record.error = {};
        auto completed = persistOperation(record, "journal.apply_complete.after");
        if (!completed) {
            return failOperation(record, completed.error());
        }
        return Result<void>::ok();
    }

    Result<std::vector<ApplyJournalOperation>> loadOperationRecords() {
        if (summary_.operationCount != plan_.operations.size()) {
            return Result<std::vector<ApplyJournalOperation>>::fail(
                {ErrorCode::ApplyFailed, "Apply journal operation count does not match its plan snapshot"});
        }
        std::vector<ApplyJournalOperation> records;
        for (std::size_t index = 0; index < plan_.operations.size(); ++index) {
            auto loaded = journal_.readOperation(transactionId_, index);
            if (!loaded) {
                return Result<std::vector<ApplyJournalOperation>>::fail(loaded.error());
            }
            if (!loaded.value()) {
                return Result<std::vector<ApplyJournalOperation>>::fail(
                    {ErrorCode::ApplyFailed, "Active apply transaction is missing a prepared operation record"});
            }
            auto expectedId = applyOperationId(transactionId_, index, plan_.operations[index]);
            if (!expectedId) {
                return Result<std::vector<ApplyJournalOperation>>::fail(expectedId.error());
            }
            const auto expectedIntent =
                plan_.operations[index].type == ApplyOperationType::Replace ? "replace" : "remove";
            if (loaded.value()->operationId != expectedId.value() || loaded.value()->intent != expectedIntent) {
                return Result<std::vector<ApplyJournalOperation>>::fail(
                    {ErrorCode::ApplyFailed, "Apply journal operation does not match its plan snapshot"});
            }
            records.push_back(std::move(*loaded.value()));
        }
        return Result<std::vector<ApplyJournalOperation>>::ok(std::move(records));
    }

    Result<void> verifyAppliedState() {
        auto records = loadOperationRecords();
        if (!records) {
            return Result<void>::fail(records.error());
        }
        if (records.value().size() != plan_.operations.size()) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Applied transaction is missing operation records"});
        }
        for (const auto& record : records.value()) {
            const auto& operation = plan_.operations[record.index];
            if (record.applyState != JournalApplyState::Complete || !record.error.empty()) {
                return Result<void>::fail(
                    {ErrorCode::ApplyFailed, "Applied transaction contains an incomplete operation"});
            }
            auto target = observeFile(installRoot_, operation.target, hashProvider_);
            if (!target) {
                return Result<void>::fail(target.error());
            }
            if (!matchesInstalled(target.value(), operation)) {
                return Result<void>::fail(
                    {ErrorCode::HashMismatch, "Installed target no longer matches the apply journal"});
            }
            if (record.originalExists) {
                if (record.backupState == JournalBackupState::NotRequired &&
                    record.rollbackState == JournalRollbackState::NotRequired &&
                    matchesOriginal(target.value(), record)) {
                    continue;
                }
                if (record.backupState != JournalBackupState::Durable) {
                    return Result<void>::fail(
                        {ErrorCode::ApplyFailed, "Applied transaction has no durable backup record"});
                }
                auto backup = backupRoot_.openRegularFile(operation.target, RootedFileOpenMode::ReadOnly);
                if (!backup) {
                    return Result<void>::fail(backup.error());
                }
                if (!backup.value().exists()) {
                    return Result<void>::fail({ErrorCode::ApplyFailed, "Applied transaction backup is missing"});
                }
                auto verified =
                    verifyOriginalEvidence(*backup.value().file, record, hashProvider_, "Applied transaction backup");
                if (!verified) {
                    return verified;
                }
            } else if (record.backupState != JournalBackupState::NotRequired) {
                return Result<void>::fail(
                    {ErrorCode::ApplyFailed, "Applied transaction has an inconsistent backup state"});
            }
        }
        return Result<void>::ok();
    }

    Result<void> rollbackAll() {
        auto records = loadOperationRecords();
        if (!records) {
            return Result<void>::fail(records.error());
        }
        for (auto iterator = records.value().rbegin(); iterator != records.value().rend(); ++iterator) {
            auto rolledBack = rollbackOne(*iterator, plan_.operations[iterator->index]);
            if (!rolledBack) {
                return rolledBack;
            }
        }
        return Result<void>::ok();
    }

    Result<void> rollbackOne(ApplyJournalOperation& record, const ApplyOperation& operation) {
        auto target = observeFile(installRoot_, operation.target, hashProvider_);
        if (!target) {
            return markRollbackOperationFailed(record, target.error());
        }
        if (matchesOriginal(target.value(), record)) {
            record.rollbackState = record.applyState == JournalApplyState::Pending ? JournalRollbackState::NotRequired
                                                                                   : JournalRollbackState::Complete;
            record.error = {};
            return persistOperation(record, "journal.rollback_complete.after");
        }

        const bool mutationMayHaveRun = record.applyState != JournalApplyState::Pending;
        const bool interruptedReplacementGap = mutationMayHaveRun && record.originalExists && !target.value().exists &&
                                               operation.type == ApplyOperationType::Replace &&
                                               record.backupState == JournalBackupState::Durable;
        if (!mutationMayHaveRun || (!matchesInstalled(target.value(), operation) && !interruptedReplacementGap)) {
            return markRollbackOperationFailed(
                record, {ErrorCode::SecurityPolicyViolation,
                         "Managed target no longer matches either the journaled original or installed content"});
        }

        record.rollbackState = JournalRollbackState::Intent;
        record.error = {};
        auto intent = persistOperation(record, "journal.rollback_intent.after");
        if (!intent) {
            return intent;
        }

        if (record.originalExists) {
            if (record.backupState != JournalBackupState::Durable) {
                return markRollbackOperationFailed(
                    record, {ErrorCode::ApplyFailed, "A durable rollback backup was not recorded"});
            }
            auto backup = backupRoot_.openRegularFile(operation.target, RootedFileOpenMode::ReadOnly);
            if (!backup) {
                return markRollbackOperationFailed(record, backup.error());
            }
            if (!backup.value().exists()) {
                return markRollbackOperationFailed(record,
                                                   {ErrorCode::ApplyFailed, "Required rollback backup is missing"});
            }
            auto verified = verifyOriginalEvidence(*backup.value().file, record, hashProvider_, "Rollback backup");
            if (!verified) {
                return markRollbackOperationFailed(record, verified.error());
            }
            auto boundary = checkpoint("rollback.replace.before", record.index);
            if (!boundary) {
                return markRollbackOperationFailed(record, boundary.error());
            }
            const auto expectation = target.value().exists ? RootedEntryExpectation::matching(target.value().metadata)
                                                           : RootedEntryExpectation::missing();
            auto restored = copyToReplacement(installRoot_, operation.target,
                                              RootedDirectoryCreationMode::InstalledContent, *backup.value().file,
                                              expectation, record.originalSize, record.originalSha256, hashProvider_);
            if (!restored) {
                return markRollbackOperationFailed(record, restored.error());
            }
            boundary = checkpoint("rollback.replace.after", record.index);
            if (!boundary) {
                return markRollbackOperationFailed(record, boundary.error());
            }
        } else {
            auto boundary = checkpoint("rollback.remove.before", record.index);
            if (!boundary) {
                return markRollbackOperationFailed(record, boundary.error());
            }
            auto removed = installRoot_.removeRegularFile(operation.target,
                                                          RootedEntryExpectation::matching(target.value().metadata));
            if (!removed) {
                return markRollbackOperationFailed(record, removed.error());
            }
            boundary = checkpoint("rollback.remove.after", record.index);
            if (!boundary) {
                return markRollbackOperationFailed(record, boundary.error());
            }
        }

        auto restored = observeFile(installRoot_, operation.target, hashProvider_);
        if (!restored) {
            return markRollbackOperationFailed(record, restored.error());
        }
        if (!matchesOriginal(restored.value(), record)) {
            return markRollbackOperationFailed(record,
                                               {ErrorCode::HashMismatch, "Rollback result does not match the journal"});
        }
        record.rollbackState = JournalRollbackState::Complete;
        record.error = {};
        return persistOperation(record, "journal.rollback_complete.after");
    }

    Result<void> markRollbackOperationFailed(ApplyJournalOperation& record, const Error& error) {
        record.rollbackState = JournalRollbackState::Failed;
        record.error = toJournalError(error);
        auto saved = journal_.writeOperation(record);
        if (!saved) {
            return Result<void>::fail(combinedError(error, "failed to record rollback error", saved.error()));
        }
        auto boundary = checkpoint("journal.rollback_failed.after", record.index);
        if (!boundary) {
            return Result<void>::fail(combinedError(error, "rollback error checkpoint failed", boundary.error()));
        }
        return Result<void>::fail(error);
    }

    Result<void> failAndRollback(const Error& original) {
        if (!active_) {
            return Result<void>::fail(original);
        }
        summary_.applyError = toJournalError(original);
        summary_.fileState = JournalFileState::RollingBack;
        summary_.rollbackError = {};
        auto marked = persistSummary("journal.rollback_summary.after");
        if (!marked) {
            return Result<void>::fail(combinedError(original, "failed to persist rollback intent", marked.error()));
        }
        auto rolledBack = rollbackAll();
        if (!rolledBack) {
            return recordRollbackFailure(original, rolledBack.error());
        }
        summary_.fileState = JournalFileState::RolledBack;
        summary_.rollbackError = {};
        auto completed = persistSummary("journal.rolled_back.after");
        if (!completed) {
            return Result<void>::fail(
                combinedError(original, "failed to persist rollback completion", completed.error()));
        }
        auto cleared = clearActive();
        if (!cleared) {
            return Result<void>::fail(
                combinedError(original, "failed to clear rolled-back transaction", cleared.error()));
        }
        return Result<void>::fail(original);
    }

    Result<void> recordRollbackFailure(const Error& original, const Error& rollbackError) {
        summary_.fileState = JournalFileState::RecoveryFailed;
        summary_.rollbackError = toJournalError(rollbackError);
        auto saved = journal_.writeSummary(summary_);
        if (!saved) {
            return Result<void>::fail(combinedError(combinedError(original, "rollback failed", rollbackError),
                                                    "failed to record rollback failure", saved.error()));
        }
        auto boundary = checkpoint("journal.recovery_failed.after", kNoOperation);
        if (!boundary) {
            return Result<void>::fail(combinedError(combinedError(original, "rollback failed", rollbackError),
                                                    "recovery failure checkpoint failed", boundary.error()));
        }
        return Result<void>::fail(combinedError(original, "rollback failed", rollbackError));
    }

    std::optional<Error> completedRestartFailure() const {
        if (summary_.restartState == JournalRestartState::Failed) {
            return Error{ErrorCode::ApplyLaunchFailed, summary_.restartError.message.empty()
                                                           ? "Files were installed but restart failed"
                                                           : summary_.restartError.message};
        }
        if (summary_.restartState == JournalRestartState::OutcomeUnknown ||
            summary_.restartState == JournalRestartState::Intent) {
            return Error{ErrorCode::ApplyLaunchFailed,
                         "Files were installed but the restart outcome is unknown; restart was not repeated"};
        }
        return std::nullopt;
    }

    Result<void> finishRestart() {
        if (plan_.restartCommand.empty()) {
            summary_.restartState = JournalRestartState::NotRequested;
            summary_.fileState = JournalFileState::Complete;
            auto completed = persistSummary("journal.complete.after");
            if (!completed) {
                return completed;
            }
            return publishTerminalAndClear();
        }

        if (summary_.restartState == JournalRestartState::Intent) {
            summary_.restartState = JournalRestartState::OutcomeUnknown;
            summary_.restartError = {toString(ErrorCode::ApplyLaunchFailed),
                                     "Restart intent was interrupted; the process was not launched again"};
            summary_.fileState = JournalFileState::Complete;
            auto completed = persistSummary("journal.complete.after");
            if (!completed) {
                return completed;
            }
            auto cleared = publishTerminalAndClear();
            if (!cleared) {
                return cleared;
            }
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, summary_.restartError.message});
        }
        if (summary_.restartState == JournalRestartState::Launched) {
            summary_.fileState = JournalFileState::Complete;
            auto completed = persistSummary("journal.complete.after");
            if (!completed) {
                return completed;
            }
            return publishTerminalAndClear();
        }
        if (summary_.restartState == JournalRestartState::Failed ||
            summary_.restartState == JournalRestartState::OutcomeUnknown) {
            summary_.fileState = JournalFileState::Complete;
            auto failure = completedRestartFailure();
            auto completed = persistSummary("journal.complete.after");
            if (!completed) {
                return completed;
            }
            auto cleared = publishTerminalAndClear();
            if (!cleared) {
                return cleared;
            }
            return Result<void>::fail(*failure);
        }
        if (summary_.restartState != JournalRestartState::NotAttempted) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Apply journal restart state is inconsistent"});
        }

        summary_.restartState = JournalRestartState::Intent;
        summary_.restartError = {};
        auto intent = persistSummary("journal.restart_intent.after");
        if (!intent) {
            return intent;
        }
        auto launched = launchRestart(plan_, processLauncher_);
        if (!launched) {
            summary_.restartState = JournalRestartState::Failed;
            summary_.restartError = toJournalError(launched.error());
            summary_.fileState = JournalFileState::Complete;
            auto recorded = persistSummary("journal.restart_failed.after");
            if (!recorded) {
                return Result<void>::fail(
                    combinedError(launched.error(), "failed to record restart failure", recorded.error()));
            }
            auto cleared = publishTerminalAndClear();
            if (!cleared) {
                return Result<void>::fail(
                    combinedError(launched.error(), "failed to clear completed transaction", cleared.error()));
            }
            return Result<void>::fail(
                {ErrorCode::ApplyLaunchFailed, "Files were installed but restart failed: " + launched.error().message});
        }
        auto boundary = checkpoint("restart.after", kNoOperation);
        if (!boundary) {
            return boundary;
        }
        summary_.restartState = JournalRestartState::Launched;
        summary_.fileState = JournalFileState::Complete;
        auto completed = persistSummary("journal.complete.after");
        if (!completed) {
            return completed;
        }
        return publishTerminalAndClear();
    }

    Result<void> publishTerminalAndClear() {
        auto published = journal_.writeTerminal({transactionId_, planDigest_});
        if (!published) {
            return published;
        }
        auto boundary = checkpoint("journal.terminal.after", kNoOperation);
        if (!boundary) {
            return boundary;
        }
        return clearActive();
    }

    Result<void> clearActive() {
        auto cleared = journal_.clearActive(transactionId_);
        if (!cleared) {
            return cleared;
        }
        active_ = false;
        return checkpoint("journal.active_clear.after", kNoOperation);
    }

    const ApplyPlan& plan_;
    IRootedDirectory& installRoot_;
    IRootedDirectory& backupRoot_;
    IRootedDirectory* stagingRoot_ = nullptr;
    IHashProvider& hashProvider_;
    IProcessLauncher& processLauncher_;
    ResourceLimits limits_;
    const ApplyExecutionHooks& hooks_;
    std::string transactionId_;
    std::string planDigest_;
    ApplyJournalStore journal_;
    ApplyJournalSummary summary_;
    std::uint64_t preparedBackupBytes_ = 0;
    bool active_ = false;
};

Result<void> validateRecoveredSummary(const ApplyPlan& plan, const ActiveTransaction& transaction,
                                      const ApplyJournalSummary& summary) {
    if (summary.transactionId != transaction.transactionId || summary.planDigest != transaction.planDigest) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Apply transaction and journal summary disagree"});
    }
    if (summary.operationCount != plan.operations.size()) {
        return Result<void>::fail(
            {ErrorCode::ApplyFailed, "Apply journal operation count does not match its plan snapshot"});
    }
    const auto completeError = [](const JournalError& error) { return error.code.empty() == error.message.empty(); };
    if (!completeError(summary.applyError) || !completeError(summary.rollbackError) ||
        !completeError(summary.restartError)) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Apply journal contains an incomplete error record"});
    }

    const bool restartRequested = !plan.restartCommand.empty();
    if ((summary.restartState == JournalRestartState::NotRequested) == restartRequested) {
        return Result<void>::fail(
            {ErrorCode::ApplyFailed, "Apply journal restart state does not match its plan snapshot"});
    }

    const bool restartErrorRequired = summary.restartState == JournalRestartState::Failed ||
                                      summary.restartState == JournalRestartState::OutcomeUnknown;
    if (summary.restartError.empty() == restartErrorRequired) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Apply journal restart error state is inconsistent"});
    }
    const bool rollbackErrorRequired = summary.fileState == JournalFileState::RecoveryFailed;
    if (summary.rollbackError.empty() == rollbackErrorRequired) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Apply journal rollback error state is inconsistent"});
    }

    bool stateCombinationAllowed = false;
    switch (summary.fileState) {
    case JournalFileState::Prepared:
    case JournalFileState::Applying:
    case JournalFileState::RollingBack:
    case JournalFileState::RolledBack:
    case JournalFileState::RecoveryFailed:
        stateCombinationAllowed = summary.restartState == JournalRestartState::NotRequested ||
                                  summary.restartState == JournalRestartState::NotAttempted;
        break;
    case JournalFileState::FilesApplied:
        stateCombinationAllowed = summary.restartState == JournalRestartState::NotRequested ||
                                  summary.restartState == JournalRestartState::NotAttempted ||
                                  summary.restartState == JournalRestartState::Intent ||
                                  summary.restartState == JournalRestartState::OutcomeUnknown;
        break;
    case JournalFileState::Complete:
        stateCombinationAllowed = summary.restartState == JournalRestartState::NotRequested ||
                                  summary.restartState == JournalRestartState::Launched ||
                                  summary.restartState == JournalRestartState::Failed ||
                                  summary.restartState == JournalRestartState::OutcomeUnknown;
        break;
    }
    if (!stateCombinationAllowed) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Apply journal file and restart states are inconsistent"});
    }
    return Result<void>::ok();
}

Result<void> recoverActiveTransaction(const ApplyPlan& requestedPlan, IRootedDirectory& installRoot,
                                      const ActiveTransaction& active, ApplyExecutorDependencies& dependencies,
                                      const ApplyExecutionHooks& hooks) {
    ApplyJournalStore journal(installRoot, dependencies.limits);
    auto planJson = journal.readPlanSnapshot(active.transactionId, active.planDigest);
    if (!planJson) {
        return Result<void>::fail(planJson.error());
    }
    auto recoveryPlan = ApplyPlan::parse(planJson.value(), dependencies.limits);
    if (!recoveryPlan) {
        return Result<void>::fail(recoveryPlan.error());
    }
    if (recoveryPlan.value().installDir.lexically_normal() != requestedPlan.installDir.lexically_normal()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Active apply journal belongs to a different install directory"});
    }
    auto summary = journal.readSummary(active.transactionId);
    if (!summary) {
        return Result<void>::fail(summary.error());
    }
    auto validSummary = validateRecoveredSummary(recoveryPlan.value(), active, summary.value());
    if (!validSummary) {
        return validSummary;
    }
    auto backupRoot = dependencies.fileSystem->openRoot(recoveryPlan.value().backupDir, RootAccess::ReadWrite, true);
    if (!backupRoot) {
        return Result<void>::fail(backupRoot.error());
    }
    ApplyTransaction transaction(recoveryPlan.value(), installRoot, *backupRoot.value(), nullptr,
                                 *dependencies.hashProvider, *dependencies.processLauncher, dependencies.limits, hooks,
                                 active.transactionId, active.planDigest, summary.value());
    return transaction.recover();
}

Result<void> replayTerminalTransaction(const ApplyPlan& requestedPlan, IRootedDirectory& installRoot,
                                       const ActiveTransaction& terminal, ApplyExecutorDependencies& dependencies,
                                       const ApplyExecutionHooks& hooks) {
    ApplyJournalStore journal(installRoot, dependencies.limits);
    auto planJson = journal.readPlanSnapshot(terminal.transactionId, terminal.planDigest);
    if (!planJson) {
        return Result<void>::fail(planJson.error());
    }
    auto terminalPlan = ApplyPlan::parse(planJson.value(), dependencies.limits);
    if (!terminalPlan) {
        return Result<void>::fail(terminalPlan.error());
    }
    if (terminalPlan.value().installDir.lexically_normal() != requestedPlan.installDir.lexically_normal()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Terminal apply receipt belongs to a different install directory"});
    }
    auto summary = journal.readSummary(terminal.transactionId);
    if (!summary) {
        return Result<void>::fail(summary.error());
    }
    auto validSummary = validateRecoveredSummary(terminalPlan.value(), terminal, summary.value());
    if (!validSummary) {
        return validSummary;
    }
    auto backupRoot = dependencies.fileSystem->openRoot(terminalPlan.value().backupDir, RootAccess::ReadOnly, false);
    if (!backupRoot) {
        return Result<void>::fail(backupRoot.error());
    }
    ApplyTransaction transaction(terminalPlan.value(), installRoot, *backupRoot.value(), nullptr,
                                 *dependencies.hashProvider, *dependencies.processLauncher, dependencies.limits, hooks,
                                 terminal.transactionId, terminal.planDigest, summary.value());
    return transaction.replayTerminal();
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return left.lexically_normal() == right.lexically_normal();
}

bool sameReference(const ApplyTransactionReference& left, const ApplyTransactionReference& right) {
    return left.transactionId == right.transactionId && left.planDigest == right.planDigest;
}

std::filesystem::path rollbackUndoRoot(const ApplyPlan& request) {
    return util::defaultStagingRoot(request.installDir) / "backup" / "rollback" /
           util::pathFromUtf8(request.rollbackOf->transactionId);
}

Result<void> validateRollbackRequest(const ApplyPlan& request, const ApplyPlan& sourcePlan) {
    if (request.schemaVersion != 2 || request.intent != ApplyPlanIntent::Rollback || !request.rollbackOf ||
        !request.operations.empty() || !request.restartCommand.empty() || !request.toVersion.empty() ||
        !request.manifestSha256.empty()) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Rollback request shape is invalid"});
    }
    if (sourcePlan.intent != ApplyPlanIntent::Install || sourcePlan.rollbackOf) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Rollback source is not a completed install transaction"});
    }
    if (!samePath(request.installDir, sourcePlan.installDir) ||
        !samePath(request.stagingDir, sourcePlan.backupDir) ||
        !samePath(request.backupDir, rollbackUndoRoot(request))) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Rollback request roots are invalid"});
    }
    if ((!request.appId.empty() && request.appId != sourcePlan.appId) ||
        request.fromVersion != sourcePlan.toVersion || request.releaseId != sourcePlan.releaseId) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Rollback request metadata is invalid"});
    }
    return Result<void>::ok();
}

Result<void> validateCompletedRollbackPlan(const ApplyPlan& request, const ApplyPlan& completedPlan) {
    if (completedPlan.intent != ApplyPlanIntent::Rollback || !completedPlan.rollbackOf || !request.rollbackOf ||
        !sameReference(*completedPlan.rollbackOf, *request.rollbackOf) ||
        !samePath(completedPlan.installDir, request.installDir) ||
        !samePath(completedPlan.stagingDir, request.stagingDir) ||
        !samePath(completedPlan.backupDir, request.backupDir) ||
        !samePath(request.backupDir, rollbackUndoRoot(request)) ||
        (!request.appId.empty() && request.appId != completedPlan.appId) ||
        request.fromVersion != completedPlan.fromVersion || request.releaseId != completedPlan.releaseId ||
        !request.toVersion.empty() || !request.manifestSha256.empty()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Latest terminal transaction is not the requested rollback"});
    }
    return Result<void>::ok();
}

Result<void> startNewTransaction(const ApplyPlan& plan, IRootedDirectory& installRoot,
                                 ApplyExecutorDependencies& dependencies, const ApplyExecutionHooks& hooks) {
    auto valid = validateApplyPlanResources(plan, dependencies.limits);
    if (!valid) {
        return valid;
    }
    auto backupRoot = dependencies.fileSystem->openRoot(plan.backupDir, RootAccess::ReadWrite, true);
    if (!backupRoot) {
        return Result<void>::fail(backupRoot.error());
    }
    std::unique_ptr<IRootedDirectory> stagingRoot;
    if (hasReplaceOperation(plan)) {
        auto openedStaging = dependencies.fileSystem->openRoot(plan.stagingDir, RootAccess::ReadOnly, false);
        if (!openedStaging) {
            return Result<void>::fail(openedStaging.error());
        }
        stagingRoot = std::move(openedStaging.value());
    }
    auto planDigest = applyPlanDigest(plan);
    if (!planDigest) {
        return Result<void>::fail(planDigest.error());
    }
    auto transactionId = createApplyTransactionId(planDigest.value());
    if (!transactionId) {
        return Result<void>::fail(transactionId.error());
    }
    ApplyTransaction transaction(plan, installRoot, *backupRoot.value(), stagingRoot.get(),
                                 *dependencies.hashProvider, *dependencies.processLauncher, dependencies.limits, hooks,
                                 transactionId.value(), planDigest.value());
    return transaction.start();
}

Result<ApplyPlan> deriveRollbackPlan(const ApplyPlan& request, const ApplyPlan& sourcePlan,
                                     const std::vector<ApplyJournalOperation>& records) {
    if (records.size() != sourcePlan.operations.size()) {
        return Result<ApplyPlan>::fail(
            {ErrorCode::ApplyFailed, "Rollback source operation records are incomplete"});
    }
    std::set<std::string> targets;
    for (const auto& operation : sourcePlan.operations) {
        if (!targets.insert(operation.target).second) {
            return Result<ApplyPlan>::fail(
                {ErrorCode::SecurityPolicyViolation, "Rollback source contains duplicate managed targets"});
        }
    }

    ApplyPlan rollback;
    rollback.schemaVersion = 2;
    rollback.intent = ApplyPlanIntent::Rollback;
    rollback.rollbackOf = request.rollbackOf;
    rollback.appId = sourcePlan.appId;
    rollback.fromVersion = sourcePlan.toVersion;
    rollback.toVersion = sourcePlan.fromVersion;
    rollback.releaseId = sourcePlan.releaseId;
    rollback.manifestSha256 = sourcePlan.manifestSha256;
    rollback.installDir = sourcePlan.installDir;
    rollback.stagingDir = sourcePlan.backupDir;
    rollback.backupDir = request.backupDir;
    rollback.restartCommand = sourcePlan.restartCommand;

    for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
        const auto& record = *iterator;
        if (record.index >= sourcePlan.operations.size()) {
            return Result<ApplyPlan>::fail(
                {ErrorCode::ApplyFailed, "Rollback source operation index is invalid"});
        }
        const auto& sourceOperation = sourcePlan.operations[record.index];
        if (record.rollbackState == JournalRollbackState::NotRequired) {
            continue;
        }
        ApplyOperation inverse;
        inverse.target = sourceOperation.target;
        if (record.originalExists) {
            if (record.backupState != JournalBackupState::Durable) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ApplyFailed, "Rollback source has no durable original backup"});
            }
            inverse.type = ApplyOperationType::Replace;
            inverse.source = sourceOperation.target;
            inverse.sha256 = record.originalSha256;
            inverse.size = record.originalSize;
            if (record.originalPermissionsKnown) {
                inverse.permissions = record.originalPermissions;
            }
        } else {
            inverse.type = ApplyOperationType::Remove;
        }
        ApplyOperationPrecondition precondition;
        precondition.exists = sourceOperation.type == ApplyOperationType::Replace;
        if (precondition.exists) {
            precondition.size = sourceOperation.size;
            precondition.sha256 = sourceOperation.sha256;
            precondition.permissions = sourceOperation.permissions;
        }
        inverse.precondition = std::move(precondition);
        rollback.operations.push_back(std::move(inverse));
    }
    return Result<ApplyPlan>::ok(std::move(rollback));
}

Result<void> executeRollbackRequest(const ApplyPlan& request, IRootedDirectory& installRoot,
                                    const std::optional<ActiveTransaction>& terminal,
                                    ApplyExecutorDependencies& dependencies, const ApplyExecutionHooks& hooks) {
    if (request.schemaVersion != 2 || request.intent != ApplyPlanIntent::Rollback || !request.rollbackOf ||
        !request.operations.empty() || !request.restartCommand.empty()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Only an operation-free rollback request may be submitted"});
    }
    if (!terminal) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "No completed transaction is available to roll back"});
    }

    ApplyJournalStore journal(installRoot, dependencies.limits);
    auto terminalJson = journal.readPlanSnapshot(terminal->transactionId, terminal->planDigest);
    if (!terminalJson) {
        return Result<void>::fail(terminalJson.error());
    }
    auto terminalPlan = ApplyPlan::parse(terminalJson.value(), dependencies.limits);
    if (!terminalPlan) {
        return Result<void>::fail(terminalPlan.error());
    }
    auto summary = journal.readSummary(terminal->transactionId);
    if (!summary) {
        return Result<void>::fail(summary.error());
    }
    auto validSummary = validateRecoveredSummary(terminalPlan.value(), *terminal, summary.value());
    if (!validSummary) {
        return validSummary;
    }

    if (terminal->transactionId != request.rollbackOf->transactionId ||
        terminal->planDigest != request.rollbackOf->planDigest) {
        auto completed = validateCompletedRollbackPlan(request, terminalPlan.value());
        if (!completed) {
            return completed;
        }
        return replayTerminalTransaction(request, installRoot, *terminal, dependencies, hooks);
    }

    auto validRequest = validateRollbackRequest(request, terminalPlan.value());
    if (!validRequest) {
        return validRequest;
    }
    auto sourceBackup =
        dependencies.fileSystem->openRoot(terminalPlan.value().backupDir, RootAccess::ReadOnly, false);
    if (!sourceBackup) {
        return Result<void>::fail(sourceBackup.error());
    }
    ApplyTransaction sourceTransaction(terminalPlan.value(), installRoot, *sourceBackup.value(), nullptr,
                                       *dependencies.hashProvider, *dependencies.processLauncher, dependencies.limits,
                                       hooks, terminal->transactionId, terminal->planDigest, summary.value());
    auto records = sourceTransaction.rollbackSourceRecords();
    if (!records) {
        return Result<void>::fail(records.error());
    }
    auto rollback = deriveRollbackPlan(request, terminalPlan.value(), records.value());
    if (!rollback) {
        return Result<void>::fail(rollback.error());
    }
    return startNewTransaction(rollback.value(), installRoot, dependencies, hooks);
}

} // namespace

Result<void> waitForProcessExit(std::uint64_t pid, std::chrono::seconds timeout) noexcept {
    if (pid == 0) {
        return Result<void>::ok();
    }

#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        return Result<void>::ok();
    }
    const DWORD waitMs = static_cast<DWORD>(timeout.count() * 1000);
    const DWORD result = WaitForSingleObject(handle, waitMs);
    CloseHandle(handle);
    if (result == WAIT_TIMEOUT) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Timed out waiting for main process exit"});
    }
    return Result<void>::ok();
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            return Result<void>::ok();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return Result<void>::fail({ErrorCode::ApplyFailed, "Timed out waiting for main process exit"});
#endif
}

Result<void> executeApplyPlanWithDependencies(const ApplyPlan& plan, ApplyExecutorDependencies dependencies,
                                              const ApplyExecutionHooks& hooks) noexcept {
    try {
        auto valid = validateApplyPlanResources(plan, dependencies.limits);
        if (!valid) {
            return valid;
        }
        if (!dependencies.fileSystem || !dependencies.hashProvider || !dependencies.processLauncher) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "Apply executor dependencies are incomplete"});
        }

        auto installRoot = dependencies.fileSystem->openRoot(plan.installDir, RootAccess::ReadWrite, true,
                                                             RootedDirectoryCreationMode::InstalledContent);
        if (!installRoot) {
            return Result<void>::fail(installRoot.error());
        }
        auto lock = installRoot.value()->acquireExclusiveLock(".autoupdater/update.lock");
        if (!lock) {
            return Result<void>::fail(lock.error());
        }

        ApplyJournalStore journal(*installRoot.value(), dependencies.limits);
        auto active = journal.loadActive();
        if (!active) {
            return Result<void>::fail(active.error());
        }
        if (active.value()) {
            auto recovered = recoverActiveTransaction(plan, *installRoot.value(), *active.value(), dependencies, hooks);
            if (plan.intent != ApplyPlanIntent::Rollback) {
                return recovered;
            }

            // A detached public rollback has no caller that can reliably retry
            // it. Continue only after the durable active pointer is gone; the
            // terminal validation below then proves whether the recovered
            // state is still the requested source (or the requested rollback
            // already completed). If recovery launched an unrelated update,
            // its new terminal receipt makes the request fail closed.
            auto remainingActive = journal.loadActive();
            if (!remainingActive) {
                return Result<void>::fail(remainingActive.error());
            }
            if (remainingActive.value()) {
                return recovered ? Result<void>::fail(
                                       {ErrorCode::ApplyFailed, "Recovered transaction remains active"})
                                 : recovered;
            }
            auto recoveredTerminal = journal.loadTerminal();
            if (!recoveredTerminal) {
                return Result<void>::fail(recoveredTerminal.error());
            }
            return executeRollbackRequest(plan, *installRoot.value(), recoveredTerminal.value(), dependencies, hooks);
        }

        auto terminal = journal.loadTerminal();
        if (!terminal) {
            return Result<void>::fail(terminal.error());
        }
        if (plan.intent == ApplyPlanIntent::Rollback) {
            return executeRollbackRequest(plan, *installRoot.value(), terminal.value(), dependencies, hooks);
        }

        auto planDigest = applyPlanDigest(plan);
        if (!planDigest) {
            return Result<void>::fail(planDigest.error());
        }
        if (terminal.value() && terminal.value()->planDigest == planDigest.value()) {
            return replayTerminalTransaction(plan, *installRoot.value(), *terminal.value(), dependencies, hooks);
        }
        return startNewTransaction(plan, *installRoot.value(), dependencies, hooks);
    } catch (...) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Unexpected apply failure"});
    }
}

Result<void> executeApplyPlan(const ApplyPlan& plan) noexcept {
    try {
        ApplyExecutorDependencies dependencies;
        dependencies.fileSystem = createDefaultFileSystem();
        dependencies.hashProvider = createDefaultHashProvider();
        dependencies.processLauncher = createDefaultProcessLauncher();
        return executeApplyPlanWithDependencies(plan, std::move(dependencies));
    } catch (...) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Failed to create default apply dependencies"});
    }
}

} // namespace autoupdater::updater
