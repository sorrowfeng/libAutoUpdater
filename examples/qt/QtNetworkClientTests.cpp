#include "QtNetworkClient.h"

#include "libAutoUpdater/interfaces/IRootedFileSystem.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

#define QT_NETWORK_REQUIRE(condition)                                                                                  \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << "Requirement failed at " << __FILE__ << ':' << __LINE__ << ": " #condition << '\n';           \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

class LoopbackHttpServer final : public QObject {
  public:
    LoopbackHttpServer() {
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this] { acceptConnections(); });
    }

    bool listen() {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(server_.serverPort()) + path;
    }

    std::size_t stallRequestCount() const noexcept {
        return stallRequestCount_;
    }

  private:
    void acceptConnections() {
        while (server_.hasPendingConnections()) {
            auto* socket = server_.nextPendingConnection();
            if (socket == nullptr) {
                continue;
            }
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { consumeRequest(socket); });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                requests_.remove(socket);
                socket->deleteLater();
            });
            if (socket->bytesAvailable() > 0) {
                consumeRequest(socket);
            }
        }
    }

    void consumeRequest(QTcpSocket* socket) {
        auto& request = requests_[socket];
        request.append(socket->readAll());
        if (!request.contains("\r\n\r\n")) {
            return;
        }
        if (request.startsWith("GET /stall ")) {
            ++stallRequestCount_;
            return;
        }

        const QByteArray body =
            request.startsWith("GET /download ") ? QByteArray("download-body") : QByteArray("hello-from-qt");
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: ";
        response += QByteArray::number(body.size());
        response += "\r\nConnection: close\r\n\r\n";
        response += body;
        socket->write(response);
        socket->disconnectFromHost();
        requests_.remove(socket);
    }

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> requests_;
    std::size_t stallRequestCount_ = 0;
};

class RecordingFile final : public autoupdater::IRootedFile {
  public:
    explicit RecordingFile(bool failWrite = false) : failWrite_(failWrite) {}

    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        const auto remaining = contents_.size() - std::min(position_, contents_.size());
        const auto count = std::min(size, remaining);
        if (count > 0) {
            std::memcpy(buffer, contents_.data() + position_, count);
            position_ += count;
        }
        return autoupdater::Result<std::size_t>::ok(count);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        writeThread_ = QThread::currentThread();
        if (failWrite_) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Injected Qt target write failure"});
        }
        const auto* bytes = static_cast<const char*>(data);
        contents_.append(bytes, size);
        position_ = contents_.size();
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > contents_.size()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Invalid recording-file seek"});
        }
        position_ = static_cast<std::size_t>(offset);
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        if (size > contents_.size()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Invalid recording-file truncate"});
        }
        contents_.resize(static_cast<std::size_t>(size));
        position_ = std::min(position_, contents_.size());
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(
            {contents_.size(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, "recording"});
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> close() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    QThread* writeThread() const noexcept {
        return writeThread_;
    }

  private:
    std::string contents_;
    std::size_t position_ = 0;
    QThread* writeThread_ = nullptr;
    bool failWrite_ = false;
};

template <typename Value, typename Operation>
std::optional<autoupdater::Result<Value>> runOnWorker(Operation operation) {
    std::optional<autoupdater::Result<Value>> result;
    std::atomic_bool completed{false};
    std::thread worker([&] {
        result.emplace(operation());
        completed.store(true, std::memory_order_release);
    });

    QEventLoop loop;
    QTimer poll;
    QTimer safety;
    poll.setInterval(5);
    safety.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (completed.load(std::memory_order_acquire)) {
            loop.quit();
        }
    });
    QObject::connect(&safety, &QTimer::timeout, &loop, [&] {
        std::cerr << "Qt worker request exceeded the test safety deadline\n";
        std::abort();
    });
    poll.start();
    safety.start(10000);
    loop.exec();
    worker.join();
    if (!completed.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return result;
}

bool testSameAndCrossThreadRequests(QtNetworkClient& client, LoopbackHttpServer& server) {
    autoupdater::NetworkOptions options;
    options.connectTimeout = std::chrono::seconds(1);
    options.transferTimeout = std::chrono::seconds(1);

    autoupdater::CancellationToken directCancel;
    auto direct = client.getText(server.url("/same-thread"), options, 1024, directCancel);
    QT_NETWORK_REQUIRE(direct);
    QT_NETWORK_REQUIRE(direct.value().body == "hello-from-qt");

    autoupdater::CancellationToken workerCancel;
    auto worker = runOnWorker<autoupdater::TextResponse>(
        [&] { return client.getText(server.url("/worker-thread"), options, 1024, workerCancel); });
    QT_NETWORK_REQUIRE(worker.has_value());
    QT_NETWORK_REQUIRE(*worker);
    QT_NETWORK_REQUIRE(worker->value().body == "hello-from-qt");
    return true;
}

bool testTimeoutAndCancellation(QtNetworkClient& client, LoopbackHttpServer& server) {
    autoupdater::NetworkOptions timeoutOptions;
    timeoutOptions.connectTimeout = std::chrono::seconds(1);
    timeoutOptions.transferTimeout = std::chrono::milliseconds(75);
    autoupdater::CancellationToken timeoutCancel;
    auto timedOut = runOnWorker<autoupdater::TextResponse>(
        [&] { return client.getText(server.url("/stall"), timeoutOptions, 1024, timeoutCancel); });
    QT_NETWORK_REQUIRE(timedOut.has_value());
    QT_NETWORK_REQUIRE(!*timedOut);
    QT_NETWORK_REQUIRE(timedOut->error().code == autoupdater::ErrorCode::NetworkError);
    QT_NETWORK_REQUIRE(timedOut->error().message.find("timed out") != std::string::npos);

    autoupdater::NetworkOptions cancelOptions;
    cancelOptions.connectTimeout = std::chrono::seconds(1);
    cancelOptions.transferTimeout = std::chrono::seconds(5);
    autoupdater::CancellationToken cancelled;
    QTimer::singleShot(75, [&] { cancelled.cancel(); });
    auto cancelledResult = runOnWorker<autoupdater::TextResponse>(
        [&] { return client.getText(server.url("/stall"), cancelOptions, 1024, cancelled); });
    QT_NETWORK_REQUIRE(cancelledResult.has_value());
    QT_NETWORK_REQUIRE(!*cancelledResult);
    QT_NETWORK_REQUIRE(cancelledResult->error().code == autoupdater::ErrorCode::Cancelled);
    return true;
}

bool testDownloadWriteFailure(QtNetworkClient& client, LoopbackHttpServer& server) {
    autoupdater::NetworkOptions options;
    options.connectTimeout = std::chrono::seconds(1);
    options.transferTimeout = std::chrono::seconds(1);
    RecordingFile target(true);
    autoupdater::CancellationToken cancel;
    auto result = runOnWorker<autoupdater::DownloadResult>(
        [&] { return client.downloadToFile(server.url("/download"), target, options, 64, std::nullopt, {}, cancel); });
    QT_NETWORK_REQUIRE(result.has_value());
    QT_NETWORK_REQUIRE(!*result);
    QT_NETWORK_REQUIRE(result->error().code == autoupdater::ErrorCode::FileSystemError);
    QT_NETWORK_REQUIRE(result->error().message == "Injected Qt target write failure");
    QT_NETWORK_REQUIRE(target.writeThread() == client.thread());
    return true;
}

bool testOverlappingAndReentrantRequestsFailFast(QtNetworkClient& client, LoopbackHttpServer& server) {
    autoupdater::NetworkOptions stalledOptions;
    stalledOptions.connectTimeout = std::chrono::seconds(1);
    stalledOptions.transferTimeout = std::chrono::milliseconds(150);
    autoupdater::CancellationToken firstCancel;
    autoupdater::CancellationToken secondCancel;
    std::optional<autoupdater::Result<autoupdater::TextResponse>> firstResult;
    std::optional<autoupdater::Result<autoupdater::TextResponse>> secondResult;
    std::atomic_bool firstDone{false};
    std::atomic_bool secondDone{false};
    const auto initialStallRequestCount = server.stallRequestCount();

    std::thread first([&] {
        firstResult.emplace(client.getText(server.url("/stall"), stalledOptions, 1024, firstCancel));
        firstDone.store(true, std::memory_order_release);
    });
    std::optional<std::thread> second;

    QEventLoop loop;
    QTimer poll;
    QTimer safety;
    poll.setInterval(5);
    safety.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!second && server.stallRequestCount() > initialStallRequestCount) {
            second.emplace([&] {
                autoupdater::NetworkOptions options;
                secondResult.emplace(client.getText(server.url("/overlap"), options, 1024, secondCancel));
                secondDone.store(true, std::memory_order_release);
            });
        }
        if (firstDone.load(std::memory_order_acquire) && secondDone.load(std::memory_order_acquire)) {
            loop.quit();
        }
    });
    QObject::connect(&safety, &QTimer::timeout, &loop, [] {
        std::cerr << "Overlapping Qt requests exceeded the safety deadline\n";
        std::abort();
    });
    poll.start();
    safety.start(3000);
    loop.exec();
    first.join();
    QT_NETWORK_REQUIRE(second.has_value());
    second->join();
    QT_NETWORK_REQUIRE(firstResult.has_value());
    QT_NETWORK_REQUIRE(!*firstResult);
    QT_NETWORK_REQUIRE(firstResult->error().message.find("timed out") != std::string::npos);
    QT_NETWORK_REQUIRE(secondResult.has_value());
    QT_NETWORK_REQUIRE(!*secondResult);
    QT_NETWORK_REQUIRE(secondResult->error().message.find("already processing") != std::string::npos);

    RecordingFile target;
    autoupdater::CancellationToken downloadCancel;
    std::optional<autoupdater::Result<autoupdater::TextResponse>> reentrant;
    auto download = client.downloadToFile(
        server.url("/download"), target, autoupdater::NetworkOptions{}, 64, std::nullopt,
        [&](const autoupdater::Progress&) {
            autoupdater::CancellationToken nestedCancel;
            reentrant.emplace(
                client.getText(server.url("/reentrant"), autoupdater::NetworkOptions{}, 1024, nestedCancel));
        },
        downloadCancel);
    QT_NETWORK_REQUIRE(download);
    QT_NETWORK_REQUIRE(reentrant.has_value());
    QT_NETWORK_REQUIRE(!*reentrant);
    QT_NETWORK_REQUIRE(reentrant->error().message.find("already processing") != std::string::npos);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    LoopbackHttpServer server;
    if (!server.listen()) {
        std::cerr << "Failed to start Qt loopback test server\n";
        return 1;
    }
    QtNetworkClient client;
    if (!testSameAndCrossThreadRequests(client, server) || !testTimeoutAndCancellation(client, server) ||
        !testDownloadWriteFailure(client, server) || !testOverlappingAndReentrantRequestsFailFast(client, server)) {
        return 1;
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return 0;
}
