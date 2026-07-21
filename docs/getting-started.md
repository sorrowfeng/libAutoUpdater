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
config.security.allowedBaseUrls = {"https://example.com/updates/releases/"};
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

The allowlist is mandatory for HTTP(S), not optional. For a production network
feed, keep `verifyTls=true`, set `requireManifestSignature=true`, and embed the
trusted release public key. Both an index and its selected release manifest are
verified when an index feed is used. Unsigned feeds should be confined to
explicit development/test workflows, including local `file:` fixtures. Local
fixtures require an absolute `file://` manifest URL and
`security.allowLocalFileUrls=true`; raw filesystem paths are not valid manifest
URLs. A production offline or shared-filesystem feed still needs signatures
unless its filesystem trust boundary provides a documented equivalent
authorization guarantee.

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

The generator writes `dist/MyApp/manifest.json` by default; `--base-url` is the
artifact base and does not determine the manifest's public URL. Once the JSON is
final, sign its exact bytes for a production feed:

```sh
python tools/sign_manifest.py dist/MyApp/manifest.json \
  --private-key release-private-key.pem \
  --algorithm ed25519
```

The default detached-signature output is `manifest.json.sig`.

For content-addressed storage, see [content-addressed-storage.md](content-addressed-storage.md).

## 4. Upload Server Files

Upload `manifest.json`, `manifest.json.sig`, and every referenced file to the
static HTTPS server. Set `Config::manifestUrl` to the published manifest URL;
the default signature lookup appends `.sig` to that URL. Do not modify the
manifest after signing. Development feeds that intentionally disable signatures
may omit the `.sig` file, but production HTTP(S) feeds may not.

## 5. Apply an Update

Recommended flow:

1. Check silently after application startup.
2. Download the update into the staging directory.
3. After user confirmation, call `applyAndRestartAsync()`.
4. Exit the main application.
5. `autoupdater_apply` waits for exit, backs up affected files, replaces files, verifies the result, rolls back on failure, and restarts the application.
