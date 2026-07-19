#include "libAutoUpdater/interfaces/INetworkClient.h"

#include "default/LocalNetworkFile.h"

#include <utility>

namespace autoupdater {

namespace {

class BasicNetworkClient final : public INetworkClient {
  public:
    Result<TextResponse> getText(const std::string& url, const NetworkOptions&, std::uint64_t maxResponseBytes,
                                 CancellationToken& cancel) noexcept override {
        return detail::readLocalText(url, maxResponseBytes, cancel);
    }

    Result<DownloadResult> downloadToFile(const std::string& url, IRootedFile& target, const NetworkOptions& options,
                                          std::uint64_t maxTotalBytes, const std::optional<DownloadResumeInfo>& resume,
                                          ProgressCallback progress, CancellationToken& cancel) noexcept override {
        const auto effectiveResume = options.enableResume ? resume : std::nullopt;
        return detail::copyLocalToFile(url, target, maxTotalBytes, effectiveResume, std::move(progress), cancel);
    }
};

} // namespace

#if !defined(LIBAUTOUPDATER_HAS_CURL) && !defined(LIBAUTOUPDATER_HAS_WINHTTP) && !defined(LIBAUTOUPDATER_HAS_CFNETWORK)
std::shared_ptr<INetworkClient> createDefaultNetworkClient() {
    return std::make_shared<BasicNetworkClient>();
}
#endif

} // namespace autoupdater
