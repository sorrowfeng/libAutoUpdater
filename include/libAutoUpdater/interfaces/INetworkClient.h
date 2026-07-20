#pragma once

#include "libAutoUpdater/Config.h"
#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/Types.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace autoupdater {

struct DownloadResumeInfo {
    std::uint64_t offset = 0;
    std::string etag;
    std::string lastModified;
};

struct NetworkHeader {
    std::string name;
    std::string value;
};

/// Metadata for exactly one transport request. Network adapters must disable
/// automatic redirects and preserve the response URL and headers for policy
/// validation by the core library.
struct NetworkResponseInfo {
    int statusCode = 0;
    std::vector<NetworkHeader> headers;
    std::string effectiveUrl;
};

struct TextResponse {
    NetworkResponseInfo response;
    std::string body;
};

/// Download result metadata used for future resume requests.
struct DownloadResult {
    NetworkResponseInfo response;
    std::string etag;
    std::string lastModified;
    std::uint64_t bytesWritten = 0;
};

/// Network abstraction for manifest fetches and file downloads.
/// Implementations must not place request URLs, authorization values, response
/// bodies, signatures, keys, or other credential material in Error::message.
class INetworkClient {
  public:
    virtual ~INetworkClient() = default;

    /// Performs exactly one request. Received HTTP responses, including 3xx,
    /// 4xx, and 5xx, are returned with status and headers; only transport or
    /// local IO failures fail the Result. Implementors must stream the body,
    /// honor positive NetworkOptions timeout bounds, and arrange transport reads
    /// so those bounds and cooperative cancellation can be observed between
    /// bounded waits. They must stop before buffering more than
    /// maxResponseBytes. Response metadata must also be bounded.
    virtual Result<TextResponse> getText(const std::string& url, const NetworkOptions& options,
                                         std::uint64_t maxResponseBytes, CancellationToken& cancel) noexcept = 0;

    /// Performs exactly one request without following redirects. Implementors
    /// must not write an HTTP response body unless the status is 200 for a
    /// full request or 206 for a resume request. Explicit local file adapters
    /// synthesize status 200 and may honor a supplied offset directly.
    /// maxTotalBytes is the signed expected artifact size, including any bytes
    /// already present for a resume request. Implementors must honor positive
    /// NetworkOptions timeout bounds and arrange transport reads so those bounds
    /// and cooperative cancellation can be observed between bounded waits. They
    /// must stop before a write would make the target exceed it. Progress
    /// callbacks report maxTotalBytes as totalBytes throughout the transfer and
    /// include any accepted resume offset in downloadedBytes.
    virtual Result<DownloadResult> downloadToFile(const std::string& url, IRootedFile& target,
                                                  const NetworkOptions& options, std::uint64_t maxTotalBytes,
                                                  const std::optional<DownloadResumeInfo>& resume,
                                                  ProgressCallback progress, CancellationToken& cancel) noexcept = 0;
};

std::shared_ptr<INetworkClient> createDefaultNetworkClient();

} // namespace autoupdater
