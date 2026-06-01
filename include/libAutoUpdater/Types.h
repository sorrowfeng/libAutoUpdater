#pragma once

#include "libAutoUpdater/Error.h"
#include "libAutoUpdater/Version.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace autoupdater {

enum class State {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    ReadyToApply,
    Applying,
    Failed
};

enum class InstallLayout {
    PortableDirectory,
    WindowsDirectory,
    MacOSAppBundle,
    LinuxAppImage,
    PackageManagerOwned
};

struct CancellationToken {
    std::atomic_bool cancelled{false};

    void cancel() noexcept { cancelled.store(true, std::memory_order_relaxed); }
    bool isCancelled() const noexcept { return cancelled.load(std::memory_order_relaxed); }
};

struct CheckResult {
    bool updateAvailable = false;
    bool reinstallRequired = false;
    bool mandatory = false;
    bool downgradeRejected = false;
    Version currentVersion;
    std::optional<Version> remoteVersion;
    std::string releaseId;
    std::string notes;
};

struct Progress {
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::string currentFile;
};

using ProgressCallback = std::function<void(const Progress&)>;

struct Callbacks {
    std::function<void(const CheckResult&)> onCheckResult;
    std::function<void(const Progress&)> onProgress;
    std::function<void()> onReadyToApply;
    std::function<void(const Error&)> onError;
    std::function<void(State)> onStateChanged;
};

} // namespace autoupdater

