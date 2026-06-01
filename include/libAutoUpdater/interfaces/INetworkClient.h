#pragma once

#include "libAutoUpdater/Config.h"
#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/Types.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace autoupdater {

struct DownloadResumeInfo {
    std::uint64_t offset = 0;
    std::string etag;
    std::string lastModified;
};

struct DownloadResult {
    std::string etag;
    std::string lastModified;
    std::uint64_t bytesWritten = 0;
};

class INetworkClient {
public:
    virtual ~INetworkClient() = default;

    virtual Result<std::string> getText(
        const std::string& url,
        const NetworkOptions& options,
        CancellationToken& cancel) noexcept = 0;

    virtual Result<DownloadResult> downloadToFile(
        const std::string& url,
        const std::filesystem::path& target,
        const NetworkOptions& options,
        const std::optional<DownloadResumeInfo>& resume,
        ProgressCallback progress,
        CancellationToken& cancel) noexcept = 0;
};

std::shared_ptr<INetworkClient> createDefaultNetworkClient();

} // namespace autoupdater
