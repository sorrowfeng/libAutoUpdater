#include "libAutoUpdater/Updater.h"

#include "ApplyLauncher.h"
#include "ApplyPlanWriter.h"
#include "DownloadExecutor.h"
#include "LocalSnapshotBuilder.h"
#include "ManifestFetcher.h"
#include "UpdatePlanner.h"
#include "util/PathUtil.h"

#include <atomic>
#include <condition_variable>
#include <utility>

namespace autoupdater {

namespace {

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
            stateStore = createJsonStateStore(util::defaultStagingRoot(config.installDir) / "state.json");
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

    // noexcept wrapper used from contexts (e.g. worker thread top level)
    // where throwing would terminate the process.
    void reportErrorCatching(Error error) noexcept {
        try {
            reportError(std::move(error));
        } catch (...) {
        }
    }

    // Reports an operation failure. A user-initiated cancellation is not an
    // error: it returns the updater to Idle without firing onError.
    void reportFailure(const Error& error) {
        if (error.code == ErrorCode::Cancelled) {
            // start() joins the previous worker before launching a new task,
            // so no newer operation can be running here; a plain check is
            // enough to avoid clobbering a state set after cancellation.
            const auto current = state();
            if (current == State::Checking || current == State::Downloading) {
                setState(State::Idle);
            }
            return;
        }
        reportError(error);
    }

    static State stateForCheck(const CheckResult& check) {
        if (check.updateAvailable) {
            return State::UpdateAvailable;
        }
        if (check.reinstallRequired) {
            // The current install is too old to update; only a manual
            // reinstall helps. Reporting UpToDate here would be misleading.
            return State::Failed;
        }
        return State::UpToDate;
    }

    void savePendingUpdate(const Dependencies& deps, const ManifestEnvelope& envelope,
                           const WrittenApplyPlan& written) {
        if (!deps.stateStore) {
            return;
        }
        PendingUpdate pending;
        pending.version = envelope.manifest.version;
        pending.releaseId = envelope.manifest.releaseId;
        pending.backupDir = written.plan.backupDir;
        pending.applyPlanPath = written.path;
        (void)deps.stateStore->savePendingUpdate(pending);
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
                                      util::sanitizePathComponent(envelope.value().manifest.version.toString());
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
            // Signal the previous operation to stop before joining it so a
            // long-running download does not block this call longer than the
            // time the operation needs to notice the cancellation.
            {
                std::lock_guard<std::mutex> lock(tokenMutex);
                if (currentToken) {
                    currentToken->cancel();
                }
            }
            joinWorkerUnlocked();
            auto token = std::make_shared<CancellationToken>();
            {
                std::lock_guard<std::mutex> lock(tokenMutex);
                currentToken = token;
            }
            worker = std::thread([this, task = std::move(task), token] {
                (void)token;
                try {
                    task();
                } catch (...) {
                    // A task must never let an exception escape the thread;
                    // that would terminate the host process. The library
                    // promises Result-based errors instead.
                    try {
                        reportErrorCatching({ErrorCode::InternalError, "Updater worker task threw unexpectedly"});
                    } catch (...) {
                    }
                }
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
            auto callback = callbacksCopy().onCheckResult;
            post([callback = std::move(callback), check] {
                if (callback) {
                    callback(check);
                }
            });
            if (check.reinstallRequired) {
                reportError({ErrorCode::ReinstallRequired,
                             "Current version is below the manifest minVersion; a manual reinstall is required"});
                return;
            }
            setState(stateForCheck(check));
        });
    }

    void checkAndDownloadAsync(bool notifyBeforeDownload) noexcept {
        start([this, notifyBeforeDownload] {
            auto deps = dependenciesCopy();
            Config effective = config;
            ManifestEnvelope envelope;
            auto decision = checkInternal(effective, envelope, deps);
            if (!decision) {
                reportError(decision.error());
                return;
            }

            const auto check = decision.value().checkResult;
            if (notifyBeforeDownload || !check.updateAvailable || check.reinstallRequired) {
                auto checkCallback = callbacksCopy().onCheckResult;
                post([callback = std::move(checkCallback), check] {
                    if (callback) {
                        callback(check);
                    }
                });
            }

            if (!check.updateAvailable || check.reinstallRequired) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    lastConfig = effective;
                    lastEnvelope = envelope;
                    lastDecision = decision.value();
                }
                if (check.reinstallRequired) {
                    reportError({ErrorCode::ReinstallRequired,
                                 "Current version is below the manifest minVersion; a manual reinstall is required"});
                    return;
                }
                setState(stateForCheck(check));
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
                reportFailure(downloaded.error());
                return;
            }

            auto written = writeApplyPlan(effective, envelope, decision.value(), *deps.fileSystem);
            if (!written) {
                reportError(written.error());
                return;
            }

            savePendingUpdate(deps, envelope, written.value());

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
            checkAndDownloadAsync(true);
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

        // Serializes concurrent start/stop calls so periodicWorker is never
        // assigned while still joinable (which would terminate the process).
        std::lock_guard<std::mutex> lifecycleLock(periodicLifecycleMutex);
        stopPeriodicCheckUnlocked();
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
                        checkAndDownloadAsync(true);
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
        std::lock_guard<std::mutex> lifecycleLock(periodicLifecycleMutex);
        stopPeriodicCheckUnlocked();
    }

    void stopPeriodicCheckUnlocked() noexcept {
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
                if (decision.checkResult.reinstallRequired) {
                    reportError({ErrorCode::ReinstallRequired,
                                 "Current version is below the manifest minVersion; a manual reinstall is required"});
                    return;
                }
                setState(stateForCheck(decision.checkResult));
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
                reportFailure(downloaded.error());
                return;
            }

            auto written = writeApplyPlan(effective, envelope, decision, *deps.fileSystem);
            if (!written) {
                reportError(written.error());
                return;
            }
            savePendingUpdate(deps, envelope, written.value());
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
        auto deps = dependenciesCopy();
        if (!deps.stateStore) {
            return Result<void>::ok();
        }
        std::string releaseId;
        std::filesystem::path backupDir;
        auto pending = deps.stateStore->loadPendingUpdate();
        if (pending && pending.value()) {
            releaseId = pending.value()->releaseId;
            backupDir = pending.value()->backupDir;
        }
        auto save = deps.stateStore->saveLastAcceptedVersion(config.currentVersion, releaseId);
        if (!save) {
            return save;
        }
        // The update proved healthy, so its rollback backup is no longer
        // needed; otherwise backups would accumulate without bound.
        if (!backupDir.empty() && deps.fileSystem) {
            (void)deps.fileSystem->removeAll(backupDir);
        }
        return deps.stateStore->clearPendingUpdate();
    }

    Result<void> rollbackLastUpdate() noexcept {
        auto deps = dependenciesCopy();
        if (!deps.stateStore) {
            return Result<void>::ok();
        }
        if (!deps.fileSystem || !deps.hashProvider) {
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

        auto text = deps.fileSystem->readText(pending.value()->applyPlanPath);
        if (!text) {
            return Result<void>::fail(text.error());
        }
        auto plan = ApplyPlan::parse(text.value());
        if (!plan) {
            return Result<void>::fail(plan.error());
        }

        // Only the operation list is taken from the plan; the install root is
        // always our own configured directory, never plan-controlled input.
        const auto backupRoot = pending.value()->backupDir.empty() ? plan.value().backupDir
                                                                   : pending.value()->backupDir;
        for (auto it = plan.value().operations.rbegin(); it != plan.value().operations.rend(); ++it) {
            auto target = util::safeJoin(config.installDir, it->target);
            if (!target) {
                return Result<void>::fail(target.error());
            }
            auto backup = util::safeJoin(backupRoot, it->target);
            if (!backup) {
                return Result<void>::fail(backup.error());
            }

            if (deps.fileSystem->exists(backup.value())) {
                auto copied = deps.fileSystem->copyFile(backup.value(), target.value(), true);
                if (!copied) {
                    return copied;
                }
                // The plan's sha256 describes the *new* file, not the backup,
                // so verify restore integrity by comparing backup and target.
                auto backupHash = deps.hashProvider->sha256File(backup.value());
                auto targetHash = deps.hashProvider->sha256File(target.value());
                if (!backupHash) {
                    return Result<void>::fail(backupHash.error());
                }
                if (!targetHash) {
                    return Result<void>::fail(targetHash.error());
                }
                if (backupHash.value() != targetHash.value()) {
                    return Result<void>::fail(
                        {ErrorCode::HashMismatch, "Rolled-back file does not match its backup"});
                }
            } else if (it->type == ApplyOperationType::Replace) {
                auto removed = deps.fileSystem->remove(target.value());
                if (!removed) {
                    return removed;
                }
            }
        }

        // Rollback succeeded: drop the consumed backup and pending record.
        if (!backupRoot.empty()) {
            (void)deps.fileSystem->removeAll(backupRoot);
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
    std::mutex periodicLifecycleMutex;
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

void Updater::checkAndDownloadAsync(bool notifyBeforeDownload) noexcept {
    impl_->checkAndDownloadAsync(notifyBeforeDownload);
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
