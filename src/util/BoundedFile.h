#pragma once

#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace autoupdater::util {

Result<std::string> readRegularFileWithLimit(const std::filesystem::path& path, std::uint64_t maxBytes,
                                             ErrorCode readErrorCode, const std::string& description) noexcept;

} // namespace autoupdater::util
