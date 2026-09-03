#include "ApplyExecutor.h"

#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "util/Json.h"
#include "util/PathUtil.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <optional>
#include <set>
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
};

struct LockCleanup {
    std::filesystem::path path;
    LockCleanup() = default;
    explicit LockCleanup(std::filesystem::path value) : path(std::move(value)) {}
    LockCleanup(const LockCleanup&) = delete;
    LockCleanup& operator=(const LockCleanup&) = delete;
    LockCleanup(LockCleanup&& other) noexcept : path(std::move(other.path)) {
        other.path.clear();
    }
    LockCleanup& operator=(LockCleanup&& other) noexcept {
        if (this != &other) {
            path = std::move(other.path);
            other.path.clear();
        }
        return *this;
    }
    ~LockCleanup() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
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

std::uint64_t selfProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

bool processAlive(std::uint64_t pid) {
    if (pid == 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        // ERROR_ACCESS_DENIED still means the process exists.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD result = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return result == WAIT_TIMEOUT;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

// Detects symlinks in the part of `full` that lies below `root`. Writing
// through a symlinked target would escape the managed directory, so apply and
// rollback paths are rejected when any existing component is a symlink.
bool containsSymlinkBelow(const std::filesystem::path& root, const std::filesystem::path& full) {
    const auto relative = full.lexically_relative(root);
    if (relative.empty()) {
        return false;
    }
    std::filesystem::path current = root;
    for (const auto& part : relative) {
        current /= part;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec || !std::filesystem::exists(status)) {
            return false; // the remaining path does not exist yet
        }
        if (status.type() == std::filesystem::file_type::symlink) {
            return true;
        }
    }
    return false;
}

Result<std::filesystem::path> joinChecked(const std::filesystem::path& root, const std::string& relative,
                                          const char* what) {
    auto joined = util::safeJoin(root, relative);
    if (!joined) {
        return Result<std::filesystem::path>::fail(joined.error());
    }
    if (containsSymlinkBelow(root, joined.value())) {
        return Result<std::filesystem::path>::fail(
            {ErrorCode::PathTraversalRejected, std::string("Symlink rejected in ") + what + ": " + relative});
    }
    return joined;
}

// Copies `from` to `to` keeping the source permissions. Used for backups and
// rollback restores, where the backup holds the original file metadata.
Result<void> copyWithSourcePermissions(const std::filesystem::path& from, const std::filesystem::path& to) {
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
    const auto sourceStatus = std::filesystem::status(from, ec);
    if (!ec) {
        std::filesystem::permissions(to, sourceStatus.permissions(), ec);
    }
#endif
    return Result<void>::ok();
}

// Replaces `to` with the content of `from` atomically: the content is first
// materialized next to the destination and then renamed over it, so a crash
// can never leave a truncated target file behind.
Result<void> replaceFileAtomically(const std::filesystem::path& from, const std::filesystem::path& to) {
    auto dirs = createDirectories(to.parent_path());
    if (!dirs) {
        return dirs;
    }

#ifndef _WIN32
    std::optional<std::filesystem::perms> targetPerms;
    {
        std::error_code ec;
        const auto status = std::filesystem::status(to, ec);
        if (!ec && std::filesystem::exists(status)) {
            targetPerms = status.permissions();
        }
    }
#endif

    auto temp = to;
    temp += ".autoupdater-new";
    {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
    }
    std::error_code ec;
    std::filesystem::copy_file(from, temp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
    }
#ifndef _WIN32
    {
        const auto sourceStatus = std::filesystem::status(from, ec);
        const auto perms = targetPerms.has_value() ? *targetPerms : sourceStatus.permissions();
        std::filesystem::permissions(temp, perms, ec);
        ec.clear();
    }
#endif

#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code removeEc;
        std::filesystem::remove(temp, removeEc);
        return Result<void>::fail({ErrorCode::FileSystemError, "Failed to replace target file"});
    }
#else
    std::filesystem::rename(temp, to, ec);
    if (ec) {
        std::error_code removeEc;
        std::filesystem::remove(temp, removeEc);
        return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
    }
#endif
    return Result<void>::ok();
}

std::filesystem::path journalPathFor(const ApplyPlan& plan) {
    const auto journalDir = plan.installDir / ".autoupdater" / "journal";
    return journalDir / (plan.releaseId.empty() ? "transaction.json" : plan.releaseId + ".json");
}

Result<void> writeJournal(const ApplyPlan& plan, const std::vector<AppliedOperation>& applied,
                          const std::string& state) {
    try {
        const auto journalPath = journalPathFor(plan);
        std::error_code ec;
        std::filesystem::create_directories(journalPath.parent_path(), ec);

        util::Json::Array appliedItems;
        for (const auto& item : applied) {
            util::Json::Object entry;
            entry.emplace("target", item.operation.target);
            entry.emplace("backupExists", item.backupExists);
            appliedItems.emplace_back(std::move(entry));
        }
        util::Json::Object root;
        root.emplace("state", state);
        root.emplace("toVersion", plan.toVersion);
        root.emplace("backupDir", util::pathToUtf8(plan.backupDir));
        root.emplace("applied", std::move(appliedItems));

        std::ofstream output(journalPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write transaction journal"});
        }
        output << util::Json(std::move(root)).stringify(2);
        output.flush();
        if (!output.good()) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write transaction journal"});
        }
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write transaction journal"});
    }
}

// Rolls back the given applied operations in reverse order. Individual
// failures are accumulated: rollback always attempts every operation so the
// install directory is left as close to the original state as possible.
Result<void> rollback(const ApplyPlan& plan, const std::vector<AppliedOperation>& applied) {
    Result<void> firstError = Result<void>::ok();
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        const auto target = joinChecked(plan.installDir, it->operation.target, "rollback target");
        const auto backup = joinChecked(plan.backupDir, it->operation.target, "rollback backup");
        if (!target || !backup) {
            if (firstError) {
                firstError = Result<void>::fail(target ? backup.error() : target.error());
            }
            continue;
        }
        std::error_code ec;
        if (it->backupExists && std::filesystem::exists(backup.value(), ec)) {
            auto restored = copyWithSourcePermissions(backup.value(), target.value());
            if (!restored && firstError) {
                firstError = restored;
            }
        } else {
            std::filesystem::remove(target.value(), ec);
            if (ec && firstError) {
                firstError = Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
        }
    }
    return firstError;
}

Result<void> verifyReplace(const ApplyPlan& plan, const ApplyOperation& operation, IHashProvider& hashProvider) {
    const auto target = joinChecked(plan.installDir, operation.target, "verify target");
    if (!target) {
        return Result<void>::fail(target.error());
    }
    auto hash = hashProvider.sha256File(target.value());
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
    // restartCommand 是 UTF-8 字符串;转 path 必须走 pathFromUtf8,
    // 否则 Windows 按 ANSI 码页解码中文路径导致重启 CreateProcessW 失败
    request.executable = util::pathFromUtf8(plan.restartCommand.front());
    request.workingDirectory = plan.installDir;
    request.detached = true;
    for (std::size_t i = 1; i < plan.restartCommand.size(); ++i) {
        request.arguments.push_back(plan.restartCommand[i]);
    }
    auto launched = launcher->launch(request);
    if (!launched) {
        return Result<void>::fail(
            {ErrorCode::ApplyLaunchFailed, "Update applied but restart failed: " + launched.error().message});
    }
    return launched;
}

// Replays the rollback described by a journal left in the "applying" state by
// a crashed or killed updater process.
void recoverJournal(const std::filesystem::path& journalPath) {
    std::ifstream input(journalPath, std::ios::binary);
    if (!input) {
        return;
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    auto json = util::Json::parse(stream.str());
    if (!json || !json.value().isObject()) {
        return;
    }
    const auto& root = json.value();
    const auto* state = root.get("state");
    if (!state || !state->isString() || state->asString() != "applying") {
        return;
    }
    const auto* backupDirText = root.get("backupDir");
    if (!backupDirText || !backupDirText->isString() || backupDirText->asString().empty()) {
        return;
    }

    ApplyPlan plan;
    plan.installDir = journalPath.parent_path().parent_path().parent_path(); // <install>/.autoupdater/journal/x.json
    plan.backupDir = util::pathFromUtf8(backupDirText->asString());

    std::vector<AppliedOperation> applied;
    const auto* items = root.get("applied");
    if (items && items->isArray()) {
        for (const auto& entry : items->asArray()) {
            const auto* target = entry.get("target");
            if (!target || !target->isString()) {
                continue;
            }
            AppliedOperation op;
            op.operation.type = ApplyOperationType::Replace;
            op.operation.target = target->asString();
            const auto* backupExists = entry.get("backupExists");
            op.backupExists = backupExists && backupExists->isBool() && backupExists->asBool();
            applied.push_back(std::move(op));
        }
    }

    (void)rollback(plan, applied);

    // Mark the journal so recovery is not repeated on the next run.
    util::Json::Object updated = root.asObject();
    updated["state"] = util::Json(std::string("rolled-back"));
    std::ofstream output(journalPath, std::ios::binary | std::ios::trunc);
    if (output) {
        output << util::Json(std::move(updated)).stringify(2);
    }
}

void recoverInterruptedTransactions(const std::filesystem::path& installDir) {
    const auto journalDir = installDir / ".autoupdater" / "journal";
    std::error_code ec;
    if (!std::filesystem::is_directory(journalDir, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(journalDir, ec)) {
        if (ec) {
            return;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        try {
            recoverJournal(entry.path());
        } catch (...) {
            // Recovery is best-effort; a broken journal must not block updates.
        }
    }
}

Result<LockCleanup> acquireUpdateLock(const std::filesystem::path& installDir) {
    const auto stateDir = installDir / ".autoupdater";
    auto dirs = createDirectories(stateDir);
    if (!dirs) {
        return Result<LockCleanup>::fail(dirs.error());
    }

    const auto lockPath = stateDir / "update.lock";
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::error_code ec;
        const bool created = std::filesystem::create_directory(lockPath, ec);
        if (ec) {
            return Result<LockCleanup>::fail({ErrorCode::FileSystemError, ec.message()});
        }
        if (created) {
            std::ofstream metadata(lockPath / "owner.txt", std::ios::binary | std::ios::trunc);
            metadata << selfProcessId() << "\n" << std::time(nullptr) << "\n";
            return Result<LockCleanup>::ok(LockCleanup(lockPath));
        }

        // The lock already exists. Reclaim it when the recorded owner process
        // is gone (stale lock left by a crashed updater).
        bool stale = false;
        {
            // Keep the stream scope tight: an open owner.txt would block the
            // remove_all below on Windows.
            std::ifstream owner(lockPath / "owner.txt", std::ios::binary);
            std::string pidText;
            if (owner && std::getline(owner, pidText)) {
                char* end = nullptr;
                const auto pid = std::strtoull(pidText.c_str(), &end, 10);
                stale = end != pidText.c_str() && pid > 0 && !processAlive(pid);
            }
        }
        if (!stale) {
            return Result<LockCleanup>::fail({ErrorCode::ApplyFailed, "Another update appears to be running"});
        }
        std::error_code removeEc;
        std::filesystem::remove_all(lockPath, removeEc);
        if (removeEc) {
            return Result<LockCleanup>::fail({ErrorCode::FileSystemError, removeEc.message()});
        }
    }
    return Result<LockCleanup>::fail({ErrorCode::ApplyFailed, "Another update appears to be running"});
}

} // namespace

Result<void> waitForProcessExit(std::uint64_t pid, std::chrono::seconds timeout) noexcept {
    if (pid == 0) {
        return Result<void>::fail({ErrorCode::InvalidConfig, "Main process id is required"});
    }

#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (!handle) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) {
                return Result<void>::ok(); // no such process anymore
            }
            // e.g. ERROR_ACCESS_DENIED: the process exists but we cannot wait
            // on it; keep polling until it disappears or we time out.
        } else {
            const DWORD waitMs = [&deadline] {
                const auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::milliseconds(0)) {
                    return static_cast<DWORD>(0);
                }
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                return static_cast<DWORD>(ms > 0x7FFFFFFF ? 0x7FFFFFFF : ms);
            }();
            const DWORD result = WaitForSingleObject(handle, waitMs);
            CloseHandle(handle);
            if (result == WAIT_OBJECT_0) {
                return Result<void>::ok();
            }
            if (result != WAIT_TIMEOUT) {
                return Result<void>::fail({ErrorCode::ApplyFailed, "Failed while waiting for main process exit"});
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Timed out waiting for main process exit"});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            return Result<void>::ok();
        }
        // EPERM means the process exists but is owned by someone else.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return Result<void>::fail({ErrorCode::ApplyFailed, "Timed out waiting for main process exit"});
#endif
}

Result<void> executeApplyPlan(const ApplyPlan& plan) noexcept {
    std::vector<AppliedOperation> applied;
    try {
        auto lock = acquireUpdateLock(plan.installDir);
        if (!lock) {
            return Result<void>::fail(lock.error());
        }
        auto lockCleanup = std::move(lock.value());

        // Recover from a previously interrupted transaction before starting a
        // new one, while holding the update lock.
        recoverInterruptedTransactions(plan.installDir);

        // Reject plans that touch the same target twice: the second backup
        // would overwrite the first one and destroy rollback fidelity.
        {
            std::set<std::string> targets;
            for (const auto& operation : plan.operations) {
                if (!targets.insert(operation.target).second) {
                    return Result<void>::fail(
                        {ErrorCode::ApplyFailed, "Apply plan contains duplicate target: " + operation.target});
                }
            }
        }

        auto backupDirs = createDirectories(plan.backupDir);
        if (!backupDirs) {
            return backupDirs;
        }
        auto hashProvider = createDefaultHashProvider();

        for (const auto& operation : plan.operations) {
            const auto target = joinChecked(plan.installDir, operation.target, "apply target");
            if (!target) {
                auto rolledBack = rollback(plan, applied);
                (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                return Result<void>::fail(target.error());
            }
            const auto backup = joinChecked(plan.backupDir, operation.target, "backup target");
            if (!backup) {
                auto rolledBack = rollback(plan, applied);
                (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                return Result<void>::fail(backup.error());
            }

            AppliedOperation appliedOperation;
            appliedOperation.operation = operation;
            std::error_code ec;
            appliedOperation.backupExists = std::filesystem::exists(target.value(), ec);
            if (appliedOperation.backupExists) {
                auto copied = copyWithSourcePermissions(target.value(), backup.value());
                if (!copied) {
                    auto rolledBack = rollback(plan, applied);
                    (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                    return copied;
                }
            }
            applied.push_back(std::move(appliedOperation));

            // Record the operation (and its backup) in the journal *before*
            // mutating the target. If the updater crashes mid-operation,
            // recovery can then restore from the backup; restoring an op that
            // never ran is harmless because backup and target are identical.
            {
                auto journaled = writeJournal(plan, applied, "applying");
                if (!journaled) {
                    // The journal is the crash-recovery record; without it the
                    // transaction cannot be recovered, so fail closed.
                    auto rolledBack = rollback(plan, applied);
                    return rolledBack ? journaled : rolledBack;
                }
            }

            if (operation.type == ApplyOperationType::Replace) {
                const auto source = joinChecked(plan.stagingDir, operation.source, "apply source");
                if (!source) {
                    auto rolledBack = rollback(plan, applied);
                    (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                    return Result<void>::fail(source.error());
                }
                auto copied = replaceFileAtomically(source.value(), target.value());
                if (!copied) {
                    auto rolledBack = rollback(plan, applied);
                    (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                    return copied;
                }
                auto verified = verifyReplace(plan, operation, *hashProvider);
                if (!verified) {
                    auto rolledBack = rollback(plan, applied);
                    (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                    return verified;
                }
            } else if (operation.type == ApplyOperationType::Remove) {
                std::filesystem::remove(target.value(), ec);
                if (ec) {
                    auto rolledBack = rollback(plan, applied);
                    (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }
        }

        auto journaled = writeJournal(plan, applied, "complete");
        if (!journaled) {
            auto rolledBack = rollback(plan, applied);
            return rolledBack ? journaled : rolledBack;
        }
        return restart(plan);
    } catch (...) {
        auto rolledBack = rollback(plan, applied);
        (void)writeJournal(plan, applied, rolledBack ? "rolled-back" : "rollback-failed");
        return Result<void>::fail({ErrorCode::ApplyFailed, "Unexpected apply failure"});
    }
}

} // namespace autoupdater::updater
