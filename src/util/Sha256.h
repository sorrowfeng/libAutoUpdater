#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

#include "libAutoUpdater/Result.h"

namespace autoupdater {

class IRootedFile;

namespace util {

std::string sha256Bytes(std::string_view data);
/// Hash an already-open stream and fail when reading stops for any reason
/// other than a clean EOF. The stream remains owned by the caller.
Result<std::string> sha256Stream(std::istream& input) noexcept;
Result<std::string> sha256File(const std::filesystem::path& path) noexcept;
Result<std::string> sha256RootedFile(autoupdater::IRootedFile& file) noexcept;
bool isLowerHexSha256(std::string_view value) noexcept;

} // namespace util

} // namespace autoupdater
