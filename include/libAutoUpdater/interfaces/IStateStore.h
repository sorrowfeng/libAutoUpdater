#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/ResourceLimits.h"
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
    /// Canonical SHA-256 of the exact forward apply plan authorized by the
    /// terminal transaction receipt.
    std::string applyPlanDigest;
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

    /// Atomically records a healthy running version and clears the exact
    /// pending update observed by the caller. Implementations must fail when
    /// the persisted pending value no longer matches expectedPending.
    virtual Result<void> commitHealthyVersion(const Version& version, const std::string& releaseId,
                                              const std::optional<PendingUpdate>& expectedPending) noexcept = 0;

    /// Atomically creates the pending update when none exists. Saving the
    /// exact persisted value again is an idempotent success; a different
    /// existing value must fail without changing persistent state.
    virtual Result<void> savePendingUpdate(const PendingUpdate& pending) noexcept = 0;
    virtual Result<std::optional<PendingUpdate>> loadPendingUpdate() noexcept = 0;

    /// Unconditionally clears any pending update.
    virtual Result<void> clearPendingUpdate() noexcept = 0;

    virtual Result<void> saveDownloadResume(const DownloadResumeState& state) noexcept = 0;
    virtual Result<std::optional<DownloadResumeState>> loadDownloadResume(const std::string& key) noexcept = 0;
    virtual Result<void> clearDownloadResume(const std::string& key) noexcept = 0;
};

/// Optional capability implemented by state stores that can atomically clear
/// an exact pending value. It is separate from IStateStore so existing custom
/// implementations keep their source and virtual-table contract. Updater
/// fails closed when rollback reconciliation needs this capability and the
/// configured store does not provide it.
class IPendingUpdateCompareAndSet {
  public:
    virtual ~IPendingUpdateCompareAndSet() = default;

    /// Atomically clears the pending update only when every persisted field
    /// matches expectedPending. Missing or mismatched state must fail without
    /// changing persistent state.
    virtual Result<void> clearPendingUpdateIfMatches(const PendingUpdate& expectedPending) noexcept = 0;
};

std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path);
std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path, const ResourceLimits& limits);

} // namespace autoupdater
