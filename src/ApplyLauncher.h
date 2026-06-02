#pragma once

#include "ApplyPlanWriter.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"

namespace autoupdater {

Result<void> launchApplyProcess(const Config& config, const std::filesystem::path& applyPlanPath,
                                IProcessLauncher& processLauncher);

} // namespace autoupdater
