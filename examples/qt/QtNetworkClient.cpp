#include "QtNetworkClient.h"

#include "NetworkLimits.h"
#include "default/LocalNetworkFile.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
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

int timerInterval(std::chrono::milliseconds timeout) {
    const auto value = timeout.count();
    const auto maximum = static_cast<long long>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min<long long>(std::max<long long>(value, 1), maximum));
}

bool isHttpResponseError(QNetworkReply::NetworkError error) {
    const auto value = static_cast<int>(error);
    return value >= 200 && value < 500;
}

autoupdater::Result<autoupdater::NetworkResponseInfo> responseInfo(QNetworkReply& reply,
                                                                   const std::string& requestedUrl) {
    autoupdater::NetworkResponseInfo response;
    const auto status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) {
        response.statusCode = status.toInt();
    }

    const auto effective = reply.url().toEncoded(QUrl::FullyEncoded);
    response.effectiveUrl = effective.isEmpty() ? requestedUrl : effective.toStdString();
    const auto pairs = reply.rawHeaderPairs();
    std::uint64_t headerBytes = 0;
    for (const auto& pair : pairs) {
        std::uint64_t fieldBytes = 0;
        if (!autoupdater::detail::checkedAdd(static_cast<std::uint64_t>(pair.first.size()),
                                             static_cast<std::uint64_t>(pair.second.size()), fieldBytes) ||
            !autoupdater::detail::checkedAdd(fieldBytes, 4, fieldBytes) ||
            !autoupdater::detail::checkedAdd(headerBytes, fieldBytes, headerBytes) ||
            headerBytes > autoupdater::detail::kMaxNetworkResponseHeaderBytes) {
            return autoupdater::Result<autoupdater::NetworkResponseInfo>::fail(
                {autoupdater::ErrorCode::ResourceLimitExceeded, "HTTP response headers exceed their byte limit"});
        }
    }
    response.headers.reserve(static_cast<std::size_t>(pairs.size()));
    for (const auto& pair : pairs) {
        response.headers.push_back({pair.first.toLower().toStdString(), pair.second.trimmed().toStdString()});
    }
    return autoupdater::Result<autoupdater::NetworkResponseInfo>::ok(std::move(response));
}

std::string responseHeader(const autoupdater::NetworkResponseInfo& response, const std::string& name) {
    for (const auto& header : response.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

bool equalsAsciiCaseInsensitive(const std::string& value, const char* expected) {
    const std::string expectedValue(expected);
    if (value.size() != expectedValue.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(expectedValue[index]))) {
            return false;
        }
    }
    return true;
}

autoupdater::Result<void> validateIdentityContentEncoding(const autoupdater::NetworkResponseInfo& response,
                                                          autoupdater::ErrorCode invalidHeaderCode) {
    for (const auto& header : response.headers) {
        if (header.name == "content-encoding" && !equalsAsciiCaseInsensitive(header.value, "identity")) {
            return autoupdater::Result<void>::fail(
                {invalidHeaderCode, "Encoded HTTP response bodies are not accepted for byte-exact transfers"});
        }
    }
    return autoupdater::Result<void>::ok();
}

autoupdater::Result<void> validateResponseMetadataBudget(const autoupdater::NetworkResponseInfo& response,
                                                         std::uint64_t actualBytes, std::uint64_t maxBytes,
                                                         autoupdater::ErrorCode invalidHeaderCode) {
    auto encoding = validateIdentityContentEncoding(response, invalidHeaderCode);
    if (!encoding) {
        return encoding;
    }
    return autoupdater::detail::validateResponseBodyBudget(response, actualBytes, maxBytes, invalidHeaderCode);
}

autoupdater::Result<void> validateCompletedBody(const autoupdater::NetworkResponseInfo& response,
                                                std::uint64_t actualBytes, std::uint64_t maxBytes,
                                                autoupdater::ErrorCode invalidHeaderCode) {
    auto budget = validateResponseMetadataBudget(response, actualBytes, maxBytes, invalidHeaderCode);
    if (!budget) {
        return budget;
    }
    auto declared = autoupdater::detail::declaredContentLength(response, invalidHeaderCode);
    if (!declared) {
        return autoupdater::Result<void>::fail(declared.error());
    }
    if (declared.value() && *declared.value() != actualBytes) {
        return autoupdater::Result<void>::fail(
            {invalidHeaderCode, "Content-Length does not match the received response body"});
    }
    return autoupdater::Result<void>::ok();
}

void configureRequest(QNetworkRequest& request) {
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept-Encoding", "identity");
}

constexpr std::size_t kReadChunkSize = 64 * 1024;

qint64 readBufferLimit(std::uint64_t remainingBytes) {
    if (remainingBytes < kReadChunkSize) {
        return static_cast<qint64>(remainingBytes + 1);
    }
    return static_cast<qint64>(kReadChunkSize);
}

qint64 nextReadSize(std::uint64_t remainingBytes) {
    return readBufferLimit(remainingBytes);
}

} // namespace

QtNetworkClient::QtNetworkClient(QObject* parent) : QObject(parent) {}

autoupdater::Result<autoupdater::TextResponse>
QtNetworkClient::getText(const std::string& url, const autoupdater::NetworkOptions& options,
                         std::uint64_t maxResponseBytes, autoupdater::CancellationToken& cancel) noexcept {
    if (!isHttpUrl(url)) {
        return autoupdater::detail::readLocalText(url, maxResponseBytes, cancel);
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
        reply->setReadBufferSize(readBufferLimit(maxResponseBytes));
        std::array<char, kReadChunkSize> buffer{};
        std::string body;
        std::uint64_t consumed = 0;
        autoupdater::Error bodyError;
        bool discardedResponseBody = false;

        const auto validateMetadata = [&] {
            if (!bodyError.ok() || discardedResponseBody) {
                return;
            }
            try {
                auto response = responseInfo(*reply, url);
                if (!response) {
                    bodyError = response.error();
                    reply->abort();
                    loop.quit();
                    return;
                }
                if (response.value().statusCode == 0) {
                    return;
                }
                if (response.value().statusCode != 200) {
                    discardedResponseBody = true;
                    reply->abort();
                    loop.quit();
                    return;
                }
                auto valid = validateResponseMetadataBudget(response.value(), consumed, maxResponseBytes,
                                                            autoupdater::ErrorCode::NetworkError);
                if (!valid) {
                    bodyError = valid.error();
                    reply->abort();
                    loop.quit();
                }
            } catch (...) {
                bodyError = {autoupdater::ErrorCode::NetworkError, "Failed to validate Qt response metadata"};
                reply->abort();
                loop.quit();
            }
        };
        const auto consumeAvailable = [&] {
            try {
                validateMetadata();
                while (bodyError.ok() && !discardedResponseBody && reply->bytesAvailable() > 0) {
                    const auto remaining = maxResponseBytes - consumed;
                    const auto count = reply->read(buffer.data(), nextReadSize(remaining));
                    if (count < 0) {
                        bodyError = {autoupdater::ErrorCode::NetworkError, "Failed to read Qt response body"};
                        reply->abort();
                        loop.quit();
                        return;
                    }
                    if (count == 0) {
                        return;
                    }
                    const auto bytes = static_cast<std::uint64_t>(count);
                    if (bytes > remaining) {
                        bodyError = {autoupdater::ErrorCode::ResourceLimitExceeded,
                                     "Qt response body exceeds its byte limit"};
                        reply->abort();
                        loop.quit();
                        return;
                    }
                    body.append(buffer.data(), static_cast<std::size_t>(count));
                    consumed += bytes;
                }
            } catch (...) {
                bodyError = {autoupdater::ErrorCode::NetworkError, "Failed to buffer Qt response body"};
                reply->abort();
                loop.quit();
            }
        };
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, validateMetadata);
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, consumeAvailable);
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
        if (!cancel.isCancelled() && !timedOut && bodyError.ok() && !discardedResponseBody) {
            validateMetadata();
            consumeAvailable();
        }

        if (cancel.isCancelled()) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
        }
        if (!bodyError.ok()) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(bodyError);
        }
        if (timedOut) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::NetworkError, "Qt network request timed out"});
        }

        const auto error = reply->error();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const bool stoppedAfterHeaders =
            discardedResponseBody && error == QNetworkReply::OperationCanceledError && status.isValid();
        if (error != QNetworkReply::NoError && !stoppedAfterHeaders &&
            !(status.isValid() && isHttpResponseError(error))) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::NetworkError, reply->errorString().toStdString()});
        }

        autoupdater::TextResponse response;
        auto info = responseInfo(*reply, url);
        if (!info) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(info.error());
        }
        response.response = std::move(info.value());
        if (response.response.statusCode == 200) {
            auto valid = validateCompletedBody(response.response, consumed, maxResponseBytes,
                                               autoupdater::ErrorCode::NetworkError);
            if (!valid) {
                return autoupdater::Result<autoupdater::TextResponse>::fail(valid.error());
            }
        }
        response.body = std::move(body);
        return autoupdater::Result<autoupdater::TextResponse>::ok(std::move(response));
    } catch (...) {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Unexpected Qt request failure"});
    }
}

autoupdater::Result<autoupdater::DownloadResult> QtNetworkClient::downloadToFile(
    const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions& options,
    std::uint64_t maxTotalBytes, const std::optional<autoupdater::DownloadResumeInfo>& resume,
    autoupdater::ProgressCallback progress, autoupdater::CancellationToken& cancel) noexcept {
    if (!isHttpUrl(url)) {
        const auto effectiveResume = options.enableResume ? resume : std::nullopt;
        return autoupdater::detail::copyLocalToFile(url, target, maxTotalBytes, effectiveResume, std::move(progress),
                                                    cancel);
    }
    if (cancel.isCancelled()) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
    }

    try {
        const bool appending = options.enableResume && resume && resume->offset > 0;
        const std::uint64_t initialBytes = appending ? resume->offset : 0;
        auto remaining = autoupdater::detail::remainingTransferBudget(initialBytes, maxTotalBytes);
        if (!remaining) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(remaining.error());
        }
        const std::uint64_t transferBudget = remaining.value();
        const int writableStatus = appending ? 206 : 200;
        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        configureRequest(request);
        if (appending) {
            const auto range = QByteArray::fromStdString("bytes=" + std::to_string(resume->offset) + "-");
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
        reply->setReadBufferSize(readBufferLimit(transferBudget));
        std::array<char, kReadChunkSize> buffer{};
        std::uint64_t written = initialBytes;
        std::uint64_t transferred = 0;
        autoupdater::Error writeError;
        bool discardedResponseBody = false;

        const auto stopWithError = [&](autoupdater::Error error) {
            writeError = std::move(error);
            reply->abort();
            loop.quit();
        };
        const auto validateMetadata = [&] {
            if (!writeError.ok() || discardedResponseBody) {
                return;
            }
            try {
                const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                if (!status.isValid()) {
                    return;
                }
                if (status.toInt() != writableStatus) {
                    discardedResponseBody = true;
                    reply->abort();
                    loop.quit();
                    return;
                }
                auto response = responseInfo(*reply, url);
                if (!response) {
                    stopWithError(response.error());
                    return;
                }
                auto valid = validateResponseMetadataBudget(response.value(), transferred, transferBudget,
                                                            autoupdater::ErrorCode::DownloadFailed);
                if (!valid) {
                    stopWithError(valid.error());
                }
            } catch (...) {
                stopWithError({autoupdater::ErrorCode::DownloadFailed, "Failed to validate Qt download metadata"});
            }
        };
        const auto consumeAvailable = [&] {
            try {
                if (!writeError.ok()) {
                    return;
                }
                const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                if (!status.isValid()) {
                    stopWithError({autoupdater::ErrorCode::DownloadFailed,
                                   "Qt received response data before HTTP status metadata"});
                    return;
                }
                if (status.toInt() != writableStatus) {
                    discardedResponseBody = true;
                    reply->abort();
                    loop.quit();
                    return;
                }
                validateMetadata();
                while (writeError.ok() && !discardedResponseBody && reply->bytesAvailable() > 0) {
                    const auto availableBudget = transferBudget - transferred;
                    const auto count = reply->read(buffer.data(), nextReadSize(availableBudget));
                    if (count < 0) {
                        stopWithError({autoupdater::ErrorCode::DownloadFailed, "Failed to read Qt download body"});
                        return;
                    }
                    if (count == 0) {
                        return;
                    }
                    const auto bytes = static_cast<std::uint64_t>(count);
                    if (bytes > availableBudget) {
                        stopWithError({autoupdater::ErrorCode::ResourceLimitExceeded,
                                       "Qt artifact exceeds its signed byte limit"});
                        return;
                    }
                    auto result = target.write(buffer.data(), static_cast<std::size_t>(count));
                    if (!result) {
                        stopWithError(result.error());
                        return;
                    }
                    std::uint64_t updatedWritten = 0;
                    if (!autoupdater::detail::checkedAdd(written, bytes, updatedWritten) ||
                        updatedWritten > maxTotalBytes) {
                        stopWithError({autoupdater::ErrorCode::ResourceLimitExceeded,
                                       "Qt download byte counter exceeded its limit"});
                        return;
                    }
                    written = updatedWritten;
                    transferred += bytes;
                    if (progress) {
                        progress({written, maxTotalBytes, {}});
                    }
                }
            } catch (...) {
                stopWithError({autoupdater::ErrorCode::DownloadFailed, "Qt download write callback failed"});
            }
        };
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, validateMetadata);
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, consumeAvailable);
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
        if (!cancel.isCancelled() && !timedOut && writeError.ok() && !discardedResponseBody) {
            validateMetadata();
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
        auto info = responseInfo(*reply, url);
        if (!info) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(info.error());
        }
        result.response = std::move(info.value());
        if (result.response.statusCode == writableStatus) {
            auto valid = validateCompletedBody(result.response, transferred, transferBudget,
                                               autoupdater::ErrorCode::DownloadFailed);
            if (!valid) {
                return autoupdater::Result<autoupdater::DownloadResult>::fail(valid.error());
            }
        }
        result.bytesWritten = written;
        result.etag = responseHeader(result.response, "etag");
        result.lastModified = responseHeader(result.response, "last-modified");
        return autoupdater::Result<autoupdater::DownloadResult>::ok(std::move(result));
    } catch (...) {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "Unexpected Qt download failure"});
    }
}
