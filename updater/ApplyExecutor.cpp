#include "ApplyExecutor.h"

#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"
#include "util/Json.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <sstream>
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

struct AppliedOperation {
    ApplyOperation operation;
    bool backupExists = false;
    RootedEntryExpectation expectedCurrent = RootedEntryExpectation::missing();
};

Result<void> consumePlanTextBudget(const std::string& value, const ResourceLimits& limits,
                                   std::uint64_t& consumedBytes) {
    if (value.size() > limits.json.maxStringBytes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan string exceeds its byte limit"});
    }
    if (value.size() > limits.maxApplyPlanBytes - consumedBytes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan exceeds its byte limit"});
    }
    consumedBytes += static_cast<std::uint64_t>(value.size());
    return Result<void>::ok();
}

Result<void> validateApplyPlanResources(const ApplyPlan& plan, const ResourceLimits& limits) {
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
    for (const auto& argument : plan.restartCommand) {
        auto valid = consumePlanTextBudget(argument, limits, consumedBytes);
        if (!valid) {
            return valid;
        }
    }

    std::uint64_t totalArtifactBytes = 0;
    for (const auto& operation : plan.operations) {
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
        if (operation.type == ApplyOperationType::Replace) {
            if (operation.size > limits.maxTotalArtifactBytes - totalArtifactBytes) {
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
    // ApplyPlan does not yet carry a signed mode field. New files therefore
    // receive a non-executable safe default instead of inheriting mutable
    // staging permissions.
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
           std::filesystem::perms::group_read | std::filesystem::perms::others_read;
}

Result<RootedEntryExpectation> expectationFor(const RootedOpenResult& opened) {
    if (!opened.exists()) {
        return Result<RootedEntryExpectation>::ok(RootedEntryExpectation::missing());
    }
    auto metadata = opened.file->metadata();
    if (!metadata) {
        return Result<RootedEntryExpectation>::fail(metadata.error());
    }
    return Result<RootedEntryExpectation>::ok(RootedEntryExpectation::matching(metadata.value()));
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

Result<void> verifySameContent(IRootedFile& source, IRootedFile& destination, IHashProvider& hashProvider) {
    auto sourceMetadata = source.metadata();
    if (!sourceMetadata) {
        return Result<void>::fail(sourceMetadata.error());
    }
    auto destinationMetadata = destination.metadata();
    if (!destinationMetadata) {
        return Result<void>::fail(destinationMetadata.error());
    }
    if (sourceMetadata.value().size != destinationMetadata.value().size) {
        return Result<void>::fail({ErrorCode::HashMismatch, "Rooted file copy size mismatch"});
    }
    auto sourceHash = hashProvider.sha256Stream(source);
    if (!sourceHash) {
        return Result<void>::fail(sourceHash.error());
    }
    auto destinationHash = hashProvider.sha256Stream(destination);
    if (!destinationHash) {
        return Result<void>::fail(destinationHash.error());
    }
    if (sourceHash.value() != destinationHash.value()) {
        return Result<void>::fail({ErrorCode::HashMismatch, "Rooted file copy SHA-256 mismatch"});
    }
    return Result<void>::ok();
}

Result<void> verifyExpectedContent(IRootedFile& file, const ApplyOperation& operation, IHashProvider& hashProvider) {
    auto metadata = file.metadata();
    if (!metadata) {
        return Result<void>::fail(metadata.error());
    }
    if (metadata.value().size != operation.size) {
        return Result<void>::fail({ErrorCode::HashMismatch, "Installed file size mismatch: " + operation.target});
    }
    auto hash = hashProvider.sha256Stream(file);
    if (!hash) {
        return Result<void>::fail(hash.error());
    }
    if (hash.value() != operation.sha256) {
        return Result<void>::fail({ErrorCode::HashMismatch, "Installed file SHA-256 mismatch: " + operation.target});
    }
    return Result<void>::ok();
}

Result<void> writeJournal(const ApplyPlan& plan, IRootedDirectory& installRoot,
                          const std::vector<AppliedOperation>& applied, const std::string& state) {
    try {
        auto fileName = transactionJournalFileName(plan);
        if (!fileName) {
            return Result<void>::fail(fileName.error());
        }
        const auto relativePath = ".autoupdater/journal/" + fileName.value();
        auto existing = installRoot.openRegularFile(relativePath, RootedFileOpenMode::ReadOnly);
        if (!existing) {
            return Result<void>::fail(existing.error());
        }
        auto expectation = expectationFor(existing.value());
        if (!expectation) {
            return Result<void>::fail(expectation.error());
        }
        existing.value().file.reset();

        std::ostringstream output;
        output << "{\n";
        output << "  \"state\": \"" << util::jsonEscape(state) << "\",\n";
        output << "  \"toVersion\": \"" << util::jsonEscape(plan.toVersion) << "\",\n";
        output << "  \"appliedCount\": " << applied.size() << "\n";
        output << "}\n";
        const auto contents = output.str();

        auto temporary = installRoot.createAtomicReplacement(relativePath);
        if (!temporary) {
            return Result<void>::fail(temporary.error());
        }
        auto written = temporary.value()->file().write(contents.data(), contents.size());
        if (!written) {
            return written;
        }
        auto flushed = temporary.value()->file().flush();
        if (!flushed) {
            return flushed;
        }
        return temporary.value()->commit(expectation.value());
    } catch (...) {
        return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write transaction journal"});
    }
}

Result<void> createBackup(IRootedDirectory& backupRoot, const std::string& target, IRootedFile& source,
                          IHashProvider& hashProvider) {
    auto existing = backupRoot.openRegularFile(target, RootedFileOpenMode::ReadOnly);
    if (!existing) {
        return Result<void>::fail(existing.error());
    }
    if (existing.value().exists()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Refusing to overwrite an existing update backup"});
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
    auto flushed = temporary.value()->file().flush();
    if (!flushed) {
        return flushed;
    }
    auto verified = verifySameContent(source, temporary.value()->file(), hashProvider);
    if (!verified) {
        return verified;
    }
    return temporary.value()->commit(RootedEntryExpectation::missing());
}

Result<void> rollback(IRootedDirectory& installRoot, IRootedDirectory& backupRoot,
                      const std::vector<AppliedOperation>& applied, IHashProvider& hashProvider) {
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        if (it->backupExists) {
            auto backup = backupRoot.openRegularFile(it->operation.target, RootedFileOpenMode::ReadOnly);
            if (!backup) {
                return Result<void>::fail(backup.error());
            }
            if (!backup.value().exists()) {
                return Result<void>::fail({ErrorCode::ApplyFailed, "Required rollback backup is missing"});
            }
            auto temporary = installRoot.createAtomicReplacement(it->operation.target,
                                                                 RootedDirectoryCreationMode::InstalledContent);
            if (!temporary) {
                return Result<void>::fail(temporary.error());
            }
            auto copied = copyRootedFile(*backup.value().file, temporary.value()->file());
            if (!copied) {
                return copied;
            }
            auto permissions = copyPermissions(*backup.value().file, temporary.value()->file());
            if (!permissions) {
                return permissions;
            }
            auto flushed = temporary.value()->file().flush();
            if (!flushed) {
                return flushed;
            }
            auto verified = verifySameContent(*backup.value().file, temporary.value()->file(), hashProvider);
            if (!verified) {
                return verified;
            }
            auto committed = temporary.value()->commit(it->expectedCurrent);
            if (!committed) {
                return committed;
            }
        } else if (it->expectedCurrent.kind == RootedEntryExpectationKind::Identity) {
            auto removed = installRoot.removeRegularFile(it->operation.target, it->expectedCurrent);
            if (!removed) {
                return removed;
            }
        }
    }
    return Result<void>::ok();
}

Result<void> failAfterRollback(const Error& original, IRootedDirectory& installRoot, IRootedDirectory& backupRoot,
                               const std::vector<AppliedOperation>& applied, IHashProvider& hashProvider) {
    auto rolledBack = rollback(installRoot, backupRoot, applied, hashProvider);
    if (!rolledBack) {
        return Result<void>::fail(
            {ErrorCode::ApplyFailed, original.message + "; rollback failed: " + rolledBack.error().message});
    }
    return Result<void>::fail(original);
}

Result<void> restart(const ApplyPlan& plan) {
    if (plan.restartCommand.empty()) {
        return Result<void>::ok();
    }
    auto launcher = createDefaultProcessLauncher();
    ProcessLaunchRequest request;
    request.executable = plan.restartCommand.front();
    request.workingDirectory = plan.installDir;
    request.detached = true;
    for (std::size_t i = 1; i < plan.restartCommand.size(); ++i) {
        request.arguments.push_back(plan.restartCommand[i]);
    }
    return launcher->launch(request);
}

bool hasReplaceOperation(const ApplyPlan& plan) {
    for (const auto& operation : plan.operations) {
        if (operation.type == ApplyOperationType::Replace) {
            return true;
        }
    }
    return false;
}

} // namespace

Result<std::string> transactionJournalFileName(const ApplyPlan& plan) noexcept {
    try {
        auto valid = validateApplyPlanResources(plan, ResourceLimits{});
        if (!valid) {
            return Result<std::string>::fail(valid.error());
        }
        return Result<std::string>::ok(util::sha256Bytes(plan.toJson()) + ".json");
    } catch (...) {
        return Result<std::string>::fail({ErrorCode::FileSystemError, "Failed to create transaction identifier"});
    }
}

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

Result<void> executeApplyPlan(const ApplyPlan& plan) noexcept {
    std::vector<AppliedOperation> applied;
    try {
        auto valid = validateApplyPlanResources(plan, ResourceLimits{});
        if (!valid) {
            return valid;
        }
        auto fileSystem = createDefaultFileSystem();
        auto hashProvider = createDefaultHashProvider();

        auto installRoot = fileSystem->openRoot(plan.installDir, RootAccess::ReadWrite, true,
                                                RootedDirectoryCreationMode::InstalledContent);
        if (!installRoot) {
            return Result<void>::fail(installRoot.error());
        }
        auto backupRoot = fileSystem->openRoot(plan.backupDir, RootAccess::ReadWrite, true);
        if (!backupRoot) {
            return Result<void>::fail(backupRoot.error());
        }

        std::unique_ptr<IRootedDirectory> stagingRoot;
        if (hasReplaceOperation(plan)) {
            auto openedStaging = fileSystem->openRoot(plan.stagingDir, RootAccess::ReadOnly, false);
            if (!openedStaging) {
                return Result<void>::fail(openedStaging.error());
            }
            stagingRoot = std::move(openedStaging.value());
        }

        auto lock = installRoot.value()->acquireExclusiveLock(".autoupdater/update.lock");
        if (!lock) {
            return Result<void>::fail(lock.error());
        }

        auto initialJournal = writeJournal(plan, *installRoot.value(), applied, "applying");
        if (!initialJournal) {
            return initialJournal;
        }

        for (const auto& operation : plan.operations) {
            auto target = installRoot.value()->openRegularFile(operation.target, RootedFileOpenMode::ReadOnly);
            if (!target) {
                return failAfterRollback(target.error(), *installRoot.value(), *backupRoot.value(), applied,
                                         *hashProvider);
            }
            auto targetExpectation = expectationFor(target.value());
            if (!targetExpectation) {
                return failAfterRollback(targetExpectation.error(), *installRoot.value(), *backupRoot.value(), applied,
                                         *hashProvider);
            }
            std::optional<std::filesystem::perms> targetPermissions;
            if (target.value().exists()) {
                auto targetMetadata = target.value().file->metadata();
                if (!targetMetadata) {
                    return failAfterRollback(targetMetadata.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                targetPermissions = targetMetadata.value().permissions;
            }

            AppliedOperation appliedOperation;
            appliedOperation.operation = operation;
            appliedOperation.backupExists = target.value().exists();
            appliedOperation.expectedCurrent = targetExpectation.value();

            if (target.value().exists()) {
                auto backedUp =
                    createBackup(*backupRoot.value(), operation.target, *target.value().file, *hashProvider);
                if (!backedUp) {
                    return failAfterRollback(backedUp.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
            }
            target.value().file.reset();
            applied.push_back(std::move(appliedOperation));

            if (operation.type == ApplyOperationType::Replace) {
                auto source = stagingRoot->openRegularFile(operation.source, RootedFileOpenMode::ReadOnly);
                if (!source) {
                    return failAfterRollback(source.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                if (!source.value().exists()) {
                    return failAfterRollback({ErrorCode::FileSystemError, "Staged update file is missing"},
                                             *installRoot.value(), *backupRoot.value(), applied, *hashProvider);
                }
                auto sourceVerified = verifyExpectedContent(*source.value().file, operation, *hashProvider);
                if (!sourceVerified) {
                    return failAfterRollback(sourceVerified.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }

                auto temporary = installRoot.value()->createAtomicReplacement(
                    operation.target, RootedDirectoryCreationMode::InstalledContent);
                if (!temporary) {
                    return failAfterRollback(temporary.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                auto copied = copyRootedFile(*source.value().file, temporary.value()->file());
                if (!copied) {
                    return failAfterRollback(copied.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                auto permissions =
                    targetPermissions
                        ? temporary.value()->file().setPermissions(sanitizedFilePermissions(*targetPermissions))
                        : temporary.value()->file().setPermissions(defaultInstalledFilePermissions());
                if (!permissions) {
                    return failAfterRollback(permissions.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                auto flushed = temporary.value()->file().flush();
                if (!flushed) {
                    return failAfterRollback(flushed.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                auto verified = verifyExpectedContent(temporary.value()->file(), operation, *hashProvider);
                if (!verified) {
                    return failAfterRollback(verified.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                auto installedMetadata = temporary.value()->file().metadata();
                if (!installedMetadata) {
                    return failAfterRollback(installedMetadata.error(), *installRoot.value(), *backupRoot.value(),
                                             applied, *hashProvider);
                }
                auto committed = temporary.value()->commit(applied.back().expectedCurrent);
                if (!committed) {
                    return failAfterRollback(committed.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                applied.back().expectedCurrent = RootedEntryExpectation::matching(installedMetadata.value());
                temporary.value().reset();
            } else if (operation.type == ApplyOperationType::Remove &&
                       targetExpectation.value().kind == RootedEntryExpectationKind::Identity) {
                auto removed = installRoot.value()->removeRegularFile(operation.target, applied.back().expectedCurrent);
                if (!removed) {
                    return failAfterRollback(removed.error(), *installRoot.value(), *backupRoot.value(), applied,
                                             *hashProvider);
                }
                applied.back().expectedCurrent = RootedEntryExpectation::missing();
            }

            auto journal = writeJournal(plan, *installRoot.value(), applied, "applying");
            if (!journal) {
                return failAfterRollback(journal.error(), *installRoot.value(), *backupRoot.value(), applied,
                                         *hashProvider);
            }
        }

        auto completedJournal = writeJournal(plan, *installRoot.value(), applied, "complete");
        if (!completedJournal) {
            return failAfterRollback(completedJournal.error(), *installRoot.value(), *backupRoot.value(), applied,
                                     *hashProvider);
        }
        return restart(plan);
    } catch (...) {
        return Result<void>::fail({ErrorCode::ApplyFailed, "Unexpected apply failure"});
    }
}

} // namespace autoupdater::updater
