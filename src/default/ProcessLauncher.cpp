#include "libAutoUpdater/interfaces/IProcessLauncher.h"

#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace autoupdater {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count);
    return output;
}

std::wstring widenPath(const std::filesystem::path& path) {
    return path.wstring();
}

std::wstring quoteWindows(const std::wstring& value) {
    std::wstring result = L"\"";
    for (const auto ch : value) {
        if (ch == L'"') {
            result += L"\\\"";
        } else {
            result += ch;
        }
    }
    result += L"\"";
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
#endif

class ProcessLauncher final : public IProcessLauncher {
  public:
    Result<void> launch(const ProcessLaunchRequest& request) noexcept override {
        if (request.executable.empty()) {
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "Executable path is empty"});
        }

#ifdef _WIN32
        std::wstring command = quoteWindows(widenPath(request.executable));
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

        const BOOL ok = CreateProcessW(
            nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, request.detached ? CREATE_NEW_PROCESS_GROUP : 0,
            nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);
        if (!ok) {
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "CreateProcessW failed"});
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return Result<void>::ok();
#else
        const pid_t pid = fork();
        if (pid < 0) {
            return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "fork failed"});
        }
        if (pid == 0) {
            if (request.detached) {
                setsid();
                detachStandardStreams();
            }
            if (!request.workingDirectory.empty()) {
                chdir(request.workingDirectory.string().c_str());
            }
            std::vector<std::string> storage;
            storage.push_back(request.executable.string());
            storage.insert(storage.end(), request.arguments.begin(), request.arguments.end());
            std::vector<char*> argv;
            for (auto& item : storage) {
                argv.push_back(item.data());
            }
            argv.push_back(nullptr);
            execv(request.executable.string().c_str(), argv.data());
            _exit(127);
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
