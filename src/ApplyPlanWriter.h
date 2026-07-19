#pragma once

#include "UpdateTypes.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

namespace autoupdater {

struct WrittenApplyPlan {
    ApplyPlan plan;
    std::filesystem::path path;
    std::string digest;
};

Result<WrittenApplyPlan> writeApplyPlan(const Config& config, const ManifestEnvelope& envelope,
                                        const UpdateDecision& decision, IFileSystem& fileSystem);
Result<WrittenApplyPlan> writeRollbackRequestPlan(const Config& config, const PendingUpdate& pending,
                                                  IFileSystem& fileSystem);

} // namespace autoupdater
