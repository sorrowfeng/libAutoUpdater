#pragma once

#include "libAutoUpdater/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace autoupdater {

enum class RootAccess { ReadOnly, ReadWrite };

/// Permissions policy for parent directories created by rooted operations.
/// Private is intended for staging, backup, journal, and state data. InstalledContent
/// creates traversable application directories while retaining safe file defaults.
enum class RootedDirectoryCreationMode { Private, InstalledContent };

enum class RootedFileOpenMode {
    ReadOnly,
    ReadWrite,
    OpenOrCreate,
    CreateOrTruncate,
};

struct RootedFileMetadata {
    std::uint64_t size = 0;
    std::filesystem::perms permissions = std::filesystem::perms::unknown;
    std::string identity;
};

class IRootedFile {
  public:
    virtual ~IRootedFile() = default;

    virtual Result<std::size_t> read(void* buffer, std::size_t size) noexcept = 0;
    virtual Result<void> write(const void* data, std::size_t size) noexcept = 0;
    virtual Result<void> seek(std::uint64_t offset) noexcept = 0;
    virtual Result<void> truncate(std::uint64_t size) noexcept = 0;
    /// Persist file contents and the metadata required to reproduce them using
    /// the strongest durability barrier supported by the backing filesystem.
    virtual Result<void> flush() noexcept = 0;
    virtual Result<RootedFileMetadata> metadata() noexcept = 0;
    virtual Result<void> setPermissions(std::filesystem::perms permissions) noexcept = 0;
};

struct RootedOpenResult {
    std::unique_ptr<IRootedFile> file;

    bool exists() const noexcept {
        return static_cast<bool>(file);
    }
};

enum class RootedEntryExpectationKind { Missing, Identity };

struct RootedEntryExpectation {
    RootedEntryExpectationKind kind = RootedEntryExpectationKind::Missing;
    std::string identity;

    static RootedEntryExpectation missing() {
        return {};
    }

    static RootedEntryExpectation matching(const RootedFileMetadata& metadata) {
        RootedEntryExpectation expectation;
        expectation.kind = RootedEntryExpectationKind::Identity;
        expectation.identity = metadata.identity;
        return expectation;
    }
};

class IRootedTemporaryFile {
  public:
    virtual ~IRootedTemporaryFile() = default;

    virtual IRootedFile& file() noexcept = 0;
    /// Atomically publish the prepared file and persist the affected namespace.
    /// A failure may be reported after the namespace mutation became visible;
    /// transaction callers must reopen and reconcile the target before retrying.
    virtual Result<void> commit(const RootedEntryExpectation& expectation) noexcept = 0;
};

class IRootedLock {
  public:
    /// Holds a non-blocking, cross-process kernel lock for this object's
    /// lifetime. Destroying the object or terminating the owning process must
    /// release ownership; the on-disk marker may remain and does not by itself
    /// indicate that the lock is held.
    virtual ~IRootedLock() = default;
};

class IRootedDirectory {
  public:
    virtual ~IRootedDirectory() = default;

    /// Resolve a portable managed path without following intermediate or leaf links.
    /// Returned file handles remain bound to the object that was opened.
    virtual Result<RootedOpenResult>
    openRegularFile(const std::string& relativePath, RootedFileOpenMode mode,
                    RootedDirectoryCreationMode directoryMode = RootedDirectoryCreationMode::Private) noexcept = 0;
    virtual Result<std::unique_ptr<IRootedTemporaryFile>> createAtomicReplacement(
        const std::string& relativePath,
        RootedDirectoryCreationMode directoryMode = RootedDirectoryCreationMode::Private) noexcept = 0;
    virtual Result<void> replaceWithOpenedFile(IRootedFile& source, const std::string& relativePath,
                                               const RootedEntryExpectation& expectation) noexcept = 0;
    /// Remove the expected entry and persist the affected namespace. A failure
    /// may be reported after removal became visible, so callers must reconcile
    /// the target from durable transaction evidence before retrying.
    virtual Result<void> removeRegularFile(const std::string& relativePath,
                                           const RootedEntryExpectation& expectation) noexcept = 0;
    /// Attempts to acquire an exclusive lock without waiting. Active
    /// contention is reported as ApplyFailed.
    virtual Result<std::unique_ptr<IRootedLock>> acquireExclusiveLock(const std::string& relativePath) noexcept = 0;
};

} // namespace autoupdater
