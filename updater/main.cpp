#include "ApplyExecutor.h"

#include "libAutoUpdater/ApplyPlan.h"

#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "util/PathUtil.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

struct Args {
    std::filesystem::path planPath;
    std::uint64_t pid = 0;
    int waitSeconds = 60;
};

#ifdef _WIN32
std::string wideToUtf8(const wchar_t* text) {
    if (!text || *text == L'\0') {
        return {};
    }
    const int length = static_cast<int>(wcslen(text));
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, output.data(), count, nullptr, nullptr);
    return output;
}
#endif

std::vector<std::string> commandLineArgs(int argc, char** argv) {
#ifdef _WIN32
    int wideCount = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArgs) {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(wideCount));
        for (int i = 0; i < wideCount; ++i) {
            args.push_back(wideToUtf8(wideArgs[i]));
        }
        LocalFree(wideArgs);
        return args;
    }
#endif
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        values.emplace_back(argv[i]);
    }
    return values;
}

bool parseArgs(const std::vector<std::string>& values, Args& args) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        const auto& arg = values[i];
        if (arg == "--plan" && i + 1 < values.size()) {
            args.planPath = autoupdater::util::pathFromUtf8(values[++i]);
        } else if (arg == "--pid" && i + 1 < values.size()) {
            args.pid = static_cast<std::uint64_t>(std::strtoull(values[++i].c_str(), nullptr, 10));
        } else if (arg == "--wait" && i + 1 < values.size()) {
            args.waitSeconds = std::atoi(values[++i].c_str());
        } else if (arg == "--help") {
            return false;
        }
    }
    return !args.planPath.empty();
}

} // namespace

int main(int argc, char** argv) {
    const auto values = commandLineArgs(argc, argv);
    Args args;
    if (!parseArgs(values, args)) {
        std::cerr << "Usage: autoupdater_apply --plan <apply-plan.json> [--pid <pid>] [--wait <seconds>]\n";
        return 2;
    }

    auto wait = autoupdater::updater::waitForProcessExit(args.pid, std::chrono::seconds(args.waitSeconds));
    if (!wait) {
        std::cerr << wait.error().message << "\n";
        return 3;
    }

    std::ifstream input(args.planPath, std::ios::binary);
    if (!input) {
        std::cerr << "Failed to open apply plan\n";
        return 4;
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    auto plan = autoupdater::ApplyPlan::parse(stream.str());
    if (!plan) {
        std::cerr << plan.error().message << "\n";
        return 5;
    }

    auto applied = autoupdater::updater::executeApplyPlan(plan.value());
    if (!applied) {
        std::cerr << applied.error().message << "\n";
        return 6;
    }

    return 0;
}
