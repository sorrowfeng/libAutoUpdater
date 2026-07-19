#include "libAutoUpdater/interfaces/IProcessLauncher.h"

#include "util/PathUtil.h"

#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace autoupdater {

namespace {

template <class Character> bool containsEmbeddedNull(const std::basic_string<Character>& value) noexcept {
    return value.find(Character{}) != std::basic_string<Character>::npos;
}

Result<void> invalidLaunchRequest(const char* message) {
    return Result<void>::fail({ErrorCode::ApplyLaunchFailed, message});
}

#ifdef _WIN32
bool widenUtf8(const std::string& text, std::wstring& output) {
    if (text.empty()) {
        output.clear();
        return true;
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    const auto length = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (count <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, output.data(), count) == count;
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

Result<void> windowsLaunchFailure(const char* operation, DWORD error) {
    std::ostringstream message;
    message << operation << " failed (Windows error " << error << ')';
    return Result<void>::fail({ErrorCode::ApplyLaunchFailed, message.str()});
}

Result<void> launchWindows(const ProcessLaunchRequest& request) {
    constexpr std::size_t kMaximumCommandLineCharacters = 32767;

    const auto executable = request.executable.native();
    std::wstring command = quoteWindowsArgument(executable);
    for (const auto& argument : request.arguments) {
        std::wstring wideArgument;
        if (!widenUtf8(argument, wideArgument)) {
            return invalidLaunchRequest("Process argument is not valid UTF-8 or is too long");
        }
        command.push_back(L' ');
        command += quoteWindowsArgument(wideArgument);
    }
    if (command.size() >= kMaximumCommandLineCharacters) {
        return invalidLaunchRequest("Process command line exceeds the Windows length limit");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    const auto workingDirectory = request.workingDirectory.native();
    const DWORD creationFlags = request.detached ? CREATE_NEW_PROCESS_GROUP : 0U;

    const BOOL created =
        CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, creationFlags, nullptr,
                       workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);
    if (!created) {
        const DWORD error = GetLastError();
        return windowsLaunchFailure("CreateProcessW", error);
    }

    (void)CloseHandle(process.hThread);
    (void)CloseHandle(process.hProcess);
    return Result<void>::ok();
}
#else
enum class ChildLaunchStage : int {
    CreateSession = 1,
    RedirectStandardInput,
    RedirectStandardOutput,
    RedirectStandardError,
    ChangeWorkingDirectory,
    Execute
};

struct ChildLaunchError {
    int stage = 0;
    int errorNumber = 0;
};

void closeDescriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
}

int descriptorFlags(int descriptor) noexcept {
    int result = -1;
    do {
        result = fcntl(descriptor, F_GETFD);
    } while (result < 0 && errno == EINTR);
    return result;
}

bool setCloseOnExec(int descriptor) noexcept {
    const int flags = descriptorFlags(descriptor);
    if (flags < 0) {
        return false;
    }

    int result = -1;
    do {
        result = fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

bool prepareChildErrorDescriptor(int& descriptor) noexcept {
    if (descriptor <= STDERR_FILENO) {
        int duplicate = -1;
#ifdef F_DUPFD_CLOEXEC
        do {
            duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        } while (duplicate < 0 && errno == EINTR);
#else
        do {
            duplicate = fcntl(descriptor, F_DUPFD, STDERR_FILENO + 1);
        } while (duplicate < 0 && errno == EINTR);
#endif
        if (duplicate < 0) {
            return false;
        }
        closeDescriptor(descriptor);
        descriptor = duplicate;
    }
    return setCloseOnExec(descriptor);
}

int openDevNull() noexcept {
    int flags = O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = -1;
    do {
        descriptor = open("/dev/null", flags);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor >= 0 && !setCloseOnExec(descriptor)) {
        const int savedError = errno;
        closeDescriptor(descriptor);
        errno = savedError;
        return -1;
    }
    return descriptor;
}

int duplicateDescriptor(int source, int destination) noexcept {
    int result = -1;
    do {
        result = dup2(source, destination);
    } while (result < 0 && errno == EINTR);
    return result;
}

[[noreturn]] void reportChildLaunchError(int descriptor, ChildLaunchStage stage, int errorNumber) noexcept {
    const ChildLaunchError error{static_cast<int>(stage), errorNumber};
    const auto* cursor = reinterpret_cast<const unsigned char*>(&error);
    std::size_t remaining = sizeof(error);
    while (remaining > 0) {
        const auto written = write(descriptor, cursor, remaining);
        if (written > 0) {
            const auto count = static_cast<std::size_t>(written);
            cursor += count;
            remaining -= count;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    _exit(127);
}

const char* childLaunchStageName(int stage) noexcept {
    switch (static_cast<ChildLaunchStage>(stage)) {
    case ChildLaunchStage::CreateSession:
        return "setsid";
    case ChildLaunchStage::RedirectStandardInput:
        return "redirect standard input";
    case ChildLaunchStage::RedirectStandardOutput:
        return "redirect standard output";
    case ChildLaunchStage::RedirectStandardError:
        return "redirect standard error";
    case ChildLaunchStage::ChangeWorkingDirectory:
        return "change working directory";
    case ChildLaunchStage::Execute:
        return "execv";
    }
    return "child setup";
}

void reapFailedChild(pid_t pid) noexcept {
    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
}

Result<void> posixLaunchFailure(const char* operation, int errorNumber) {
    std::ostringstream message;
    message << operation << " failed (errno " << errorNumber;
    const std::error_code error(errorNumber, std::generic_category());
    const auto errorText = error.message();
    if (!errorText.empty()) {
        message << ": " << errorText;
    }
    message << ')';
    return Result<void>::fail({ErrorCode::ApplyLaunchFailed, message.str()});
}

Result<void> launchPosix(const ProcessLaunchRequest& request) {
    const bool detached = request.detached;
    // Prepare every owning C++ object before fork. The child must not allocate,
    // acquire a library lock, or run a destructor inherited from another thread.
    std::string executable = util::pathToUtf8(request.executable);
    if (executable.empty()) {
        return invalidLaunchRequest("Executable path cannot be represented as UTF-8");
    }

    std::string workingDirectory;
    if (!request.workingDirectory.empty()) {
        workingDirectory = util::pathToUtf8(request.workingDirectory);
        if (workingDirectory.empty()) {
            return invalidLaunchRequest("Working directory cannot be represented as UTF-8");
        }
    }

    std::vector<std::string> argumentStorage;
    argumentStorage.reserve(request.arguments.size() + 1);
    argumentStorage.push_back(std::move(executable));
    argumentStorage.insert(argumentStorage.end(), request.arguments.begin(), request.arguments.end());

    std::vector<char*> argumentVector;
    argumentVector.reserve(argumentStorage.size() + 1);
    for (auto& argument : argumentStorage) {
        argumentVector.push_back(argument.data());
    }
    argumentVector.push_back(nullptr);

    const char* const executablePath = argumentStorage.front().c_str();
    const char* const workingDirectoryPath = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    char* const* const arguments = argumentVector.data();

    int devNull = -1;
    if (detached) {
        devNull = openDevNull();
        if (devNull < 0) {
            const int error = errno;
            return posixLaunchFailure("open /dev/null", error);
        }
    }

    int errorPipe[2] = {-1, -1};
    int pipeResult = -1;
    do {
        pipeResult = pipe(errorPipe);
    } while (pipeResult < 0 && errno == EINTR);
    if (pipeResult < 0) {
        const int error = errno;
        closeDescriptor(devNull);
        return posixLaunchFailure("pipe", error);
    }
    if (!setCloseOnExec(errorPipe[0]) || !prepareChildErrorDescriptor(errorPipe[1])) {
        const int error = errno;
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        closeDescriptor(devNull);
        return posixLaunchFailure("fcntl FD_CLOEXEC", error);
    }

    pid_t pid = -1;
    do {
        pid = fork();
    } while (pid < 0 && errno == EINTR);
    if (pid < 0) {
        const int error = errno;
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        closeDescriptor(devNull);
        return posixLaunchFailure("fork", error);
    }

    if (pid == 0) {
        // Keep this branch limited to POSIX async-signal-safe operations. A
        // successful exec closes errorPipe[1]; every setup failure writes one
        // fixed-size record before exiting without C++ stack unwinding.
        closeDescriptor(errorPipe[0]);
        if (detached) {
            pid_t session = -1;
            do {
                session = setsid();
            } while (session < 0 && errno == EINTR);
            if (session < 0) {
                reportChildLaunchError(errorPipe[1], ChildLaunchStage::CreateSession, errno);
            }
            if (duplicateDescriptor(devNull, STDIN_FILENO) < 0) {
                reportChildLaunchError(errorPipe[1], ChildLaunchStage::RedirectStandardInput, errno);
            }
            if (duplicateDescriptor(devNull, STDOUT_FILENO) < 0) {
                reportChildLaunchError(errorPipe[1], ChildLaunchStage::RedirectStandardOutput, errno);
            }
            if (duplicateDescriptor(devNull, STDERR_FILENO) < 0) {
                reportChildLaunchError(errorPipe[1], ChildLaunchStage::RedirectStandardError, errno);
            }
        }
        if (devNull > STDERR_FILENO) {
            closeDescriptor(devNull);
        }
        if (workingDirectoryPath != nullptr) {
            int changed = -1;
            do {
                changed = chdir(workingDirectoryPath);
            } while (changed < 0 && errno == EINTR);
            if (changed < 0) {
                reportChildLaunchError(errorPipe[1], ChildLaunchStage::ChangeWorkingDirectory, errno);
            }
        }
        do {
            execv(executablePath, arguments);
        } while (errno == EINTR);
        reportChildLaunchError(errorPipe[1], ChildLaunchStage::Execute, errno);
    }

    closeDescriptor(errorPipe[1]);
    closeDescriptor(devNull);

    ChildLaunchError childError{};
    auto* cursor = reinterpret_cast<unsigned char*>(&childError);
    std::size_t received = 0;
    while (received < sizeof(childError)) {
        const auto count = read(errorPipe[0], cursor + received, sizeof(childError) - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const int error = errno;
            closeDescriptor(errorPipe[0]);
            return posixLaunchFailure("read child launch status", error);
        }
        break;
    }
    closeDescriptor(errorPipe[0]);

    // EOF without a record means exec closed the descriptor atomically while
    // installing the new image, giving POSIX the same launch boundary as
    // CreateProcessW on Windows.
    if (received == 0) {
        return Result<void>::ok();
    }
    if (received != sizeof(childError)) {
        reapFailedChild(pid);
        return invalidLaunchRequest("Child process returned an incomplete launch error");
    }

    reapFailedChild(pid);
    return posixLaunchFailure(childLaunchStageName(childError.stage), childError.errorNumber);
}
#endif

class ProcessLauncher final : public IProcessLauncher {
  public:
    Result<void> launch(const ProcessLaunchRequest& request) noexcept override {
        try {
            if (request.executable.empty()) {
                return invalidLaunchRequest("Executable path is empty");
            }
            if (containsEmbeddedNull(request.executable.native()) ||
                containsEmbeddedNull(request.workingDirectory.native())) {
                return invalidLaunchRequest("Process path contains an embedded NUL character");
            }
            for (const auto& argument : request.arguments) {
                if (containsEmbeddedNull(argument)) {
                    return invalidLaunchRequest("Process argument contains an embedded NUL character");
                }
            }

#ifdef _WIN32
            return launchWindows(request);
#else
            return launchPosix(request);
#endif
        } catch (...) {
            return invalidLaunchRequest("Failed to prepare process launch");
        }
    }
};

} // namespace

std::shared_ptr<IProcessLauncher> createDefaultProcessLauncher() {
    return std::make_shared<ProcessLauncher>();
}

} // namespace autoupdater
