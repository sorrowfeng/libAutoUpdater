#include "TestCommon.h"

#include "ApplyLauncher.h"
#include "ApplyTransactionReceipt.h"
#include "ProcessWait.h"
#include "util/Rfc3339.h"
#include "util/Sha256.h"
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
#include <condition_variable>
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

class ScriptedManifestNetwork final : public autoupdater::INetworkClient {
  public:
    struct Step {
        enum class Kind { Response, Failure, WaitForCancellation };

        Kind kind = Kind::Response;
        std::string body;
        autoupdater::Error error{autoupdater::ErrorCode::ManifestDownloadFailed,
                                 "scripted manifest request failed"};

        static Step response(std::string body) {
            Step step;
            step.body = std::move(body);
            return step;
        }

        static Step failure(autoupdater::ErrorCode code = autoupdater::ErrorCode::ManifestDownloadFailed) {
            Step step;
            step.kind = Kind::Failure;
            step.error = {code, "scripted manifest request failed"};
            return step;
        }

        static Step waitForCancellation() {
            Step step;
            step.kind = Kind::WaitForCancellation;
            return step;
        }
    };

    explicit ScriptedManifestNetwork(std::vector<Step> steps) : steps_(std::move(steps)) {}

    autoupdater::Result<autoupdater::TextResponse> getText(const std::string& url,
                                                           const autoupdater::NetworkOptions&, std::uint64_t,
                                                           autoupdater::CancellationToken& cancel) noexcept override {
        try {
            Step step;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto index = calls_++;
                if (index >= steps_.size()) {
                    callsCv_.notify_all();
                    return autoupdater::Result<autoupdater::TextResponse>::fail(
                        {autoupdater::ErrorCode::ManifestDownloadFailed, "manifest request script exhausted"});
                }
                step = steps_[index];
            }
            callsCv_.notify_all();

            if (step.kind == Step::Kind::WaitForCancellation) {
                std::unique_lock<std::mutex> lock(mutex_);
                while (!cancel.isCancelled() && !releaseBlocked_) {
                    releaseCv_.wait_for(lock, std::chrono::milliseconds(2));
                }
                return autoupdater::Result<autoupdater::TextResponse>::fail(
                    {autoupdater::ErrorCode::Cancelled, "scripted manifest request cancelled"});
            }
            if (cancel.isCancelled()) {
                return autoupdater::Result<autoupdater::TextResponse>::fail(
                    {autoupdater::ErrorCode::Cancelled, "a new task inherited a cancelled token"});
            }
            if (step.kind == Step::Kind::Failure) {
                return autoupdater::Result<autoupdater::TextResponse>::fail(std::move(step.error));
            }

            autoupdater::TextResponse response;
            response.response.statusCode = 200;
            response.response.effectiveUrl = url;
            response.body = std::move(step.body);
            return autoupdater::Result<autoupdater::TextResponse>::ok(std::move(response));
        } catch (...) {
            return autoupdater::Result<autoupdater::TextResponse>::fail(
                {autoupdater::ErrorCode::InternalError, "unexpected scripted network failure"});
        }
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, autoupdater::IRootedFile&, const autoupdater::NetworkOptions&, std::uint64_t,
                   const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "scripted tests use manifests without artifacts"});
    }

    bool waitForCalls(std::size_t expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return callsCv_.wait_for(lock, timeout, [this, expected] { return calls_ >= expected; });
    }

    std::size_t calls() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    void releaseBlockedRequests() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            releaseBlocked_ = true;
        }
        releaseCv_.notify_all();
    }

  private:
    std::vector<Step> steps_;
    mutable std::mutex mutex_;
    std::condition_variable callsCv_;
    std::condition_variable releaseCv_;
    std::size_t calls_ = 0;
    bool releaseBlocked_ = false;
};

std::filesystem::path uniqueTempDir();

class PendingStateStore final : public autoupdater::IStateStore,
                                public autoupdater::IPendingUpdateCompareAndSet {
  public:
    PendingStateStore() = default;
    explicit PendingStateStore(std::optional<autoupdater::PendingUpdate> pending) : pending_(std::move(pending)) {}

    autoupdater::Result<void> saveLastAcceptedVersion(const autoupdater::Version& version,
                                                      const std::string& releaseId) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++saveLastAcceptedCalls_;
        lastAcceptedVersion_ = version;
        lastAcceptedReleaseId_ = releaseId;
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::Version>> loadLastAcceptedVersion() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failAcceptedLoads_) {
            return autoupdater::Result<std::optional<autoupdater::Version>>::fail(
                {autoupdater::ErrorCode::StateStoreError, "scripted accepted-state corruption"});
        }
        return autoupdater::Result<std::optional<autoupdater::Version>>::ok(lastAcceptedVersion_);
    }

    autoupdater::Result<std::string> loadLastAcceptedReleaseId() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return autoupdater::Result<std::string>::ok(lastAcceptedReleaseId_);
    }

    autoupdater::Result<void>
    commitHealthyVersion(const autoupdater::Version& version, const std::string& releaseId,
                         const std::optional<autoupdater::PendingUpdate>& expectedPending) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto samePending = [](const autoupdater::PendingUpdate& left,
                                    const autoupdater::PendingUpdate& right) {
            return left.version.toString() == right.version.toString() && left.releaseId == right.releaseId &&
                   left.backupDir.lexically_normal() == right.backupDir.lexically_normal() &&
                   left.applyPlanPath.lexically_normal() == right.applyPlanPath.lexically_normal() &&
                   left.applyPlanDigest == right.applyPlanDigest;
        };
        if (pending_.has_value() != expectedPending.has_value() ||
            (pending_ && !samePending(*pending_, *expectedPending))) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "scripted pending state changed"});
        }
        ++saveLastAcceptedCalls_;
        ++clearCalls_;
        lastAcceptedVersion_ = version;
        lastAcceptedReleaseId_ = releaseId;
        pending_.reset();
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> savePendingUpdate(const autoupdater::PendingUpdate& pending) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++savePendingCalls_;
        if (failSavePending_) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "scripted pending-state save failed"});
        }
        const auto samePending = [](const autoupdater::PendingUpdate& left,
                                    const autoupdater::PendingUpdate& right) {
            return left.version.toString() == right.version.toString() && left.releaseId == right.releaseId &&
                   left.backupDir.lexically_normal() == right.backupDir.lexically_normal() &&
                   left.applyPlanPath.lexically_normal() == right.applyPlanPath.lexically_normal() &&
                   left.applyPlanDigest == right.applyPlanDigest;
        };
        if (pending_) {
            if (samePending(*pending_, pending)) {
                return autoupdater::Result<void>::ok();
            }
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "scripted different pending state exists"});
        }
        pending_ = pending;
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::PendingUpdate>> loadPendingUpdate() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return autoupdater::Result<std::optional<autoupdater::PendingUpdate>>::ok(pending_);
    }

    autoupdater::Result<void> clearPendingUpdate() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++clearCalls_;
        pending_.reset();
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void>
    clearPendingUpdateIfMatches(const autoupdater::PendingUpdate& expectedPending) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto samePending = [](const autoupdater::PendingUpdate& left,
                                    const autoupdater::PendingUpdate& right) {
            return left.version.toString() == right.version.toString() && left.releaseId == right.releaseId &&
                   left.backupDir.lexically_normal() == right.backupDir.lexically_normal() &&
                   left.applyPlanPath.lexically_normal() == right.applyPlanPath.lexically_normal() &&
                   left.applyPlanDigest == right.applyPlanDigest;
        };
        if (!pending_ || !samePending(*pending_, expectedPending)) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "scripted pending state changed"});
        }
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
        std::lock_guard<std::mutex> lock(mutex_);
        return clearCalls_;
    }

    bool hasPendingUpdate() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.has_value();
    }

    std::optional<autoupdater::PendingUpdate> pendingUpdate() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }

    void setPendingUpdate(std::optional<autoupdater::PendingUpdate> pending) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(pending);
    }

    void failPendingSaves(bool fail) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        failSavePending_ = fail;
    }

    void failAcceptedLoads(bool fail) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        failAcceptedLoads_ = fail;
    }

    int savePendingCalls() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return savePendingCalls_;
    }

    int saveLastAcceptedCalls() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return saveLastAcceptedCalls_;
    }

  private:
    mutable std::mutex mutex_;
    std::optional<autoupdater::PendingUpdate> pending_;
    std::optional<autoupdater::Version> lastAcceptedVersion_;
    std::string lastAcceptedReleaseId_;
    bool failSavePending_ = false;
    bool failAcceptedLoads_ = false;
    int savePendingCalls_ = 0;
    int saveLastAcceptedCalls_ = 0;
    int clearCalls_ = 0;
};

class StateStoreWithoutPendingCompareAndSet final : public autoupdater::IStateStore {
  public:
    explicit StateStoreWithoutPendingCompareAndSet(std::shared_ptr<autoupdater::IStateStore> delegate)
        : delegate_(std::move(delegate)) {}

    autoupdater::Result<void> saveLastAcceptedVersion(const autoupdater::Version& version,
                                                      const std::string& releaseId) noexcept override {
        return delegate_->saveLastAcceptedVersion(version, releaseId);
    }

    autoupdater::Result<std::optional<autoupdater::Version>> loadLastAcceptedVersion() noexcept override {
        return delegate_->loadLastAcceptedVersion();
    }

    autoupdater::Result<std::string> loadLastAcceptedReleaseId() noexcept override {
        return delegate_->loadLastAcceptedReleaseId();
    }

    autoupdater::Result<void>
    commitHealthyVersion(const autoupdater::Version& version, const std::string& releaseId,
                         const std::optional<autoupdater::PendingUpdate>& expectedPending) noexcept override {
        return delegate_->commitHealthyVersion(version, releaseId, expectedPending);
    }

    autoupdater::Result<void> savePendingUpdate(const autoupdater::PendingUpdate& pending) noexcept override {
        return delegate_->savePendingUpdate(pending);
    }

    autoupdater::Result<std::optional<autoupdater::PendingUpdate>> loadPendingUpdate() noexcept override {
        return delegate_->loadPendingUpdate();
    }

    autoupdater::Result<void> clearPendingUpdate() noexcept override {
        return delegate_->clearPendingUpdate();
    }

    autoupdater::Result<void> saveDownloadResume(const autoupdater::DownloadResumeState& state) noexcept override {
        return delegate_->saveDownloadResume(state);
    }

    autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>
    loadDownloadResume(const std::string& key) noexcept override {
        return delegate_->loadDownloadResume(key);
    }

    autoupdater::Result<void> clearDownloadResume(const std::string& key) noexcept override {
        return delegate_->clearDownloadResume(key);
    }

  private:
    std::shared_ptr<autoupdater::IStateStore> delegate_;
};

class CapturingProcessLauncher final : public autoupdater::IProcessLauncher {
  public:
    autoupdater::Result<void> launch(const autoupdater::ProcessLaunchRequest& request) noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = request;
            ++launchCalls_;
        }
        launchCv_.notify_all();
        return autoupdater::Result<void>::ok();
    }

    int launchCalls() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return launchCalls_;
    }

    bool waitForLaunchCalls(int expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return launchCv_.wait_for(lock, timeout, [this, expected] { return launchCalls_ >= expected; });
    }

    std::optional<autoupdater::ProcessLaunchRequest> request() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable launchCv_;
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
                          const std::string& planDigest,
                          std::optional<autoupdater::util::UtcInstant> completedAt = std::nullopt) {
    auto serialized =
        autoupdater::serializeApplyTransactionReceipt({transactionId, planDigest, std::move(completedAt)});
    LAU_REQUIRE(serialized);
    writeFileContents(installDir / ".autoupdater" / "journal" / "terminal.json", serialized.value());
}

std::string writeApplyPlanSnapshot(const std::filesystem::path& installDir,
                                   const std::string& transactionId,
                                   const autoupdater::ApplyPlan& plan) {
    const auto json = plan.toJson();
    const auto digest = autoupdater::util::sha256Bytes(json);
    writeFileContents(installDir / ".autoupdater" / "journal" /
                          (transactionId + ".plan.json"),
                      json);
    return digest;
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

void writeLegacyPendingState(const std::filesystem::path& statePath,
                             const autoupdater::PendingUpdate& pending) {
    const std::string json =
        "{\"pendingUpdate\":{\"version\":\"" + pending.version.toString() +
        "\",\"releaseId\":\"" + pending.releaseId + "\",\"backupDir\":\"" +
        pending.backupDir.generic_u8string() + "\",\"applyPlanPath\":\"" +
        pending.applyPlanPath.generic_u8string() + "\"}}";
    writeFileContents(statePath, json);
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
    static std::atomic<std::uint64_t> sequence{0};
    auto path = base / (std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path);
    return path;
}

class ScopedTempDir final {
  public:
    ScopedTempDir() : path_(uniqueTempDir()) {}

    ~ScopedTempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::string manifestJson(const std::string& version, const std::string& releaseId) {
    return std::string(R"json({
      "schemaVersion": 1,
      "version": ")json") +
           version + R"json(",
      "releaseId": ")json" +
           releaseId + R"json(",
      "baseUrl": "https://updates.example.test/release/",
      "files": []
    })json";
}

autoupdater::Config updaterConfig(const std::filesystem::path& root) {
    autoupdater::Config config;
    config.appId = "com.example.updater-state-test";
    config.manifestUrl = "https://updates.example.test/manifest.json";
    config.currentVersion = autoupdater::Version::parse("1.0.0").value();
    config.installDir = root / "install";
    config.tempDir = root / "staging";
    config.updaterExecutable = root / "autoupdater_apply";
    config.security.allowedBaseUrls = {"https://updates.example.test/"};
    std::filesystem::create_directories(config.installDir);
    return config;
}

template <class Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
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

void testUpdaterDirectCallbacksAreExceptionSafeAndReentrant() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2"))});
    auto stateStore = std::make_shared<PendingStateStore>();
    auto launcher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater updater(config);
    updater.setNetworkClient(network);
    updater.setStateStore(stateStore);
    updater.setProcessLauncher(launcher);

    std::atomic<int> checkCallbacks{0};
    std::atomic<int> readyCallbacks{0};
    std::atomic<int> errorCallbacks{0};
    std::atomic<int> stateCallbacks{0};
    std::atomic_bool sawAvailableUpdate{false};
    autoupdater::Callbacks callbacks;
    callbacks.onStateChanged = [&](autoupdater::State) {
        stateCallbacks.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("state callback failure");
    };
    callbacks.onCheckResult = [&](const autoupdater::CheckResult& result) {
        checkCallbacks.fetch_add(1, std::memory_order_relaxed);
        sawAvailableUpdate.store(result.updateAvailable, std::memory_order_relaxed);
        // This request is bound while no plan is ready and must not borrow the
        // ready generation produced by the queued download that follows it.
        updater.applyAndRestartAsync();
        updater.downloadAsync();
        throw std::runtime_error("check callback failure");
    };
    callbacks.onReadyToApply = [&] {
        readyCallbacks.fetch_add(1, std::memory_order_relaxed);
        updater.applyAndRestartAsync();
        updater.applyAndRestartAsync();
        throw std::runtime_error("ready callback failure");
    };
    callbacks.onError = [&](const autoupdater::Error&) {
        errorCallbacks.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("error callback failure");
    };
    updater.setCallbacks(std::move(callbacks));

    updater.checkAsync();
    LAU_REQUIRE(launcher->waitForLaunchCalls(1, std::chrono::seconds(2)));
    LAU_REQUIRE(waitUntil([&] { return errorCallbacks.load(std::memory_order_relaxed) >= 2; }));
    LAU_REQUIRE(waitUntil([&] { return updater.state() == autoupdater::State::Applying; }));
    LAU_REQUIRE(checkCallbacks.load(std::memory_order_relaxed) == 1);
    LAU_REQUIRE(readyCallbacks.load(std::memory_order_relaxed) == 1);
    LAU_REQUIRE(errorCallbacks.load(std::memory_order_relaxed) == 2);
    LAU_REQUIRE(stateCallbacks.load(std::memory_order_relaxed) >= 4);
    LAU_REQUIRE(sawAvailableUpdate.load(std::memory_order_relaxed));
    LAU_REQUIRE(launcher->launchCalls() == 1);
    LAU_REQUIRE(stateStore->savePendingCalls() == 1);
    LAU_REQUIRE(network->calls() == 1);

    updater.checkAsync();
    updater.startPeriodicCheck(std::chrono::milliseconds(0), false, false);
    LAU_REQUIRE(waitUntil([&] { return errorCallbacks.load(std::memory_order_relaxed) >= 4; }));
    LAU_REQUIRE(errorCallbacks.load(std::memory_order_relaxed) == 4);
    LAU_REQUIRE(updater.state() == autoupdater::State::Applying);
    LAU_REQUIRE(network->calls() == 1);
}

void testUpdaterOverlappingChecksAreNonBlockingAndCancellationIsolated() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::waitForCancellation(),
            ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1"))});
    auto stateStore = std::make_shared<PendingStateStore>();
    autoupdater::Updater updater(config);
    updater.setNetworkClient(network);
    updater.setStateStore(stateStore);

    updater.checkAsync();
    const auto healthyWhileQueued = updater.markCurrentVersionHealthy();
    const auto rollbackWhileQueued = updater.rollbackLastUpdate();
    LAU_REQUIRE(!healthyWhileQueued);
    LAU_REQUIRE(healthyWhileQueued.error().code == autoupdater::ErrorCode::InternalError);
    LAU_REQUIRE(!rollbackWhileQueued);
    LAU_REQUIRE(rollbackWhileQueued.error().code == autoupdater::ErrorCode::InternalError);
    LAU_REQUIRE(network->waitForCalls(1, std::chrono::seconds(2)));

    std::atomic_bool callerStarted{false};
    std::atomic_bool callerReturned{false};
    std::thread overlappingCaller([&] {
        callerStarted.store(true, std::memory_order_release);
        updater.checkAsync();
        callerReturned.store(true, std::memory_order_release);
    });
    const bool startedInTime = waitUntil(
        [&] { return callerStarted.load(std::memory_order_acquire); }, std::chrono::milliseconds(500));
    const bool returnedBeforeCancellation =
        startedInTime && waitUntil([&] { return callerReturned.load(std::memory_order_acquire); },
                                   std::chrono::milliseconds(500));

    updater.cancel();
    network->releaseBlockedRequests();
    overlappingCaller.join();

    LAU_REQUIRE(startedInTime);
    LAU_REQUIRE(returnedBeforeCancellation);
    LAU_REQUIRE(network->waitForCalls(2, std::chrono::seconds(2)));
    LAU_REQUIRE(waitUntil([&] { return updater.state() == autoupdater::State::UpToDate; }));
    LAU_REQUIRE(network->calls() == 2);
}

void testUpdaterCanBeDestroyedFromDirectCallback() {
    const auto root = uniqueTempDir();
    const auto config = updaterConfig(root);
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1"))});
    auto owner = std::make_shared<std::unique_ptr<autoupdater::Updater>>(
        std::make_unique<autoupdater::Updater>(config));
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    (*owner)->setNetworkClient(network);

    autoupdater::Callbacks callbacks;
    callbacks.onCheckResult = [owner, destroyed](const autoupdater::CheckResult&) {
        owner->reset();
        destroyed->store(true, std::memory_order_release);
    };
    (*owner)->setCallbacks(std::move(callbacks));
    (*owner)->checkAsync();

    LAU_REQUIRE(waitUntil([&] { return destroyed->load(std::memory_order_acquire); }));
    LAU_REQUIRE(!*owner);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testUpdaterNewGenerationInvalidatesReadyPlan() {
    for (const bool failSecondCheck : {false, true}) {
        ScopedTempDir temp;
        const auto config = updaterConfig(temp.path());
        std::vector<ScriptedManifestNetwork::Step> steps;
        steps.push_back(ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2")));
        if (failSecondCheck) {
            steps.push_back(ScriptedManifestNetwork::Step::failure());
        } else {
            steps.push_back(ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1")));
        }
        auto network = std::make_shared<ScriptedManifestNetwork>(std::move(steps));
        auto stateStore = std::make_shared<PendingStateStore>();
        auto launcher = std::make_shared<CapturingProcessLauncher>();
        autoupdater::Updater updater(config);
        updater.setNetworkClient(network);
        updater.setStateStore(stateStore);
        updater.setProcessLauncher(launcher);

        std::atomic<int> readyCallbacks{0};
        std::atomic<int> errorCallbacks{0};
        autoupdater::Callbacks callbacks;
        callbacks.onReadyToApply = [&] { readyCallbacks.fetch_add(1, std::memory_order_relaxed); };
        callbacks.onError = [&](const autoupdater::Error&) {
            errorCallbacks.fetch_add(1, std::memory_order_relaxed);
        };
        updater.setCallbacks(std::move(callbacks));

        updater.checkAndDownloadAsync();
        LAU_REQUIRE(waitUntil([&] {
            return updater.state() == autoupdater::State::ReadyToApply &&
                   readyCallbacks.load(std::memory_order_relaxed) == 1;
        }));
        LAU_REQUIRE(stateStore->hasPendingUpdate());

        updater.checkAsync();
        const auto expectedState = failSecondCheck ? autoupdater::State::Failed : autoupdater::State::UpToDate;
        LAU_REQUIRE(waitUntil([&] {
            return network->calls() == 2 && updater.state() == expectedState &&
                   (!failSecondCheck || errorCallbacks.load(std::memory_order_relaxed) >= 1);
        }));

        const auto errorsBeforeApply = errorCallbacks.load(std::memory_order_relaxed);
        updater.applyAndRestartAsync();
        LAU_REQUIRE(waitUntil([&] {
            return errorCallbacks.load(std::memory_order_relaxed) > errorsBeforeApply;
        }));
        LAU_REQUIRE(launcher->launchCalls() == 0);
        LAU_REQUIRE(updater.state() == expectedState);
    }
}

void testUpdaterQueuedDownloadKeepsRequestedGeneration() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2")),
            ScriptedManifestNetwork::Step::response(manifestJson("3.0.0", "release-3"))});
    auto stateStore = std::make_shared<PendingStateStore>();
    auto launcher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater updater(config);
    updater.setNetworkClient(network);
    updater.setStateStore(stateStore);
    updater.setProcessLauncher(launcher);

    std::atomic_bool scheduled{false};
    std::atomic<int> readyCallbacks{0};
    std::atomic<int> errorCallbacks{0};
    autoupdater::Callbacks callbacks;
    callbacks.onCheckResult = [&](const autoupdater::CheckResult& result) {
        if (result.remoteVersion && result.remoteVersion->toString() == "2.0.0" &&
            !scheduled.exchange(true, std::memory_order_relaxed)) {
            updater.checkAsync();
            updater.downloadAsync();
            // A non-generation-bound sentinel runs after the stale download,
            // making completion observable without relying on sleeps.
            updater.applyAndRestartAsync();
        }
    };
    callbacks.onReadyToApply = [&] { readyCallbacks.fetch_add(1, std::memory_order_relaxed); };
    callbacks.onError = [&](const autoupdater::Error&) {
        errorCallbacks.fetch_add(1, std::memory_order_relaxed);
    };
    updater.setCallbacks(std::move(callbacks));

    updater.checkAsync();
    LAU_REQUIRE(waitUntil([&] { return errorCallbacks.load(std::memory_order_relaxed) >= 1; }));
    LAU_REQUIRE(scheduled.load(std::memory_order_relaxed));
    LAU_REQUIRE(network->calls() == 2);
    LAU_REQUIRE(updater.state() == autoupdater::State::UpdateAvailable);
    LAU_REQUIRE(stateStore->savePendingCalls() == 0);
    LAU_REQUIRE(!stateStore->hasPendingUpdate());
    LAU_REQUIRE(readyCallbacks.load(std::memory_order_relaxed) == 0);
    LAU_REQUIRE(launcher->launchCalls() == 0);
}

void testUpdaterRequiresPersistedPendingBeforeReady() {
    for (const bool nullStore : {false, true}) {
        ScopedTempDir temp;
        const auto config = updaterConfig(temp.path());
        auto network = std::make_shared<ScriptedManifestNetwork>(
            std::vector<ScriptedManifestNetwork::Step>{
                ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2"))});
        auto stateStore = std::make_shared<PendingStateStore>();
        stateStore->failPendingSaves(true);
        auto launcher = std::make_shared<CapturingProcessLauncher>();
        autoupdater::Updater updater(config);
        updater.setNetworkClient(network);
        updater.setStateStore(nullStore ? std::shared_ptr<autoupdater::IStateStore>{} : stateStore);
        updater.setProcessLauncher(launcher);

        std::atomic<int> readyCallbacks{0};
        std::atomic<int> readyStates{0};
        std::atomic<int> errorCallbacks{0};
        std::atomic<autoupdater::ErrorPhase> errorPhase{autoupdater::ErrorPhase::General};
        autoupdater::Callbacks callbacks;
        callbacks.onReadyToApply = [&] { readyCallbacks.fetch_add(1, std::memory_order_relaxed); };
        callbacks.onStateChanged = [&](autoupdater::State state) {
            if (state == autoupdater::State::ReadyToApply) {
                readyStates.fetch_add(1, std::memory_order_relaxed);
            }
        };
        callbacks.onError = [&](const autoupdater::Error& error) {
            errorPhase.store(error.phase, std::memory_order_relaxed);
            errorCallbacks.fetch_add(1, std::memory_order_release);
        };
        updater.setCallbacks(std::move(callbacks));

        updater.checkAndDownloadAsync();
        LAU_REQUIRE(waitUntil([&] {
            return updater.state() == autoupdater::State::Failed &&
                   errorCallbacks.load(std::memory_order_acquire) >= 1;
        }));
        LAU_REQUIRE(readyCallbacks.load(std::memory_order_relaxed) == 0);
        LAU_REQUIRE(readyStates.load(std::memory_order_relaxed) == 0);
        LAU_REQUIRE(errorPhase.load(std::memory_order_relaxed) == autoupdater::ErrorPhase::StatePersistence);
        LAU_REQUIRE(launcher->launchCalls() == 0);
        if (!nullStore) {
            LAU_REQUIRE(stateStore->savePendingCalls() == 1);
            LAU_REQUIRE(!stateStore->hasPendingUpdate());
        }

        const auto errorsBeforeApply = errorCallbacks.load(std::memory_order_acquire);
        updater.applyAndRestartAsync();
        LAU_REQUIRE(waitUntil([&] {
            return errorCallbacks.load(std::memory_order_acquire) > errorsBeforeApply;
        }));
        LAU_REQUIRE(launcher->launchCalls() == 0);
    }
}

void testUpdaterApplyRequiresMatchingPersistedPending() {
    for (const bool clearPending : {false, true}) {
        ScopedTempDir temp;
        const auto config = updaterConfig(temp.path());
        auto network = std::make_shared<ScriptedManifestNetwork>(
            std::vector<ScriptedManifestNetwork::Step>{
                ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2"))});
        auto stateStore = std::make_shared<PendingStateStore>();
        auto launcher = std::make_shared<CapturingProcessLauncher>();
        autoupdater::Updater updater(config);
        updater.setNetworkClient(network);
        updater.setStateStore(stateStore);
        updater.setProcessLauncher(launcher);

        std::atomic<int> readyCallbacks{0};
        std::atomic<int> errorCallbacks{0};
        autoupdater::Callbacks callbacks;
        callbacks.onReadyToApply = [&] { readyCallbacks.fetch_add(1, std::memory_order_relaxed); };
        callbacks.onError = [&](const autoupdater::Error&) {
            errorCallbacks.fetch_add(1, std::memory_order_relaxed);
        };
        updater.setCallbacks(std::move(callbacks));

        updater.checkAndDownloadAsync();
        LAU_REQUIRE(waitUntil([&] {
            return updater.state() == autoupdater::State::ReadyToApply &&
                   readyCallbacks.load(std::memory_order_relaxed) == 1;
        }));
        auto pending = stateStore->pendingUpdate();
        LAU_REQUIRE(pending.has_value());
        if (clearPending) {
            LAU_REQUIRE(stateStore->clearPendingUpdate());
        } else {
            pending->releaseId += "-tampered";
            stateStore->setPendingUpdate(std::move(pending));
        }

        updater.applyAndRestartAsync();
        LAU_REQUIRE(waitUntil([&] {
            return updater.state() == autoupdater::State::Failed &&
                   errorCallbacks.load(std::memory_order_relaxed) >= 1;
        }));
        LAU_REQUIRE(launcher->launchCalls() == 0);
    }
}

void testUpdaterHealthyMarkPreservesFuturePending() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    autoupdater::PendingUpdate pending;
    pending.version = autoupdater::Version::parse("2.0.0").value();
    pending.releaseId = "release-2";
    pending.backupDir = config.installDir / ".autoupdater" / "backup" / "1.0.0-to-2.0.0";
    pending.applyPlanPath = config.tempDir / "apply-plan.json";
    pending.applyPlanDigest = std::string(64, 'a');
    auto stateStore = std::make_shared<PendingStateStore>(pending);
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);

    const auto healthy = updater.markCurrentVersionHealthy();
    LAU_REQUIRE(!healthy);
    LAU_REQUIRE(healthy.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(stateStore->hasPendingUpdate());
    LAU_REQUIRE(stateStore->clearCalls() == 0);
    LAU_REQUIRE(stateStore->saveLastAcceptedCalls() == 0);
    const auto preserved = stateStore->pendingUpdate();
    LAU_REQUIRE(preserved.has_value());
    LAU_REQUIRE(preserved->version.toString() == "2.0.0");
}

void testUpdaterPeriodicCheckPreservesReadyGeneration() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2"))});
    auto stateStore = std::make_shared<PendingStateStore>();
    auto launcher = std::make_shared<CapturingProcessLauncher>();
    autoupdater::Updater updater(config);
    updater.setNetworkClient(network);
    updater.setStateStore(stateStore);
    updater.setProcessLauncher(launcher);

    std::atomic<int> readyCallbacks{0};
    std::atomic<int> errorCallbacks{0};
    autoupdater::Callbacks callbacks;
    callbacks.onReadyToApply = [&] { readyCallbacks.fetch_add(1, std::memory_order_relaxed); };
    callbacks.onError = [&](const autoupdater::Error&) {
        errorCallbacks.fetch_add(1, std::memory_order_relaxed);
    };
    updater.setCallbacks(std::move(callbacks));
    updater.checkAndDownloadAsync();
    LAU_REQUIRE(waitUntil([&] {
        return updater.state() == autoupdater::State::ReadyToApply &&
               readyCallbacks.load(std::memory_order_relaxed) == 1;
    }));

    updater.startPeriodicCheck(std::chrono::milliseconds(0), false, false);
    LAU_REQUIRE(errorCallbacks.load(std::memory_order_relaxed) == 1);
    LAU_REQUIRE(updater.state() == autoupdater::State::ReadyToApply);

    updater.startPeriodicCheck(std::chrono::milliseconds(5), false, true);
    const bool unexpectedSecondRequest = network->waitForCalls(2, std::chrono::milliseconds(150));
    const auto finalState = updater.state();
    updater.stopPeriodicCheck();

    LAU_REQUIRE(!unexpectedSecondRequest);
    LAU_REQUIRE(network->calls() == 1);
    LAU_REQUIRE(finalState == autoupdater::State::ReadyToApply);
    LAU_REQUIRE(readyCallbacks.load(std::memory_order_relaxed) == 1);
    LAU_REQUIRE(stateStore->savePendingCalls() == 1);

    updater.applyAndRestartAsync();
    LAU_REQUIRE(launcher->waitForLaunchCalls(1, std::chrono::seconds(2)));
    LAU_REQUIRE(updater.state() == autoupdater::State::Applying);
}

void testUpdaterQueueOverflowErrorReentryIsBounded() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{ScriptedManifestNetwork::Step::waitForCancellation()});
    autoupdater::Updater updater(config);
    updater.setNetworkClient(network);

    std::atomic<int> errorCallbacks{0};
    autoupdater::Callbacks callbacks;
    callbacks.onError = [&](const autoupdater::Error&) {
        if (errorCallbacks.fetch_add(1, std::memory_order_relaxed) == 0) {
            updater.checkAsync();
        }
    };
    updater.setCallbacks(std::move(callbacks));

    updater.checkAsync();
    LAU_REQUIRE(network->waitForCalls(1, std::chrono::seconds(2)));
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < 65; ++index) {
        updater.checkAsync();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    LAU_REQUIRE(elapsed < std::chrono::seconds(1));
    LAU_REQUIRE(errorCallbacks.load(std::memory_order_relaxed) == 1);

    updater.setCallbacks({});
    updater.cancel();
    network->releaseBlockedRequests();
}

void testUpdaterQueuedDispatcherSuppressesStaleGenerationAfterDestruction() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto dispatcher = std::make_shared<QueuedDispatcher>();
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2")),
            ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1"))});
    std::atomic<int> staleCallbacks{0};
    std::atomic<int> currentCallbacks{0};

    {
        autoupdater::Updater updater(config);
        updater.setNetworkClient(network);
        updater.setEventDispatcher(dispatcher);
        autoupdater::Callbacks callbacks;
        callbacks.onCheckResult = [&](const autoupdater::CheckResult& result) {
            if (result.remoteVersion && result.remoteVersion->toString() == "2.0.0") {
                staleCallbacks.fetch_add(1, std::memory_order_relaxed);
            } else {
                currentCallbacks.fetch_add(1, std::memory_order_relaxed);
            }
            throw std::runtime_error("queued callback failure");
        };
        updater.setCallbacks(std::move(callbacks));

        updater.checkAsync();
        LAU_REQUIRE(waitUntil([&] { return updater.state() == autoupdater::State::UpdateAvailable; }));
        updater.checkAsync();
        LAU_REQUIRE(waitUntil([&] { return updater.state() == autoupdater::State::UpToDate; }));
    }

    dispatcher->drain();
    LAU_REQUIRE(staleCallbacks.load(std::memory_order_relaxed) == 0);
    LAU_REQUIRE(currentCallbacks.load(std::memory_order_relaxed) == 1);
}

void testUpdaterHealthyMarkRequiresMatchingTerminalReceipt() {
    ScopedTempDir temp;
    auto config = updaterConfig(temp.path());
    config.currentVersion = autoupdater::Version::parse("2.0.0").value();
    const std::string planDigest(64, 'b');
    auto pending = pendingUpdateFor(config, planDigest);
    writeTerminalReceipt(config.installDir, std::string(64, 'a'), planDigest);
    auto stateStore = std::make_shared<PendingStateStore>(pending);
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);

    const auto healthy = updater.markCurrentVersionHealthy();
    LAU_REQUIRE(healthy);
    LAU_REQUIRE(stateStore->saveLastAcceptedCalls() == 1);
    LAU_REQUIRE(stateStore->clearCalls() == 1);
    LAU_REQUIRE(!stateStore->hasPendingUpdate());
}

void testUpdaterConfirmsLegacyPendingFromTerminalSnapshot() {
    ScopedTempDir temp;
    auto config = updaterConfig(temp.path());
    config.currentVersion = autoupdater::Version::parse("2.0.0").value();
    auto pending = pendingUpdateFor(config, "");
    const auto backupMarker = pending.backupDir / "retained.marker";
    writeFileContents(backupMarker, "legacy rollback evidence");

    autoupdater::ApplyPlan forward;
    forward.schemaVersion = 2;
    forward.intent = autoupdater::ApplyPlanIntent::Install;
    forward.appId = config.appId;
    forward.fromVersion = "1.0.0";
    forward.toVersion = pending.version.toString();
    forward.releaseId = pending.releaseId;
    forward.manifestSha256 = std::string(64, 'e');
    forward.installDir = config.installDir;
    forward.stagingDir = pending.applyPlanPath.parent_path();
    forward.backupDir = pending.backupDir;
    const std::string transactionId(64, 'a');
    const auto forwardDigest = writeApplyPlanSnapshot(config.installDir, transactionId, forward);
    // A schema-v2 terminal receipt has no completedAt. It remains confirmable,
    // but the legacy pending value is authorized only by its exact snapshot.
    writeTerminalReceipt(config.installDir, transactionId, forwardDigest);

    const auto statePath = config.installDir / ".autoupdater" / "state.json";
    writeLegacyPendingState(statePath, pending);
    auto stateStore = autoupdater::createJsonStateStore(statePath);
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);

    const auto healthy = updater.markCurrentVersionHealthy();
    LAU_REQUIRE(healthy);
    auto afterHealthy = stateStore->loadPendingUpdate();
    LAU_REQUIRE(afterHealthy);
    LAU_REQUIRE(!afterHealthy.value().has_value());
    auto accepted = stateStore->loadLastAcceptedVersion();
    LAU_REQUIRE(accepted);
    LAU_REQUIRE(accepted.value().has_value());
    LAU_REQUIRE(accepted.value()->toString() == "2.0.0");
    LAU_REQUIRE(std::filesystem::is_regular_file(backupMarker));
}

void testUpdaterScopesCustomStagingByManifest() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());

    const auto prepare = [&](const std::string& releaseId) {
        auto network = std::make_shared<ScriptedManifestNetwork>(
            std::vector<ScriptedManifestNetwork::Step>{
                ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", releaseId))});
        auto stateStore = std::make_shared<PendingStateStore>();
        {
            autoupdater::Updater updater(config);
            updater.setNetworkClient(network);
            updater.setStateStore(stateStore);
            updater.checkAndDownloadAsync();
            LAU_REQUIRE(waitUntil([&] { return updater.state() == autoupdater::State::ReadyToApply; }));
        }
        auto pending = stateStore->pendingUpdate();
        LAU_REQUIRE(pending.has_value());
        LAU_REQUIRE(std::filesystem::is_regular_file(pending->applyPlanPath));
        return *pending;
    };

    const auto first = prepare("release-2-first");
    const auto firstPlanContents = readFileContents(first.applyPlanPath);
    const auto second = prepare("release-2-second");

    LAU_REQUIRE(first.applyPlanPath.parent_path().parent_path().parent_path().lexically_normal() ==
                config.tempDir.lexically_normal());
    LAU_REQUIRE(second.applyPlanPath.parent_path().parent_path().parent_path().lexically_normal() ==
                config.tempDir.lexically_normal());
    LAU_REQUIRE(first.applyPlanPath.parent_path().parent_path().filename() == "2.0.0");
    LAU_REQUIRE(second.applyPlanPath.parent_path().parent_path().filename() == "2.0.0");
    LAU_REQUIRE(first.applyPlanPath.parent_path().filename().string().size() == 64);
    LAU_REQUIRE(second.applyPlanPath.parent_path().filename().string().size() == 64);
    LAU_REQUIRE(first.applyPlanPath.parent_path().lexically_normal() !=
                second.applyPlanPath.parent_path().lexically_normal());
    LAU_REQUIRE(first.backupDir.lexically_normal() != second.backupDir.lexically_normal());
    LAU_REQUIRE(readFileContents(first.applyPlanPath) == firstPlanContents);
}

void testUpdaterHealthConfirmationDeadlineAndRetention() {
    {
        ScopedTempDir temp;
        auto config = updaterConfig(temp.path());
        config.currentVersion = autoupdater::Version::parse("2.0.0").value();
        config.healthConfirmationTimeout = std::chrono::seconds(1);
        const std::string planDigest(64, 'b');
        const auto pending = pendingUpdateFor(config, planDigest);
        const auto backupMarker = pending.backupDir / "retained.marker";
        writeFileContents(backupMarker, "rollback evidence");
        writeTerminalReceipt(config.installDir, std::string(64, 'a'), planDigest,
                             autoupdater::util::UtcInstant{0, 0});

        auto stateStore = std::make_shared<PendingStateStore>(pending);
        auto network = std::make_shared<ScriptedManifestNetwork>(
            std::vector<ScriptedManifestNetwork::Step>{
                ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2"))});
        autoupdater::Updater updater(config);
        updater.setStateStore(stateStore);
        updater.setNetworkClient(network);

        std::atomic<int> deadlineErrors{0};
        autoupdater::Callbacks callbacks;
        callbacks.onError = [&](const autoupdater::Error& error) {
            if (error.code == autoupdater::ErrorCode::ApplyFailed &&
                error.message.find("deadline expired") != std::string::npos) {
                deadlineErrors.fetch_add(1, std::memory_order_relaxed);
            }
        };
        updater.setCallbacks(std::move(callbacks));
        updater.checkOnStartupAsync();
        LAU_REQUIRE(waitUntil([&] {
            return updater.state() == autoupdater::State::Failed &&
                   deadlineErrors.load(std::memory_order_relaxed) == 1;
        }));
        LAU_REQUIRE(network->calls() == 0);
        LAU_REQUIRE(stateStore->hasPendingUpdate());
        LAU_REQUIRE(std::filesystem::is_regular_file(backupMarker));

        std::optional<autoupdater::Result<void>> rejectedHealth;
        LAU_REQUIRE(waitUntil([&] {
            auto attempted = updater.markCurrentVersionHealthy();
            if (!attempted && attempted.error().code == autoupdater::ErrorCode::InternalError) {
                return false;
            }
            rejectedHealth = std::move(attempted);
            return true;
        }));
        LAU_REQUIRE(rejectedHealth.has_value());
        LAU_REQUIRE(!*rejectedHealth);
        LAU_REQUIRE(rejectedHealth->error().code == autoupdater::ErrorCode::ApplyFailed);
        LAU_REQUIRE(stateStore->hasPendingUpdate());
        LAU_REQUIRE(std::filesystem::is_regular_file(backupMarker));
    }

    {
        ScopedTempDir temp;
        auto config = updaterConfig(temp.path());
        config.currentVersion = autoupdater::Version::parse("2.0.0").value();
        config.healthConfirmationTimeout = std::chrono::seconds(60);
        const std::string planDigest(64, 'f');
        const auto pending = pendingUpdateFor(config, planDigest);
        const auto completedAt = autoupdater::util::currentUtcInstant();
        LAU_REQUIRE(completedAt);
        writeTerminalReceipt(config.installDir, std::string(64, 'e'), planDigest, completedAt.value());
        auto stateStore = std::make_shared<PendingStateStore>(pending);
        autoupdater::Updater updater(config);
        updater.setStateStore(stateStore);

        const auto healthy = updater.markCurrentVersionHealthy();
        LAU_REQUIRE(healthy);
        LAU_REQUIRE(!stateStore->hasPendingUpdate());
    }

    {
        ScopedTempDir temp;
        auto config = updaterConfig(temp.path());
        config.currentVersion = autoupdater::Version::parse("2.0.0").value();
        config.healthConfirmationTimeout = std::chrono::seconds(0);
        const std::string planDigest(64, 'd');
        const auto pending = pendingUpdateFor(config, planDigest);
        const auto backupMarker = pending.backupDir / "retained.marker";
        writeFileContents(backupMarker, "rollback evidence");
        writeTerminalReceipt(config.installDir, std::string(64, 'c'), planDigest,
                             autoupdater::util::UtcInstant{0, 0});
        auto stateStore = std::make_shared<PendingStateStore>(pending);
        autoupdater::Updater updater(config);
        updater.setStateStore(stateStore);

        const auto healthy = updater.markCurrentVersionHealthy();
        LAU_REQUIRE(healthy);
        LAU_REQUIRE(!stateStore->hasPendingUpdate());
        LAU_REQUIRE(stateStore->clearCalls() == 1);
        LAU_REQUIRE(std::filesystem::is_regular_file(backupMarker));
    }

    for (const auto invalidTimeout : {std::chrono::seconds(-1), std::chrono::seconds(24 * 60 * 60 + 1)}) {
        ScopedTempDir temp;
        auto config = updaterConfig(temp.path());
        config.healthConfirmationTimeout = invalidTimeout;
        auto network = std::make_shared<ScriptedManifestNetwork>(
            std::vector<ScriptedManifestNetwork::Step>{
                ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1"))});
        autoupdater::Updater updater(config);
        updater.setNetworkClient(network);
        std::atomic<int> invalidConfigErrors{0};
        autoupdater::Callbacks callbacks;
        callbacks.onError = [&](const autoupdater::Error& error) {
            if (error.code == autoupdater::ErrorCode::InvalidConfig) {
                invalidConfigErrors.fetch_add(1, std::memory_order_relaxed);
            }
        };
        updater.setCallbacks(std::move(callbacks));
        updater.checkOnStartupAsync();
        LAU_REQUIRE(waitUntil([&] {
            return updater.state() == autoupdater::State::Failed &&
                   invalidConfigErrors.load(std::memory_order_relaxed) == 1;
        }));
        LAU_REQUIRE(network->calls() == 0);
    }
}

void testUpdaterReconcilesCompletedRollbackPendingState() {
    ScopedTempDir temp;
    auto config = updaterConfig(temp.path());
    const std::string forwardTransactionId(64, 'a');
    const std::string forwardPlanDigest(64, 'b');
    autoupdater::PendingUpdate pending;
    pending.version = autoupdater::Version::parse("2.0.0").value();
    pending.releaseId = "release-2";
    pending.backupDir = config.installDir / ".autoupdater" / "backup" / "1.0.0-to-2.0.0" /
                        std::string(64, 'e');
    pending.applyPlanPath = config.tempDir / "apply-plan.json";
    pending.applyPlanDigest = forwardPlanDigest;

    autoupdater::ApplyPlan rollback;
    rollback.schemaVersion = 2;
    rollback.intent = autoupdater::ApplyPlanIntent::Rollback;
    rollback.rollbackOf = autoupdater::ApplyTransactionReference{forwardTransactionId, forwardPlanDigest};
    rollback.appId = config.appId;
    rollback.fromVersion = pending.version.toString();
    rollback.toVersion = config.currentVersion.toString();
    rollback.releaseId = pending.releaseId;
    rollback.manifestSha256 = std::string(64, 'e');
    rollback.installDir = config.installDir;
    rollback.stagingDir = pending.backupDir;
    rollback.backupDir = config.installDir / ".autoupdater" / "backup" / "rollback" /
                         forwardTransactionId;
    const auto rollbackJson = rollback.toJson();
    const auto rollbackDigest = autoupdater::util::sha256Bytes(rollbackJson);
    const std::string rollbackTransactionId(64, 'c');
    writeFileContents(config.installDir / ".autoupdater" / "journal" /
                          (rollbackTransactionId + ".plan.json"),
                      rollbackJson);
    writeTerminalReceipt(config.installDir, rollbackTransactionId, rollbackDigest,
                         autoupdater::util::UtcInstant{0, 0});

    auto stateStore = std::make_shared<PendingStateStore>(pending);
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1"))});
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);
    updater.setNetworkClient(network);
    updater.checkOnStartupAsync();
    LAU_REQUIRE(waitUntil([&] {
        return updater.state() == autoupdater::State::UpToDate && network->calls() == 1;
    }));
    LAU_REQUIRE(!stateStore->hasPendingUpdate());
    LAU_REQUIRE(stateStore->clearCalls() == 1);

    auto pendingWithoutCapability = std::make_shared<PendingStateStore>(pending);
    auto stateStoreWithoutCapability = std::make_shared<StateStoreWithoutPendingCompareAndSet>(
        pendingWithoutCapability);
    auto unusedNetwork = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1"))});
    autoupdater::Updater updaterWithoutCapability(config);
    updaterWithoutCapability.setStateStore(stateStoreWithoutCapability);
    updaterWithoutCapability.setNetworkClient(unusedNetwork);
    std::atomic<int> capabilityErrors{0};
    autoupdater::Callbacks callbacks;
    callbacks.onError = [&](const autoupdater::Error& error) {
        if (error.code == autoupdater::ErrorCode::StateStoreError &&
            error.message.find("atomic pending-update reconciliation") != std::string::npos) {
            capabilityErrors.fetch_add(1, std::memory_order_relaxed);
        }
    };
    updaterWithoutCapability.setCallbacks(std::move(callbacks));
    updaterWithoutCapability.checkOnStartupAsync();
    LAU_REQUIRE(waitUntil([&] {
        return updaterWithoutCapability.state() == autoupdater::State::Failed &&
               capabilityErrors.load(std::memory_order_relaxed) == 1;
    }));
    LAU_REQUIRE(unusedNetwork->calls() == 0);
    LAU_REQUIRE(pendingWithoutCapability->hasPendingUpdate());
    LAU_REQUIRE(pendingWithoutCapability->clearCalls() == 0);
}

void testUpdaterReconcilesLegacyCompletedRollbackPendingState() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    autoupdater::PendingUpdate pending;
    pending.version = autoupdater::Version::parse("2.0.0").value();
    pending.releaseId = "release-2";
    pending.backupDir = config.installDir / ".autoupdater" / "backup" /
                        "1.0.0-to-2.0.0" / std::string(64, 'e');
    pending.applyPlanPath = config.tempDir / "2.0.0" / std::string(64, 'e') /
                            "apply-plan.json";

    autoupdater::ApplyPlan forward;
    forward.schemaVersion = 2;
    forward.intent = autoupdater::ApplyPlanIntent::Install;
    forward.appId = config.appId;
    forward.fromVersion = config.currentVersion.toString();
    forward.toVersion = pending.version.toString();
    forward.releaseId = pending.releaseId;
    forward.manifestSha256 = std::string(64, 'e');
    forward.installDir = config.installDir;
    forward.stagingDir = pending.applyPlanPath.parent_path();
    forward.backupDir = pending.backupDir;
    const std::string forwardTransactionId(64, 'a');
    const auto forwardDigest = writeApplyPlanSnapshot(config.installDir, forwardTransactionId, forward);

    autoupdater::ApplyPlan rollback;
    rollback.schemaVersion = 2;
    rollback.intent = autoupdater::ApplyPlanIntent::Rollback;
    rollback.rollbackOf = autoupdater::ApplyTransactionReference{forwardTransactionId, forwardDigest};
    rollback.appId = forward.appId;
    rollback.fromVersion = forward.toVersion;
    rollback.toVersion = forward.fromVersion;
    rollback.releaseId = forward.releaseId;
    rollback.manifestSha256 = forward.manifestSha256;
    rollback.installDir = forward.installDir;
    rollback.stagingDir = forward.backupDir;
    rollback.backupDir = config.installDir / ".autoupdater" / "backup" / "rollback" /
                         forwardTransactionId;
    const std::string rollbackTransactionId(64, 'c');
    const auto rollbackDigest = writeApplyPlanSnapshot(config.installDir, rollbackTransactionId, rollback);
    writeTerminalReceipt(config.installDir, rollbackTransactionId, rollbackDigest,
                         autoupdater::util::UtcInstant{0, 0});

    const auto statePath = config.installDir / ".autoupdater" / "state.json";
    writeLegacyPendingState(statePath, pending);
    auto stateStore = autoupdater::createJsonStateStore(statePath);
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("1.0.0", "release-1")),
            ScriptedManifestNetwork::Step::response(manifestJson("3.0.0", "release-3")),
        });
    autoupdater::Updater updater(config);
    updater.setStateStore(stateStore);
    updater.setNetworkClient(network);

    updater.checkOnStartupAsync();
    LAU_REQUIRE(waitUntil([&] {
        return updater.state() == autoupdater::State::UpToDate && network->calls() == 1;
    }));
    auto afterRollback = stateStore->loadPendingUpdate();
    LAU_REQUIRE(afterRollback);
    LAU_REQUIRE(!afterRollback.value().has_value());

    updater.checkAndDownloadAsync();
    LAU_REQUIRE(waitUntil([&] {
        return updater.state() == autoupdater::State::ReadyToApply && network->calls() == 2;
    }));
    auto nextPending = stateStore->loadPendingUpdate();
    LAU_REQUIRE(nextPending);
    LAU_REQUIRE(nextPending.value().has_value());
    LAU_REQUIRE(nextPending.value()->version.toString() == "3.0.0");
    LAU_REQUIRE(!nextPending.value()->applyPlanDigest.empty());
}

void testApplyLauncherBoundsProcessWaitTimeout() {
    autoupdater::Config config;
    config.updaterExecutable = "autoupdater_apply";
    config.installDir = "install";
    CapturingProcessLauncher launcher;
    const std::string digest(64, 'a');

    config.applyWaitTimeout = std::chrono::seconds(-1);
    auto launched = autoupdater::launchApplyProcess(config, "apply-plan.json", digest,
                                                     autoupdater::ApplyLaunchIntent::Install, launcher);
    LAU_REQUIRE(!launched);
    LAU_REQUIRE(launched.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
    LAU_REQUIRE(launcher.launchCalls() == 0);

    config.applyWaitTimeout =
        autoupdater::detail::kMaximumProcessWaitTimeout + std::chrono::seconds(1);
    launched = autoupdater::launchApplyProcess(config, "apply-plan.json", digest,
                                                autoupdater::ApplyLaunchIntent::Install, launcher);
    LAU_REQUIRE(!launched);
    LAU_REQUIRE(launcher.launchCalls() == 0);

    config.applyWaitTimeout = std::chrono::seconds(0);
    launched = autoupdater::launchApplyProcess(config, "apply-plan.json", digest,
                                                autoupdater::ApplyLaunchIntent::Install, launcher);
    LAU_REQUIRE(launched);
    LAU_REQUIRE(launcher.launchCalls() == 1);
    const auto request = launcher.request();
    LAU_REQUIRE(request);
    LAU_REQUIRE(argumentValue(*request, "--wait") == std::optional<std::string>("0"));
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

    const auto launch = launcher->request();
    LAU_REQUIRE(launch.has_value());
    LAU_REQUIRE(launch->executable == config.updaterExecutable);
    LAU_REQUIRE(launch->workingDirectory == config.installDir);
    LAU_REQUIRE(launch->detached);
    LAU_REQUIRE(hasArgument(*launch, "--rollback"));
    const auto launchedPlan = argumentValue(*launch, "--plan");
    const auto launchedDigest = argumentValue(*launch, "--plan-sha256");
    const auto launchedInstallRoot = argumentValue(*launch, "--install-root");
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

void testUpdaterFailsClosedWhenAcceptedStateIsUnreadable() {
    ScopedTempDir temp;
    const auto config = updaterConfig(temp.path());
    auto network = std::make_shared<ScriptedManifestNetwork>(
        std::vector<ScriptedManifestNetwork::Step>{
            ScriptedManifestNetwork::Step::response(manifestJson("2.0.0", "release-2"))});
    auto stateStore = std::make_shared<PendingStateStore>();
    stateStore->failAcceptedLoads(true);

    autoupdater::Updater updater(config);
    updater.setNetworkClient(network);
    updater.setStateStore(stateStore);

    std::atomic<int> checkCallbacks{0};
    std::atomic<int> stateErrors{0};
    autoupdater::Callbacks callbacks;
    callbacks.onCheckResult = [&](const autoupdater::CheckResult&) {
        checkCallbacks.fetch_add(1, std::memory_order_relaxed);
    };
    callbacks.onError = [&](const autoupdater::Error& error) {
        if (error.code == autoupdater::ErrorCode::StateStoreError) {
            stateErrors.fetch_add(1, std::memory_order_relaxed);
        }
    };
    updater.setCallbacks(std::move(callbacks));

    updater.checkAsync();
    LAU_REQUIRE(waitUntil([&] {
        return updater.state() == autoupdater::State::Failed &&
               stateErrors.load(std::memory_order_relaxed) == 1;
    }));
    LAU_REQUIRE(checkCallbacks.load(std::memory_order_relaxed) == 0);
    LAU_REQUIRE(network->calls() == 1);
}
