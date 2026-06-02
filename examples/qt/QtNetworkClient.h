#pragma once

#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <QNetworkAccessManager>
#include <QObject>

class QtNetworkClient final : public QObject, public autoupdater::INetworkClient {
    Q_OBJECT

  public:
    explicit QtNetworkClient(QObject* parent = nullptr);

    autoupdater::Result<std::string> getText(const std::string& url, const autoupdater::NetworkOptions& options,
                                             autoupdater::CancellationToken& cancel) noexcept override;

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string& url, const std::filesystem::path& target,
                   const autoupdater::NetworkOptions& options,
                   const std::optional<autoupdater::DownloadResumeInfo>& resume, autoupdater::ProgressCallback progress,
                   autoupdater::CancellationToken& cancel) noexcept override;

  private:
    QNetworkAccessManager manager_;
};
