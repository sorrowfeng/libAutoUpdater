# Integration Guide

## CMake add_subdirectory

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

## CMake find_package

After installation:

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

## Network Backends

Default selection order:

- Local paths and `file://` are always available.
- libcurl is used when CMake finds `CURL::libcurl`.
- Windows uses WinHTTP when libcurl is unavailable and `LIBAUTOUPDATER_WITH_WINHTTP=ON`.
- macOS uses CFNetwork when libcurl is unavailable and `LIBAUTOUPDATER_WITH_CFNETWORK=ON`.
- Qt users can inject a `QNetworkAccessManager` adapter.

## Callback Threading

`Updater` performs checks and downloads on background threads. Callbacks are delivered through `IEventDispatcher`:

- The default dispatcher invokes callbacks directly.
- GUI applications should inject a dispatcher that posts callbacks back to the UI thread.
- The Qt example includes `QtDispatcher`.

## Cancellation Semantics

`Updater::cancel()` requests cancellation of the current check or download. Cancellation is cooperative:

- Network backends periodically observe the cancellation token.
- Interrupted downloads may leave temporary files and resume metadata.
- A later retry can resume the download if the server supports Range requests.

## Mandatory Updates

When `mandatory=true` appears in the manifest, `CheckResult::mandatory` is true. The library does not directly disable a "later" UI action; callers should apply their own product policy.

## Package-Manager-Owned Installs

If an application is managed by the system package manager, self-replacement is usually the wrong model. Set `Config::installLayout` to `PackageManagerOwned`; the library rejects the self-update flow and the caller should direct users to the package manager.
