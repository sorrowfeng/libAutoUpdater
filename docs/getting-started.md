# Getting Started

This guide shows the shortest path to build and integrate `libAutoUpdater`. See [architecture-plan.md](architecture-plan.md) for the full design.

## 1. Build the Library and Updater

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

For application integration, install the release build into a prefix:

```sh
cmake --preset release
cmake --build --preset release --parallel
cmake --install build/release --config Release --prefix install-package
```

## 2. Use It in an Application

```cpp
#include <libAutoUpdater/Updater.h>

autoupdater::Config config;
config.appId = "com.example.myapp";
config.platform = "windows";
config.arch = "x64";
config.manifestUrl = "https://example.com/updates/releases/1.2.3/manifest.json";
config.currentVersion = autoupdater::Version::parse("1.0.0").value();
config.installDir = "C:/Program Files/MyApp";
config.updaterExecutable = "C:/Program Files/MyApp/autoupdater_apply.exe";
config.restartCommand = {"C:/Program Files/MyApp/MyApp.exe"};

autoupdater::Updater updater(config);
updater.setCallbacks({
    .onCheckResult = [](const autoupdater::CheckResult& result) {
        // Update UI or log result.
    },
    .onProgress = [](const autoupdater::Progress& progress) {
        // Update progress UI.
    },
    .onReadyToApply = [] {
        // Ask the user to restart now, or call applyAndRestartAsync().
    },
});
updater.checkAndDownloadAsync();
```

## 3. Generate a Manifest

Version-directory layout:

```sh
python tools/make_manifest.py dist/MyApp \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.2.3 \
  --release-date 2026-06-02T00:00:00Z \
  --base-url https://example.com/updates/releases/1.2.3/windows-x64/
```

For content-addressed storage, see [content-addressed-storage.md](content-addressed-storage.md).

## 4. Upload Server Files

Upload `manifest.json` and every referenced file to any static HTTP/HTTPS server. The client only needs to access `Config::manifestUrl`.

## 5. Apply an Update

Recommended flow:

1. Check silently after application startup.
2. Download the update into the staging directory.
3. After user confirmation, call `applyAndRestartAsync()`.
4. Exit the main application.
5. `autoupdater_apply` waits for exit, backs up affected files, replaces files, verifies the result, rolls back on failure, and restarts the application.
