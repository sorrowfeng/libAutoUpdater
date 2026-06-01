#pragma once

#include "UpdateTypes.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"

namespace autoupdater {

Result<LocalSnapshot> buildLocalSnapshot(const Config& config,
                                         const Manifest& manifest,
                                         IFileSystem& fileSystem,
                                         IHashProvider& hashProvider);

} // namespace autoupdater

