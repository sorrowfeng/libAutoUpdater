#include "ApplyExecutor.h"

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/ResourceLimits.h"
#include "util/BoundedFile.h"
#include "util/Sha256.h"

#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
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
    std::string planDigest;
    std::filesystem::path installRoot;
    std::uint64_t pid = 0;
    int waitSeconds = 60;
    bool rollback = false;
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
        } else if (arg == "--plan-sha256" && i + 1 < values.size()) {
            args.planDigest = values[++i];
        } else if (arg == "--install-root" && i + 1 < values.size()) {
            args.installRoot = autoupdater::util::pathFromUtf8(values[++i]);
        } else if (arg == "--pid" && i + 1 < values.size()) {
            args.pid = static_cast<std::uint64_t>(std::strtoull(values[++i].c_str(), nullptr, 10));
        } else if (arg == "--wait" && i + 1 < values.size()) {
            args.waitSeconds = std::atoi(values[++i].c_str());
        } else if (arg == "--rollback") {
            args.rollback = true;
        } else if (arg == "--help") {
            return false;
        } else {
            return false;
        }
    }
    return !args.planPath.empty() && !args.installRoot.empty() &&
           autoupdater::util::isLowerHexSha256(args.planDigest);
}

} // namespace

int main(int argc, char** argv) {
    const auto values = commandLineArgs(argc, argv);
    Args args;
    if (!parseArgs(values, args)) {
        std::cerr << "Usage: autoupdater_apply --plan <apply-plan.json> --plan-sha256 <digest> "
                     "--install-root <path> [--rollback] [--pid <pid>] [--wait <seconds>]\n";
        return 2;
    }

    auto wait = autoupdater::updater::waitForProcessExit(args.pid, std::chrono::seconds(args.waitSeconds));
    if (!wait) {
        std::cerr << wait.error().message << "\n";
        return 3;
    }

    const autoupdater::ResourceLimits limits;
    auto contents = autoupdater::util::readRegularFileWithLimit(args.planPath, limits.maxApplyPlanBytes,
                                                                autoupdater::ErrorCode::ApplyFailed, "apply plan");
    if (!contents) {
        std::cerr << contents.error().message << "\n";
        return 4;
    }
    if (autoupdater::util::sha256Bytes(contents.value()) != args.planDigest) {
        std::cerr << "Apply plan does not match the digest authorized by its launcher\n";
        return 5;
    }
    auto plan = autoupdater::ApplyPlan::parse(contents.value(), limits);
    if (!plan) {
        std::cerr << plan.error().message << "\n";
        return 6;
    }
    if (plan.value().installDir.lexically_normal() != args.installRoot.lexically_normal()) {
        std::cerr << "Apply plan install root does not match its launcher binding\n";
        return 7;
    }
    const bool rollbackPlan = plan.value().intent == autoupdater::ApplyPlanIntent::Rollback;
    if (rollbackPlan != args.rollback) {
        std::cerr << "Apply plan intent does not match its launch mode\n";
        return 8;
    }

    auto applied = autoupdater::updater::executeApplyPlan(plan.value());
    if (!applied) {
        std::cerr << applied.error().message << "\n";
        return 9;
    }

    return 0;
}
