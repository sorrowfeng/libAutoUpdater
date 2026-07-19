#include "libAutoUpdater/Updater.h"

#include "ApplyLauncher.h"
#include "ApplyPlanWriter.h"
#include "ApplyTransactionReceipt.h"
#include "DownloadExecutor.h"
#include "LocalSnapshotBuilder.h"
#include "ManifestFetcher.h"
#include "UpdatePlanner.h"
#include "util/PathUtil.h"

#include <atomic>
#include <condition_variable>
#include <deque>
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

struct Updater::Impl : std::enable_shared_from_this<Updater::Impl> {
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
        shutdown();
    }

    void setCallbacks(Callbacks value) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::swap(callbacks, value);
        }
        // Destroy the previous callback targets after releasing mutex because
        // target destructors may re-enter the updater.
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

    struct CheckedGeneration {
        std::uint64_t id = 0;
        Config config;
        ManifestEnvelope envelope;
        UpdateDecision decision;
    };

    struct ReadyGeneration {
        CheckedGeneration checked;
        WrittenApplyPlan plan;
        PendingUpdate persistedPending;
        std::shared_ptr<std::atomic_bool> callbackGuard;
    };

    static bool samePendingUpdate(const PendingUpdate& left, const PendingUpdate& right) {
        return left.version.toString() == right.version.toString() && left.releaseId == right.releaseId &&
               left.backupDir.lexically_normal() == right.backupDir.lexically_normal() &&
               left.applyPlanPath.lexically_normal() == right.applyPlanPath.lexically_normal() &&
               left.applyPlanDigest == right.applyPlanDigest;
    }

    std::uint64_t beginGeneration() {
        auto guard = std::make_shared<std::atomic_bool>(true);
        std::lock_guard<std::mutex> lock(mutex);
        if (currentGenerationGuard) {
            currentGenerationGuard->store(false, std::memory_order_release);
        }
        invalidateReadyLocked();
        const auto generation = nextGeneration++;
        currentGeneration = generation;
        currentGenerationGuard = std::move(guard);
        checkedGeneration.reset();
        return generation;
    }

    void invalidateReadyLocked() {
        if (readyGeneration && readyGeneration->callbackGuard) {
            readyGeneration->callbackGuard->store(false, std::memory_order_release);
        }
        readyGeneration.reset();
    }

    void postForGeneration(std::uint64_t generation, std::function<void()> fn) {
        std::shared_ptr<std::atomic_bool> guard;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (currentGeneration && *currentGeneration == generation) {
                guard = currentGenerationGuard;
            }
        }
        if (!guard) {
            return;
        }
        post([guard = std::move(guard), fn = std::move(fn)] {
            if (!guard->load(std::memory_order_acquire)) {
                return;
            }
            if (fn) {
                fn();
            }
        });
    }

    bool setGenerationState(std::uint64_t generation, State next) {
        std::function<void(State)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!currentGeneration || *currentGeneration != generation) {
                return false;
            }
            stateValue.store(next, std::memory_order_relaxed);
            callback = callbacks.onStateChanged;
        }
        postForGeneration(generation, [callback = std::move(callback), next] {
            if (callback) {
                callback(next);
            }
        });
        return !shuttingDown.load(std::memory_order_acquire);
    }

    bool storeChecked(CheckedGeneration checked) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!currentGeneration || *currentGeneration != checked.id) {
            return false;
        }
        checkedGeneration = std::move(checked);
        invalidateReadyLocked();
        return true;
    }

    Dependencies dependenciesCopy() {
        std::lock_guard<std::mutex> lock(mutex);
        return Dependencies{
            network, hashProvider, fileSystem, signatureVerifier, dispatcher, processLauncher, stateStore,
        };
    }

    bool setState(State next) {
        stateValue.store(next, std::memory_order_relaxed);
        auto callback = callbacksCopy().onStateChanged;
        post([callback = std::move(callback), next] {
            if (callback) {
                callback(next);
            }
        });
        return !shuttingDown.load(std::memory_order_acquire);
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

    void joinWorker() noexcept {
        std::thread joining;
        {
            std::lock_guard<std::mutex> lock(workerMutex);
            if (worker.joinable()) {
                joining = std::move(worker);
            }
        }
        if (joining.joinable()) {
            if (joining.get_id() == std::this_thread::get_id()) {
                joining.detach();
            } else {
                joining.join();
            }
        }
    }

    void shutdown() noexcept {
        shuttingDown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(workerMutex);
            pendingTasks.clear();
        }
        cancel();
        stopPeriodicCheck();
        joinWorker();
    }

    Callbacks callbacksCopy() {
        std::lock_guard<std::mutex> lock(mutex);
        return callbacks;
    }

    void post(std::function<void()> fn) noexcept {
        if (!fn || shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        auto target = dependenciesCopy().dispatcher;
        if (target) {
            target->post([fn = std::move(fn)]() noexcept {
                try {
                    fn();
                } catch (...) {
                    // User callbacks cannot escape the dispatcher boundary.
                }
            });
        }
    }

    void notifyError(Error error) {
        auto callback = callbacksCopy().onError;
        post([callback = std::move(callback), error = std::move(error)] {
            if (callback) {
                callback(error);
            }
        });
    }

    void reportError(Error error) {
        setState(State::Failed);
        notifyError(std::move(error));
    }

    void notifyGenerationError(std::uint64_t generation, Error error) {
        auto callback = callbacksCopy().onError;
        postForGeneration(generation, [callback = std::move(callback), error = std::move(error)] {
            if (callback) {
                callback(error);
            }
        });
    }

    void reportGenerationError(std::uint64_t generation, Error error) {
        std::function<void(State)> stateCallback;
        std::function<void(const Error&)> errorCallback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!currentGeneration || *currentGeneration != generation ||
                shuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            checkedGeneration.reset();
            invalidateReadyLocked();
            stateValue.store(State::Failed, std::memory_order_relaxed);
            stateCallback = callbacks.onStateChanged;
            errorCallback = callbacks.onError;
        }
        postForGeneration(generation, [stateCallback = std::move(stateCallback)] {
            if (stateCallback) {
                stateCallback(State::Failed);
            }
        });
        postForGeneration(generation, [errorCallback = std::move(errorCallback), error = std::move(error)] {
            if (errorCallback) {
                errorCallback(error);
            }
        });
    }

    void reportCurrentError(Error error) {
        std::optional<std::uint64_t> generation;
        {
            std::lock_guard<std::mutex> lock(mutex);
            generation = currentGeneration;
        }
        if (generation) {
            reportGenerationError(*generation, std::move(error));
        } else {
            reportError(std::move(error));
        }
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

    Result<UpdateDecision> checkInternal(std::uint64_t generation, Config& effectiveConfig,
                                         ManifestEnvelope& envelopeOut, const Dependencies& deps) {
        auto valid = validateConfig(deps);
        if (!valid) {
            return Result<UpdateDecision>::fail(valid.error());
        }

        if (!setGenerationState(generation, State::Checking)) {
            return Result<UpdateDecision>::fail({ErrorCode::Cancelled, "Updater is shutting down"});
        }
        auto token = tokenCopy();
        if (!token) {
            return Result<UpdateDecision>::fail({ErrorCode::InternalError, "Updater task has no cancellation token"});
        }
        auto envelope = fetchAndVerifyManifest(effectiveConfig, *deps.network, *deps.hashProvider,
                                               *deps.signatureVerifier, *token);
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

    void runPendingTasks() noexcept {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(workerMutex);
                if (shuttingDown.load(std::memory_order_acquire) || pendingTasks.empty()) {
                    workerBusy = false;
                    return;
                }
                task = std::move(pendingTasks.front());
                pendingTasks.pop_front();
            }

            auto token = std::make_shared<CancellationToken>();
            {
                std::lock_guard<std::mutex> lock(tokenMutex);
                currentToken = token;
            }
            try {
                std::unique_lock<std::mutex> operationLock(operationMutex);
                task();
            } catch (...) {
                reportCurrentError({ErrorCode::InternalError, "Updater task failed with an unexpected exception"});
            }
            {
                std::lock_guard<std::mutex> lock(tokenMutex);
                if (currentToken == token) {
                    currentToken.reset();
                }
            }
        }
    }

    void start(std::function<void()> task) noexcept {
        std::thread completedWorker;
        bool ownsWorkerStart = false;
        try {
            bool queueFull = false;
            {
                std::lock_guard<std::mutex> lock(workerMutex);
                if (shuttingDown.load(std::memory_order_acquire)) {
                    return;
                }
                if (pendingTasks.size() >= maxPendingTasks) {
                    queueFull = true;
                } else {
                    pendingTasks.push_back(std::move(task));
                }
                if (queueFull) {
                    // Report outside workerMutex because a direct callback may re-enter the updater.
                } else if (workerBusy) {
                    return;
                } else {
                    workerBusy = true;
                    ownsWorkerStart = true;
                    if (worker.joinable()) {
                        completedWorker = std::move(worker);
                    }
                }
            }
            if (queueFull) {
                bool expected = false;
                if (queueOverflowNotification.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    notifyError({ErrorCode::InternalError, "Updater task queue is full"});
                    queueOverflowNotification.store(false, std::memory_order_release);
                }
                return;
            }

            if (completedWorker.joinable()) {
                if (completedWorker.get_id() == std::this_thread::get_id()) {
                    completedWorker.detach();
                } else {
                    completedWorker.join();
                }
            }

            std::lock_guard<std::mutex> lock(workerMutex);
            if (shuttingDown.load(std::memory_order_acquire)) {
                pendingTasks.clear();
                workerBusy = false;
                return;
            }
            auto self = shared_from_this();
            worker = std::thread([self = std::move(self)] { self->runPendingTasks(); });
        } catch (...) {
            if (ownsWorkerStart) {
                std::lock_guard<std::mutex> lock(workerMutex);
                pendingTasks.clear();
                workerBusy = false;
            }
            notifyError({ErrorCode::InternalError, "Failed to start updater worker"});
        }
    }

    std::optional<CheckedGeneration> checkedForDownload(std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!currentGeneration || *currentGeneration != generation || !checkedGeneration ||
            checkedGeneration->id != generation || readyGeneration ||
            stateValue.load(std::memory_order_relaxed) != State::UpdateAvailable) {
            return std::nullopt;
        }
        return checkedGeneration;
    }

    void publishCheckResult(std::uint64_t generation, const CheckResult& check) {
        auto callback = callbacksCopy().onCheckResult;
        postForGeneration(generation, [callback = std::move(callback), check] {
            if (callback) {
                callback(check);
            }
        });
    }

    bool commitReady(ReadyGeneration ready) {
        std::function<void(State)> stateCallback;
        std::function<void()> readyCallback;
        const auto generation = ready.checked.id;
        ready.callbackGuard = std::make_shared<std::atomic_bool>(true);
        auto readyGuard = ready.callbackGuard;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!currentGeneration || *currentGeneration != generation || !checkedGeneration ||
                checkedGeneration->id != generation) {
                return false;
            }
            invalidateReadyLocked();
            readyGeneration = std::move(ready);
            stateValue.store(State::ReadyToApply, std::memory_order_relaxed);
            stateCallback = callbacks.onStateChanged;
            readyCallback = callbacks.onReadyToApply;
        }
        postForGeneration(generation, [stateCallback = std::move(stateCallback)] {
            if (stateCallback) {
                stateCallback(State::ReadyToApply);
            }
        });
        postForGeneration(generation, [readyGuard = std::move(readyGuard), readyCallback = std::move(readyCallback)] {
            if (!readyGuard->load(std::memory_order_acquire)) {
                return;
            }
            if (readyCallback) {
                readyCallback();
            }
        });
        return !shuttingDown.load(std::memory_order_acquire);
    }

    Result<void> downloadAndPrepare(const CheckedGeneration& checked, const Dependencies& deps) {
        const auto generation = checked.id;
        auto valid = validateConfig(deps);
        if (!valid) {
            return valid;
        }
        if (!deps.stateStore) {
            return Result<void>::fail(
                {ErrorCode::InvalidConfig, "A state store is required before an update can become ready"});
        }
        if (!setGenerationState(generation, State::Downloading)) {
            return Result<void>::fail({ErrorCode::Cancelled, "Updater is shutting down"});
        }
        auto token = tokenCopy();
        if (!token) {
            return Result<void>::fail({ErrorCode::InternalError, "Updater task has no cancellation token"});
        }
        auto downloaded = executeDownloads(
            checked.config, checked.decision, *deps.network, *deps.fileSystem, *deps.hashProvider,
            deps.stateStore.get(),
            [this, generation](const Progress& progress) {
                auto callback = callbacksCopy().onProgress;
                postForGeneration(generation, [callback = std::move(callback), progress] {
                    if (callback) {
                        callback(progress);
                    }
                });
            },
            *token);
        if (!downloaded) {
            return downloaded;
        }

        auto written = writeApplyPlan(checked.config, checked.envelope, checked.decision, *deps.fileSystem);
        if (!written) {
            return Result<void>::fail(written.error());
        }
        PendingUpdate pending;
        pending.version = checked.envelope.manifest.version;
        pending.releaseId = checked.envelope.manifest.releaseId;
        pending.backupDir = written.value().plan.backupDir;
        pending.applyPlanPath = written.value().path;
        pending.applyPlanDigest = written.value().digest;
        auto saved = deps.stateStore->savePendingUpdate(pending);
        if (!saved) {
            return saved;
        }

        ReadyGeneration ready;
        ready.checked = checked;
        ready.plan = std::move(written.value());
        ready.persistedPending = std::move(pending);
        if (!commitReady(std::move(ready))) {
            return Result<void>::fail({ErrorCode::Cancelled, "Update generation was invalidated before readiness"});
        }
        return Result<void>::ok();
    }

    void checkAsync(std::shared_ptr<std::atomic_bool> periodicToken = {}) noexcept {
        start([this, periodicToken = std::move(periodicToken)] {
            const bool periodicRequest = static_cast<bool>(periodicToken);
            if (periodicToken && periodicToken->load(std::memory_order_acquire)) {
                return;
            }
            const auto current = state();
            if (current == State::Applying || (periodicRequest && current == State::ReadyToApply)) {
                if (!periodicRequest) {
                    notifyError({ErrorCode::ApplyLaunchFailed, "A new check cannot start while apply is in progress"});
                }
                return;
            }
            const auto generation = beginGeneration();
            auto deps = dependenciesCopy();
            Config effective = config;
            ManifestEnvelope envelope;
            auto decision = checkInternal(generation, effective, envelope, deps);
            if (!decision) {
                reportGenerationError(generation, decision.error());
                return;
            }

            CheckedGeneration checked;
            checked.id = generation;
            checked.config = std::move(effective);
            checked.envelope = std::move(envelope);
            checked.decision = std::move(decision.value());
            const auto check = checked.decision.checkResult;
            if (!storeChecked(std::move(checked))) {
                return;
            }
            setGenerationState(generation, check.updateAvailable ? State::UpdateAvailable : State::UpToDate);
            publishCheckResult(generation, check);
        });
    }

    void checkAndDownloadAsync(std::shared_ptr<std::atomic_bool> periodicToken = {}) noexcept {
        start([this, periodicToken = std::move(periodicToken)] {
            const bool periodicRequest = static_cast<bool>(periodicToken);
            if (periodicToken && periodicToken->load(std::memory_order_acquire)) {
                return;
            }
            const auto current = state();
            if (current == State::Applying || (periodicRequest && current == State::ReadyToApply)) {
                if (!periodicRequest) {
                    notifyError({ErrorCode::ApplyLaunchFailed, "A new check cannot start while apply is in progress"});
                }
                return;
            }
            const auto generation = beginGeneration();
            auto deps = dependenciesCopy();
            Config effective = config;
            ManifestEnvelope envelope;
            auto decision = checkInternal(generation, effective, envelope, deps);
            if (!decision) {
                reportGenerationError(generation, decision.error());
                return;
            }

            CheckedGeneration checked;
            checked.id = generation;
            checked.config = std::move(effective);
            checked.envelope = std::move(envelope);
            checked.decision = std::move(decision.value());
            const auto check = checked.decision.checkResult;
            if (!storeChecked(checked)) {
                return;
            }

            if (!check.updateAvailable || check.reinstallRequired) {
                setGenerationState(generation, check.updateAvailable ? State::UpdateAvailable : State::UpToDate);
                publishCheckResult(generation, check);
                return;
            }

            publishCheckResult(generation, check);
            auto prepared = downloadAndPrepare(checked, deps);
            if (!prepared) {
                reportGenerationError(generation, prepared.error());
                return;
            }
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
            notifyError({ErrorCode::InvalidConfig, "Periodic check interval must be positive"});
            return;
        }

        bool startFailed = false;
        {
            std::lock_guard<std::mutex> controlLock(periodicControlMutex);
            stopPeriodicCheckLocked();
            if (shuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            auto stopToken = std::make_shared<std::atomic_bool>(false);
            {
                std::lock_guard<std::mutex> lock(periodicMutex);
                periodicStopToken = stopToken;
            }

            try {
                auto self = shared_from_this();
                periodicWorker = std::thread([self = std::move(self), stopToken, interval, downloadWhenAvailable,
                                              runImmediately] {
                    auto trigger = [self, stopToken, downloadWhenAvailable] {
                        const auto current = self->state();
                        if (stopToken->load(std::memory_order_acquire) ||
                            self->shuttingDown.load(std::memory_order_acquire) || self->hasPendingWork() ||
                            current == State::Checking || current == State::Downloading ||
                            current == State::ReadyToApply || current == State::Applying) {
                            return;
                        }
                        if (stopToken->load(std::memory_order_acquire)) {
                            return;
                        }
                        if (downloadWhenAvailable) {
                            self->checkAndDownloadAsync(stopToken);
                        } else {
                            self->checkAsync(stopToken);
                        }
                    };

                    if (runImmediately) {
                        trigger();
                    }

                    std::unique_lock<std::mutex> lock(self->periodicMutex);
                    while (!stopToken->load(std::memory_order_acquire)) {
                        if (self->periodicCv.wait_for(lock, interval, [stopToken] {
                                return stopToken->load(std::memory_order_acquire);
                            })) {
                            break;
                        }
                        lock.unlock();
                        trigger();
                        lock.lock();
                    }
                });
            } catch (...) {
                stopToken->store(true, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(periodicMutex);
                    if (periodicStopToken == stopToken) {
                        periodicStopToken.reset();
                    }
                }
                startFailed = true;
            }
        }
        if (startFailed) {
            notifyError({ErrorCode::InternalError, "Failed to start periodic updater thread"});
        }
    }

    bool hasPendingWork() {
        std::lock_guard<std::mutex> lock(workerMutex);
        return workerBusy || !pendingTasks.empty();
    }

    void stopPeriodicCheckLocked() noexcept {
        std::thread stopping;
        {
            std::lock_guard<std::mutex> lock(periodicMutex);
            if (periodicStopToken) {
                periodicStopToken->store(true, std::memory_order_release);
                periodicStopToken.reset();
            }
            if (periodicWorker.joinable()) {
                stopping = std::move(periodicWorker);
            }
        }
        periodicCv.notify_all();
        if (stopping.joinable()) {
            // Each run owns an independent stop token and keeps Impl alive.
            // Detaching avoids a control-lock/join cycle when an error callback
            // restarts periodic checks from the periodic thread itself.
            stopping.detach();
        }
    }

    void stopPeriodicCheck() noexcept {
        std::lock_guard<std::mutex> controlLock(periodicControlMutex);
        stopPeriodicCheckLocked();
    }

    std::optional<CheckedGeneration> currentCheckedGeneration(std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!currentGeneration || *currentGeneration != generation || !checkedGeneration ||
            checkedGeneration->id != generation) {
            return std::nullopt;
        }
        return checkedGeneration;
    }

    void downloadAsync() noexcept {
        std::optional<std::uint64_t> requestedGeneration;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto current = stateValue.load(std::memory_order_relaxed);
            if (currentGeneration && current != State::Idle && current != State::Failed) {
                requestedGeneration = currentGeneration;
            }
        }
        start([this, requestedGeneration] {
            auto deps = dependenciesCopy();
            std::optional<CheckedGeneration> checked;
            bool implicitCheck = !requestedGeneration.has_value();
            if (requestedGeneration) {
                checked = currentCheckedGeneration(*requestedGeneration);
                if (!checked) {
                    notifyGenerationError(
                        *requestedGeneration,
                        {ErrorCode::DownloadFailed, "The requested checked generation is no longer current"});
                    return;
                }
            } else {
                const auto current = state();
                if (current == State::Checking || current == State::Downloading ||
                    current == State::ReadyToApply || current == State::Applying) {
                    notifyError({ErrorCode::DownloadFailed, "No checked update generation is available to download"});
                    return;
                }

                const auto generation = beginGeneration();
                Config effective = config;
                ManifestEnvelope envelope;
                auto decision = checkInternal(generation, effective, envelope, deps);
                if (!decision) {
                    reportGenerationError(generation, decision.error());
                    return;
                }

                CheckedGeneration fresh;
                fresh.id = generation;
                fresh.config = std::move(effective);
                fresh.envelope = std::move(envelope);
                fresh.decision = std::move(decision.value());
                const auto check = fresh.decision.checkResult;
                if (!storeChecked(fresh)) {
                    return;
                }
                publishCheckResult(generation, check);
                checked = std::move(fresh);
                if (!check.updateAvailable || check.reinstallRequired) {
                    setGenerationState(generation, check.updateAvailable ? State::UpdateAvailable : State::UpToDate);
                    return;
                }
            }

            const auto generation = checked->id;
            const auto& check = checked->decision.checkResult;
            if (!check.updateAvailable || check.reinstallRequired) {
                return;
            }
            if (!implicitCheck && !checkedForDownload(generation)) {
                notifyGenerationError(
                    generation, {ErrorCode::DownloadFailed, "The checked update generation is no longer downloadable"});
                return;
            }
            auto prepared = downloadAndPrepare(*checked, deps);
            if (!prepared) {
                reportGenerationError(generation, prepared.error());
            }
        });
    }

    void applyAndRestartAsync() noexcept {
        std::optional<std::uint64_t> requestedGeneration;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (currentGeneration && checkedGeneration && readyGeneration &&
                checkedGeneration->id == *currentGeneration && readyGeneration->checked.id == *currentGeneration &&
                stateValue.load(std::memory_order_relaxed) == State::ReadyToApply) {
                requestedGeneration = currentGeneration;
            }
        }
        start([this, requestedGeneration] {
            if (!requestedGeneration) {
                notifyError({ErrorCode::ApplyLaunchFailed,
                             "Apply was requested without a current ready-to-apply generation"});
                return;
            }
            auto deps = dependenciesCopy();
            std::optional<ReadyGeneration> ready;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (currentGeneration && checkedGeneration && readyGeneration &&
                    *currentGeneration == *requestedGeneration && checkedGeneration->id == *requestedGeneration &&
                    readyGeneration->checked.id == *requestedGeneration &&
                    stateValue.load(std::memory_order_relaxed) == State::ReadyToApply) {
                    ready = readyGeneration;
                }
            }
            if (!ready) {
                notifyError({ErrorCode::ApplyLaunchFailed, "No current ready-to-apply generation is available"});
                return;
            }
            const auto generation = ready->checked.id;
            if (!deps.processLauncher) {
                reportGenerationError(generation, {ErrorCode::InvalidConfig, "Process launcher dependency is missing"});
                return;
            }
            if (!deps.stateStore) {
                reportGenerationError(generation,
                                      {ErrorCode::StateStoreError, "Pending update state store is unavailable"});
                return;
            }
            auto pending = deps.stateStore->loadPendingUpdate();
            if (!pending) {
                reportGenerationError(generation, pending.error());
                return;
            }
            if (!pending.value() || !samePendingUpdate(*pending.value(), ready->persistedPending)) {
                reportGenerationError(
                    generation,
                    {ErrorCode::StateStoreError, "Persisted pending update does not match the ready generation"});
                return;
            }

            std::function<void(State)> stateCallback;
            bool generationChanged = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!currentGeneration || *currentGeneration != generation || !readyGeneration ||
                    readyGeneration->checked.id != generation ||
                    stateValue.load(std::memory_order_relaxed) != State::ReadyToApply) {
                    generationChanged = true;
                } else {
                    if (readyGeneration->callbackGuard) {
                        readyGeneration->callbackGuard->store(false, std::memory_order_release);
                    }
                    readyGeneration.reset();
                    stateValue.store(State::Applying, std::memory_order_relaxed);
                    stateCallback = callbacks.onStateChanged;
                }
            }
            if (generationChanged) {
                notifyGenerationError(generation,
                                      {ErrorCode::ApplyLaunchFailed, "Ready generation changed before apply launch"});
                return;
            }
            postForGeneration(generation, [stateCallback = std::move(stateCallback)] {
                if (stateCallback) {
                    stateCallback(State::Applying);
                }
            });
            if (shuttingDown.load(std::memory_order_acquire)) {
                return;
            }

            auto launched = launchApplyProcess(ready->checked.config, ready->plan.path, ready->plan.digest,
                                               ApplyLaunchIntent::Install, *deps.processLauncher);
            if (!launched) {
                reportGenerationError(generation, launched.error());
            }
        });
    }

    Result<void> markCurrentVersionHealthy() noexcept {
        std::unique_lock<std::mutex> workerLock(workerMutex);
        if (shuttingDown.load(std::memory_order_acquire) || workerBusy || !pendingTasks.empty()) {
            return Result<void>::fail({ErrorCode::InternalError, "Another updater operation is in progress"});
        }
        std::unique_lock<std::mutex> operationLock(operationMutex, std::try_to_lock);
        workerLock.unlock();
        if (!operationLock.owns_lock()) {
            return Result<void>::fail({ErrorCode::InternalError, "Another updater operation is in progress"});
        }
        auto validResources = validateResourceLimits(config.resources);
        if (!validResources) {
            return validResources;
        }
        auto deps = dependenciesCopy();
        if (!deps.stateStore) {
            return Result<void>::ok();
        }
        auto pending = deps.stateStore->loadPendingUpdate();
        if (!pending) {
            return Result<void>::fail(pending.error());
        }

        std::string releaseId;
        if (pending.value()) {
            if (pending.value()->version.toString() != config.currentVersion.toString()) {
                return Result<void>::fail(
                    {ErrorCode::StateStoreError, "Pending update version does not match the running version"});
            }
            if (!deps.fileSystem) {
                return Result<void>::fail({ErrorCode::InvalidConfig, "File system dependency is missing"});
            }
            auto terminal = loadTerminalApplyTransaction(*deps.fileSystem, config.installDir);
            if (!terminal) {
                return Result<void>::fail(terminal.error());
            }
            if (!terminal.value() || terminal.value()->planDigest != pending.value()->applyPlanDigest) {
                return Result<void>::fail(
                    {ErrorCode::ApplyFailed, "Pending update is not authorized by the terminal apply receipt"});
            }
            releaseId = pending.value()->releaseId;
        }
        auto save = deps.stateStore->saveLastAcceptedVersion(config.currentVersion, releaseId);
        if (!save) {
            return save;
        }
        return deps.stateStore->clearPendingUpdate();
    }

    Result<void> rollbackLastUpdate() noexcept {
        std::unique_lock<std::mutex> workerLock(workerMutex);
        if (shuttingDown.load(std::memory_order_acquire) || workerBusy || !pendingTasks.empty()) {
            return Result<void>::fail({ErrorCode::InternalError, "Another updater operation is in progress"});
        }
        std::unique_lock<std::mutex> operationLock(operationMutex, std::try_to_lock);
        workerLock.unlock();
        if (!operationLock.owns_lock()) {
            return Result<void>::fail({ErrorCode::InternalError, "Another updater operation is in progress"});
        }
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
        if (!deps.processLauncher) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "Process launcher dependency is missing"});
        }
        auto pending = deps.stateStore->loadPendingUpdate();
        if (!pending) {
            return Result<void>::fail(pending.error());
        }
        if (!pending.value()) {
            return Result<void>::ok();
        }
        auto written = writeRollbackRequestPlan(config, *pending.value(), *deps.fileSystem);
        if (!written) {
            return Result<void>::fail(written.error());
        }
        return launchApplyProcess(config, written.value().path, written.value().digest, ApplyLaunchIntent::Rollback,
                                  *deps.processLauncher);
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
    std::atomic_bool shuttingDown{false};
    std::mutex operationMutex;
    std::mutex workerMutex;
    std::deque<std::function<void()>> pendingTasks;
    static constexpr std::size_t maxPendingTasks = 64;
    bool workerBusy = false;
    std::atomic_bool queueOverflowNotification{false};
    std::mutex tokenMutex;
    std::shared_ptr<CancellationToken> currentToken;
    std::mutex periodicControlMutex;
    std::mutex periodicMutex;
    std::condition_variable periodicCv;
    std::shared_ptr<std::atomic_bool> periodicStopToken;

    std::uint64_t nextGeneration = 1;
    std::optional<std::uint64_t> currentGeneration;
    std::shared_ptr<std::atomic_bool> currentGenerationGuard;
    std::optional<CheckedGeneration> checkedGeneration;
    std::optional<ReadyGeneration> readyGeneration;
};

Updater::Updater(Config config) : impl_(std::make_shared<Impl>(std::move(config))) {}

Updater::~Updater() {
    if (impl_) {
        impl_->shutdown();
    }
}

void Updater::setCallbacks(Callbacks callbacks) {
    auto impl = impl_;
    impl->setCallbacks(std::move(callbacks));
}

void Updater::setNetworkClient(std::shared_ptr<INetworkClient> network) {
    auto impl = impl_;
    std::shared_ptr<INetworkClient> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->network, std::move(network));
    }
    previous.reset();
}

void Updater::setHashProvider(std::shared_ptr<IHashProvider> hashProvider) {
    auto impl = impl_;
    std::shared_ptr<IHashProvider> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->hashProvider, std::move(hashProvider));
    }
    previous.reset();
}

void Updater::setFileSystem(std::shared_ptr<IFileSystem> fileSystem) {
    auto impl = impl_;
    std::shared_ptr<IFileSystem> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->fileSystem, std::move(fileSystem));
    }
    previous.reset();
}

void Updater::setSignatureVerifier(std::shared_ptr<ISignatureVerifier> verifier) {
    auto impl = impl_;
    std::shared_ptr<ISignatureVerifier> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->signatureVerifier, std::move(verifier));
    }
    previous.reset();
}

void Updater::setEventDispatcher(std::shared_ptr<IEventDispatcher> dispatcher) {
    auto impl = impl_;
    std::shared_ptr<IEventDispatcher> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->dispatcher, std::move(dispatcher));
    }
    previous.reset();
}

void Updater::setProcessLauncher(std::shared_ptr<IProcessLauncher> launcher) {
    auto impl = impl_;
    std::shared_ptr<IProcessLauncher> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->processLauncher, std::move(launcher));
    }
    previous.reset();
}

void Updater::setStateStore(std::shared_ptr<IStateStore> store) {
    auto impl = impl_;
    std::shared_ptr<IStateStore> previous;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        previous = std::exchange(impl->stateStore, std::move(store));
    }
    previous.reset();
}

void Updater::checkAsync() noexcept {
    auto impl = impl_;
    impl->checkAsync();
}

void Updater::checkOnStartupAsync(bool downloadWhenAvailable) noexcept {
    auto impl = impl_;
    impl->checkOnStartupAsync(downloadWhenAvailable);
}

void Updater::checkAndDownloadAsync() noexcept {
    auto impl = impl_;
    impl->checkAndDownloadAsync();
}

void Updater::downloadAsync() noexcept {
    auto impl = impl_;
    impl->downloadAsync();
}

void Updater::applyAndRestartAsync() noexcept {
    auto impl = impl_;
    impl->applyAndRestartAsync();
}

void Updater::startPeriodicCheck(std::chrono::milliseconds interval, bool downloadWhenAvailable,
                                 bool runImmediately) noexcept {
    auto impl = impl_;
    impl->startPeriodicCheck(interval, downloadWhenAvailable, runImmediately);
}

void Updater::stopPeriodicCheck() noexcept {
    auto impl = impl_;
    impl->stopPeriodicCheck();
}

Result<void> Updater::markCurrentVersionHealthy() noexcept {
    auto impl = impl_;
    return impl->markCurrentVersionHealthy();
}

Result<void> Updater::rollbackLastUpdate() noexcept {
    auto impl = impl_;
    return impl->rollbackLastUpdate();
}

void Updater::cancel() noexcept {
    auto impl = impl_;
    impl->cancel();
}

State Updater::state() const noexcept {
    auto impl = impl_;
    return impl->state();
}

} // namespace autoupdater
