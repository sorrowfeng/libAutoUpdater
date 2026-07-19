#include "ApplyLauncher.h"

#include "ProcessWait.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

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
                                const std::string& applyPlanDigest, ApplyLaunchIntent intent,
                                IProcessLauncher& processLauncher) {
    if (config.updaterExecutable.empty()) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "updaterExecutable is required"});
    }
    if (!util::isLowerHexSha256(applyPlanDigest)) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "A valid apply-plan digest is required"});
    }
    if (!detail::validProcessWaitTimeout(config.applyWaitTimeout)) {
        return Result<void>::fail({ErrorCode::ApplyLaunchFailed, "applyWaitTimeout is outside the safe range"});
    }

    ProcessLaunchRequest request;
    request.executable = config.updaterExecutable;
    request.workingDirectory = config.installDir;
    request.detached = true;
    request.arguments = {"--plan",
                         util::pathToUtf8(applyPlanPath),
                         "--plan-sha256",
                         applyPlanDigest,
                         "--install-root",
                         util::pathToUtf8(config.installDir),
                         "--pid",
                         std::to_string(currentProcessId()),
                         "--wait",
                         std::to_string(config.applyWaitTimeout.count())};
    if (intent == ApplyLaunchIntent::Rollback) {
        request.arguments.push_back("--rollback");
    }
    return processLauncher.launch(request);
}

} // namespace autoupdater
