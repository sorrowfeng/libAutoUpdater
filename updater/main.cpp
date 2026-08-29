#include "ApplyExecutor.h"

#include "libAutoUpdater/ApplyPlan.h"

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <ctime>
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
    bool hasPid = false;
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

bool parseUint64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool parseWaitSeconds(const std::string& text, int& out) {
    std::uint64_t value = 0;
    if (!parseUint64(text, value) || value == 0 || value > 3600) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool parseArgs(const std::vector<std::string>& values, Args& args) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        const auto& arg = values[i];
        if (arg == "--plan" && i + 1 < values.size()) {
            args.planPath = autoupdater::util::pathFromUtf8(values[++i]);
        } else if (arg == "--pid" && i + 1 < values.size()) {
            if (!parseUint64(values[++i], args.pid)) {
                return false;
            }
            args.hasPid = true;
        } else if (arg == "--wait" && i + 1 < values.size()) {
            if (!parseWaitSeconds(values[++i], args.waitSeconds)) {
                return false;
            }
        } else if (arg == "--help") {
            return false;
        }
    }
    return !args.planPath.empty() && args.hasPid;
}

std::filesystem::path normalizedAbsolute(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec && !canonical.empty()) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec).lexically_normal();
}

bool isPathInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto normalizedChild = normalizedAbsolute(child);
    const auto normalizedParent = normalizedAbsolute(parent);
    auto childIt = normalizedChild.begin();
    for (auto parentIt = normalizedParent.begin(); parentIt != normalizedParent.end(); ++parentIt, ++childIt) {
        if (childIt == normalizedChild.end() || *childIt != *parentIt) {
            return false;
        }
    }
    return true;
}

void appendLog(const std::filesystem::path& installDir, const std::string& message) {
    if (installDir.empty()) {
        return;
    }
    try {
        const auto logPath = installDir / ".autoupdater" / "updater.log";
        std::error_code ec;
        std::filesystem::create_directories(logPath.parent_path(), ec);
        std::ofstream output(logPath, std::ios::binary | std::ios::app);
        if (output) {
            const std::time_t now = std::time(nullptr);
            output << "[" << now << "] " << message << "\n";
        }
    } catch (...) {
    }
}

// Validates that the apply plan cannot affect anything outside the install
// directory it declares. The plan file is a contract between two processes on
// the same machine, not a trusted document.
bool planDirectoriesAreSafe(const autoupdater::ApplyPlan& plan, const std::filesystem::path& planPath,
                            std::string& problem) {
    const auto& installDir = plan.installDir;
    if (installDir.empty() || installDir == installDir.root_path()) {
        problem = "Plan installDir is empty or a filesystem root";
        return false;
    }
    if (!isPathInside(planPath, installDir)) {
        problem = "Apply plan must live inside installDir";
        return false;
    }
    if (!isPathInside(plan.stagingDir, installDir)) {
        problem = "Plan stagingDir must live inside installDir";
        return false;
    }
    if (!isPathInside(plan.backupDir, installDir)) {
        problem = "Plan backupDir must live inside installDir";
        return false;
    }
    if (!plan.restartCommand.empty()) {
        std::filesystem::path restartExe = autoupdater::util::pathFromUtf8(plan.restartCommand.front());
        if (restartExe.is_relative()) {
            restartExe = installDir / restartExe;
        }
        if (!isPathInside(restartExe, installDir)) {
            problem = "Plan restartCommand must resolve inside installDir";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const auto values = commandLineArgs(argc, argv);
    Args args;
    if (!parseArgs(values, args)) {
        std::cerr << "Usage: autoupdater_apply --plan <apply-plan.json> --pid <pid> [--wait <seconds>]\n";
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

    std::string problem;
    if (!planDirectoriesAreSafe(plan.value(), args.planPath, problem)) {
        std::cerr << problem << "\n";
        appendLog(plan.value().installDir, "plan rejected: " + problem);
        return 5;
    }

    auto applied = autoupdater::updater::executeApplyPlan(plan.value());
    if (!applied) {
        std::cerr << applied.error().message << "\n";
        appendLog(plan.value().installDir, "apply failed: " + applied.error().message);
        // The update itself succeeded when only the restart failed; let the
        // caller distinguish that from a failed (and rolled back) apply.
        if (applied.error().code == autoupdater::ErrorCode::ApplyLaunchFailed) {
            return 7;
        }
        return 6;
    }

    appendLog(plan.value().installDir, "apply complete: " + plan.value().toVersion);
    return 0;
}
