#include "ApplyExecutor.h"

#include "ProcessWait.h"
#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/ResourceLimits.h"
#include "util/BoundedFile.h"
#include "util/Sha256.h"

#include <chrono>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    std::chrono::seconds waitTimeout{60};
    bool rollback = false;
};

#ifdef _WIN32
bool wideToUtf8(const wchar_t* text, std::string& output) {
    if (!text) {
        return false;
    }
    if (*text == L'\0') {
        output.clear();
        return true;
    }
    const auto size = wcslen(text);
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int length = static_cast<int>(size);
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, output.data(), count, nullptr,
                               nullptr) == count;
}
#endif

std::optional<std::vector<std::string>> commandLineArgs(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int wideCount = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (!wideArgs || wideCount < 0) {
        return std::nullopt;
    }
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(wideCount));
    bool converted = true;
    for (int i = 0; i < wideCount; ++i) {
        std::string value;
        if (!wideToUtf8(wideArgs[i], value)) {
            converted = false;
            break;
        }
        args.push_back(std::move(value));
    }
    (void)LocalFree(wideArgs);
    if (!converted) {
        return std::nullopt;
    }
    return args;
#else
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        values.emplace_back(argv[i]);
    }
    return values;
#endif
}

bool parseUnsignedDecimal(std::string_view text, std::uint64_t maximum, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const unsigned char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool parseArgs(const std::vector<std::string>& values, Args& args) {
    bool planSeen = false;
    bool digestSeen = false;
    bool rootSeen = false;
    bool pidSeen = false;
    bool waitSeen = false;
    bool rollbackSeen = false;
    for (std::size_t i = 1; i < values.size(); ++i) {
        const auto& arg = values[i];
        if (arg == "--plan" && i + 1 < values.size() && !planSeen) {
            planSeen = true;
            args.planPath = autoupdater::util::pathFromUtf8(values[++i]);
        } else if (arg == "--plan-sha256" && i + 1 < values.size() && !digestSeen) {
            digestSeen = true;
            args.planDigest = values[++i];
        } else if (arg == "--install-root" && i + 1 < values.size() && !rootSeen) {
            rootSeen = true;
            args.installRoot = autoupdater::util::pathFromUtf8(values[++i]);
        } else if (arg == "--pid" && i + 1 < values.size() && !pidSeen) {
            pidSeen = true;
            if (!parseUnsignedDecimal(values[++i], autoupdater::detail::maximumPlatformProcessId(), args.pid)) {
                return false;
            }
        } else if (arg == "--wait" && i + 1 < values.size() && !waitSeen) {
            waitSeen = true;
            std::uint64_t seconds = 0;
            const auto maximum =
                static_cast<std::uint64_t>(autoupdater::detail::kMaximumProcessWaitTimeout.count());
            if (!parseUnsignedDecimal(values[++i], maximum, seconds)) {
                return false;
            }
            args.waitTimeout = std::chrono::seconds(static_cast<std::chrono::seconds::rep>(seconds));
        } else if (arg == "--rollback" && !rollbackSeen) {
            rollbackSeen = true;
            args.rollback = true;
        } else if (arg == "--help") {
            return false;
        } else {
            return false;
        }
    }
    return planSeen && digestSeen && rootSeen && !args.planPath.empty() && !args.installRoot.empty() &&
           autoupdater::util::isLowerHexSha256(args.planDigest);
}

} // namespace

int main(int argc, char** argv) {
    const auto values = commandLineArgs(argc, argv);
    Args args;
    if (!values || !parseArgs(*values, args)) {
        std::cerr << "Usage: autoupdater_apply --plan <apply-plan.json> --plan-sha256 <digest> "
                     "--install-root <path> [--rollback] [--pid <pid>] [--wait <seconds>]\n";
        return 2;
    }

    auto wait = autoupdater::updater::waitForProcessExit(args.pid, args.waitTimeout);
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
