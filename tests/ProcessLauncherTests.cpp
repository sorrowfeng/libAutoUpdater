#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "util/PathUtil.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

constexpr char kProtocolMagic[] = "LAUPROC1";
constexpr auto kHelperTimeout = std::chrono::seconds(10);

std::uint64_t processId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"libAutoUpdater-process-launcher") /
                std::filesystem::u8path(std::to_string(processId()) + "-" + name)) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("Failed to create process-launcher test directory: " + error.message());
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (length + 1 < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("_NSGetExecutablePath failed");
    }
    return std::filesystem::u8path(buffer.data());
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("readlink(/proc/self/exe) failed");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::u8path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::filesystem::path helperExecutablePath() {
#ifdef _WIN32
    const auto configuredSize = GetEnvironmentVariableW(L"LIBAUTOUPDATER_PROCESS_LAUNCHER_HELPER", nullptr, 0);
    if (configuredSize > 1) {
        std::vector<wchar_t> configured(configuredSize, L'\0');
        if (GetEnvironmentVariableW(L"LIBAUTOUPDATER_PROCESS_LAUNCHER_HELPER", configured.data(), configuredSize) ==
            0) {
            throw std::runtime_error("GetEnvironmentVariableW failed");
        }
        return std::filesystem::path(configured.data());
    }
#else
    if (const auto* configured = std::getenv("LIBAUTOUPDATER_PROCESS_LAUNCHER_HELPER")) {
        if (*configured != '\0') {
            return std::filesystem::u8path(configured);
        }
    }
#endif

    auto sibling = currentExecutablePath().parent_path() / "libAutoUpdater process launcher helper";
#ifdef _WIN32
    sibling += ".exe";
#endif
    return sibling;
}

template <class Value> Value readValue(std::ifstream& input) {
    Value value{};
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(value)));
    if (!input) {
        throw std::runtime_error("Process-launcher helper result is truncated");
    }
    return value;
}

std::string readString(std::ifstream& input) {
    const auto length = readValue<std::uint64_t>(input);
    if (length > 1024U * 1024U || length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("Process-launcher helper returned an invalid string length");
    }
    std::string value(static_cast<std::size_t>(length), '\0');
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!input) {
        throw std::runtime_error("Process-launcher helper result is truncated");
    }
    return value;
}

struct HelperResult {
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
};

HelperResult waitForHelperResult(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + kHelperTimeout;
    std::error_code error;
    while (!std::filesystem::exists(path, error)) {
        if (error) {
            throw std::runtime_error("Failed to inspect process-launcher helper result: " + error.message());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Process-launcher helper timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open process-launcher helper result");
    }
    std::string magic(sizeof(kProtocolMagic) - 1, '\0');
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kProtocolMagic) {
        throw std::runtime_error("Process-launcher helper returned an invalid protocol marker");
    }

    HelperResult result;
    result.workingDirectory = autoupdater::util::pathFromUtf8(readString(input));
    const auto argumentCount = readValue<std::uint32_t>(input);
    if (argumentCount > 256) {
        throw std::runtime_error("Process-launcher helper returned too many arguments");
    }
    result.arguments.reserve(argumentCount);
    for (std::uint32_t index = 0; index < argumentCount; ++index) {
        result.arguments.push_back(readString(input));
    }
    if (input.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("Process-launcher helper returned trailing data");
    }
    return result;
}

std::vector<std::string> difficultArguments() {
    std::vector<std::string> result = {"", "plain", "contains spaces", "contains\ta tab", "a\"quoted\"value"};
    for (std::size_t count = 1; count <= 4; ++count) {
        result.push_back("slashes-before-quote-" + std::string(count, '\\') + "\"");
        result.push_back("trailing-slashes-" + std::string(count, '\\'));
        result.push_back("space-and-trailing-slashes " + std::string(count, '\\'));
    }
    result.emplace_back(u8"Unicode-中文-🚀");
    return result;
}

} // namespace

void testProcessLauncherReportsSetupAndExecFailures() {
    TemporaryDirectory temporary("failures");
    const auto launcher = autoupdater::createDefaultProcessLauncher();

    autoupdater::ProcessLaunchRequest empty;
    auto result = launcher->launch(empty);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);

    autoupdater::ProcessLaunchRequest missingExecutable;
    missingExecutable.executable = temporary.path() / "missing executable";
    missingExecutable.workingDirectory = temporary.path();
    missingExecutable.detached = true;
    result = launcher->launch(missingExecutable);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
#ifndef _WIN32
    LAU_REQUIRE(result.error().message.find("execv") != std::string::npos);
#endif

    const auto marker = temporary.path() / "invalid-working-directory-result.bin";
    autoupdater::ProcessLaunchRequest missingWorkingDirectory;
    missingWorkingDirectory.executable = helperExecutablePath();
    missingWorkingDirectory.workingDirectory = temporary.path() / "missing working directory";
    missingWorkingDirectory.arguments = {autoupdater::util::pathToUtf8(marker)};
    missingWorkingDirectory.detached = true;
    result = launcher->launch(missingWorkingDirectory);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
#ifndef _WIN32
    LAU_REQUIRE(result.error().message.find("change working directory") != std::string::npos);
#endif
    std::error_code error;
    LAU_REQUIRE(!std::filesystem::exists(marker, error));
    LAU_REQUIRE(!error);
}

void testProcessLauncherPreservesArgumentsAndWorkingDirectory() {
    TemporaryDirectory temporary("roundtrip");
    const auto workingDirectory = temporary.path() / std::filesystem::u8path(u8"working directory 中文");
    std::filesystem::create_directories(workingDirectory);
    const auto marker = temporary.path() / std::filesystem::u8path(u8"argument result 中文.bin");
    const auto expectedArguments = difficultArguments();

    autoupdater::ProcessLaunchRequest request;
    request.executable = helperExecutablePath();
    request.workingDirectory = workingDirectory;
    request.detached = true;
    request.arguments.push_back(autoupdater::util::pathToUtf8(marker));
    request.arguments.insert(request.arguments.end(), expectedArguments.begin(), expectedArguments.end());

    const auto launcher = autoupdater::createDefaultProcessLauncher();
    const auto launched = launcher->launch(request);
    LAU_REQUIRE(launched);

    const auto result = waitForHelperResult(marker);
    std::error_code error;
    LAU_REQUIRE(std::filesystem::equivalent(result.workingDirectory, workingDirectory, error));
    LAU_REQUIRE(!error);
    LAU_REQUIRE(result.arguments == expectedArguments);
}

void testProcessLauncherRejectsLossyArguments() {
    TemporaryDirectory temporary("invalid-arguments");
    const auto launcher = autoupdater::createDefaultProcessLauncher();

    autoupdater::ProcessLaunchRequest request;
    request.executable = helperExecutablePath();
    request.workingDirectory = temporary.path();
    request.detached = true;
    request.arguments = {std::string("prefix\0suffix", 13)};
    auto result = launcher->launch(request);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
    LAU_REQUIRE(result.error().message.find("embedded NUL") != std::string::npos);

#ifdef _WIN32
    request.arguments = {std::string("\xC3\x28", 2)};
    result = launcher->launch(request);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);

    request.arguments = {std::string(32768, 'x')};
    result = launcher->launch(request);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
#endif
}
