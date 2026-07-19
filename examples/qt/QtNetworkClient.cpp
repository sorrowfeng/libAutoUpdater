#include "QtNetworkClient.h"

#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

bool isHttpUrl(const std::string& url) {
    const auto separator = url.find("://");
    if (separator == std::string::npos) {
        return false;
    }
    auto scheme = url.substr(0, separator);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return scheme == "http" || scheme == "https";
}

autoupdater::Result<std::filesystem::path> localPathFromUrl(const std::string& url) {
    if (autoupdater::util::isFileUrl(url)) {
        return autoupdater::Result<std::filesystem::path>::ok(autoupdater::util::fileUrlToPath(url));
    }
    return autoupdater::Result<std::filesystem::path>::fail(
        {autoupdater::ErrorCode::NetworkError, "Qt accepts only HTTP, HTTPS, and explicit file: URLs"});
}

autoupdater::Result<autoupdater::TextResponse> readLocalText(const std::string& url,
                                                             autoupdater::CancellationToken& cancel) {
    if (cancel.isCancelled()) {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
    }
    auto path = localPathFromUrl(url);
    if (!path) {
        return autoupdater::Result<autoupdater::TextResponse>::fail(path.error());
    }
    try {
        std::ifstream input(path.value(), std::ios::binary);
        if (!input) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::ManifestDownloadFailed, "Failed to open local source"});
        }
        std::ostringstream stream;
        stream << input.rdbuf();
        if (input.bad()) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::NetworkError, "Failed to read local source"});
        }
        autoupdater::TextResponse response;
        response.response.statusCode = 200;
        response.response.effectiveUrl = url;
        response.body = stream.str();
        return autoupdater::Result<autoupdater::TextResponse>::ok(std::move(response));
    } catch (...) {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Failed to read local source"});
    }
}

autoupdater::Result<autoupdater::DownloadResult>
copyLocalToFile(const std::string& url, autoupdater::IRootedFile& target,
                const std::optional<autoupdater::DownloadResumeInfo>& resume, autoupdater::ProgressCallback progress,
                autoupdater::CancellationToken& cancel) {
    auto source = localPathFromUrl(url);
    if (!source) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(source.error());
    }
    try {
        std::error_code ec;
        const auto total = std::filesystem::file_size(source.value(), ec);
        if (ec) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, ec.message()});
        }
        std::ifstream input(source.value(), std::ios::binary);
        if (resume && resume->offset > 0) {
            input.seekg(static_cast<std::streamoff>(resume->offset), std::ios::beg);
        }
        if (!input) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "Failed to open local source"});
        }

        std::array<char, 64 * 1024> buffer{};
        std::uint64_t written = resume ? resume->offset : 0;
        while (input) {
            if (cancel.isCancelled()) {
                return autoupdater::Result<autoupdater::DownloadResult>::fail(
                    {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
            }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                auto write = target.write(buffer.data(), static_cast<std::size_t>(count));
                if (!write) {
                    return autoupdater::Result<autoupdater::DownloadResult>::fail(write.error());
                }
                written += static_cast<std::uint64_t>(count);
                if (progress) {
                    progress({written, static_cast<std::uint64_t>(total), {}});
                }
            }
        }
        if (input.bad()) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "Failed to read local source"});
        }

        autoupdater::DownloadResult result;
        result.response.statusCode = 200;
        result.response.effectiveUrl = url;
        result.bytesWritten = written;
        return autoupdater::Result<autoupdater::DownloadResult>::ok(std::move(result));
    } catch (...) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "Failed to copy local source"});
    }
}

int timerInterval(std::chrono::milliseconds timeout) {
    const auto value = timeout.count();
    const auto maximum = static_cast<long long>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min<long long>(std::max<long long>(value, 1), maximum));
}

bool isHttpResponseError(QNetworkReply::NetworkError error) {
    const auto value = static_cast<int>(error);
    return value >= 200 && value < 500;
}

autoupdater::NetworkResponseInfo responseInfo(QNetworkReply& reply, const std::string& requestedUrl) {
    autoupdater::NetworkResponseInfo response;
    const auto status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) {
        response.statusCode = status.toInt();
    }

    const auto effective = reply.url().toEncoded(QUrl::FullyEncoded);
    response.effectiveUrl = effective.isEmpty() ? requestedUrl : effective.toStdString();
    const auto pairs = reply.rawHeaderPairs();
    response.headers.reserve(static_cast<std::size_t>(pairs.size()));
    for (const auto& pair : pairs) {
        response.headers.push_back({pair.first.toLower().toStdString(), pair.second.trimmed().toStdString()});
    }
    return response;
}

std::string responseHeader(const autoupdater::NetworkResponseInfo& response, const std::string& name) {
    for (const auto& header : response.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

void configureRequest(QNetworkRequest& request) {
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
}

} // namespace

QtNetworkClient::QtNetworkClient(QObject* parent) : QObject(parent) {}

autoupdater::Result<autoupdater::TextResponse>
QtNetworkClient::getText(const std::string& url, const autoupdater::NetworkOptions& options,
                         autoupdater::CancellationToken& cancel) noexcept {
    if (!isHttpUrl(url)) {
        return readLocalText(url, cancel);
    }
    if (cancel.isCancelled()) {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
    }

    try {
        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        configureRequest(request);

        QNetworkAccessManager manager;
        QEventLoop loop;
        QTimer transferTimer;
        QTimer cancellationTimer;
        bool timedOut = false;
        transferTimer.setSingleShot(true);
        cancellationTimer.setInterval(25);

        auto* reply = manager.get(request);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&transferTimer, &QTimer::timeout, [&] {
            timedOut = true;
            reply->abort();
            loop.quit();
        });
        QObject::connect(&cancellationTimer, &QTimer::timeout, [&] {
            if (cancel.isCancelled()) {
                reply->abort();
                loop.quit();
            }
        });
        if (options.transferTimeout.count() > 0) {
            transferTimer.start(timerInterval(options.transferTimeout));
        }
        cancellationTimer.start();
        loop.exec();

        if (cancel.isCancelled()) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
        }
        if (timedOut) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::NetworkError, "Qt network request timed out"});
        }

        const auto error = reply->error();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (error != QNetworkReply::NoError && !(status.isValid() && isHttpResponseError(error))) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::NetworkError, reply->errorString().toStdString()});
        }

        const auto bytes = reply->readAll();
        autoupdater::TextResponse response;
        response.response = responseInfo(*reply, url);
        response.body.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        return autoupdater::Result<autoupdater::TextResponse>::ok(std::move(response));
    } catch (...) {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Unexpected Qt request failure"});
    }
}

autoupdater::Result<autoupdater::DownloadResult> QtNetworkClient::downloadToFile(
    const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions& options,
    const std::optional<autoupdater::DownloadResumeInfo>& resume, autoupdater::ProgressCallback progress,
    autoupdater::CancellationToken& cancel) noexcept {
    if (!isHttpUrl(url)) {
        const auto effectiveResume = options.enableResume ? resume : std::nullopt;
        return copyLocalToFile(url, target, effectiveResume, std::move(progress), cancel);
    }
    if (cancel.isCancelled()) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
    }

    try {
        const bool appending = options.enableResume && resume && resume->offset > 0;
        const int writableStatus = appending ? 206 : 200;
        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        configureRequest(request);
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

        QNetworkAccessManager manager;
        QEventLoop loop;
        QTimer transferTimer;
        QTimer cancellationTimer;
        bool timedOut = false;
        transferTimer.setSingleShot(true);
        cancellationTimer.setInterval(25);

        auto* reply = manager.get(request);
        std::uint64_t written = appending ? resume->offset : 0;
        autoupdater::Error writeError;
        bool discardedResponseBody = false;
        const auto consumeAvailable = [&] {
            try {
                const auto data = reply->readAll();
                if (data.isEmpty()) {
                    return;
                }
                if (!writeError.ok()) {
                    return;
                }
                const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                if (!status.isValid()) {
                    writeError = {autoupdater::ErrorCode::DownloadFailed,
                                  "Qt received response data before HTTP status metadata"};
                    reply->abort();
                    return;
                }
                if (status.toInt() != writableStatus) {
                    discardedResponseBody = true;
                    reply->abort();
                    return;
                }
                auto result = target.write(data.constData(), static_cast<std::size_t>(data.size()));
                if (!result) {
                    writeError = result.error();
                    reply->abort();
                    return;
                }
                written += static_cast<std::uint64_t>(data.size());
            } catch (...) {
                writeError = {autoupdater::ErrorCode::DownloadFailed, "Qt download write callback failed"};
                reply->abort();
            }
        };
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, consumeAvailable);
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 received, qint64 total) {
            try {
                const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                if (progress && status.isValid() && status.toInt() == writableStatus) {
                    const auto base = appending ? resume->offset : 0;
                    progress({base + static_cast<std::uint64_t>(std::max<qint64>(received, 0)),
                              total > 0 ? base + static_cast<std::uint64_t>(total) : 0,
                              {}});
                }
            } catch (...) {
                writeError = {autoupdater::ErrorCode::DownloadFailed, "Qt progress callback failed"};
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&transferTimer, &QTimer::timeout, [&] {
            timedOut = true;
            reply->abort();
            loop.quit();
        });
        QObject::connect(&cancellationTimer, &QTimer::timeout, [&] {
            if (cancel.isCancelled()) {
                reply->abort();
                loop.quit();
            }
        });
        if (options.transferTimeout.count() > 0) {
            transferTimer.start(timerInterval(options.transferTimeout));
        }
        cancellationTimer.start();
        loop.exec();
        if (!cancel.isCancelled() && !timedOut) {
            consumeAvailable();
        }

        if (cancel.isCancelled()) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
        }
        if (!writeError.ok()) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(writeError);
        }
        if (timedOut) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "Qt download timed out"});
        }

        const auto error = reply->error();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const bool stoppedAfterHeaders =
            discardedResponseBody && error == QNetworkReply::OperationCanceledError && status.isValid();
        if (error != QNetworkReply::NoError && !stoppedAfterHeaders &&
            !(status.isValid() && isHttpResponseError(error))) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, reply->errorString().toStdString()});
        }

        autoupdater::DownloadResult result;
        result.response = responseInfo(*reply, url);
        result.bytesWritten = written;
        result.etag = responseHeader(result.response, "etag");
        result.lastModified = responseHeader(result.response, "last-modified");
        return autoupdater::Result<autoupdater::DownloadResult>::ok(std::move(result));
    } catch (...) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "Unexpected Qt download failure"});
    }
}
