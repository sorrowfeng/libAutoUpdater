#include "TestCommon.h"

#include "ApplyTransactionReceipt.h"
#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Updater.h"
#include "libAutoUpdater/interfaces/IEventDispatcher.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

std::filesystem::path uniqueTempDir();

class PendingStateStore final : public autoupdater::IStateStore {
  public:
    explicit PendingStateStore(autoupdater::PendingUpdate pending) : pending_(std::move(pending)) {}

    autoupdater::Result<void> saveLastAcceptedVersion(const autoupdater::Version&,
                                                      const std::string&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::Version>> loadLastAcceptedVersion() noexcept override {
        return autoupdater::Result<std::optional<autoupdater::Version>>::ok(std::nullopt);
    }

    autoupdater::Result<std::string> loadLastAcceptedReleaseId() noexcept override {
        return autoupdater::Result<std::string>::ok({});
    }

    autoupdater::Result<void> savePendingUpdate(const autoupdater::PendingUpdate& pending) noexcept override {
        pending_ = pending;
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::PendingUpdate>> loadPendingUpdate() noexcept override {
        return autoupdater::Result<std::optional<autoupdater::PendingUpdate>>::ok(pending_);
    }

    autoupdater::Result<void> clearPendingUpdate() noexcept override {
        ++clearCalls_;
        pending_.reset();
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> saveDownloadResume(const autoupdater::DownloadResumeState&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>
    loadDownloadResume(const std::string&) noexcept override {
        return autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>::ok(std::nullopt);
    }

    autoupdater::Result<void> clearDownloadResume(const std::string&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    int clearCalls() const noexcept {
        return clearCalls_;
    }

    bool hasPendingUpdate() const noexcept {
        return pending_.has_value();
    }

  private:
    std::optional<autoupdater::PendingUpdate> pending_;
    int clearCalls_ = 0;
};

class CapturingProcessLauncher final : public autoupdater::IProcessLauncher {
  public:
    autoupdater::Result<void> launch(const autoupdater::ProcessLaunchRequest& request) noexcept override {
        ++launchCalls_;
        request_ = request;
        return autoupdater::Result<void>::ok();
    }

    int launchCalls() const noexcept {
        return launchCalls_;
    }

    const std::optional<autoupdater::ProcessLaunchRequest>& request() const noexcept {
        return request_;
    }

  private:
    int launchCalls_ = 0;
    std::optional<autoupdater::ProcessLaunchRequest> request_;
};

class ForwardingFileSystemSpy final : public autoupdater::IFileSystem {
  public:
    struct RootOpen {
        std::filesystem::path path;
        autoupdater::RootAccess access = autoupdater::RootAccess::ReadOnly;
        bool create = false;
        autoupdater::RootedDirectoryCreationMode directoryMode =
            autoupdater::RootedDirectoryCreationMode::Private;
    };

    explicit ForwardingFileSystemSpy(std::shared_ptr<autoupdater::IFileSystem> delegate)
        : delegate_(std::move(delegate)) {}

    bool exists(const std::filesystem::path& path) noexcept override {
        return delegate_->exists(path);
    }

    bool isRegularFile(const std::filesystem::path& path) noexcept override {
        return delegate_->isRegularFile(path);
    }

    autoupdater::Result<std::uint64_t> fileSize(const std::filesystem::path& path) noexcept override {
        return delegate_->fileSize(path);
    }

    autoupdater::Result<void> createDirectories(const std::filesystem::path& path) noexcept override {
        ++directWriteCalls_;
        return delegate_->createDirectories(path);
    }

    autoupdater::Result<void> copyFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                       bool overwrite) noexcept override {
        ++directWriteCalls_;
        return delegate_->copyFile(from, to, overwrite);
    }

    autoupdater::Result<void> renameOrReplace(const std::filesystem::path& from,
                                              const std::filesystem::path& to) noexcept override {
        ++directWriteCalls_;
        return delegate_->renameOrReplace(from, to);
    }

    autoupdater::Result<void> remove(const std::filesystem::path& path) noexcept override {
        ++directWriteCalls_;
        return delegate_->remove(path);
    }

    autoupdater::Result<void> removeAll(const std::filesystem::path& path) noexcept override {
        ++directWriteCalls_;
        return delegate_->removeAll(path);
    }

    autoupdater::Result<std::string> readText(const std::filesystem::path& path,
                                              std::uint64_t maxBytes) noexcept override {
        return delegate_->readText(path, maxBytes);
    }

    autoupdater::Result<void> writeText(const std::filesystem::path& path,
                                        const std::string& text) noexcept override {
        ++directWriteCalls_;
        return delegate_->writeText(path, text);
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedDirectory>>
    openRoot(const std::filesystem::path& path, autoupdater::RootAccess access, bool create,
             autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        rootOpens_.push_back({path, access, create, directoryMode});
        return delegate_->openRoot(path, access, create, directoryMode);
    }

    const std::vector<RootOpen>& rootOpens() const noexcept {
        return rootOpens_;
    }

    int directWriteCalls() const noexcept {
        return directWriteCalls_;
    }

  private:
    std::shared_ptr<autoupdater::IFileSystem> delegate_;
    std::vector<RootOpen> rootOpens_;
    int directWriteCalls_ = 0;
};

void writeFileContents(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    LAU_REQUIRE(output.good());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    LAU_REQUIRE(output.good());
}

std::string readFileContents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    LAU_REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void writeTerminalReceipt(const std::filesystem::path& installDir, const std::string& transactionId,
                          const std::string& planDigest) {
    auto serialized = autoupdater::serializeApplyTransactionReceipt({transactionId, planDigest});
    LAU_REQUIRE(serialized);
    writeFileContents(installDir / ".autoupdater" / "journal" / "terminal.json", serialized.value());
}

autoupdater::Config rollbackConfig(const std::filesystem::path& root) {
    autoupdater::Config config;
    config.appId = "com.example.rollback-test";
    config.currentVersion = autoupdater::Version::parse("2.0.0").value();
    config.installDir = root / "install";
    config.tempDir = root / "requests";
    config.updaterExecutable = root / "autoupdater_apply";
    config.applyWaitTimeout = std::chrono::seconds(17);
    return config;
}

autoupdater::PendingUpdate pendingUpdateFor(const autoupdater::Config& config, const std::string& planDigest) {
    autoupdater::PendingUpdate pending;
    pending.version = config.currentVersion;
    pending.releaseId = "release-2";
    pending.backupDir = config.installDir / ".autoupdater" / "backup" / "1.0.0-to-2.0.0";
    pending.applyPlanPath = config.tempDir / "apply-plan.json";
    pending.applyPlanDigest = planDigest;
    return pending;
}

std::optional<std::string> argumentValue(const autoupdater::ProcessLaunchRequest& request,
                                         const std::string& name) {
    for (std::size_t index = 0; index + 1 < request.arguments.size(); ++index) {
        if (request.arguments[index] == name) {
            return request.arguments[index + 1];
        }
    }
    return std::nullopt;
}

bool hasArgument(const autoupdater::ProcessLaunchRequest& request, const std::string& name) {
    for (const auto& argument : request.arguments) {
        if (argument == name) {
            return true;
        }
    }
    return false;
}

void requireInstallRootWasReadOnly(const ForwardingFileSystemSpy& fileSystem,
                                   const std::filesystem::path& installDir) {
    std::size_t installRootOpenCount = 0;
    for (const auto& opened : fileSystem.rootOpens()) {
        if (opened.path.lexically_normal() != installDir.lexically_normal()) {
            continue;
        }
        ++installRootOpenCount;
        LAU_REQUIRE(opened.access == autoupdater::RootAccess::ReadOnly);
        LAU_REQUIRE(!opened.create);
        LAU_REQUIRE(opened.directoryMode == autoupdater::RootedDirectoryCreationMode::InstalledContent);
    }
    LAU_REQUIRE(installRootOpenCount == 1);
}

void requireManagedInstallContentWasNotOpenedWritable(const ForwardingFileSystemSpy& fileSystem,
                                                       const std::filesystem::path& installDir) {
    for (const auto& opened : fileSystem.rootOpens()) {
        const auto relative = opened.path.lexically_normal().lexically_relative(installDir.lexically_normal());
        if (relative.empty() || relative.is_absolute()) {
            continue;
        }
        auto component = relative.begin();
        if (component != relative.end() && *component == "..") {
            continue;
        }
        const bool privateUpdaterState = relative != "." && component != relative.end() &&
                                         *component == ".autoupdater";
        if (!privateUpdaterState) {
            LAU_REQUIRE(opened.access == autoupdater::RootAccess::ReadOnly);
            LAU_REQUIRE(!opened.create);
        }
    }
}

void requireRejectedRollbackWithoutTerminal(bool writeMismatchedTerminal) {
    const auto root = uniqueTempDir();
    const auto config = rollbackConfig(root);
    const std::string transactionId(64, 'a');
    const std::string pendingDigest(64, 'b');
    const auto managedFile = config.installDir / "bin" / "app.bin";
    writeFileContents(managedFile, "installed-version-2");
    if (writeMismatchedTerminal) {
        writeTerminalReceipt(config.installDir, transactionId, std::string(64, 'c'));
    }

    auto stateStore = std::make_shared<PendingStateStore>(pendingUpdateFor(config, pendingDigest));
    auto fileSystem = std::make_shared<ForwardingFileSystemSpy>(autoupdater::createDefaultFileSystem());
    auto launcher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);
    updater.setFileSystem(fileSystem);
    updater.setProcessLauncher(launcher);

    const auto result = updater.rollbackLastUpdate();
    LAU_REQUIRE(!result);
    LAU_REQUIRE(launcher->launchCalls() == 0);
    LAU_REQUIRE(stateStore->clearCalls() == 0);
    LAU_REQUIRE(stateStore->hasPendingUpdate());
    LAU_REQUIRE(readFileContents(managedFile) == "installed-version-2");
    LAU_REQUIRE(!std::filesystem::exists(config.tempDir / "rollback-plan.json"));
    LAU_REQUIRE(fileSystem->directWriteCalls() == 0);
    requireInstallRootWasReadOnly(*fileSystem, config.installDir);
    requireManagedInstallContentWasNotOpenedWritable(*fileSystem, config.installDir);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

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

void testUpdaterDelegatesRollbackToTerminalBoundExternalPlan() {
    const auto root = uniqueTempDir();
    const auto config = rollbackConfig(root);
    const std::string transactionId(64, 'a');
    const std::string planDigest(64, 'b');
    const auto managedFile = config.installDir / "bin" / "app.bin";
    writeFileContents(managedFile, "installed-version-2");
    writeTerminalReceipt(config.installDir, transactionId, planDigest);

    const auto pending = pendingUpdateFor(config, planDigest);
    auto stateStore = std::make_shared<PendingStateStore>(pending);
    auto fileSystem = std::make_shared<ForwardingFileSystemSpy>(autoupdater::createDefaultFileSystem());
    auto launcher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);
    updater.setFileSystem(fileSystem);
    updater.setProcessLauncher(launcher);

    const auto result = updater.rollbackLastUpdate();
    LAU_REQUIRE(result);
    LAU_REQUIRE(launcher->launchCalls() == 1);
    LAU_REQUIRE(launcher->request());
    LAU_REQUIRE(stateStore->clearCalls() == 0);
    LAU_REQUIRE(stateStore->hasPendingUpdate());
    LAU_REQUIRE(readFileContents(managedFile) == "installed-version-2");
    LAU_REQUIRE(fileSystem->directWriteCalls() == 0);
    requireInstallRootWasReadOnly(*fileSystem, config.installDir);
    requireManagedInstallContentWasNotOpenedWritable(*fileSystem, config.installDir);

    const auto requestPath = config.tempDir / "rollback-plan.json";
    LAU_REQUIRE(std::filesystem::is_regular_file(requestPath));
    const auto requestText = readFileContents(requestPath);
    auto requestPlan = autoupdater::ApplyPlan::parse(requestText, config.resources);
    LAU_REQUIRE(requestPlan);
    LAU_REQUIRE(requestPlan.value().intent == autoupdater::ApplyPlanIntent::Rollback);
    LAU_REQUIRE(requestPlan.value().rollbackOf.has_value());
    LAU_REQUIRE(requestPlan.value().rollbackOf->transactionId == transactionId);
    LAU_REQUIRE(requestPlan.value().rollbackOf->planDigest == planDigest);
    LAU_REQUIRE(requestPlan.value().installDir.lexically_normal() == config.installDir.lexically_normal());
    LAU_REQUIRE(requestPlan.value().stagingDir.lexically_normal() == pending.backupDir.lexically_normal());
    LAU_REQUIRE(requestPlan.value().operations.empty());
    LAU_REQUIRE(requestPlan.value().restartCommand.empty());

    const auto& launch = *launcher->request();
    LAU_REQUIRE(launch.executable == config.updaterExecutable);
    LAU_REQUIRE(launch.workingDirectory == config.installDir);
    LAU_REQUIRE(launch.detached);
    LAU_REQUIRE(hasArgument(launch, "--rollback"));
    const auto launchedPlan = argumentValue(launch, "--plan");
    const auto launchedDigest = argumentValue(launch, "--plan-sha256");
    const auto launchedInstallRoot = argumentValue(launch, "--install-root");
    LAU_REQUIRE(launchedPlan.has_value());
    LAU_REQUIRE(launchedDigest.has_value());
    LAU_REQUIRE(launchedInstallRoot.has_value());
    LAU_REQUIRE(std::filesystem::u8path(*launchedPlan).lexically_normal() == requestPath.lexically_normal());
    LAU_REQUIRE(std::filesystem::u8path(*launchedInstallRoot).lexically_normal() == config.installDir.lexically_normal());
    auto hashProvider = autoupdater::createDefaultHashProvider();
    auto requestDigest = hashProvider->sha256Bytes(requestText);
    LAU_REQUIRE(requestDigest);
    LAU_REQUIRE(*launchedDigest == requestDigest.value());

    std::error_code error;
    std::filesystem::remove_all(root, error);

    const auto legacyRoot = uniqueTempDir();
    const auto legacyConfig = rollbackConfig(legacyRoot);
    writeFileContents(legacyConfig.installDir / "bin" / "app.bin", "installed-version-2");
    writeTerminalReceipt(legacyConfig.installDir, transactionId, planDigest);
    auto legacyPending = pendingUpdateFor(legacyConfig, "");
    auto legacyStore = std::make_shared<PendingStateStore>(legacyPending);
    auto legacyLauncher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater legacyUpdater(legacyConfig);
    legacyUpdater.setStateStore(legacyStore);
    legacyUpdater.setProcessLauncher(legacyLauncher);
    const auto legacyResult = legacyUpdater.rollbackLastUpdate();
    LAU_REQUIRE(legacyResult);
    LAU_REQUIRE(legacyLauncher->launchCalls() == 1);
    const auto legacyRequest = autoupdater::ApplyPlan::parse(
        readFileContents(legacyConfig.tempDir / "rollback-plan.json"), legacyConfig.resources);
    LAU_REQUIRE(legacyRequest);
    LAU_REQUIRE(legacyRequest.value().rollbackOf.has_value());
    LAU_REQUIRE(legacyRequest.value().rollbackOf->transactionId == transactionId);
    LAU_REQUIRE(legacyRequest.value().rollbackOf->planDigest == planDigest);
    std::filesystem::remove_all(legacyRoot, error);

    const auto overlapRoot = uniqueTempDir();
    auto overlapConfig = rollbackConfig(overlapRoot);
    overlapConfig.tempDir = overlapConfig.installDir / "bin";
    const auto overlapManagedFile = overlapConfig.installDir / "bin" / "app.bin";
    writeFileContents(overlapManagedFile, "installed-version-2");
    writeTerminalReceipt(overlapConfig.installDir, transactionId, planDigest);
    auto overlapStore =
        std::make_shared<PendingStateStore>(pendingUpdateFor(overlapConfig, planDigest));
    auto overlapLauncher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater overlapUpdater(overlapConfig);
    overlapUpdater.setStateStore(overlapStore);
    overlapUpdater.setProcessLauncher(overlapLauncher);
    const auto overlapResult = overlapUpdater.rollbackLastUpdate();
    LAU_REQUIRE(!overlapResult);
    LAU_REQUIRE(overlapResult.error().code == autoupdater::ErrorCode::InvalidConfig);
    LAU_REQUIRE(overlapLauncher->launchCalls() == 0);
    LAU_REQUIRE(readFileContents(overlapManagedFile) == "installed-version-2");
    LAU_REQUIRE(!std::filesystem::exists(overlapConfig.tempDir / "rollback-plan.json"));
    std::filesystem::remove_all(overlapRoot, error);

    const auto privateRoot = uniqueTempDir();
    auto privateConfig = rollbackConfig(privateRoot);
    privateConfig.tempDir = privateConfig.installDir / ".autoupdater" / "staging" / "requests";
    writeFileContents(privateConfig.installDir / "bin" / "app.bin", "installed-version-2");
    writeTerminalReceipt(privateConfig.installDir, transactionId, planDigest);
    auto privateStore =
        std::make_shared<PendingStateStore>(pendingUpdateFor(privateConfig, planDigest));
    auto privateLauncher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater privateUpdater(privateConfig);
    privateUpdater.setStateStore(privateStore);
    privateUpdater.setProcessLauncher(privateLauncher);
    const auto privateResult = privateUpdater.rollbackLastUpdate();
    LAU_REQUIRE(privateResult);
    LAU_REQUIRE(privateLauncher->launchCalls() == 1);
    LAU_REQUIRE(std::filesystem::is_regular_file(privateConfig.tempDir / "rollback-plan.json"));
    std::filesystem::remove_all(privateRoot, error);

    const auto backupOverlapRoot = uniqueTempDir();
    auto backupOverlapConfig = rollbackConfig(backupOverlapRoot);
    auto backupOverlapPending = pendingUpdateFor(backupOverlapConfig, planDigest);
    backupOverlapConfig.tempDir = backupOverlapPending.backupDir;
    writeFileContents(backupOverlapConfig.installDir / "bin" / "app.bin", "installed-version-2");
    writeTerminalReceipt(backupOverlapConfig.installDir, transactionId, planDigest);
    auto backupOverlapStore =
        std::make_shared<PendingStateStore>(backupOverlapPending);
    auto backupOverlapLauncher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater backupOverlapUpdater(backupOverlapConfig);
    backupOverlapUpdater.setStateStore(backupOverlapStore);
    backupOverlapUpdater.setProcessLauncher(backupOverlapLauncher);
    const auto backupOverlapResult = backupOverlapUpdater.rollbackLastUpdate();
    LAU_REQUIRE(!backupOverlapResult);
    LAU_REQUIRE(backupOverlapResult.error().code == autoupdater::ErrorCode::InvalidConfig);
    LAU_REQUIRE(backupOverlapLauncher->launchCalls() == 0);
    LAU_REQUIRE(!std::filesystem::exists(backupOverlapConfig.tempDir / "rollback-plan.json"));
    std::filesystem::remove_all(backupOverlapRoot, error);

    requireRejectedRollbackWithoutTerminal(false);
    requireRejectedRollbackWithoutTerminal(true);
}
