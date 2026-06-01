#pragma once

#include "UpdateTypes.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"

namespace autoupdater {

Result<void> executeDownloads(const Config& config,
                              const UpdateDecision& decision,
                              INetworkClient& network,
                              IFileSystem& fileSystem,
                              IHashProvider& hashProvider,
                              ProgressCallback progress,
                              CancellationToken& cancel);

} // namespace autoupdater

