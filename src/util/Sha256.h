#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "libAutoUpdater/Result.h"

namespace autoupdater {

class IRootedFile;

namespace util {

std::string sha256Bytes(std::string_view data);
Result<std::string> sha256File(const std::filesystem::path& path) noexcept;
Result<std::string> sha256RootedFile(autoupdater::IRootedFile& file) noexcept;

} // namespace util

} // namespace autoupdater
