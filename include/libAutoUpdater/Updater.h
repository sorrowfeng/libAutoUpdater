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

    void checkAsync() noexcept;
    void checkOnStartupAsync(bool downloadWhenAvailable = false) noexcept;
    void checkAndDownloadAsync() noexcept;
    void downloadAsync() noexcept;
    void applyAndRestartAsync() noexcept;
    void startPeriodicCheck(std::chrono::milliseconds interval,
                            bool downloadWhenAvailable = false,
                            bool runImmediately = false) noexcept;
    void stopPeriodicCheck() noexcept;
    Result<void> markCurrentVersionHealthy() noexcept;
    Result<void> rollbackLastUpdate() noexcept;

    void cancel() noexcept;
    State state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace autoupdater
