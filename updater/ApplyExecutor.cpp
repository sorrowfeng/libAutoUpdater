#include "ApplyExecutor.h"

#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "util/PathUtil.h"

#include <chrono>
#include <fstream>
#include <thread>
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
};

struct LockCleanup {
    std::filesystem::path path;
    ~LockCleanup() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

Result<void> createDirectories(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
    }
    return Result<void>::ok();
}

Result<void> copyReplacing(const std::filesystem::path& from, const std::filesystem::path& to) {
    auto dirs = createDirectories(to.parent_path());
    if (!dirs) {
        return dirs;
    }
    std::error_code ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
    }
#ifndef _WIN32
    std::filesystem::permissions(to, std::filesystem::status(from, ec).permissions(), ec);
#endif
    return Result<void>::ok();
}

std::filesystem::path joinChecked(const std::filesystem::path& root, const std::string& relative) {
    auto joined = util::safeJoin(root, relative);
    if (!joined) {
        return {};
    }
    return joined.value();
}

Result<void> writeJournal(const ApplyPlan& plan, const std::vector<AppliedOperation>& applied, const std::string& state) {
    try {
        const auto journalDir = plan.installDir / ".autoupdater" / "journal";
        std::error_code ec;
        std::filesystem::create_directories(journalDir, ec);
        const auto journalPath = journalDir / (plan.releaseId.empty() ? "transaction.json" : plan.releaseId + ".json");
        std::ofstream output(journalPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write transaction journal"});
        }
        output << "{\n";
        output << "  \"state\": \"" << state << "\",\n";
        output << "  \"toVersion\": \"" << plan.toVersion << "\",\n";
        output << "  \"appliedCount\": " << applied.size() << "\n";
        output << "}\n";
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write transaction journal"});
    }
}

void rollback(const ApplyPlan& plan, const std::vector<AppliedOperation>& applied) {
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        const auto target = joinChecked(plan.installDir, it->operation.target);
        const auto backup = joinChecked(plan.backupDir, it->operation.target);
        if (target.empty() || backup.empty()) {
            continue;
        }
        std::error_code ec;
        if (it->backupExists && std::filesystem::exists(backup, ec)) {
            std::filesystem::create_directories(target.parent_path(), ec);
            std::filesystem::copy_file(backup, target, std::filesystem::copy_options::overwrite_existing, ec);
        } else {
            std::filesystem::remove(target, ec);
        }
    }
}

Result<void> verifyReplace(const ApplyPlan& plan, const ApplyOperation& operation, IHashProvider& hashProvider) {
    const auto target = joinChecked(plan.installDir, operation.target);
    if (target.empty()) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, "Invalid apply target"});
    }
    auto hash = hashProvider.sha256File(target);
    if (!hash) {
        return Result<void>::fail(hash.error());
    }
    if (hash.value() != operation.sha256) {
        return Result<void>::fail({ErrorCode::HashMismatch, "Installed file SHA-256 mismatch: " + operation.target});
    }
    return Result<void>::ok();
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

Result<void> executeApplyPlan(const ApplyPlan& plan) noexcept {
    std::vector<AppliedOperation> applied;
    try {
        auto lockDir = plan.installDir / ".autoupdater";
        auto dirs = createDirectories(lockDir);
        if (!dirs) {
            return dirs;
        }
        auto lockPath = lockDir / "update.lock";
        if (std::filesystem::exists(lockPath)) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Another update appears to be running"});
        }
        {
            std::ofstream lock(lockPath, std::ios::binary | std::ios::trunc);
            lock << "locked\n";
        }
        LockCleanup lockCleanup{lockPath};

        auto backupDirs = createDirectories(plan.backupDir);
        if (!backupDirs) {
            return backupDirs;
        }
        auto hashProvider = createDefaultHashProvider();

        for (const auto& operation : plan.operations) {
            const auto target = joinChecked(plan.installDir, operation.target);
            if (target.empty()) {
                rollback(plan, applied);
                return Result<void>::fail({ErrorCode::PathTraversalRejected, "Invalid apply target"});
            }
            const auto backup = joinChecked(plan.backupDir, operation.target);
            if (backup.empty()) {
                rollback(plan, applied);
                return Result<void>::fail({ErrorCode::PathTraversalRejected, "Invalid backup target"});
            }

            AppliedOperation appliedOperation;
            appliedOperation.operation = operation;
            std::error_code ec;
            appliedOperation.backupExists = std::filesystem::exists(target, ec);
            if (appliedOperation.backupExists) {
                auto copied = copyReplacing(target, backup);
                if (!copied) {
                    rollback(plan, applied);
                    return copied;
                }
            }
            applied.push_back(std::move(appliedOperation));

            if (operation.type == ApplyOperationType::Replace) {
                const auto source = joinChecked(plan.stagingDir, operation.source);
                if (source.empty()) {
                    rollback(plan, applied);
                    return Result<void>::fail({ErrorCode::PathTraversalRejected, "Invalid apply source"});
                }
                auto copied = copyReplacing(source, target);
                if (!copied) {
                    rollback(plan, applied);
                    return copied;
                }
                auto verified = verifyReplace(plan, operation, *hashProvider);
                if (!verified) {
                    rollback(plan, applied);
                    return verified;
                }
            } else if (operation.type == ApplyOperationType::Remove) {
                std::filesystem::remove(target, ec);
                if (ec) {
                    rollback(plan, applied);
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }

            writeJournal(plan, applied, "applying");
        }

        writeJournal(plan, applied, "complete");
        return restart(plan);
    } catch (...) {
        rollback(plan, applied);
        return Result<void>::fail({ErrorCode::ApplyFailed, "Unexpected apply failure"});
    }
}

} // namespace autoupdater::updater
