#pragma once

#include "UpdateTypes.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

namespace autoupdater {

struct WrittenApplyPlan {
    ApplyPlan plan;
    std::filesystem::path path;
};

Result<WrittenApplyPlan> writeApplyPlan(const Config& config,
                                        const ManifestEnvelope& envelope,
                                        const UpdateDecision& decision,
                                        IFileSystem& fileSystem);

} // namespace autoupdater

