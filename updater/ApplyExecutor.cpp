#include "ApplyExecutor.h"

#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "util/Json.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
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

Result<void> systemFailure(const std::string& action, int code) {
#ifdef _WIN32
    return Result<void>::fail({ErrorCode::FileSystemError,
                               action + ": " + std::system_category().message(code)});
#else
    return Result<void>::fail({ErrorCode::FileSystemError,
                               action + ": " + std::generic_category().message(code)});
#endif
}

std::filesystem::path journalTemporaryPath(const std::filesystem::path& path) {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    std::string seed = util::pathToUtf8(path);
    seed.append(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    seed.append(std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    seed.append(std::to_string(random()));
    seed.append(std::to_string(random()));
    return std::filesystem::path(path).concat(".tmp." + util::sha256Bytes(seed).substr(0, 24));
}

Result<void> atomicWriteFile(const std::filesystem::path& path, const std::string& contents) {
    const auto temporaryPath = journalTemporaryPath(path);

#ifdef _WIN32
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return systemFailure("Failed to create journal temporary file", static_cast<int>(GetLastError()));
    }

    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto remaining = contents.size() - offset;
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
            const auto error = GetLastError();
            CloseHandle(file);
            DeleteFileW(temporaryPath.c_str());
            return systemFailure("Failed to write journal temporary file", static_cast<int>(error));
        }
        offset += written;
    }

    if (!FlushFileBuffers(file)) {
        const auto error = GetLastError();
        CloseHandle(file);
        DeleteFileW(temporaryPath.c_str());
        return systemFailure("Failed to flush journal temporary file", static_cast<int>(error));
    }
    if (!CloseHandle(file)) {
        const auto error = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        return systemFailure("Failed to close journal temporary file", static_cast<int>(error));
    }
    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        return systemFailure("Failed to replace transaction journal", static_cast<int>(error));
    }
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int file = ::open(temporaryPath.c_str(), flags, S_IRUSR | S_IWUSR);
    if (file < 0) {
        return systemFailure("Failed to create journal temporary file", errno);
    }

    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = ::write(file, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const auto error = errno;
            ::close(file);
            ::unlink(temporaryPath.c_str());
            return systemFailure("Failed to write journal temporary file", error);
        }
        offset += static_cast<std::size_t>(written);
    }

    if (::fsync(file) != 0) {
        const auto error = errno;
        ::close(file);
        ::unlink(temporaryPath.c_str());
        return systemFailure("Failed to flush journal temporary file", error);
    }
    if (::close(file) != 0) {
        const auto error = errno;
        ::unlink(temporaryPath.c_str());
        return systemFailure("Failed to close journal temporary file", error);
    }
    if (::rename(temporaryPath.c_str(), path.c_str()) != 0) {
        const auto error = errno;
        ::unlink(temporaryPath.c_str());
        return systemFailure("Failed to replace transaction journal", error);
    }

    int directoryFlags = O_RDONLY;
#ifdef O_CLOEXEC
    directoryFlags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    directoryFlags |= O_DIRECTORY;
#endif
    const int directory = ::open(path.parent_path().c_str(), directoryFlags);
    if (directory < 0) {
        return systemFailure("Failed to open journal directory for flush", errno);
    }
    if (::fsync(directory) != 0) {
        const auto error = errno;
        ::close(directory);
        return systemFailure("Failed to flush journal directory", error);
    }
    if (::close(directory) != 0) {
        return systemFailure("Failed to close journal directory", errno);
    }
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

Result<void> writeJournal(const ApplyPlan& plan, const std::vector<AppliedOperation>& applied,
                          const std::string& state) {
    try {
        const auto journalDir = plan.installDir / ".autoupdater" / "journal";
        auto dirs = createDirectories(journalDir);
        if (!dirs) {
            return dirs;
        }
        auto fileName = transactionJournalFileName(plan);
        if (!fileName) {
            return Result<void>::fail(fileName.error());
        }

        std::ostringstream output;
        output << "{\n";
        output << "  \"state\": \"" << util::jsonEscape(state) << "\",\n";
        output << "  \"toVersion\": \"" << util::jsonEscape(plan.toVersion) << "\",\n";
        output << "  \"appliedCount\": " << applied.size() << "\n";
        output << "}\n";
        return atomicWriteFile(journalDir / fileName.value(), output.str());
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

Result<LockCleanup> acquireUpdateLock(const std::filesystem::path& installDir) {
    const auto stateDir = installDir / ".autoupdater";
    auto dirs = createDirectories(stateDir);
    if (!dirs) {
        return Result<LockCleanup>::fail(dirs.error());
    }

    const auto lockPath = stateDir / "update.lock";
    std::error_code ec;
    if (!std::filesystem::create_directory(lockPath, ec)) {
        return Result<LockCleanup>::fail({ErrorCode::ApplyFailed, "Another update appears to be running"});
    }
    if (ec) {
        return Result<LockCleanup>::fail({ErrorCode::FileSystemError, ec.message()});
    }

    std::ofstream metadata(lockPath / "owner.txt", std::ios::binary | std::ios::trunc);
    metadata << "autoupdater_apply\n";
    return Result<LockCleanup>::ok(LockCleanup(lockPath));
}

} // namespace

Result<std::string> transactionJournalFileName(const ApplyPlan& plan) noexcept {
    try {
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
        auto lock = acquireUpdateLock(plan.installDir);
        if (!lock) {
            return Result<void>::fail(lock.error());
        }
        auto lockCleanup = std::move(lock.value());

        auto backupDirs = createDirectories(plan.backupDir);
        if (!backupDirs) {
            return backupDirs;
        }
        auto hashProvider = createDefaultHashProvider();

        auto initialJournal = writeJournal(plan, applied, "applying");
        if (!initialJournal) {
            return initialJournal;
        }

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

            auto journal = writeJournal(plan, applied, "applying");
            if (!journal) {
                rollback(plan, applied);
                return journal;
            }
        }

        auto completedJournal = writeJournal(plan, applied, "complete");
        if (!completedJournal) {
            rollback(plan, applied);
            return completedJournal;
        }
        return restart(plan);
    } catch (...) {
        rollback(plan, applied);
        return Result<void>::fail({ErrorCode::ApplyFailed, "Unexpected apply failure"});
    }
}

} // namespace autoupdater::updater
