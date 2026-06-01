#include "libAutoUpdater/interfaces/INetworkClient.h"

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
    if (url.find("://") == std::string::npos) {
        return Result<std::filesystem::path>::ok(url);
    }
    return Result<std::filesystem::path>::fail({ErrorCode::NetworkError, "No HTTP network adapter is available"});
}

class BasicNetworkClient final : public INetworkClient {
public:
    Result<std::string> getText(const std::string& url,
                                const NetworkOptions&,
                                CancellationToken& cancel) noexcept override {
        if (cancel.isCancelled()) {
            return Result<std::string>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }
        auto path = localPathFromUrl(url);
        if (!path) {
            return Result<std::string>::fail(path.error());
        }
        try {
            std::ifstream input(path.value(), std::ios::binary);
            if (!input) {
                return Result<std::string>::fail({ErrorCode::ManifestDownloadFailed, "Failed to open local source"});
            }
            std::ostringstream stream;
            stream << input.rdbuf();
            return Result<std::string>::ok(stream.str());
        } catch (...) {
            return Result<std::string>::fail({ErrorCode::NetworkError, "Failed to read local source"});
        }
    }

    Result<DownloadResult> downloadToFile(const std::string& url,
                                          const std::filesystem::path& target,
                                          const NetworkOptions&,
                                          const std::optional<DownloadResumeInfo>&,
                                          ProgressCallback progress,
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
            if (!target.parent_path().empty()) {
                std::filesystem::create_directories(target.parent_path(), ec);
                if (ec) {
                    return Result<DownloadResult>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }

            std::ifstream input(source.value(), std::ios::binary);
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            if (!input || !output) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open local download streams"});
            }

            std::array<char, 64 * 1024> buffer{};
            std::uint64_t written = 0;
            while (input) {
                if (cancel.isCancelled()) {
                    return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
                }
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0) {
                    output.write(buffer.data(), count);
                    written += static_cast<std::uint64_t>(count);
                    if (progress) {
                        progress({written, static_cast<std::uint64_t>(total), target.generic_string()});
                    }
                }
            }

            DownloadResult result;
            result.bytesWritten = written;
            return Result<DownloadResult>::ok(result);
        } catch (...) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to copy local source"});
        }
    }
};

} // namespace

#ifndef LIBAUTOUPDATER_HAS_CURL
std::shared_ptr<INetworkClient> createDefaultNetworkClient() {
    return std::make_shared<BasicNetworkClient>();
}
#endif

} // namespace autoupdater
