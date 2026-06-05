#pragma once

#include "libAutoUpdater/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace autoupdater::util {

std::filesystem::path pathFromUtf8(const std::string& utf8Path) noexcept;
std::string pathToUtf8(const std::filesystem::path& path) noexcept;
Result<void> validateManagedPath(const std::string& path) noexcept;
Result<std::filesystem::path> safeJoin(const std::filesystem::path& root, const std::string& relativePath) noexcept;
bool pathAllowedByWhitelist(const std::string& path, const std::vector<std::string>& whitelist) noexcept;
std::string normalizeManifestPath(std::string path);
std::filesystem::path defaultStagingRoot(const std::filesystem::path& installDir);

} // namespace autoupdater::util
