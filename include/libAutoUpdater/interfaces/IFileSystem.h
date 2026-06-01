#pragma once

#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace autoupdater {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual bool exists(const std::filesystem::path& path) noexcept = 0;
    virtual bool isRegularFile(const std::filesystem::path& path) noexcept = 0;
    virtual Result<std::uint64_t> fileSize(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> createDirectories(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> copyFile(const std::filesystem::path& from,
                                  const std::filesystem::path& to,
                                  bool overwrite) noexcept = 0;
    virtual Result<void> renameOrReplace(const std::filesystem::path& from,
                                         const std::filesystem::path& to) noexcept = 0;
    virtual Result<void> remove(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> removeAll(const std::filesystem::path& path) noexcept = 0;
    virtual Result<std::string> readText(const std::filesystem::path& path) noexcept = 0;
    virtual Result<void> writeText(const std::filesystem::path& path,
                                   const std::string& text) noexcept = 0;
};

std::shared_ptr<IFileSystem> createDefaultFileSystem();

} // namespace autoupdater
