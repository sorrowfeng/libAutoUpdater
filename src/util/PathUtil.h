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
std::filesystem::path defaultStagingRoot(const std::filesystem::path& installDir);

// True when `value` is a non-empty identifier made only of [A-Za-z0-9._+-]
// and is not "." or "..". Suitable for values interpolated into file names
// such as release ids or version strings.
bool isSafePathComponent(const std::string& value) noexcept;

// True when `value` is exactly 64 lowercase-or-uppercase hex characters.
bool isValidSha256Hex(const std::string& value) noexcept;

// Replaces characters that are unsafe in file names ('+', '/', '\', ':')
// with '_'. Used to derive directory names from version strings.
std::string sanitizePathComponent(std::string value);

} // namespace autoupdater::util
