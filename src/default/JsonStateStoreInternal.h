#pragma once

#include "libAutoUpdater/ResourceLimits.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace autoupdater::detail {

enum class JsonStateStoreCheckpoint {
    BackupCommitted,
    PrimaryCommitted,
};

struct JsonStateStoreHooks {
    std::function<void(JsonStateStoreCheckpoint)> checkpoint;
};

/// Internal construction seam used by deterministic persistence and crash
/// tests. Production callers use createJsonStateStore().
std::shared_ptr<IStateStore> createJsonStateStoreForTesting(const std::filesystem::path& path,
                                                            const ResourceLimits& limits,
                                                            std::shared_ptr<IFileSystem> fileSystem,
                                                            JsonStateStoreHooks hooks = {});

} // namespace autoupdater::detail
