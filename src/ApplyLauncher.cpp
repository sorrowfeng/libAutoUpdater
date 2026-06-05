#include "ApplyLauncher.h"

#include "util/PathUtil.h"

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

} // namespace

Result<void> launchApplyProcess(const Config& config, const std::filesystem::path& applyPlanPath,
                                IProcessLauncher& processLauncher) {
    if (config.updaterExecutable.empty()) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "updaterExecutable is required"});
    }

    ProcessLaunchRequest request;
    request.executable = config.updaterExecutable;
    request.workingDirectory = config.installDir;
    request.detached = true;
    request.arguments = {"--plan", util::pathToUtf8(applyPlanPath),
                         "--pid",  std::to_string(currentProcessId()),
                         "--wait", std::to_string(config.applyWaitTimeout.count())};
    return processLauncher.launch(request);
}

} // namespace autoupdater
