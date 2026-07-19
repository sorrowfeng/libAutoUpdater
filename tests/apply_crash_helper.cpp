#include "ApplyExecutor.h"

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Error.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr int kCrashExitCode = 86;
constexpr int kCheckpointNotReachedExitCode = 87;
constexpr int kUnexpectedRecoveryExitCode = 88;

class MarkerProcessLauncher final : public autoupdater::IProcessLauncher {
  public:
    MarkerProcessLauncher(std::filesystem::path marker, bool failLaunch)
        : marker_(std::move(marker)), failLaunch_(failLaunch) {}

    autoupdater::Result<void> launch(const autoupdater::ProcessLaunchRequest&) noexcept override {
        try {
            std::ofstream output(marker_, std::ios::binary | std::ios::app);
            if (!output) {
                return autoupdater::Result<void>::fail(
                    {autoupdater::ErrorCode::FileSystemError, "Failed to open restart launch marker"});
            }
            output << "launch\n";
            output.flush();
            if (!output) {
                return autoupdater::Result<void>::fail(
                    {autoupdater::ErrorCode::FileSystemError, "Failed to persist restart launch marker"});
            }
            if (failLaunch_) {
                return autoupdater::Result<void>::fail(
                    {autoupdater::ErrorCode::ApplyLaunchFailed, "Injected restart launch failure"});
            }
            return autoupdater::Result<void>::ok();
        } catch (...) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Unexpected restart launch marker failure"});
        }
    }

  private:
    std::filesystem::path marker_;
    bool failLaunch_ = false;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open apply plan");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

autoupdater::Result<autoupdater::ApplyPlan> readPlan(const std::filesystem::path& path) {
    try {
        return autoupdater::ApplyPlan::parse(readFile(path), autoupdater::ResourceLimits{});
    } catch (const std::exception& error) {
        return autoupdater::Result<autoupdater::ApplyPlan>::fail(
            {autoupdater::ErrorCode::FileSystemError, error.what()});
    }
}

autoupdater::updater::ApplyExecutorDependencies dependencies(const std::filesystem::path& launchMarker = {},
                                                             bool failLaunch = false) {
    autoupdater::updater::ApplyExecutorDependencies result;
    result.fileSystem = autoupdater::createDefaultFileSystem();
    result.hashProvider = autoupdater::createDefaultHashProvider();
    result.processLauncher = launchMarker.empty() ? autoupdater::createDefaultProcessLauncher()
                                                  : std::make_shared<MarkerProcessLauncher>(launchMarker, failLaunch);
    return result;
}

int reportUnexpectedResult(const autoupdater::Result<void>& result, const char* context) {
    if (result) {
        std::cerr << context << " completed without reaching the requested crash checkpoint\n";
    } else {
        std::cerr << context << " failed before reaching the requested crash checkpoint: "
                  << autoupdater::toString(result.error().code) << ": " << result.error().message << "\n";
    }
    return kCheckpointNotReachedExitCode;
}

int runCrash(const autoupdater::ApplyPlan& plan, std::string checkpoint, std::string operationIndexText,
             const std::filesystem::path& launchMarker = {}, bool failLaunch = false) {
    const bool anyOperation = operationIndexText == "any";
    std::size_t operationIndex = 0;
    if (!anyOperation) {
        try {
            std::size_t consumed = 0;
            const auto value = std::stoull(operationIndexText, &consumed, 10);
            if (consumed != operationIndexText.size() ||
                value > static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)())) {
                throw std::out_of_range("operation index");
            }
            operationIndex = static_cast<std::size_t>(value);
        } catch (...) {
            std::cerr << "operation-index must be a non-negative integer or 'any'\n";
            return 64;
        }
    }

    autoupdater::updater::ApplyExecutionHooks hooks;
    hooks.checkpoint = [checkpoint = std::move(checkpoint), anyOperation, operationIndex](std::string_view name,
                                                                                          std::size_t observedIndex) {
        if (name == checkpoint && (anyOperation || observedIndex == operationIndex)) {
            // This deliberately bypasses stack unwinding. It models power loss or
            // forced termination after the executor has made the boundary durable.
            std::_Exit(kCrashExitCode);
        }
        return autoupdater::updater::ApplyFaultAction::Continue;
    };
    const auto result =
        autoupdater::updater::executeApplyPlanWithDependencies(plan, dependencies(launchMarker, failLaunch), hooks);
    return reportUnexpectedResult(result, "apply/recovery");
}

int runInjectedFailure(const autoupdater::ApplyPlan& plan, std::string checkpoint, std::string operationIndexText,
                       const std::filesystem::path& launchMarker = {}, bool failLaunch = false) {
    const bool anyOperation = operationIndexText == "any";
    std::size_t operationIndex = 0;
    if (!anyOperation) {
        try {
            std::size_t consumed = 0;
            const auto value = std::stoull(operationIndexText, &consumed, 10);
            if (consumed != operationIndexText.size() ||
                value > static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)())) {
                throw std::out_of_range("operation index");
            }
            operationIndex = static_cast<std::size_t>(value);
        } catch (...) {
            std::cerr << "operation-index must be a non-negative integer or 'any'\n";
            return 64;
        }
    }

    bool checkpointReached = false;
    autoupdater::updater::ApplyExecutionHooks hooks;
    hooks.checkpoint = [&checkpointReached, checkpoint = std::move(checkpoint), anyOperation,
                        operationIndex](std::string_view name, std::size_t observedIndex) {
        if (name == checkpoint && (anyOperation || observedIndex == operationIndex)) {
            checkpointReached = true;
            return autoupdater::updater::ApplyFaultAction::Fail;
        }
        return autoupdater::updater::ApplyFaultAction::Continue;
    };
    const auto result =
        autoupdater::updater::executeApplyPlanWithDependencies(plan, dependencies(launchMarker, failLaunch), hooks);
    if (checkpointReached && !result) {
        return 0;
    }
    return reportUnexpectedResult(result, "injected failure");
}

int runBlocked(const autoupdater::ApplyPlan& plan) {
    const auto result = autoupdater::updater::executeApplyPlanWithDependencies(plan, dependencies());
    if (!result) {
        return 0;
    }
    std::cerr << "apply unexpectedly succeeded despite damaged journal evidence\n";
    return kUnexpectedRecoveryExitCode;
}

int runLockBlocked(const autoupdater::ApplyPlan& plan) {
    const auto result = autoupdater::updater::executeApplyPlanWithDependencies(plan, dependencies());
    if (!result && result.error().code == autoupdater::ErrorCode::ApplyFailed &&
        result.error().message == "Another update appears to be running") {
        return 0;
    }
    std::cerr << "apply was not rejected by the active update lock";
    if (!result) {
        std::cerr << ": " << autoupdater::toString(result.error().code) << ": " << result.error().message;
    }
    std::cerr << "\n";
    return kUnexpectedRecoveryExitCode;
}

int runRecovery(const autoupdater::ApplyPlan& plan, const std::filesystem::path& launchMarker = {},
                bool expectOperatorIntervention = false, bool failLaunch = false) {
    const auto result =
        autoupdater::updater::executeApplyPlanWithDependencies(plan, dependencies(launchMarker, failLaunch));
    const bool active = std::filesystem::exists(plan.installDir / ".autoupdater" / "journal" / "active.json");
    if (expectOperatorIntervention) {
        if (!result && result.error().code == autoupdater::ErrorCode::ApplyFailed && active) {
            return 0;
        }
        std::cerr << "recovery did not retain the transaction for operator intervention";
        if (!result) {
            std::cerr << ": " << autoupdater::toString(result.error().code) << ": " << result.error().message;
        }
        std::cerr << "\n";
        return kUnexpectedRecoveryExitCode;
    }
    if (result && !active) {
        return 0;
    }

    // Recovery intentionally reports ApplyFailed after restoring the prior file
    // state. An interrupted restart similarly reports ApplyLaunchFailed after
    // recording OutcomeUnknown. Both are accepted only after active cleanup.
    if (!result && !active &&
        (result.error().code == autoupdater::ErrorCode::ApplyFailed ||
         result.error().code == autoupdater::ErrorCode::ApplyLaunchFailed)) {
        return 0;
    }
    if (result) {
        std::cerr << "recovery returned success but left an active transaction\n";
    } else {
        std::cerr << "recovery failed: " << autoupdater::toString(result.error().code) << ": " << result.error().message
                  << "\n";
    }
    return kUnexpectedRecoveryExitCode;
}

void printUsage() {
    std::cerr << "usage:\n"
                 "  libAutoUpdater_apply_crash_helper --crash <plan.json> <checkpoint> <operation-index|any>\n"
                 "  libAutoUpdater_apply_crash_helper --crash-marker <plan.json> <checkpoint> "
                 "<operation-index|any> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --crash-marker-fail <plan.json> <checkpoint> "
                 "<operation-index|any> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --fail <plan.json> <checkpoint> <operation-index|any>\n"
                 "  libAutoUpdater_apply_crash_helper --fail-marker <plan.json> <checkpoint> "
                 "<operation-index|any> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --fail-marker-fail <plan.json> <checkpoint> "
                 "<operation-index|any> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --recover <plan.json>\n"
                 "  libAutoUpdater_apply_crash_helper --recover-marker <plan.json> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --recover-marker-fail <plan.json> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --recover-intervention <plan.json> <marker>\n"
                 "  libAutoUpdater_apply_crash_helper --expect-blocked <plan.json>\n"
                 "  libAutoUpdater_apply_crash_helper --expect-lock-blocked <plan.json>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return 64;
    }

    const auto plan = readPlan(std::filesystem::u8path(argv[2]));
    if (!plan) {
        std::cerr << "failed to read apply plan: " << autoupdater::toString(plan.error().code) << ": "
                  << plan.error().message << "\n";
        return 65;
    }

    const std::string mode = argv[1];
    if (mode == "--crash" && argc == 5) {
        return runCrash(plan.value(), argv[3], argv[4]);
    }
    if (mode == "--crash-marker" && argc == 6) {
        return runCrash(plan.value(), argv[3], argv[4], std::filesystem::u8path(argv[5]));
    }
    if (mode == "--crash-marker-fail" && argc == 6) {
        return runCrash(plan.value(), argv[3], argv[4], std::filesystem::u8path(argv[5]), true);
    }
    if (mode == "--fail" && argc == 5) {
        return runInjectedFailure(plan.value(), argv[3], argv[4]);
    }
    if (mode == "--fail-marker" && argc == 6) {
        return runInjectedFailure(plan.value(), argv[3], argv[4], std::filesystem::u8path(argv[5]));
    }
    if (mode == "--fail-marker-fail" && argc == 6) {
        return runInjectedFailure(plan.value(), argv[3], argv[4], std::filesystem::u8path(argv[5]), true);
    }
    if (mode == "--recover" && argc == 3) {
        return runRecovery(plan.value());
    }
    if (mode == "--recover-marker" && argc == 4) {
        return runRecovery(plan.value(), std::filesystem::u8path(argv[3]));
    }
    if (mode == "--recover-marker-fail" && argc == 4) {
        return runRecovery(plan.value(), std::filesystem::u8path(argv[3]), false, true);
    }
    if (mode == "--recover-intervention" && argc == 4) {
        return runRecovery(plan.value(), std::filesystem::u8path(argv[3]), true);
    }
    if (mode == "--expect-blocked" && argc == 3) {
        return runBlocked(plan.value());
    }
    if (mode == "--expect-lock-blocked" && argc == 3) {
        return runLockBlocked(plan.value());
    }
    printUsage();
    return 64;
}
