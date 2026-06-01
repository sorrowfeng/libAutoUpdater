#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "libAutoUpdater/Result.h"

namespace autoupdater::util {

std::string sha256Bytes(std::string_view data);
Result<std::string> sha256File(const std::filesystem::path& path) noexcept;

} // namespace autoupdater::util

