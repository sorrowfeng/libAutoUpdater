#include "ApplyLauncher.h"

#include "util/PathUtil.h"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace autoupdater {

namespace {

std::uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

Result<std::filesystem::path> prepareLauncherCopy(const Config& config) {
    const auto source = config.updaterExecutable;
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || !std::filesystem::is_regular_file(source, ec)) {
        return Result<std::filesystem::path>::fail({ErrorCode::ApplyLaunchFailed, "updaterExecutable does not exist"});
    }

    const auto fileName = source.filename();
    if (fileName.empty()) {
        return Result<std::filesystem::path>::fail(
            {ErrorCode::ApplyLaunchFailed, "updaterExecutable has no file name"});
    }

    const auto launcherDir = config.tempDir / "launcher";
    std::filesystem::create_directories(launcherDir, ec);
    if (ec) {
        return Result<std::filesystem::path>::fail({ErrorCode::FileSystemError, ec.message()});
    }

    const auto target = launcherDir / fileName;
    if (std::filesystem::exists(target, ec) && std::filesystem::equivalent(source, target, ec)) {
        return Result<std::filesystem::path>::ok(target);
    }

    ec.clear();
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<std::filesystem::path>::fail({ErrorCode::FileSystemError, ec.message()});
    }

#ifndef _WIN32
    auto permissions = std::filesystem::status(source, ec).permissions();
    if (!ec) {
        std::filesystem::permissions(target, permissions, ec);
    }
#endif

    return Result<std::filesystem::path>::ok(target);
}

} // namespace

Result<void> launchApplyProcess(const Config& config, const std::filesystem::path& applyPlanPath,
                                IProcessLauncher& processLauncher) {
    if (config.updaterExecutable.empty()) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "updaterExecutable is required"});
    }
    if (config.tempDir.empty()) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "tempDir is required"});
    }

    auto launcherExecutable = prepareLauncherCopy(config);
    if (!launcherExecutable) {
        return Result<void>::fail(launcherExecutable.error());
    }

    ProcessLaunchRequest request;
    request.executable = launcherExecutable.value();
    request.workingDirectory = config.installDir;
    request.detached = true;
    request.arguments = {"--plan", util::pathToUtf8(applyPlanPath),
                         "--pid",  std::to_string(currentProcessId()),
                         "--wait", std::to_string(config.applyWaitTimeout.count())};
    return processLauncher.launch(request);
}

} // namespace autoupdater
