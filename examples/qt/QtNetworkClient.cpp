#include "QtNetworkClient.h"

#include <QEventLoop>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

QtNetworkClient::QtNetworkClient(QObject* parent) : QObject(parent) {}

autoupdater::Result<std::string> QtNetworkClient::getText(
    const std::string& url,
    const autoupdater::NetworkOptions& options,
    autoupdater::CancellationToken& cancel) noexcept {
    QNetworkRequest request(QUrl(QString::fromStdString(url)));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    auto* reply = manager_.get(request);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        reply->abort();
        loop.quit();
    });
    timer.start(static_cast<int>(options.transferTimeout.count()));
    loop.exec();

    if (cancel.isCancelled()) {
        reply->deleteLater();
        return autoupdater::Result<std::string>::fail({autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
    }
    if (reply->error() != QNetworkReply::NoError) {
        const auto message = reply->errorString().toStdString();
        reply->deleteLater();
        return autoupdater::Result<std::string>::fail({autoupdater::ErrorCode::NetworkError, message});
    }
    const auto bytes = reply->readAll();
    reply->deleteLater();
    return autoupdater::Result<std::string>::ok(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

autoupdater::Result<autoupdater::DownloadResult> QtNetworkClient::downloadToFile(
    const std::string& url,
    const std::filesystem::path& target,
    const autoupdater::NetworkOptions& options,
    const std::optional<autoupdater::DownloadResumeInfo>& resume,
    autoupdater::ProgressCallback progress,
    autoupdater::CancellationToken& cancel) noexcept {
    try {
        std::error_code ec;
        if (!target.parent_path().empty()) {
            std::filesystem::create_directories(target.parent_path(), ec);
        }
        if (ec) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail({autoupdater::ErrorCode::FileSystemError, ec.message()});
        }

        QFile file(QString::fromStdString(target.u8string()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail({autoupdater::ErrorCode::DownloadFailed, "Failed to open target"});
        }

        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        if (options.enableResume && resume && resume->offset > 0) {
            QByteArray range("bytes=");
            range += QByteArray::number(static_cast<qint64>(resume->offset));
            range += "-";
            request.setRawHeader("Range", range);
            if (!resume->etag.empty()) {
                request.setRawHeader("If-Range", QByteArray::fromStdString(resume->etag));
            }
        }

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        auto* reply = manager_.get(request);
        std::uint64_t written = 0;
        QObject::connect(reply, &QNetworkReply::readyRead, [&] {
            const auto data = reply->readAll();
            file.write(data);
            written += static_cast<std::uint64_t>(data.size());
        });
        QObject::connect(reply, &QNetworkReply::downloadProgress, [&](qint64 received, qint64 total) {
            if (progress) {
                progress({static_cast<std::uint64_t>(received),
                          total > 0 ? static_cast<std::uint64_t>(total) : 0,
                          target.generic_string()});
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            reply->abort();
            loop.quit();
        });
        timer.start(static_cast<int>(options.transferTimeout.count()));
        loop.exec();

        if (cancel.isCancelled()) {
            reply->deleteLater();
            return autoupdater::Result<autoupdater::DownloadResult>::fail({autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
        }
        if (reply->error() != QNetworkReply::NoError) {
            const auto message = reply->errorString().toStdString();
            reply->deleteLater();
            return autoupdater::Result<autoupdater::DownloadResult>::fail({autoupdater::ErrorCode::DownloadFailed, message});
        }

        autoupdater::DownloadResult result;
        result.bytesWritten = written;
        reply->deleteLater();
        return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
    } catch (...) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail({autoupdater::ErrorCode::DownloadFailed, "Unexpected Qt download failure"});
    }
}
