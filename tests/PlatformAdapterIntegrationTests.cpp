#include "TestCommon.h"

#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "ApplyExecutor.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

using namespace std::chrono_literals;
constexpr auto kTestSafetyTimeout = 10s;

#ifdef LIBAUTOUPDATER_TEST_HAS_HTTP_BACKEND

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

void closeSocket(NativeSocket socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    (void)closesocket(socket);
#else
    (void)close(socket);
#endif
}

bool interruptedSocketCall() noexcept {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

int sendFlags() noexcept {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool sendAll(NativeSocket socket, const std::string& bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto bounded = (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)()));
#ifdef _WIN32
        const int sent = send(socket, bytes.data() + offset, static_cast<int>(bounded), sendFlags());
#else
        const auto sent = send(socket, bytes.data() + offset, bounded, sendFlags());
#endif
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && interruptedSocketCall()) {
            continue;
        }
        return false;
    }
    return true;
}

class SocketRuntime final {
  public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
    }

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;

    ~SocketRuntime() {
#ifdef _WIN32
        (void)WSACleanup();
#endif
    }
};

class LoopbackHttpServer final {
  public:
    LoopbackHttpServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kInvalidSocket) {
            throw std::runtime_error("Failed to create loopback listener");
        }

        int reuse = 1;
#ifdef _WIN32
        (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 8) != 0) {
            closeSocket(listener_);
            listener_ = kInvalidSocket;
            throw std::runtime_error("Failed to bind loopback listener");
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int length = sizeof(bound);
#else
        socklen_t length = sizeof(bound);
#endif
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
            closeSocket(listener_);
            listener_ = kInvalidSocket;
            throw std::runtime_error("Failed to inspect loopback listener");
        }
        port_ = ntohs(bound.sin_port);
        worker_ = std::thread([this] { run(); });
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    ~LoopbackHttpServer() {
        stopping_.store(true, std::memory_order_release);
#ifdef _WIN32
        (void)shutdown(listener_, SD_BOTH);
#else
        (void)shutdown(listener_, SHUT_RDWR);
#endif
        closeSocket(listener_);
        requestsChanged_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        for (auto& worker : clientWorkers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        listener_ = kInvalidSocket;
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    bool waitForRequest(const std::string& path, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(requestMutex_);
        return requestsChanged_.wait_for(lock, timeout, [&] { return requestCounts_[path] > 0; });
    }

  private:
    void run() noexcept {
        while (!stopping_.load(std::memory_order_acquire)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            timeval timeout{};
            timeout.tv_usec = 100000;
#ifdef _WIN32
            const int selected = select(0, &readable, nullptr, nullptr, &timeout);
#else
            const int selected = select(listener_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
            if (selected == 0) {
                continue;
            }
            if (selected < 0) {
                if (interruptedSocketCall()) {
                    continue;
                }
                break;
            }

            const auto client = accept(listener_, nullptr, nullptr);
            if (client == kInvalidSocket) {
                if (!stopping_.load(std::memory_order_acquire) && interruptedSocketCall()) {
                    continue;
                }
                break;
            }
#ifdef __APPLE__
            int suppressBrokenPipe = 1;
            (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &suppressBrokenPipe, sizeof(suppressBrokenPipe));
#endif
            try {
                clientWorkers_.emplace_back([this, client] {
                    handle(client);
                    closeSocket(client);
                });
            } catch (...) {
                closeSocket(client);
            }
        }
    }

    void handle(NativeSocket client) noexcept {
        std::string request;
        char buffer[1024]{};
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
#ifdef _WIN32
            const int count = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            const auto count = recv(client, buffer, sizeof(buffer), 0);
#endif
            if (count > 0) {
                request.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && interruptedSocketCall()) {
                continue;
            }
            return;
        }

        const auto firstSpace = request.find(' ');
        const auto secondSpace =
            firstSpace == std::string::npos ? std::string::npos : request.find(' ', firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
            return;
        }
        const auto path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            ++requestCounts_[path];
        }
        requestsChanged_.notify_all();

        if (path == "/redirect-one") {
            (void)sendAll(client, response(302, "Found", {}, {{"Location", "/redirect-two"}}));
            return;
        }
        if (path == "/redirect-two") {
            (void)sendAll(client, response(307, "Temporary Redirect", {}, {{"Location", "/slow"}}));
            return;
        }
        if (path == "/slow") {
            const std::string body = "slow-adapter";
            if (!sendAll(client, responseHeaders(200, "OK", body.size()))) {
                return;
            }
            for (const char byte : body) {
                if (!sendAll(client, std::string(1, byte))) {
                    return;
                }
                std::this_thread::sleep_for(15ms);
            }
            return;
        }
        if (path == "/timeout") {
            if (!sendAll(client, responseHeaders(200, "OK", 1))) {
                return;
            }
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!stopping_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(10ms);
            }
            return;
        }
        if (path == "/cancel") {
            constexpr std::size_t kBodyBytes = 4096;
            if (!sendAll(client, responseHeaders(200, "OK", kBodyBytes))) {
                return;
            }
            for (std::size_t index = 0; index < kBodyBytes && !stopping_.load(std::memory_order_acquire); ++index) {
                if (!sendAll(client, "x")) {
                    return;
                }
                std::this_thread::sleep_for(10ms);
            }
            return;
        }
        if (path == "/download") {
            (void)sendAll(client, response(200, "OK", "download-adapter"));
            return;
        }
        if (path == "/ok") {
            (void)sendAll(client, response(200, "OK", "adapter-ok"));
            return;
        }
        (void)sendAll(client, response(404, "Not Found", {}));
    }

    static std::string responseHeaders(int status, const std::string& reason, std::size_t contentLength,
                                       const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        std::string result = "HTTP/1.1 " + std::to_string(status) + ' ' + reason + "\r\n";
        for (const auto& header : headers) {
            result += header.first + ": " + header.second + "\r\n";
        }
        result += "Content-Length: " + std::to_string(contentLength) + "\r\n";
        result += "Content-Type: application/octet-stream\r\n";
        result += "Connection: close\r\n\r\n";
        return result;
    }

    static std::string response(int status, const std::string& reason, const std::string& body,
                                const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        return responseHeaders(status, reason, body.size(), headers) + body;
    }

    NativeSocket listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
    std::atomic_bool stopping_{false};
    std::thread worker_;
    std::vector<std::thread> clientWorkers_;
    std::mutex requestMutex_;
    std::condition_variable requestsChanged_;
    std::map<std::string, std::size_t> requestCounts_;
};

class RecordingFile final : public autoupdater::IRootedFile {
  public:
    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        const auto remaining = contents_.size() - (std::min)(position_, contents_.size());
        const auto count = (std::min)(size, remaining);
        if (count > 0) {
            std::memcpy(buffer, contents_.data() + position_, count);
            position_ += count;
        }
        return autoupdater::Result<std::size_t>::ok(count);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        try {
            const auto* bytes = static_cast<const char*>(data);
            contents_.append(bytes, size);
            position_ = contents_.size();
            return autoupdater::Result<void>::ok();
        } catch (...) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Recording file write failed"});
        }
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > contents_.size()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Recording file seek is out of range"});
        }
        position_ = static_cast<std::size_t>(offset);
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        if (size > contents_.size()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Recording file truncate is out of range"});
        }
        contents_.resize(static_cast<std::size_t>(size));
        position_ = (std::min)(position_, contents_.size());
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(
            {contents_.size(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
             "recording-file"});
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> close() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    const std::string& contents() const noexcept {
        return contents_;
    }

  private:
    std::string contents_;
    std::size_t position_ = 0;
};

std::string responseHeader(const autoupdater::NetworkResponseInfo& response, const std::string& expectedName) {
    for (const auto& header : response.headers) {
        std::string name = header.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (name == expectedName) {
            return header.value;
        }
    }
    return {};
}

#endif

std::uint64_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                std::filesystem::u8path("libAutoUpdater-platform-adapter-" + std::to_string(currentProcessId()))) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("Failed to create platform-adapter test directory: " + error.message());
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (length + 1 < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("_NSGetExecutablePath failed");
    }
    return std::filesystem::u8path(buffer.data());
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("readlink(/proc/self/exe) failed");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::u8path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::filesystem::path helperExecutablePath() {
#ifdef _WIN32
    const auto configuredSize = GetEnvironmentVariableW(L"LIBAUTOUPDATER_PLATFORM_ADAPTER_HELPER", nullptr, 0);
    if (configuredSize > 1) {
        std::vector<wchar_t> configured(configuredSize, L'\0');
        if (GetEnvironmentVariableW(L"LIBAUTOUPDATER_PLATFORM_ADAPTER_HELPER", configured.data(), configuredSize) ==
            0) {
            throw std::runtime_error("GetEnvironmentVariableW failed");
        }
        return std::filesystem::path(configured.data());
    }
#else
    if (const auto* configured = std::getenv("LIBAUTOUPDATER_PLATFORM_ADAPTER_HELPER")) {
        if (*configured != '\0') {
            return std::filesystem::u8path(configured);
        }
    }
#endif

    auto sibling = currentExecutablePath().parent_path() / "libAutoUpdater platform adapter helper";
#ifdef _WIN32
    sibling += ".exe";
#endif
    return sibling;
}

std::uint64_t waitForPublishedProcessId(const std::filesystem::path& marker) {
    const auto deadline = std::chrono::steady_clock::now() + kTestSafetyTimeout;
    for (;;) {
        std::ifstream input(marker, std::ios::binary);
        std::uint64_t pid = 0;
        char trailing = '\0';
        if (input >> pid && pid > 0 && !(input >> trailing)) {
            return pid;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Platform-adapter process helper timed out");
        }
        std::this_thread::sleep_for(10ms);
    }
}

} // namespace

void testDefaultNetworkAdapterUsesLoopbackTransportContract() {
#ifndef LIBAUTOUPDATER_TEST_HAS_HTTP_BACKEND
    LAU_SKIP("No HTTP transport backend is configured");
#else
    SocketRuntime socketRuntime;
    LoopbackHttpServer server;
    auto network = autoupdater::createDefaultNetworkClient();

    autoupdater::NetworkOptions options;
    options.connectTimeout = 1s;
    options.transferTimeout = 2s;

    autoupdater::CancellationToken directCancel;
    auto direct = network->getText(server.url("/ok"), options, 1024, directCancel);
    LAU_REQUIRE(direct);
    LAU_REQUIRE(direct.value().response.statusCode == 200);
    LAU_REQUIRE(direct.value().response.effectiveUrl == server.url("/ok"));
    LAU_REQUIRE(direct.value().body == "adapter-ok");

    autoupdater::CancellationToken singleHopCancel;
    auto singleHop = network->getText(server.url("/redirect-one"), options, 1024, singleHopCancel);
    LAU_REQUIRE(singleHop);
    LAU_REQUIRE(singleHop.value().response.statusCode == 302);
    LAU_REQUIRE(singleHop.value().response.effectiveUrl == server.url("/redirect-one"));
    LAU_REQUIRE(responseHeader(singleHop.value().response, "location") == "/redirect-two");
    LAU_REQUIRE(singleHop.value().body.empty());

    autoupdater::CancellationToken secondHopCancel;
    auto secondHop = network->getText(server.url("/redirect-two"), options, 1024, secondHopCancel);
    LAU_REQUIRE(secondHop);
    LAU_REQUIRE(secondHop.value().response.statusCode == 307);
    LAU_REQUIRE(secondHop.value().response.effectiveUrl == server.url("/redirect-two"));
    LAU_REQUIRE(responseHeader(secondHop.value().response, "location") == "/slow");
    LAU_REQUIRE(secondHop.value().body.empty());

    autoupdater::CancellationToken finalHopCancel;
    auto finalHop = network->getText(server.url("/slow"), options, 1024, finalHopCancel);
    LAU_REQUIRE(finalHop);
    LAU_REQUIRE(finalHop.value().response.statusCode == 200);
    LAU_REQUIRE(finalHop.value().response.effectiveUrl == server.url("/slow"));
    LAU_REQUIRE(finalHop.value().body == "slow-adapter");

    RecordingFile file;
    std::vector<autoupdater::Progress> progress;
    autoupdater::CancellationToken downloadCancel;
    const std::string expectedDownload = "download-adapter";
    auto download = network->downloadToFile(
        server.url("/download"), file, options, expectedDownload.size(), std::nullopt,
        [&](const autoupdater::Progress& value) { progress.push_back(value); }, downloadCancel);
    LAU_REQUIRE(download);
    LAU_REQUIRE(download.value().response.statusCode == 200);
    LAU_REQUIRE(download.value().bytesWritten == expectedDownload.size());
    LAU_REQUIRE(file.contents() == expectedDownload);
    LAU_REQUIRE(!progress.empty());
    LAU_REQUIRE(progress.back().downloadedBytes == expectedDownload.size());
    LAU_REQUIRE(progress.back().totalBytes == expectedDownload.size());
#endif
}

void testDefaultNetworkAdapterHonorsTimeoutAndCancellation() {
#ifndef LIBAUTOUPDATER_TEST_HAS_HTTP_BACKEND
    LAU_SKIP("No HTTP transport backend is configured");
#else
    SocketRuntime socketRuntime;
    LoopbackHttpServer server;
    auto network = autoupdater::createDefaultNetworkClient();

    autoupdater::NetworkOptions cancelOptions;
    cancelOptions.connectTimeout = 1s;
    cancelOptions.transferTimeout = 2s;
    autoupdater::CancellationToken cancel;
    auto pending = std::async(std::launch::async,
                              [&] { return network->getText(server.url("/cancel"), cancelOptions, 8192, cancel); });
    LAU_REQUIRE(server.waitForRequest("/cancel", 2s));
    std::this_thread::sleep_for(50ms);
    cancel.cancel();
    LAU_REQUIRE(pending.wait_for(kTestSafetyTimeout) == std::future_status::ready);
    auto cancelled = pending.get();
    LAU_REQUIRE(!cancelled);
    LAU_REQUIRE(cancelled.error().code == autoupdater::ErrorCode::Cancelled);

    autoupdater::NetworkOptions timeoutOptions;
    timeoutOptions.connectTimeout = 1s;
    timeoutOptions.transferTimeout = 150ms;
    autoupdater::CancellationToken timeoutCancel;
    const auto started = std::chrono::steady_clock::now();
    auto timedOut = network->getText(server.url("/timeout"), timeoutOptions, 1024, timeoutCancel);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    LAU_REQUIRE(!timedOut);
    LAU_REQUIRE(timedOut.error().code == autoupdater::ErrorCode::NetworkError);
    LAU_REQUIRE(elapsed < 6s);
#endif
}

void testPlatformProcessAdapterLaunchesAndWaitsNatively() {
    TemporaryDirectory temporary;
    const auto marker = temporary.path() / std::filesystem::u8path(u8"helper-ready-中文.txt");

    autoupdater::ProcessLaunchRequest request;
    request.executable = helperExecutablePath();
    request.workingDirectory = temporary.path();
    request.arguments = {autoupdater::util::pathToUtf8(marker), "1000"};
    request.detached = true;

    const auto launcher = autoupdater::createDefaultProcessLauncher();
    const auto launched = launcher->launch(request);
    LAU_REQUIRE(launched);
    const auto pid = waitForPublishedProcessId(marker);

#ifdef _WIN32
    const auto immediate = autoupdater::updater::waitForProcessExit(pid, 0s);
    LAU_REQUIRE(!immediate);
    LAU_REQUIRE(immediate.error().code == autoupdater::ErrorCode::ApplyFailed);

    const auto completed = autoupdater::updater::waitForProcessExit(pid, 3s);
    LAU_REQUIRE(completed);
#else
    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(static_cast<pid_t>(pid), &status, 0);
    } while (waited < 0 && errno == EINTR);
    LAU_REQUIRE(waited == static_cast<pid_t>(pid));
    LAU_REQUIRE(WIFEXITED(status));
    LAU_REQUIRE(WEXITSTATUS(status) == 0);
#endif
}
