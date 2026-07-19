#include "default/LocalNetworkFile.h"

#include "NetworkLimits.h"
#include "util/UrlUtil.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>

namespace autoupdater::detail {

namespace {

Result<std::filesystem::path> localPathFromUrl(const std::string& url) {
    if (util::isFileUrl(url)) {
        return Result<std::filesystem::path>::ok(util::fileUrlToPath(url));
    }
    return Result<std::filesystem::path>::fail(
        {ErrorCode::NetworkError, "Local network access requires an explicit file: URL"});
}

} // namespace

Result<TextResponse> readLocalText(const std::string& url, std::uint64_t maxResponseBytes,
                                   CancellationToken& cancel) noexcept {
    if (cancel.isCancelled()) {
        return Result<TextResponse>::fail({ErrorCode::Cancelled, "Operation cancelled"});
    }
    auto path = localPathFromUrl(url);
    if (!path) {
        return Result<TextResponse>::fail(path.error());
    }

    try {
        std::error_code error;
        const auto declaredSize = std::filesystem::file_size(path.value(), error);
        if (error) {
            return Result<TextResponse>::fail({ErrorCode::ManifestDownloadFailed, error.message()});
        }
        if (declaredSize > maxResponseBytes) {
            return Result<TextResponse>::fail(
                {ErrorCode::ResourceLimitExceeded, "Local response exceeds its byte limit"});
        }

        std::ifstream input(path.value(), std::ios::binary);
        if (!input) {
            return Result<TextResponse>::fail({ErrorCode::ManifestDownloadFailed, "Failed to open local source"});
        }

        TextResponse response;
        response.response.statusCode = 200;
        response.response.effectiveUrl = url;
        std::array<char, 64 * 1024> buffer{};
        std::uint64_t consumed = 0;
        for (;;) {
            if (cancel.isCancelled()) {
                return Result<TextResponse>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) {
                break;
            }
            const auto bytes = static_cast<std::uint64_t>(count);
            if (bytes > maxResponseBytes - consumed) {
                return Result<TextResponse>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Local response exceeds its byte limit"});
            }
            response.body.append(buffer.data(), static_cast<std::size_t>(count));
            consumed += bytes;
        }
        if (input.bad()) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Failed to read local source"});
        }
        return Result<TextResponse>::ok(std::move(response));
    } catch (...) {
        return Result<TextResponse>::fail({ErrorCode::NetworkError, "Failed to read local source"});
    }
}

Result<DownloadResult> copyLocalToFile(const std::string& url, IRootedFile& target, std::uint64_t maxTotalBytes,
                                       const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                       CancellationToken& cancel) noexcept {
    auto source = localPathFromUrl(url);
    if (!source) {
        return Result<DownloadResult>::fail(source.error());
    }

    try {
        const auto initialBytes = resume ? resume->offset : 0;
        auto remaining = remainingTransferBudget(initialBytes, maxTotalBytes);
        if (!remaining) {
            return Result<DownloadResult>::fail(remaining.error());
        }
        if (initialBytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return Result<DownloadResult>::fail(
                {ErrorCode::ResourceLimitExceeded, "Resume offset exceeds the local stream range"});
        }

        std::error_code error;
        const auto sourceSize = std::filesystem::file_size(source.value(), error);
        if (error) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, error.message()});
        }
        if (sourceSize > maxTotalBytes || initialBytes > sourceSize) {
            return Result<DownloadResult>::fail(
                {ErrorCode::ResourceLimitExceeded, "Local artifact exceeds its signed byte limit"});
        }

        std::ifstream input(source.value(), std::ios::binary);
        if (initialBytes > 0) {
            input.seekg(static_cast<std::streamoff>(initialBytes), std::ios::beg);
        }
        if (!input) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open local source"});
        }

        std::array<char, 64 * 1024> buffer{};
        std::uint64_t written = initialBytes;
        for (;;) {
            if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) {
                break;
            }
            const auto bytes = static_cast<std::uint64_t>(count);
            if (bytes > maxTotalBytes - written) {
                return Result<DownloadResult>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Local artifact exceeds its signed byte limit"});
            }
            auto result = target.write(buffer.data(), static_cast<std::size_t>(count));
            if (!result) {
                return Result<DownloadResult>::fail(result.error());
            }
            written += bytes;
            if (progress) {
                progress({written, maxTotalBytes, {}});
            }
        }
        if (input.bad()) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to read local source"});
        }

        DownloadResult result;
        result.response.statusCode = 200;
        result.response.effectiveUrl = url;
        result.bytesWritten = written;
        return Result<DownloadResult>::ok(std::move(result));
    } catch (...) {
        return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to copy local source"});
    }
}

} // namespace autoupdater::detail
