#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr char kProtocolMagic[] = "LAUPROC1";

struct HelperArguments {
    std::filesystem::path outputPath;
    std::vector<std::string> payload;
};

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}

#ifdef _WIN32
bool wideToUtf8(const wchar_t* text, std::string& output) {
    if (text == nullptr || *text == L'\0') {
        output.clear();
        return true;
    }
    const auto length = std::char_traits<wchar_t>::length(text);
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int wideLength = static_cast<int>(length);
    const int count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, wideLength, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, wideLength, output.data(), count, nullptr,
                               nullptr) == count;
}

bool readArguments(HelperArguments& result) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr || argumentCount < 2) {
        if (arguments != nullptr) {
            (void)LocalFree(arguments);
        }
        return false;
    }

    result.outputPath = std::filesystem::path(arguments[1]);
    result.payload.reserve(static_cast<std::size_t>(argumentCount - 2));
    for (int index = 2; index < argumentCount; ++index) {
        std::string value;
        if (!wideToUtf8(arguments[index], value)) {
            (void)LocalFree(arguments);
            return false;
        }
        result.payload.push_back(std::move(value));
    }
    (void)LocalFree(arguments);
    return true;
}
#else
bool readArguments(int argc, char** argv, HelperArguments& result) {
    if (argc < 2) {
        return false;
    }
    result.outputPath = std::filesystem::u8path(argv[1]);
    result.payload.reserve(static_cast<std::size_t>(argc - 2));
    for (int index = 2; index < argc; ++index) {
        result.payload.emplace_back(argv[index]);
    }
    return true;
}
#endif

template <class Value> bool writeValue(std::ofstream& output, const Value& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(value)));
    return static_cast<bool>(output);
}

bool writeString(std::ofstream& output, const std::string& value) {
    const auto length = static_cast<std::uint64_t>(value.size());
    if (!writeValue(output, length)) {
        return false;
    }
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

int writeResult(const HelperArguments& arguments) {
    std::error_code error;
    const auto workingDirectory = std::filesystem::current_path(error);
    if (error) {
        return 3;
    }

    auto temporaryPath = arguments.outputPath;
    temporaryPath += ".tmp";
    std::filesystem::remove(temporaryPath, error);
    error.clear();

    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return 4;
    }
    output.write(kProtocolMagic, static_cast<std::streamsize>(sizeof(kProtocolMagic) - 1));
    const auto argumentCount = static_cast<std::uint32_t>(arguments.payload.size());
    if (!output || !writeString(output, pathToUtf8(workingDirectory)) || !writeValue(output, argumentCount)) {
        return 5;
    }
    for (const auto& argument : arguments.payload) {
        if (!writeString(output, argument)) {
            return 5;
        }
    }
    output.close();
    if (!output) {
        return 5;
    }

    std::filesystem::remove(arguments.outputPath, error);
    error.clear();
    std::filesystem::rename(temporaryPath, arguments.outputPath, error);
    return error ? 6 : 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        HelperArguments arguments;
#ifdef _WIN32
        (void)argc;
        (void)argv;
        if (!readArguments(arguments)) {
#else
        if (!readArguments(argc, argv, arguments)) {
#endif
            return 2;
        }
        return writeResult(arguments);
    } catch (...) {
        return 7;
    }
}
