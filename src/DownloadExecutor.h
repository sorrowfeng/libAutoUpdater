#pragma once

#include "UpdateTypes.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

namespace autoupdater {

namespace detail {

/// Credential-free identities used only for persistent resume metadata.
Result<std::string> downloadResumeReleaseKey(const Config& config, const UpdateDecision& decision) noexcept;
Result<std::string> downloadResumeResourceKey(const std::string& releaseKey, const PlannedDownload& download) noexcept;

} // namespace detail

Result<void> executeDownloads(const Config& config, const UpdateDecision& decision, INetworkClient& network,
                              IFileSystem& fileSystem, IHashProvider& hashProvider, IStateStore* stateStore,
                              ProgressCallback progress, CancellationToken& cancel);

} // namespace autoupdater
