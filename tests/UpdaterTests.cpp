#include "TestCommon.h"

#include "libAutoUpdater/Updater.h"
#include "libAutoUpdater/interfaces/IEventDispatcher.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

class QueuedDispatcher final : public autoupdater::IEventDispatcher {
  public:
    void post(std::function<void()> fn) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(fn));
    }

    void drain() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks.swap(queue_);
        }
        for (auto& callback : callbacks) {
            if (callback) {
                callback();
            }
        }
    }

  private:
    std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
};

class StaticManifestNetwork final : public autoupdater::INetworkClient {
  public:
    autoupdater::Result<std::string> getText(const std::string&, const autoupdater::NetworkOptions&,
                                             autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<std::string>::ok(R"json({
          "schemaVersion": 1,
          "version": "1.0.0",
          "baseUrl": "file:///release/",
          "files": []
        })json");
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, const std::filesystem::path&, const autoupdater::NetworkOptions&,
                   const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "not used"});
    }
};

// Serves a manifest describing one file and the file content itself.
class FileServingNetwork final : public autoupdater::INetworkClient {
  public:
    std::string manifestText;
    std::string fileContent;

    autoupdater::Result<std::string> getText(const std::string&, const autoupdater::NetworkOptions&,
                                             autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<std::string>::ok(manifestText);
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, const std::filesystem::path& target, const autoupdater::NetworkOptions&,
                   const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        try {
            std::filesystem::create_directories(target.parent_path());
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            output << fileContent;
            output.close();
            autoupdater::DownloadResult result;
            result.bytesWritten = fileContent.size();
            return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
        } catch (...) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "unexpected failure"});
        }
    }
};

std::filesystem::path uniqueTempDir() {
    auto base = std::filesystem::temp_directory_path() / "libAutoUpdater-updater-test";
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = base / std::to_string(tick);
    std::filesystem::create_directories(path);
    return path;
}

} // namespace

void testUpdaterQueuedCallbacksOutliveUpdater() {
    auto dispatcher = std::make_shared<QueuedDispatcher>();
    std::atomic<int> checks{0};
    auto installDir = uniqueTempDir();

    {
        autoupdater::Config config;
        config.manifestUrl = "mock://manifest";
        config.currentVersion = autoupdater::Version::parse("1.0.0").value();
        config.installDir = installDir;

        autoupdater::Updater updater(config);
        updater.setNetworkClient(std::make_shared<StaticManifestNetwork>());
        updater.setEventDispatcher(dispatcher);

        autoupdater::Callbacks callbacks;
        callbacks.onCheckResult = [&](const autoupdater::CheckResult& result) {
            LAU_REQUIRE(!result.updateAvailable);
            ++checks;
        };
        updater.setCallbacks(std::move(callbacks));
        updater.checkAsync();

        for (int i = 0; i < 200 && updater.state() != autoupdater::State::UpToDate; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        LAU_REQUIRE(updater.state() == autoupdater::State::UpToDate);
    }

    dispatcher->drain();
    LAU_REQUIRE(checks.load() == 1);

    std::error_code ec;
    std::filesystem::remove_all(installDir, ec);
}

void testUpdaterCanSuppressIntermediateCheckResultBeforeDownload() {
    auto dispatcher = std::make_shared<QueuedDispatcher>();
    std::atomic<int> checks{0};
    std::atomic<int> ready{0};
    auto installDir = uniqueTempDir();

    {
        autoupdater::Config config;
        config.manifestUrl = "mock://manifest";
        config.currentVersion = autoupdater::Version::parse("0.9.0").value();
        config.installDir = installDir;

        autoupdater::Updater updater(config);
        updater.setNetworkClient(std::make_shared<StaticManifestNetwork>());
        updater.setEventDispatcher(dispatcher);

        autoupdater::Callbacks callbacks;
        callbacks.onCheckResult = [&](const autoupdater::CheckResult&) { ++checks; };
        callbacks.onReadyToApply = [&] { ++ready; };
        updater.setCallbacks(std::move(callbacks));
        updater.checkAndDownloadAsync(false);

        for (int i = 0; i < 200 && updater.state() != autoupdater::State::ReadyToApply; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        LAU_REQUIRE(updater.state() == autoupdater::State::ReadyToApply);
    }

    dispatcher->drain();
    LAU_REQUIRE(checks.load() == 0);
    LAU_REQUIRE(ready.load() == 1);

    std::error_code ec;
    std::filesystem::remove_all(installDir, ec);
}

void testUpdaterStillReportsTerminalCheckResultWhenSuppressed() {
    auto dispatcher = std::make_shared<QueuedDispatcher>();
    std::atomic<int> checks{0};
    auto installDir = uniqueTempDir();

    {
        autoupdater::Config config;
        config.manifestUrl = "mock://manifest";
        config.currentVersion = autoupdater::Version::parse("1.0.0").value();
        config.installDir = installDir;

        autoupdater::Updater updater(config);
        updater.setNetworkClient(std::make_shared<StaticManifestNetwork>());
        updater.setEventDispatcher(dispatcher);

        autoupdater::Callbacks callbacks;
        callbacks.onCheckResult = [&](const autoupdater::CheckResult& result) {
            LAU_REQUIRE(!result.updateAvailable);
            ++checks;
        };
        updater.setCallbacks(std::move(callbacks));
        updater.checkAndDownloadAsync(false);

        for (int i = 0; i < 200 && updater.state() != autoupdater::State::UpToDate; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        LAU_REQUIRE(updater.state() == autoupdater::State::UpToDate);
    }

    dispatcher->drain();
    LAU_REQUIRE(checks.load() == 1);

    std::error_code ec;
    std::filesystem::remove_all(installDir, ec);
}

void testDownloadAsyncPersistsPendingUpdate() {
    auto dispatcher = std::make_shared<QueuedDispatcher>();
    auto installDir = uniqueTempDir();

    const auto content = std::string("new binary content");
    const auto contentHash = autoupdater::createDefaultHashProvider()->sha256Bytes(content).value();

    auto network = std::make_shared<FileServingNetwork>();
    network->fileContent = content;
    network->manifestText = std::string("{") +
                            "\"schemaVersion\": 1,"
                            "\"version\": \"1.1.0\","
                            "\"releaseId\": \"rel-1.1.0\","
                            "\"baseUrl\": \"mock://server/\","
                            "\"files\": [{\"path\": \"bin/app.txt\", \"sha256\": \"" +
                            contentHash + "\", \"size\": " + std::to_string(content.size()) + "}]}";

    {
        autoupdater::Config config;
        config.manifestUrl = "mock://manifest";
        config.currentVersion = autoupdater::Version::parse("1.0.0").value();
        config.installDir = installDir;
        config.retry.retryDelay = std::chrono::milliseconds(1);

        autoupdater::Updater updater(config);
        updater.setNetworkClient(network);
        updater.setEventDispatcher(dispatcher);

        // The split flow: check first, then download.
        updater.checkAsync();
        for (int i = 0; i < 400 && updater.state() != autoupdater::State::UpdateAvailable; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        LAU_REQUIRE(updater.state() == autoupdater::State::UpdateAvailable);

        updater.downloadAsync();
        for (int i = 0; i < 400 && updater.state() != autoupdater::State::ReadyToApply; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        LAU_REQUIRE(updater.state() == autoupdater::State::ReadyToApply);

        // The pending update must be persisted so rollbackLastUpdate() and
        // markCurrentVersionHealthy() work after a restart.
        auto store = autoupdater::createJsonStateStore(installDir / ".autoupdater" / "state.json");
        auto pending = store->loadPendingUpdate();
        LAU_REQUIRE(pending);
        LAU_REQUIRE(pending.value().has_value());
        LAU_REQUIRE(pending.value()->version.toString() == "1.1.0");
        LAU_REQUIRE(pending.value()->releaseId == "rel-1.1.0");

        auto healthy = updater.markCurrentVersionHealthy();
        LAU_REQUIRE(healthy);
        auto cleared = store->loadPendingUpdate();
        LAU_REQUIRE(cleared);
        LAU_REQUIRE(!cleared.value().has_value());
    }

    std::error_code ec;
    std::filesystem::remove_all(installDir, ec);
}
