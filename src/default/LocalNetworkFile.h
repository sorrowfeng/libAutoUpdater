#pragma once

#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <cstdint>

namespace autoupdater::detail {

Result<TextResponse> readLocalText(const std::string& url, std::uint64_t maxResponseBytes,
                                   CancellationToken& cancel) noexcept;

Result<DownloadResult> copyLocalToFile(const std::string& url, IRootedFile& target, std::uint64_t maxTotalBytes,
                                       const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                       CancellationToken& cancel) noexcept;

} // namespace autoupdater::detail
