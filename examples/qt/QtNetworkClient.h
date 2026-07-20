#pragma once

#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <QObject>

#include <atomic>
class QNetworkAccessManager;

class QtNetworkClient final : public QObject, public autoupdater::INetworkClient {
    Q_OBJECT

  public:
    explicit QtNetworkClient(QObject* parent = nullptr);
    ~QtNetworkClient() override;

    autoupdater::Result<autoupdater::TextResponse> getText(const std::string& url,
                                                           const autoupdater::NetworkOptions& options,
                                                           std::uint64_t maxResponseBytes,
                                                           autoupdater::CancellationToken& cancel) noexcept override;

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions& options,
                   std::uint64_t maxTotalBytes, const std::optional<autoupdater::DownloadResumeInfo>& resume,
                   autoupdater::ProgressCallback progress, autoupdater::CancellationToken& cancel) noexcept override;

  private:
    autoupdater::Result<autoupdater::TextResponse>
    getTextOnOwnerThread(const std::string& url, const autoupdater::NetworkOptions& options,
                         std::uint64_t maxResponseBytes, autoupdater::CancellationToken& cancel) noexcept;

    autoupdater::Result<autoupdater::DownloadResult> downloadToFileOnOwnerThread(
        const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions& options,
        std::uint64_t maxTotalBytes, const std::optional<autoupdater::DownloadResumeInfo>& resume,
        autoupdater::ProgressCallback progress, autoupdater::CancellationToken& cancel) noexcept;

    QNetworkAccessManager* manager_ = nullptr;
    std::atomic_bool shuttingDown_{false};
    std::atomic_bool requestActive_{false};
};
