#include "ApplyExecutor.h"

#include "libAutoUpdater/ApplyPlan.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

struct Args {
    std::filesystem::path planPath;
    std::uint64_t pid = 0;
    int waitSeconds = 60;
};

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--plan" && i + 1 < argc) {
            args.planPath = argv[++i];
        } else if (arg == "--pid" && i + 1 < argc) {
            args.pid = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--wait" && i + 1 < argc) {
            args.waitSeconds = std::atoi(argv[++i]);
        } else if (arg == "--help") {
            return false;
        }
    }
    return !args.planPath.empty();
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
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
