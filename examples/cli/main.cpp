#include "libAutoUpdater/Updater.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>

namespace {

void usage() {
    std::cout << "Usage:\n"
              << "  libAutoUpdater_cli --manifest <url-or-file> --version <x.y.z> --install <dir> [--updater <path>] [--apply]\n";
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

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int exitCode = 0;

    autoupdater::Updater updater(config);
    autoupdater::Callbacks callbacks;
    callbacks.onStateChanged = [&](autoupdater::State state) {
        std::cout << "state=" << static_cast<int>(state) << "\n";
        if (state == autoupdater::State::UpToDate ||
            state == autoupdater::State::UpdateAvailable ||
            state == autoupdater::State::Applying ||
            state == autoupdater::State::Failed) {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
            cv.notify_one();
        }
    };
    callbacks.onCheckResult = [](const autoupdater::CheckResult& result) {
        std::cout << "updateAvailable=" << (result.updateAvailable ? "true" : "false") << "\n";
        if (result.remoteVersion) {
            std::cout << "remoteVersion=" << result.remoteVersion->toString() << "\n";
        }
        if (result.reinstallRequired) {
            std::cout << "reinstallRequired=true\n";
        }
    };
    callbacks.onProgress = [](const autoupdater::Progress& progress) {
        std::cout << "download " << progress.downloadedBytes << "/" << progress.totalBytes
                  << " " << progress.currentFile << "\n";
    };
    callbacks.onReadyToApply = [&] {
        std::cout << "readyToApply=true\n";
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

    if (exitCode == 0 && applyWhenReady && updater.state() == autoupdater::State::ReadyToApply) {
        done = false;
        lock.unlock();
        updater.applyAndRestartAsync();
        lock.lock();
        cv.wait_for(lock, std::chrono::seconds(5), [&] {
            const auto state = updater.state();
            return done || state == autoupdater::State::Applying || state == autoupdater::State::Failed;
        });
    }

    return exitCode;
}
