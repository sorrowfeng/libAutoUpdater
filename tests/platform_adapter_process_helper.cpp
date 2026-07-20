#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::uint64_t processId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

template <typename Character> bool parseDelay(const Character* text, std::chrono::milliseconds& delay) {
    if (text == nullptr || *text == Character{}) {
        return false;
    }
    std::uint64_t value = 0;
    for (const Character* cursor = text; *cursor != Character{}; ++cursor) {
        const auto ch = *cursor;
        if (ch < static_cast<Character>('0') || ch > static_cast<Character>('9')) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - static_cast<Character>('0'));
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    constexpr std::uint64_t kMaximumDelayMilliseconds = 10000;
    if (value > kMaximumDelayMilliseconds) {
        return false;
    }
    delay = std::chrono::milliseconds(value);
    return true;
}

struct HelperArguments {
    std::filesystem::path marker;
    std::chrono::milliseconds delay{0};
};

bool readArguments(int argc, char* argv[], HelperArguments& result) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr || argumentCount != 3) {
        if (arguments != nullptr) {
            (void)LocalFree(arguments);
        }
        return false;
    }
    result.marker = std::filesystem::path(arguments[1]);
    const bool parsed = parseDelay(arguments[2], result.delay);
    (void)LocalFree(arguments);
    return parsed;
#else
    if (argc != 3) {
        return false;
    }
    result.marker = std::filesystem::u8path(argv[1]);
    return parseDelay(argv[2], result.delay);
#endif
}

bool publishProcessId(const std::filesystem::path& path) {
    auto temporary = path;
    temporary += ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary, error);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << processId() << '\n';
    output.close();
    if (!output) {
        return false;
    }

    error.clear();
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace

int main(int argc, char* argv[]) {
    HelperArguments arguments;
    if (!readArguments(argc, argv, arguments)) {
        return 2;
    }
    if (!publishProcessId(arguments.marker)) {
        return 4;
    }

    std::this_thread::sleep_for(arguments.delay);
    return 0;
}
