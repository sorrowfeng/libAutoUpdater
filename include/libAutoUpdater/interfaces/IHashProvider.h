#pragma once

#include "libAutoUpdater/Result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace autoupdater {

class IHashProvider {
public:
    virtual ~IHashProvider() = default;

    virtual Result<std::string> sha256File(const std::filesystem::path& path) noexcept = 0;
    virtual Result<std::string> sha256Bytes(std::string_view data) noexcept = 0;
};

std::shared_ptr<IHashProvider> createDefaultHashProvider();

} // namespace autoupdater
