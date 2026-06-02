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

/// Public lifecycle state exposed by Updater.
enum class State { Idle, Checking, UpToDate, UpdateAvailable, Downloading, ReadyToApply, Applying, Failed };

/// Installation layout used to decide whether self-replacement is supported.
enum class InstallLayout { PortableDirectory, WindowsDirectory, MacOSAppBundle, LinuxAppImage, PackageManagerOwned };

/// Cooperative cancellation token passed into adapters.
struct CancellationToken {
    std::atomic_bool cancelled{false};

    void cancel() noexcept {
        cancelled.store(true, std::memory_order_relaxed);
    }
    bool isCancelled() const noexcept {
        return cancelled.load(std::memory_order_relaxed);
    }
};

/// Result of a manifest check.
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

/// Byte progress for downloads.
struct Progress {
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::string currentFile;
};

using ProgressCallback = std::function<void(const Progress&)>;

/// Callback bundle used by Updater.
struct Callbacks {
    std::function<void(const CheckResult&)> onCheckResult;
    std::function<void(const Progress&)> onProgress;
    std::function<void()> onReadyToApply;
    std::function<void(const Error&)> onError;
    std::function<void(State)> onStateChanged;
};

} // namespace autoupdater
