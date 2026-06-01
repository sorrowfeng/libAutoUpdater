#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/Version.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace autoupdater {

struct PendingUpdate {
    Version version;
    std::string releaseId;
    std::filesystem::path backupDir;
    std::filesystem::path applyPlanPath;
};

class IStateStore {
public:
    virtual ~IStateStore() = default;

    virtual Result<void> saveLastAcceptedVersion(const Version& version,
                                                 const std::string& releaseId) noexcept = 0;
    virtual Result<std::optional<Version>> loadLastAcceptedVersion() noexcept = 0;
    virtual Result<std::string> loadLastAcceptedReleaseId() noexcept = 0;

    virtual Result<void> savePendingUpdate(const PendingUpdate& pending) noexcept = 0;
    virtual Result<std::optional<PendingUpdate>> loadPendingUpdate() noexcept = 0;
    virtual Result<void> clearPendingUpdate() noexcept = 0;
};

std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path);

} // namespace autoupdater
