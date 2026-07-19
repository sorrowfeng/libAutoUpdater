#pragma once

#include "UrlPolicy.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <optional>
#include <string>

namespace autoupdater {

struct RedirectedTextResult {
    std::string body;
    std::string effectiveUrl;
    NetworkResponseInfo response;
};

struct RedirectedDownloadResult {
    DownloadResult download;
    std::string effectiveUrl;
};

Result<RedirectedTextResult> fetchTextWithRedirects(const std::string& initialUrl, const NetworkOptions& options,
                                                    const UrlPolicy& policy, INetworkClient& network,
                                                    CancellationToken& cancel) noexcept;

Result<RedirectedDownloadResult> downloadWithRedirects(const std::string& initialUrl, IRootedFile& target,
                                                       const NetworkOptions& options, const UrlPolicy& policy,
                                                       INetworkClient& network,
                                                       const std::optional<DownloadResumeInfo>& resume,
                                                       ProgressCallback progress, CancellationToken& cancel) noexcept;

} // namespace autoupdater
