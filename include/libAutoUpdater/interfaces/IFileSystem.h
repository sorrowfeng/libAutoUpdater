#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace autoupdater {

/// Filesystem abstraction used by core logic and tests.
class IFileSystem {
  public:
    virtual ~IFileSystem() = default;

    virtual bool exists(const std::filesystem::path& path) noexcept = 0;
    virtual bool isRegularFile(const std::filesystem::path& path) noexcept = 0;
    virtual Result<std::uint64_t> fileSize(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> createDirectories(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> copyFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                  bool overwrite) noexcept = 0;
    virtual Result<void> renameOrReplace(const std::filesystem::path& from,
                                         const std::filesystem::path& to) noexcept = 0;
    virtual Result<void> remove(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> removeAll(const std::filesystem::path& path) noexcept = 0;
    /// Reads at most maxBytes and fails before retaining any byte beyond that
    /// limit, including when the file grows after metadata inspection.
    virtual Result<std::string> readText(const std::filesystem::path& path, std::uint64_t maxBytes) noexcept = 0;
    virtual Result<void> writeText(const std::filesystem::path& path, const std::string& text) noexcept = 0;
    virtual Result<std::unique_ptr<IRootedDirectory>>
    openRoot(const std::filesystem::path& path, RootAccess access, bool create,
             RootedDirectoryCreationMode directoryMode = RootedDirectoryCreationMode::Private) noexcept = 0;
};

std::shared_ptr<IFileSystem> createDefaultFileSystem();

} // namespace autoupdater
