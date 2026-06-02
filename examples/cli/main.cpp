#include "libAutoUpdater/Updater.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>

namespace {

void usage() {
    std::cout << "Usage:\n"
              << "  libAutoUpdater_cli --manifest <url-or-file> --version <x.y.z> --install <dir> [--updater <path>] "
                 "[--apply]\n";
}

const char* stateName(autoupdater::State state) noexcept {
    switch (state) {
    case autoupdater::State::Idle:
        return "Idle";
    case autoupdater::State::Checking:
        return "Checking";
    case autoupdater::State::UpToDate:
        return "UpToDate";
    case autoupdater::State::UpdateAvailable:
        return "UpdateAvailable";
    case autoupdater::State::Downloading:
        return "Downloading";
    case autoupdater::State::ReadyToApply:
        return "ReadyToApply";
    case autoupdater::State::Applying:
        return "Applying";
    case autoupdater::State::Failed:
        return "Failed";
    }
    return "Unknown";
}

void printBanner(const autoupdater::Config& config, bool applyWhenReady) {
    std::cout << "libAutoUpdater CLI example\n";
    std::cout << "  currentVersion: " << config.currentVersion.toString() << "\n";
    std::cout << "  manifestUrl:    " << config.manifestUrl << "\n";
    std::cout << "  installDir:     " << config.installDir.string() << "\n";
    std::cout << "  apply:          " << (applyWhenReady ? "yes" : "no") << "\n";
    if (applyWhenReady) {
        std::cout << "  updater:        " << config.updaterExecutable.string() << "\n";
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char** argv) {
    autoupdater::Config config;
    config.appId = "libAutoUpdater.example";
    config.restartCommand = {};
    bool applyWhenReady = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--manifest" && i + 1 < argc) {
            config.manifestUrl = argv[++i];
        } else if (arg == "--version" && i + 1 < argc) {
            auto parsed = autoupdater::Version::parse(argv[++i]);
            if (!parsed) {
                std::cerr << parsed.error().message << "\n";
                return 2;
            }
            config.currentVersion = parsed.value();
        } else if (arg == "--install" && i + 1 < argc) {
            config.installDir = argv[++i];
        } else if (arg == "--updater" && i + 1 < argc) {
            config.updaterExecutable = argv[++i];
        } else if (arg == "--apply") {
            applyWhenReady = true;
        } else if (arg == "--help") {
            usage();
            return 0;
        }
    }

    if (config.manifestUrl.empty() || config.installDir.empty()) {
        usage();
        return 2;
    }
    if (applyWhenReady && config.updaterExecutable.empty()) {
        std::cerr << "--updater is required when --apply is used\n";
        return 2;
    }

    printBanner(config, applyWhenReady);

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int exitCode = 0;
    bool printedDownloadHeader = false;
    bool readyToApply = false;
    std::string remoteVersionText;

    autoupdater::Updater updater(config);
    autoupdater::Callbacks callbacks;
    callbacks.onStateChanged = [&](autoupdater::State state) {
        std::cout << "state=" << stateName(state) << "\n";
        if (state == autoupdater::State::Checking) {
            std::cout << "[1/4] Checking manifest and comparing versions...\n";
        } else if (state == autoupdater::State::Downloading) {
            std::cout << "[2/4] Downloading changed or missing files...\n";
        } else if (state == autoupdater::State::ReadyToApply) {
            std::cout << "[3/4] Update files are staged and verified.\n";
        } else if (state == autoupdater::State::Applying) {
            std::cout << "[4/4] External updater launched. The main process can now exit.\n";
        } else if (state == autoupdater::State::UpToDate) {
            std::cout << "No update is required.\n";
        }
        if (state == autoupdater::State::UpToDate || state == autoupdater::State::UpdateAvailable ||
            state == autoupdater::State::Applying) {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
            cv.notify_one();
        }
    };
    callbacks.onCheckResult = [&](const autoupdater::CheckResult& result) {
        std::cout << "\nCheck result\n";
        std::cout << "  currentVersion: " << result.currentVersion.toString() << "\n";
        std::cout << "  updateAvailable: " << (result.updateAvailable ? "yes" : "no") << "\n";
        std::cout << "  mandatory: " << (result.mandatory ? "yes" : "no") << "\n";
        std::cout << "updateAvailable=" << (result.updateAvailable ? "true" : "false") << "\n";
        if (result.remoteVersion) {
            remoteVersionText = result.remoteVersion->toString();
            std::cout << "  remoteVersion: " << result.remoteVersion->toString() << "\n";
            std::cout << "remoteVersion=" << result.remoteVersion->toString() << "\n";
        }
        if (result.reinstallRequired) {
            std::cout << "  reinstallRequired: yes\n";
            std::cout << "reinstallRequired=true\n";
        }
        if (!result.notes.empty()) {
            std::cout << "  notes: " << result.notes << "\n";
        }
        std::cout << "\n";
    };
    callbacks.onProgress = [&](const autoupdater::Progress& progress) {
        if (!printedDownloadHeader) {
            std::cout << "Download plan\n";
            printedDownloadHeader = true;
        }
        double percent = 0.0;
        if (progress.totalBytes > 0) {
            percent = static_cast<double>(progress.downloadedBytes) * 100.0 / static_cast<double>(progress.totalBytes);
        }
        std::cout << "download " << progress.downloadedBytes << "/" << progress.totalBytes << " "
                  << progress.currentFile << "\n";
        std::cout << "  [" << std::fixed << std::setprecision(1) << percent << "%] " << progress.currentFile << " ("
                  << progress.downloadedBytes << "/" << progress.totalBytes << " bytes)\n";
    };
    callbacks.onReadyToApply = [&] {
        readyToApply = true;
        std::cout << "\nReady to apply\n";
        std::cout << "readyToApply=true\n";
        if (!remoteVersionText.empty()) {
            std::cout << "  stagedDir: "
                      << (config.installDir / ".autoupdater" / "staging" / remoteVersionText).string() << "\n";
        }
        if (applyWhenReady) {
            std::cout << "  action: launching external updater\n";
        } else {
            std::cout << "  action: run again with --apply to launch the external updater\n";
        }
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        cv.notify_one();
    };
    callbacks.onError = [&](const autoupdater::Error& error) {
        std::cerr << autoupdater::toString(error.code) << ": " << error.message << "\n";
        std::lock_guard<std::mutex> lock(mutex);
        exitCode = 1;
        done = true;
        cv.notify_one();
    };
    updater.setCallbacks(std::move(callbacks));
    updater.checkAndDownloadAsync();

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] {
        const auto state = updater.state();
        return done || state == autoupdater::State::UpToDate || state == autoupdater::State::UpdateAvailable;
    });

    if (exitCode == 0 && applyWhenReady && readyToApply) {
        done = false;
        lock.unlock();
        updater.applyAndRestartAsync();
        lock.lock();
        cv.wait_for(lock, std::chrono::seconds(5), [&] {
            const auto state = updater.state();
            return done || state == autoupdater::State::Applying;
        });
    }

    return exitCode;
}
