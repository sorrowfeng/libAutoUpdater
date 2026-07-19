#include "libAutoUpdater/interfaces/INetworkClient.h"

#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace autoupdater {

namespace {

Result<std::filesystem::path> localPathFromUrl(const std::string& url) {
    if (util::isFileUrl(url)) {
        return Result<std::filesystem::path>::ok(util::fileUrlToPath(url));
    }
    return Result<std::filesystem::path>::fail(
        {ErrorCode::NetworkError, "The basic network adapter accepts only explicit file: URLs"});
}

class BasicNetworkClient final : public INetworkClient {
  public:
    Result<TextResponse> getText(const std::string& url, const NetworkOptions&,
                                 CancellationToken& cancel) noexcept override {
        if (cancel.isCancelled()) {
            return Result<TextResponse>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }
        auto path = localPathFromUrl(url);
        if (!path) {
            return Result<TextResponse>::fail(path.error());
        }
        try {
            std::ifstream input(path.value(), std::ios::binary);
            if (!input) {
                return Result<TextResponse>::fail({ErrorCode::ManifestDownloadFailed, "Failed to open local source"});
            }
            std::ostringstream stream;
            stream << input.rdbuf();
            if (input.bad()) {
                return Result<TextResponse>::fail({ErrorCode::NetworkError, "Failed to read local source"});
            }
            TextResponse response;
            response.response.statusCode = 200;
            response.response.effectiveUrl = url;
            response.body = stream.str();
            return Result<TextResponse>::ok(std::move(response));
        } catch (...) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Failed to read local source"});
        }
    }

    Result<DownloadResult> downloadToFile(const std::string& url, IRootedFile& target, const NetworkOptions& options,
                                          const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                          CancellationToken& cancel) noexcept override {
        auto source = localPathFromUrl(url);
        if (!source) {
            return Result<DownloadResult>::fail(source.error());
        }
        try {
            std::error_code ec;
            const auto total = std::filesystem::file_size(source.value(), ec);
            if (ec) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, ec.message()});
            }
            std::ifstream input(source.value(), std::ios::binary);
            const bool appending = options.enableResume && resume && resume->offset > 0;
            if (appending) {
                input.seekg(static_cast<std::streamoff>(resume->offset), std::ios::beg);
            }
            if (!input) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open local source"});
            }

            std::array<char, 64 * 1024> buffer{};
            std::uint64_t written = appending ? resume->offset : 0;
            while (input) {
                if (cancel.isCancelled()) {
                    return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
                }
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0) {
                    auto write = target.write(buffer.data(), static_cast<std::size_t>(count));
                    if (!write) {
                        return Result<DownloadResult>::fail(write.error());
                    }
                    written += static_cast<std::uint64_t>(count);
                    if (progress) {
                        progress({written, static_cast<std::uint64_t>(total), {}});
                    }
                }
            }
            if (input.bad()) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to read local source"});
            }

            DownloadResult result;
            result.response.statusCode = 200;
            result.response.effectiveUrl = url;
            result.bytesWritten = written;
            return Result<DownloadResult>::ok(result);
        } catch (...) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to copy local source"});
        }
    }
};

} // namespace

#if !defined(LIBAUTOUPDATER_HAS_CURL) && !defined(LIBAUTOUPDATER_HAS_WINHTTP) && !defined(LIBAUTOUPDATER_HAS_CFNETWORK)
std::shared_ptr<INetworkClient> createDefaultNetworkClient() {
    return std::make_shared<BasicNetworkClient>();
}
#endif

} // namespace autoupdater
