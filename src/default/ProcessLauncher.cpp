#include "libAutoUpdater/interfaces/IProcessLauncher.h"

#include "util/PathUtil.h"

#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern char** environ;
#endif

// posix_spawn extensions used below are not available everywhere.
#if !defined(_WIN32)
#if (defined(__GLIBC__) && __GLIBC_PREREQ(2, 26)) || defined(__APPLE__)
#define LIBAUTOUPDATER_POSIX_SPAWN_SETSID 1
#endif
#if (defined(__GLIBC__) && __GLIBC_PREREQ(2, 29)) || defined(__APPLE__)
#define LIBAUTOUPDATER_POSIX_SPAWN_CHDIR 1
#endif
#endif

namespace autoupdater {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count);
    return output;
}

std::wstring widenPath(const std::filesystem::path& path) {
    return path.wstring();
}

// Escapes one argument the way CommandLineToArgvW parses it: backslashes are
// literal except when they precede a quote; a quote must be backslash-escaped;
// trailing backslashes before the closing quote must be doubled.
std::wstring quoteWindows(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
        } else {
            result.append(backslashes, L'\\');
            result += ch;
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}
#else
void detachStandardStreams() noexcept {
    const int devNull = open("/dev/null", O_RDWR);
    if (devNull < 0) {
        return;
    }
    dup2(devNull, STDIN_FILENO);
    dup2(devNull, STDOUT_FILENO);
    dup2(devNull, STDERR_FILENO);
    if (devNull > STDERR_FILENO) {
        close(devNull);
    }
}

// Fallback for platforms without the posix_spawn extensions needed for
// setsid/chdir. All path and argument strings are materialized in the parent
// so the child only calls async-signal-safe functions between fork and exec.
Result<void> launchWithFork(const std::string& executable, const std::string& workingDirectory,
                            const std::vector<std::string>& arguments, bool detached) {
    std::vector<std::string> storage;
    storage.push_back(executable);
    storage.insert(storage.end(), arguments.begin(), arguments.end());

    const pid_t pid = fork();
    if (pid < 0) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "fork failed"});
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls are allowed in here.
        if (detached) {
            setsid();
            detachStandardStreams();
        }
        if (!workingDirectory.empty()) {
            if (chdir(workingDirectory.c_str()) != 0) {
                _exit(126);
            }
        }
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& item : storage) {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);
        execv(storage.front().c_str(), argv.data());
        _exit(127);
    }
    return Result<void>::ok();
}
#endif

class ProcessLauncher final : public IProcessLauncher {
  public:
    Result<void> launch(const ProcessLaunchRequest& request) noexcept override {
        if (request.executable.empty()) {
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "Executable path is empty"});
        }

#ifdef _WIN32
        const auto executable = quoteWindows(widenPath(request.executable));
        if (executable.empty()) {
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "Executable path is not valid Unicode"});
        }
        std::wstring command = executable;
        for (const auto& arg : request.arguments) {
            command += L" ";
            command += quoteWindows(widen(arg));
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring mutableCommand = command;
        std::wstring workingDirectory =
            request.workingDirectory.empty() ? std::wstring() : widenPath(request.workingDirectory);

        // DETACHED_PROCESS gives the child its own lifetime: closing the
        // parent's console must not take the updater down with it.
        const DWORD flags = request.detached ? (DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP) : 0;
        const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                       workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup,
                                       &process);
        if (!ok) {
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "CreateProcessW failed"});
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return Result<void>::ok();
#else
        // All string materialization happens here, in the parent, before any
        // spawn/fork takes place.
        const auto executable = util::pathToUtf8(request.executable);
        const auto workingDirectory = util::pathToUtf8(request.workingDirectory);

        bool canSpawnDetached = true;
        bool canSpawnChdir = true;
#ifndef LIBAUTOUPDATER_POSIX_SPAWN_SETSID
        if (request.detached) {
            canSpawnDetached = false;
        }
#endif
#ifndef LIBAUTOUPDATER_POSIX_SPAWN_CHDIR
        if (!workingDirectory.empty()) {
            canSpawnChdir = false;
        }
#endif
        if (!canSpawnDetached || !canSpawnChdir) {
            return launchWithFork(executable, workingDirectory, request.arguments, request.detached);
        }

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);

        short flags = 0;
#ifdef LIBAUTOUPDATER_POSIX_SPAWN_SETSID
        if (request.detached) {
            flags |= POSIX_SPAWN_SETSID;
        }
#endif
        posix_spawnattr_setflags(&attr, flags);

        if (request.detached) {
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDWR, 0);
            posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDERR_FILENO);
        }
#ifdef LIBAUTOUPDATER_POSIX_SPAWN_CHDIR
        if (!workingDirectory.empty()) {
            if (posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str()) != 0) {
                posix_spawn_file_actions_destroy(&actions);
                posix_spawnattr_destroy(&attr);
                return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "Failed to set working directory"});
            }
        }
#endif

        std::vector<std::string> storage;
        storage.push_back(executable);
        storage.insert(storage.end(), request.arguments.begin(), request.arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& item : storage) {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);

        pid_t pid = 0;
        // posix_spawn (unlike posix_spawnp) does not search PATH, matching the
        // previous execv behavior.
        const int rc = posix_spawn(&pid, executable.c_str(), &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        if (rc != 0) {
            // Unlike fork+execv, the error surfaces here instead of being
            // silently swallowed in the child.
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "posix_spawn failed: " + std::to_string(rc)});
        }
        return Result<void>::ok();
#endif
    }
};

} // namespace

std::shared_ptr<IProcessLauncher> createDefaultProcessLauncher() {
    return std::make_shared<ProcessLauncher>();
}

} // namespace autoupdater
