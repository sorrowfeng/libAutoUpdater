#pragma once

#include "ApplyPlanWriter.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"

namespace autoupdater {

enum class ApplyLaunchIntent { Install, Rollback };

Result<void> launchApplyProcess(const Config& config, const std::filesystem::path& applyPlanPath,
                                const std::string& applyPlanDigest, ApplyLaunchIntent intent,
                                IProcessLauncher& processLauncher);

} // namespace autoupdater
