#include "TestCommon.h"

#include "libAutoUpdater/Updater.h"
#include "libAutoUpdater/interfaces/IEventDispatcher.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <atomic>
#include <chrono>
#include <filesystem>
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
    autoupdater::Result<autoupdater::TextResponse> getText(const std::string& url, const autoupdater::NetworkOptions&,
                                                           std::uint64_t,
                                                           autoupdater::CancellationToken&) noexcept override {
        autoupdater::TextResponse response;
        response.response.statusCode = 200;
        response.response.effectiveUrl = url;
        response.body = R"json({
          "schemaVersion": 1,
          "version": "1.0.0",
          "baseUrl": "https://updates.example.test/release/",
          "files": []
        })json";
        return autoupdater::Result<autoupdater::TextResponse>::ok(std::move(response));
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, autoupdater::IRootedFile&, const autoupdater::NetworkOptions&, std::uint64_t,
                   const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "not used"});
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
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.currentVersion = autoupdater::Version::parse("1.0.0").value();
        config.installDir = installDir;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};

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
