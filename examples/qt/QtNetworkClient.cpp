#include "QtNetworkClient.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

QtNetworkClient::QtNetworkClient(QObject* parent) : QObject(parent) {}

autoupdater::Result<std::string> QtNetworkClient::getText(const std::string& url,
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
    const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions& options,
    const std::optional<autoupdater::DownloadResumeInfo>& resume, autoupdater::ProgressCallback progress,
    autoupdater::CancellationToken& cancel) noexcept {
    try {
        const bool appending = options.enableResume && resume && resume->offset > 0;
        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        if (appending) {
            QByteArray range("bytes=");
            range += QByteArray::number(static_cast<qint64>(resume->offset));
            range += "-";
            request.setRawHeader("Range", range);
            if (!resume->etag.empty()) {
                request.setRawHeader("If-Range", QByteArray::fromStdString(resume->etag));
            } else if (!resume->lastModified.empty()) {
                request.setRawHeader("If-Range", QByteArray::fromStdString(resume->lastModified));
            }
        }

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        auto* reply = manager_.get(request);
        std::uint64_t written = appending ? resume->offset : 0;
        autoupdater::Error writeError;
        QObject::connect(reply, &QNetworkReply::readyRead, [&] {
            const auto data = reply->readAll();
            auto result = target.write(data.constData(), static_cast<std::size_t>(data.size()));
            if (!result) {
                writeError = result.error();
                reply->abort();
                return;
            }
            written += static_cast<std::uint64_t>(data.size());
        });
        QObject::connect(reply, &QNetworkReply::downloadProgress, [&](qint64 received, qint64 total) {
            if (progress) {
                const auto base = appending ? resume->offset : 0;
                progress({base + static_cast<std::uint64_t>(received),
                          total > 0 ? base + static_cast<std::uint64_t>(total) : 0,
                          {}});
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
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
        }
        if (reply->error() != QNetworkReply::NoError) {
            if (!writeError.ok()) {
                reply->deleteLater();
                return autoupdater::Result<autoupdater::DownloadResult>::fail(writeError);
            }
            const auto message = reply->errorString().toStdString();
            reply->deleteLater();
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, message});
        }
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (appending && status == 200) {
            auto truncated = target.truncate(0);
            if (!truncated) {
                reply->deleteLater();
                return autoupdater::Result<autoupdater::DownloadResult>::fail(truncated.error());
            }
            auto rewound = target.seek(0);
            if (!rewound) {
                reply->deleteLater();
                return autoupdater::Result<autoupdater::DownloadResult>::fail(rewound.error());
            }
            reply->deleteLater();
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "Server ignored Range request"});
        }

        autoupdater::DownloadResult result;
        result.bytesWritten = written;
        result.etag = reply->rawHeader("ETag").toStdString();
        result.lastModified = reply->rawHeader("Last-Modified").toStdString();
        reply->deleteLater();
        return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
    } catch (...) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "Unexpected Qt download failure"});
    }
}
