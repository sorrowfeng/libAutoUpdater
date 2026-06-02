#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/Version.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace autoupdater {

/// Pending update metadata persisted between download and apply/rollback.
struct PendingUpdate {
    Version version;
    std::string releaseId;
    std::filesystem::path backupDir;
    std::filesystem::path applyPlanPath;
};

/// Resume metadata for one partially downloaded file.
struct DownloadResumeState {
    std::string key;
    std::uint64_t offset = 0;
    std::string etag;
    std::string lastModified;
    std::string sha256;
};

/// Persistent state abstraction for anti-replay, pending update, and resume data.
class IStateStore {
  public:
    virtual ~IStateStore() = default;

    virtual Result<void> saveLastAcceptedVersion(const Version& version, const std::string& releaseId) noexcept = 0;
    virtual Result<std::optional<Version>> loadLastAcceptedVersion() noexcept = 0;
    virtual Result<std::string> loadLastAcceptedReleaseId() noexcept = 0;

    virtual Result<void> savePendingUpdate(const PendingUpdate& pending) noexcept = 0;
    virtual Result<std::optional<PendingUpdate>> loadPendingUpdate() noexcept = 0;
    virtual Result<void> clearPendingUpdate() noexcept = 0;

    virtual Result<void> saveDownloadResume(const DownloadResumeState& state) noexcept = 0;
    virtual Result<std::optional<DownloadResumeState>> loadDownloadResume(const std::string& key) noexcept = 0;
    virtual Result<void> clearDownloadResume(const std::string& key) noexcept = 0;
};

std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path);

} // namespace autoupdater
