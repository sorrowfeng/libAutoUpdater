#pragma once

#include "libAutoUpdater/Config.h"
#include "libAutoUpdater/Manifest.h"
#include "libAutoUpdater/Types.h"
#include "libAutoUpdater/interfaces/IEventDispatcher.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"
#include "libAutoUpdater/interfaces/ISignatureVerifier.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

#include <memory>
#include <mutex>
#include <thread>

namespace autoupdater {

/// Facade for checking, downloading, staging, and applying application updates.
///
/// Operations run asynchronously. User callbacks are delivered through the
/// configured IEventDispatcher; GUI applications should inject a dispatcher that
/// posts back to the UI thread.
class Updater {
  public:
    explicit Updater(Config config);
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    void setCallbacks(Callbacks callbacks);
    void setNetworkClient(std::shared_ptr<INetworkClient> network);
    void setHashProvider(std::shared_ptr<IHashProvider> hashProvider);
    void setFileSystem(std::shared_ptr<IFileSystem> fileSystem);
    void setSignatureVerifier(std::shared_ptr<ISignatureVerifier> verifier);
    void setEventDispatcher(std::shared_ptr<IEventDispatcher> dispatcher);
    void setProcessLauncher(std::shared_ptr<IProcessLauncher> launcher);
    void setStateStore(std::shared_ptr<IStateStore> store);

    /// Check the configured manifest URL without downloading files.
    void checkAsync() noexcept;
    /// Run a startup check and optionally download when an update is available.
    void checkOnStartupAsync(bool downloadWhenAvailable = false) noexcept;
    /// Check and immediately download changed or missing files.
    ///
    /// When notifyBeforeDownload is true, onCheckResult is emitted after the
    /// internal check and before downloads start. Set it to false when the
    /// caller already presented a previous check result and is now continuing
    /// with download/apply; terminal results such as up-to-date or reinstall
    /// required are still reported.
    void checkAndDownloadAsync(bool notifyBeforeDownload = true) noexcept;
    /// Download the update selected by the latest successful check.
    void downloadAsync() noexcept;
    /// Launch the external updater with the prepared apply plan.
    void applyAndRestartAsync() noexcept;
    /// Start periodic background checks until stopPeriodicCheck() is called.
    void startPeriodicCheck(std::chrono::milliseconds interval, bool downloadWhenAvailable = false,
                            bool runImmediately = false) noexcept;
    void stopPeriodicCheck() noexcept;
    /// Mark the currently running version as healthy after a successful restart.
    Result<void> markCurrentVersionHealthy() noexcept;
    /// Roll back the last pending update when backup data is still available.
    Result<void> rollbackLastUpdate() noexcept;

    /// Request cooperative cancellation of the active check or download.
    void cancel() noexcept;
    State state() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace autoupdater
