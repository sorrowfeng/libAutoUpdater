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

When `LIBAUTOUPDATER_BUILD_UPDATER=ON`, the installed package also exports
`libAutoUpdater::autoupdater_apply` as an imported executable target. Use
`$<TARGET_FILE:libAutoUpdater::autoupdater_apply>` when a packaging step needs
the helper's installed location; do not pass the executable target to
`target_link_libraries()`.

## Static and Shared Libraries

The default build is static. Set the standard CMake option
`BUILD_SHARED_LIBS=ON` to build and install a shared library, including a DLL
and import library on Windows. A shared deployment must keep the runtime
library available to both the application and `autoupdater_apply`:

- On Windows, install `libAutoUpdater.dll` beside the executables or add its
  directory to the process `PATH`.
- On Linux and macOS, preserve the installed `bin` and `lib` layout. The
  installed updater helper uses a relative runtime search path to locate the
  library under `lib`.

Applications remain responsible for deploying runtime libraries required by
optional backends, such as libcurl or OpenSSL.

## Network Backends

Default selection order:

- Local paths and `file://` are always available.
- libcurl is used when CMake finds `CURL::libcurl`.
- Windows uses WinHTTP when libcurl is unavailable and `LIBAUTOUPDATER_WITH_WINHTTP=ON`.
- macOS uses CFNetwork when libcurl is unavailable and `LIBAUTOUPDATER_WITH_CFNETWORK=ON`.
- The CFNetwork backend schedules each request in a private run-loop mode and
  performs reads only after a readable event. Positive connect and total
  transfer timeouts, as well as cancellation from another thread, are observed
  without leaving a blocked read or background worker behind.
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
- The bundled store identifies a resource without persisting URL query
  credentials, batches changes once per download task, and retains only
  current-release records no older than seven days within fixed count and byte
  limits. Resume state is advisory; losing it restarts the download rather than
  weakening artifact hash verification.

## Mandatory Updates

When `mandatory=true` appears in the manifest, `CheckResult::mandatory` is true. The library does not directly disable a "later" UI action; callers should apply their own product policy.

## Package-Manager-Owned Installs

If an application is managed by the system package manager, self-replacement is usually the wrong model. Set `Config::installLayout` to `PackageManagerOwned`; the library rejects the self-update flow and the caller should direct users to the package manager.
