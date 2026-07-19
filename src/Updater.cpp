#include "libAutoUpdater/Updater.h"

#include "ApplyLauncher.h"
#include "ApplyPlanWriter.h"
#include "DownloadExecutor.h"
#include "LocalSnapshotBuilder.h"
#include "ManifestFetcher.h"
#include "UpdatePlanner.h"
#include "util/PathUtil.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <utility>

namespace autoupdater {

namespace {

std::string safeVersionForPath(const Version& version) {
    auto text = version.toString();
    for (auto& ch : text) {
        if (ch == '+' || ch == '/' || ch == '\\' || ch == ':') {
            ch = '_';
        }
    }
    return text;
}

std::string detectPlatform() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string detectArch() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::filesystem::perms sanitizedFilePermissions(std::filesystem::perms permissions) {
    constexpr auto allowed = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                             std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                             std::filesystem::perms::group_write | std::filesystem::perms::group_exec |
                             std::filesystem::perms::others_read | std::filesystem::perms::others_write |
                             std::filesystem::perms::others_exec;
    return permissions & allowed;
}

Result<void> validateResourceLimits(const ResourceLimits& resources) {
    const auto& json = resources.json;
    const ResourceLimits ceilings;
    if (json.maxDepth == 0 || json.maxNodes == 0 || json.maxStringBytes == 0 || json.maxNumberBytes == 0 ||
        json.maxContainerEntries == 0 || json.maxDepth > JsonResourceLimits::absoluteMaxDepth ||
        json.maxNodes > JsonResourceLimits::absoluteMaxNodes ||
        json.maxStringBytes > JsonResourceLimits::absoluteMaxStringBytes ||
        json.maxNumberBytes > JsonResourceLimits::absoluteMaxNumberBytes ||
        json.maxContainerEntries > JsonResourceLimits::absoluteMaxContainerEntries) {
        return Result<void>::fail({ErrorCode::InvalidConfig, "JSON resource limits are outside the safe range"});
    }
    if (resources.maxIndexBytes > ceilings.maxIndexBytes || resources.maxManifestBytes > ceilings.maxManifestBytes ||
        resources.maxSignatureBytes > ceilings.maxSignatureBytes ||
        resources.maxArtifactBytes > ceilings.maxArtifactBytes ||
        resources.maxTotalArtifactBytes > ceilings.maxTotalArtifactBytes ||
        resources.maxApplyPlanBytes > ceilings.maxApplyPlanBytes || resources.maxStateBytes > ceilings.maxStateBytes ||
        json.maxDepth > ceilings.json.maxDepth || json.maxNodes > ceilings.json.maxNodes ||
        json.maxStringBytes > ceilings.json.maxStringBytes || json.maxNumberBytes > ceilings.json.maxNumberBytes ||
        json.maxContainerEntries > ceilings.json.maxContainerEntries) {
        return Result<void>::fail({ErrorCode::InvalidConfig, "Resource limits exceed the updater safety ceiling"});
    }
    if (resources.maxIndexBytes == 0 || resources.maxManifestBytes == 0 || resources.maxApplyPlanBytes == 0 ||
        resources.maxStateBytes == 0 || resources.maxArtifactBytes > resources.maxTotalArtifactBytes) {
        return Result<void>::fail({ErrorCode::InvalidConfig, "Resource byte limits are inconsistent"});
    }
    return Result<void>::ok();
}

} // namespace

struct Updater::Impl {
    explicit Impl(Config cfg)
        : config(std::move(cfg)), network(createDefaultNetworkClient()), hashProvider(createDefaultHashProvider()),
          fileSystem(createDefaultFileSystem()), signatureVerifier(createDefaultSignatureVerifier()),
          dispatcher(createDirectDispatcher()), processLauncher(createDefaultProcessLauncher()) {
        if (config.platform.empty()) {
            config.platform = detectPlatform();
        }
        if (config.arch.empty()) {
            config.arch = detectArch();
        }
        if (config.tempDir.empty() && !config.installDir.empty()) {
            config.tempDir = util::defaultStagingRoot(config.installDir) / "staging";
        }
        if (!config.installDir.empty()) {
            stateStore =
                createJsonStateStore(util::defaultStagingRoot(config.installDir) / "state.json", config.resources);
        }
    }

    ~Impl() {
        cancel();
        stopPeriodicCheck();
        joinWorker();
    }

    void setCallbacks(Callbacks value) {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks = std::move(value);
    }

    struct Dependencies {
        std::shared_ptr<INetworkClient> network;
        std::shared_ptr<IHashProvider> hashProvider;
        std::shared_ptr<IFileSystem> fileSystem;
        std::shared_ptr<ISignatureVerifier> signatureVerifier;
        std::shared_ptr<IEventDispatcher> dispatcher;
        std::shared_ptr<IProcessLauncher> processLauncher;
        std::shared_ptr<IStateStore> stateStore;
    };

    Dependencies dependenciesCopy() {
        std::lock_guard<std::mutex> lock(mutex);
        return Dependencies{
            network, hashProvider, fileSystem, signatureVerifier, dispatcher, processLauncher, stateStore,
        };
    }

    void setState(State next) {
        stateValue.store(next, std::memory_order_relaxed);
        auto callback = callbacksCopy().onStateChanged;
        post([callback = std::move(callback), next] {
            if (callback) {
                callback(next);
            }
        });
    }

    State state() const noexcept {
        return stateValue.load(std::memory_order_relaxed);
    }

    std::shared_ptr<CancellationToken> tokenCopy() {
        std::lock_guard<std::mutex> lock(tokenMutex);
        return currentToken;
    }

    void cancel() noexcept {
        auto token = tokenCopy();
        if (token) {
            token->cancel();
        }
    }

    void joinWorker() {
        std::lock_guard<std::mutex> lock(workerMutex);
        joinWorkerUnlocked();
    }

    void joinWorkerUnlocked() {
        if (worker.joinable()) {
            worker.join();
        }
    }

    Callbacks callbacksCopy() {
        std::lock_guard<std::mutex> lock(mutex);
        return callbacks;
    }

    void post(std::function<void()> fn) {
        auto target = dependenciesCopy().dispatcher;
        if (target) {
            target->post(std::move(fn));
        }
    }

    void reportError(Error error) {
        setState(State::Failed);
        auto callback = callbacksCopy().onError;
        post([callback = std::move(callback), error = std::move(error)] {
            if (callback) {
                callback(error);
            }
        });
    }

    Result<void> validateConfig(const Dependencies& deps) {
        if (config.manifestUrl.empty()) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "manifestUrl is required"});
        }
        if (config.installDir.empty()) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "installDir is required"});
        }
        if (!deps.network || !deps.hashProvider || !deps.fileSystem || !deps.signatureVerifier || !deps.dispatcher ||
            !deps.processLauncher) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "Updater dependencies are incomplete"});
        }
        auto resources = validateResourceLimits(config.resources);
        if (!resources) {
            return resources;
        }
        return Result<void>::ok();
    }

    Result<UpdateDecision> checkInternal(Config& effectiveConfig, ManifestEnvelope& envelopeOut,
                                         const Dependencies& deps) {
        auto valid = validateConfig(deps);
        if (!valid) {
            return Result<UpdateDecision>::fail(valid.error());
        }

        setState(State::Checking);
        auto envelope = fetchAndVerifyManifest(effectiveConfig, *deps.network, *deps.hashProvider,
                                               *deps.signatureVerifier, *tokenCopy());
        if (!envelope) {
            return Result<UpdateDecision>::fail(envelope.error());
        }

        if (effectiveConfig.tempDir == util::defaultStagingRoot(effectiveConfig.installDir) / "staging") {
            effectiveConfig.tempDir = util::defaultStagingRoot(effectiveConfig.installDir) / "staging" /
                                      safeVersionForPath(envelope.value().manifest.version);
        }

        auto snapshot =
            buildLocalSnapshot(effectiveConfig, envelope.value().manifest, *deps.fileSystem, *deps.hashProvider);
        if (!snapshot) {
            return Result<UpdateDecision>::fail(snapshot.error());
        }

        std::optional<Version> lastAccepted;
        if (deps.stateStore) {
            auto loaded = deps.stateStore->loadLastAcceptedVersion();
            if (loaded) {
                lastAccepted = loaded.value();
            }
        }

        auto decision = planUpdate(effectiveConfig, envelope.value(), snapshot.value(), lastAccepted);
        if (!decision) {
            return Result<UpdateDecision>::fail(decision.error());
        }

        envelopeOut = std::move(envelope.value());
        return decision;
    }

    void start(std::function<void()> task) noexcept {
        try {
            std::lock_guard<std::mutex> workerLock(workerMutex);
            joinWorkerUnlocked();
            auto token = std::make_shared<CancellationToken>();
            {
                std::lock_guard<std::mutex> lock(tokenMutex);
                currentToken = token;
            }
            worker = std::thread([task = std::move(task), token] {
                (void)token;
                task();
            });
        } catch (...) {
            reportError({ErrorCode::InternalError, "Failed to start updater worker"});
        }
    }

    void checkAsync() noexcept {
        start([this] {
            auto deps = dependenciesCopy();
            Config effective = config;
            ManifestEnvelope envelope;
            auto decision = checkInternal(effective, envelope, deps);
            if (!decision) {
                reportError(decision.error());
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                lastConfig = effective;
                lastEnvelope = envelope;
                lastDecision = decision.value();
            }

            const auto check = decision.value().checkResult;
            setState(check.updateAvailable ? State::UpdateAvailable : State::UpToDate);
            auto callback = callbacksCopy().onCheckResult;
            post([callback = std::move(callback), check] {
                if (callback) {
                    callback(check);
                }
            });
        });
    }

    void checkAndDownloadAsync() noexcept {
        start([this] {
            auto deps = dependenciesCopy();
            Config effective = config;
            ManifestEnvelope envelope;
            auto decision = checkInternal(effective, envelope, deps);
            if (!decision) {
                reportError(decision.error());
                return;
            }

            const auto check = decision.value().checkResult;
            auto checkCallback = callbacksCopy().onCheckResult;
            post([callback = std::move(checkCallback), check] {
                if (callback) {
                    callback(check);
                }
            });

            if (!check.updateAvailable || check.reinstallRequired) {
                setState(check.updateAvailable ? State::UpdateAvailable : State::UpToDate);
                std::lock_guard<std::mutex> lock(mutex);
                lastConfig = effective;
                lastEnvelope = envelope;
                lastDecision = decision.value();
                return;
            }

            setState(State::Downloading);
            auto downloaded = executeDownloads(
                effective, decision.value(), *deps.network, *deps.fileSystem, *deps.hashProvider, deps.stateStore.get(),
                [this](const Progress& progress) {
                    auto callback = callbacksCopy().onProgress;
                    post([callback = std::move(callback), progress] {
                        if (callback) {
                            callback(progress);
                        }
                    });
                },
                *tokenCopy());
            if (!downloaded) {
                reportError(downloaded.error());
                return;
            }

            auto written = writeApplyPlan(effective, envelope, decision.value(), *deps.fileSystem);
            if (!written) {
                reportError(written.error());
                return;
            }

            if (deps.stateStore) {
                PendingUpdate pending;
                pending.version = envelope.manifest.version;
                pending.releaseId = envelope.manifest.releaseId;
                pending.backupDir = written.value().plan.backupDir;
                pending.applyPlanPath = written.value().path;
                deps.stateStore->savePendingUpdate(pending);
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                lastConfig = effective;
                lastEnvelope = envelope;
                lastDecision = decision.value();
                lastApplyPlan = written.value();
            }

            setState(State::ReadyToApply);
            auto readyCallback = callbacksCopy().onReadyToApply;
            post([callback = std::move(readyCallback)] {
                if (callback) {
                    callback();
                }
            });
        });
    }

    void checkOnStartupAsync(bool downloadWhenAvailable) noexcept {
        if (downloadWhenAvailable) {
            checkAndDownloadAsync();
        } else {
            checkAsync();
        }
    }

    void startPeriodicCheck(std::chrono::milliseconds interval, bool downloadWhenAvailable,
                            bool runImmediately) noexcept {
        if (interval.count() <= 0) {
            reportError({ErrorCode::InvalidConfig, "Periodic check interval must be positive"});
            return;
        }

        stopPeriodicCheck();
        {
            std::lock_guard<std::mutex> lock(periodicMutex);
            periodicStop = false;
        }

        try {
            periodicWorker = std::thread([this, interval, downloadWhenAvailable, runImmediately] {
                auto trigger = [this, downloadWhenAvailable] {
                    const auto current = state();
                    if (current == State::Checking || current == State::Downloading || current == State::Applying) {
                        return;
                    }
                    if (downloadWhenAvailable) {
                        checkAndDownloadAsync();
                    } else {
                        checkAsync();
                    }
                };

                if (runImmediately) {
                    trigger();
                }

                std::unique_lock<std::mutex> lock(periodicMutex);
                while (!periodicStop) {
                    if (periodicCv.wait_for(lock, interval, [this] { return periodicStop; })) {
                        break;
                    }
                    lock.unlock();
                    trigger();
                    lock.lock();
                }
            });
        } catch (...) {
            reportError({ErrorCode::InternalError, "Failed to start periodic updater thread"});
        }
    }

    void stopPeriodicCheck() noexcept {
        {
            std::lock_guard<std::mutex> lock(periodicMutex);
            periodicStop = true;
        }
        periodicCv.notify_all();
        if (periodicWorker.joinable()) {
            periodicWorker.join();
        }
    }

    void downloadAsync() noexcept {
        start([this] {
            auto deps = dependenciesCopy();
            Config effective;
            ManifestEnvelope envelope;
            UpdateDecision decision;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!lastDecision || !lastEnvelope || !lastConfig) {
                    effective = config;
                } else {
                    effective = *lastConfig;
                    envelope = *lastEnvelope;
                    decision = *lastDecision;
                }
            }

            bool hadDecision = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                hadDecision = lastDecision.has_value();
            }

            if (!hadDecision) {
                auto checked = checkInternal(effective, envelope, deps);
                if (!checked) {
                    reportError(checked.error());
                    return;
                }
                decision = checked.value();
            }

            if (!decision.checkResult.updateAvailable || decision.checkResult.reinstallRequired) {
                setState(decision.checkResult.updateAvailable ? State::UpdateAvailable : State::UpToDate);
                return;
            }

            setState(State::Downloading);
            auto downloaded = executeDownloads(
                effective, decision, *deps.network, *deps.fileSystem, *deps.hashProvider, deps.stateStore.get(),
                [this](const Progress& progress) {
                    auto callback = callbacksCopy().onProgress;
                    post([callback = std::move(callback), progress] {
                        if (callback) {
                            callback(progress);
                        }
                    });
                },
                *tokenCopy());
            if (!downloaded) {
                reportError(downloaded.error());
                return;
            }

            auto written = writeApplyPlan(effective, envelope, decision, *deps.fileSystem);
            if (!written) {
                reportError(written.error());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastConfig = effective;
                lastEnvelope = envelope;
                lastDecision = decision;
                lastApplyPlan = written.value();
            }
            setState(State::ReadyToApply);
            auto readyCallback = callbacksCopy().onReadyToApply;
            post([callback = std::move(readyCallback)] {
                if (callback) {
                    callback();
                }
            });
        });
    }

    void applyAndRestartAsync() noexcept {
        start([this] {
            auto deps = dependenciesCopy();
            std::optional<Config> effective;
            std::optional<WrittenApplyPlan> plan;
            {
                std::lock_guard<std::mutex> lock(mutex);
                effective = lastConfig;
                plan = lastApplyPlan;
            }
            if (!effective || !plan) {
                reportError({ErrorCode::ApplyLaunchFailed, "No ready apply plan"});
                return;
            }
            setState(State::Applying);
            auto launched = launchApplyProcess(*effective, plan->path, *deps.processLauncher);
            if (!launched) {
                reportError(launched.error());
                return;
            }
        });
    }

    Result<void> markCurrentVersionHealthy() noexcept {
        auto validResources = validateResourceLimits(config.resources);
        if (!validResources) {
            return validResources;
        }
        auto deps = dependenciesCopy();
        if (!deps.stateStore) {
            return Result<void>::ok();
        }
        std::string releaseId;
        auto pending = deps.stateStore->loadPendingUpdate();
        if (pending && pending.value()) {
            releaseId = pending.value()->releaseId;
        }
        auto save = deps.stateStore->saveLastAcceptedVersion(config.currentVersion, releaseId);
        if (!save) {
            return save;
        }
        return deps.stateStore->clearPendingUpdate();
    }

    Result<void> rollbackLastUpdate() noexcept {
        auto validResources = validateResourceLimits(config.resources);
        if (!validResources) {
            return validResources;
        }
        auto deps = dependenciesCopy();
        if (!deps.stateStore) {
            return Result<void>::ok();
        }
        if (!deps.fileSystem) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "File system dependency is missing"});
        }
        auto pending = deps.stateStore->loadPendingUpdate();
        if (!pending) {
            return Result<void>::fail(pending.error());
        }
        if (!pending.value()) {
            return Result<void>::ok();
        }
        if (pending.value()->applyPlanPath.empty()) {
            return Result<void>::fail({ErrorCode::ApplyFailed, "Pending update has no apply plan path"});
        }

        auto text = deps.fileSystem->readText(pending.value()->applyPlanPath, config.resources.maxApplyPlanBytes);
        if (!text) {
            return Result<void>::fail(text.error());
        }
        auto plan = ApplyPlan::parse(text.value(), config.resources);
        if (!plan) {
            return Result<void>::fail(plan.error());
        }

        if (!deps.hashProvider) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "Hash provider dependency is missing"});
        }
        auto installRoot = deps.fileSystem->openRoot(plan.value().installDir, RootAccess::ReadWrite, false,
                                                     RootedDirectoryCreationMode::InstalledContent);
        if (!installRoot) {
            return Result<void>::fail(installRoot.error());
        }
        auto backupRoot = deps.fileSystem->openRoot(plan.value().backupDir, RootAccess::ReadOnly, false);
        if (!backupRoot) {
            return Result<void>::fail(backupRoot.error());
        }

        const auto copyOpenedFile = [](IRootedFile& source, IRootedFile& destination) -> Result<void> {
            auto sourceStart = source.seek(0);
            if (!sourceStart) {
                return sourceStart;
            }
            auto cleared = destination.truncate(0);
            if (!cleared) {
                return cleared;
            }
            auto destinationStart = destination.seek(0);
            if (!destinationStart) {
                return destinationStart;
            }
            std::array<unsigned char, 64 * 1024> buffer{};
            for (;;) {
                auto read = source.read(buffer.data(), buffer.size());
                if (!read) {
                    return Result<void>::fail(read.error());
                }
                if (read.value() == 0) {
                    return Result<void>::ok();
                }
                auto written = destination.write(buffer.data(), read.value());
                if (!written) {
                    return written;
                }
            }
        };

        for (auto it = plan.value().operations.rbegin(); it != plan.value().operations.rend(); ++it) {
            auto backup = backupRoot.value()->openRegularFile(it->target, RootedFileOpenMode::ReadOnly);
            if (!backup) {
                return Result<void>::fail(backup.error());
            }
            auto target = installRoot.value()->openRegularFile(it->target, RootedFileOpenMode::ReadOnly);
            if (!target) {
                return Result<void>::fail(target.error());
            }
            RootedEntryExpectation targetExpectation = RootedEntryExpectation::missing();
            if (target.value().exists()) {
                auto metadata = target.value().file->metadata();
                if (!metadata) {
                    return Result<void>::fail(metadata.error());
                }
                targetExpectation = RootedEntryExpectation::matching(metadata.value());
            }
            target.value().file.reset();

            if (backup.value().exists()) {
                auto temporary = installRoot.value()->createAtomicReplacement(
                    it->target, RootedDirectoryCreationMode::InstalledContent);
                if (!temporary) {
                    return Result<void>::fail(temporary.error());
                }
                auto copied = copyOpenedFile(*backup.value().file, temporary.value()->file());
                if (!copied) {
                    return copied;
                }
                auto backupMetadata = backup.value().file->metadata();
                if (!backupMetadata) {
                    return Result<void>::fail(backupMetadata.error());
                }
                auto permissions = temporary.value()->file().setPermissions(
                    sanitizedFilePermissions(backupMetadata.value().permissions));
                if (!permissions) {
                    return permissions;
                }
                auto temporaryMetadata = temporary.value()->file().metadata();
                if (!temporaryMetadata) {
                    return Result<void>::fail(temporaryMetadata.error());
                }
                if (temporaryMetadata.value().size != backupMetadata.value().size) {
                    return Result<void>::fail({ErrorCode::HashMismatch, "Rollback backup size mismatch"});
                }
                auto backupHash = deps.hashProvider->sha256Stream(*backup.value().file);
                if (!backupHash) {
                    return Result<void>::fail(backupHash.error());
                }
                auto temporaryHash = deps.hashProvider->sha256Stream(temporary.value()->file());
                if (!temporaryHash) {
                    return Result<void>::fail(temporaryHash.error());
                }
                if (backupHash.value() != temporaryHash.value()) {
                    return Result<void>::fail({ErrorCode::HashMismatch, "Rollback backup SHA-256 mismatch"});
                }
                auto committed = temporary.value()->commit(targetExpectation);
                if (!committed) {
                    return committed;
                }
            } else if (it->type == ApplyOperationType::Replace &&
                       targetExpectation.kind == RootedEntryExpectationKind::Identity) {
                auto removed = installRoot.value()->removeRegularFile(it->target, targetExpectation);
                if (!removed) {
                    return removed;
                }
            }
        }

        return deps.stateStore->clearPendingUpdate();
    }

    Config config;
    std::shared_ptr<INetworkClient> network;
    std::shared_ptr<IHashProvider> hashProvider;
    std::shared_ptr<IFileSystem> fileSystem;
    std::shared_ptr<ISignatureVerifier> signatureVerifier;
    std::shared_ptr<IEventDispatcher> dispatcher;
    std::shared_ptr<IProcessLauncher> processLauncher;
    std::shared_ptr<IStateStore> stateStore;

    mutable std::mutex mutex;
    Callbacks callbacks;
    std::thread worker;
    std::thread periodicWorker;
    std::atomic<State> stateValue{State::Idle};
    std::mutex workerMutex;
    std::mutex tokenMutex;
    std::shared_ptr<CancellationToken> currentToken;
    std::mutex periodicMutex;
    std::condition_variable periodicCv;
    bool periodicStop = false;

    std::optional<Config> lastConfig;
    std::optional<ManifestEnvelope> lastEnvelope;
    std::optional<UpdateDecision> lastDecision;
    std::optional<WrittenApplyPlan> lastApplyPlan;
};

Updater::Updater(Config config) : impl_(new Impl(std::move(config))) {}

Updater::~Updater() = default;

void Updater::setCallbacks(Callbacks callbacks) {
    impl_->setCallbacks(std::move(callbacks));
}

void Updater::setNetworkClient(std::shared_ptr<INetworkClient> network) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->network = std::move(network);
}

void Updater::setHashProvider(std::shared_ptr<IHashProvider> hashProvider) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->hashProvider = std::move(hashProvider);
}

void Updater::setFileSystem(std::shared_ptr<IFileSystem> fileSystem) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->fileSystem = std::move(fileSystem);
}

void Updater::setSignatureVerifier(std::shared_ptr<ISignatureVerifier> verifier) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->signatureVerifier = std::move(verifier);
}

void Updater::setEventDispatcher(std::shared_ptr<IEventDispatcher> dispatcher) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->dispatcher = std::move(dispatcher);
}

void Updater::setProcessLauncher(std::shared_ptr<IProcessLauncher> launcher) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->processLauncher = std::move(launcher);
}

void Updater::setStateStore(std::shared_ptr<IStateStore> store) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stateStore = std::move(store);
}

void Updater::checkAsync() noexcept {
    impl_->checkAsync();
}

void Updater::checkOnStartupAsync(bool downloadWhenAvailable) noexcept {
    impl_->checkOnStartupAsync(downloadWhenAvailable);
}

void Updater::checkAndDownloadAsync() noexcept {
    impl_->checkAndDownloadAsync();
}

void Updater::downloadAsync() noexcept {
    impl_->downloadAsync();
}

void Updater::applyAndRestartAsync() noexcept {
    impl_->applyAndRestartAsync();
}

void Updater::startPeriodicCheck(std::chrono::milliseconds interval, bool downloadWhenAvailable,
                                 bool runImmediately) noexcept {
    impl_->startPeriodicCheck(interval, downloadWhenAvailable, runImmediately);
}

void Updater::stopPeriodicCheck() noexcept {
    impl_->stopPeriodicCheck();
}

Result<void> Updater::markCurrentVersionHealthy() noexcept {
    return impl_->markCurrentVersionHealthy();
}

Result<void> Updater::rollbackLastUpdate() noexcept {
    return impl_->rollbackLastUpdate();
}

void Updater::cancel() noexcept {
    impl_->cancel();
}

State Updater::state() const noexcept {
    return impl_->state();
}

} // namespace autoupdater
