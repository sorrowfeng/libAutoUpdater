#pragma once

#include "libAutoUpdater/interfaces/IRootedFileSystem.h"

#include <filesystem>
#include <memory>

namespace autoupdater {

Result<std::unique_ptr<IRootedDirectory>>
openDefaultRootedDirectory(const std::filesystem::path& path, RootAccess access, bool create,
                           RootedDirectoryCreationMode directoryMode) noexcept;

} // namespace autoupdater
