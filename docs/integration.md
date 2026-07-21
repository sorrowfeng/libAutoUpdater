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

## Dependency Policy

These are source/API compatibility floors, not recommendations to deploy an
unsupported dependency release:

| Dependency | Minimum | Scope and production guidance |
| --- | --- | --- |
| libcurl | 7.19.4 | Optional HTTP backend. Use a currently supported build with a maintained TLS backend. |
| OpenSSL | 1.1.1 | Bundled signature verifier and Ed25519 floor. Prefer OpenSSL 3.x or a vendor-maintained build. |
| Qt | 6.0.0 | Optional example/adapter only; Qt 6.3+ also exposes the connection signal used when available. |

`LIBAUTOUPDATER_WITH_CURL=ON` and `LIBAUTOUPDATER_WITH_OPENSSL=ON` probe for
compatible dependencies. Add `LIBAUTOUPDATER_REQUIRE_CURL=ON` or
`LIBAUTOUPDATER_REQUIRE_OPENSSL=ON` to fail configuration when the requested
backend is missing or too old. Production builds that rely on the bundled
signature verifier should require OpenSSL explicitly; applications injecting a
custom verifier may keep it disabled. This build-time requirement does not
enable runtime verification by itself; production configuration must also set
`SecurityOptions::requireManifestSignature=true` and provide the trusted key.

## Network Backends

Default selection order:

- The built-in `file://` transport requires no optional backend, but `Updater`
  accepts only an absolute file URL with `allowLocalFileUrls=true`; a raw local
  path is not a valid manifest URL.
- libcurl is used by the default core client when
  `LIBAUTOUPDATER_WITH_CURL=ON` and CMake finds `CURL::libcurl`.
- Windows uses WinHTTP when libcurl is unavailable and `LIBAUTOUPDATER_WITH_WINHTTP=ON`.
- macOS uses CFNetwork when libcurl is unavailable and `LIBAUTOUPDATER_WITH_CFNETWORK=ON`.
- The CFNetwork backend schedules each request in a private run-loop mode and
  performs reads only after a readable event. Positive connect and total
  transfer timeouts, as well as cancellation from another thread, are observed
  without leaving a blocked read or background worker behind.
- Qt users can inject the `QNetworkAccessManager` adapter from `examples/qt`.
  It is example source, not an installed or exported CMake package target.

## Production Security Checklist

For every production HTTP(S) feed:

- Use HTTPS-only, query-free `allowedBaseUrls` narrowed to the required release
  paths. The list is mandatory for all HTTP(S) configurations.
- Keep `verifyTls=true`; require detached manifest signatures and embed the
  trusted release public key. An index and its selected release manifest are
  verified independently.
- Publish a bounded `expiresAt`, keep `rejectExpiredManifest=true` and
  `rejectDowngrade=true`, and define server retention for old signed manifests.
  Release `publishedAt`, index `generatedAt`, and `lastAcceptedReleaseId` do not
  currently provide monotonic release ordering; an index has no client-enforced
  expiry of its own.
- Configure redirect-capable custom adapters for one-hop responses only. The
  core re-authorizes each redirect and rejects TLS downgrade.
- Do not put reusable or long-lived credentials in URLs. If short-lived signed
  URLs are unavoidable, scope them narrowly and redact them outside the core as
  well.

Before installation, create and verify platform ownership and ACLs for
`installDir`, `installDir/.autoupdater`, any custom `tempDir`, apply plans,
state, journals, backups, `autoupdater_apply`, and the restart executable. New
POSIX private entries use restrictive modes, but existing ancestors are not
repaired. Windows entries inherit deployment ACLs; the library does not create
or audit a private DACL.

Custom `IStateStore` implementations need equivalent access control, bounded
storage, inter-process synchronization, atomic replace/compare-and-set
semantics, and crash durability.

The default launcher has no built-in elevation and inherits the application's
current credentials. Do not solve a permission error by exposing the helper's
command line through an unrestricted administrator/root launcher:
`--plan-sha256` detects plan changes but is not authorization. A
privileged broker must authenticate callers, constrain trusted roots, validate
owner/ACL, bind digest/intent/install root plus a one-time nonce to the session,
control `restartCommand`, and define the privilege of the restarted process. It
must independently verify signed release authorization and rebuild or validate
the complete plan (or require a plan signed by a trusted release authority),
then publish accepted plans into a broker-only writable root. Caller-supplied
digests, nonces, and file ownership are not content authorization.

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

## Apply, Recovery, and Rollback Lifecycle

`applyAndRestartAsync()` launches `autoupdater_apply` as a detached process.
The helper waits for the calling process to exit, acquires the installation
lock, and performs all managed-file changes. After requesting apply, let the
application terminate before `Config::applyWaitTimeout`; the library does not
replace its own running files.

The helper journals the immutable plan, operation state, rollback evidence, and
restart state under `installDir/.autoupdater/journal`. On every helper
invocation it recovers an active transaction before considering a new one.
Recovery can roll back a partially applied transaction or finish a durably
applied transaction; it launches restart only when the journal proves that no
earlier restart attempt occurred. It deliberately does not repeat an ambiguous
restart or roll back files after restart may have begun. Do not delete
`active.json`, transaction records, or backups to clear an error: correct the
reported storage or permission problem and invoke an authorized helper plan
again. Starting the main application by itself does not run this recovery.

After the updated application has completed its own startup checks, confirm the
running version:

```cpp
auto confirmed = updater.markCurrentVersionHealthy();
if (!confirmed) {
    // Keep the pending state and surface a recovery or rollback action.
}
```

`healthConfirmationTimeout` accepts zero through 24 hours. Zero disables the
deadline. Otherwise, checks and health confirmation fail when the local wall
clock reaches the terminal apply receipt's completion time plus the configured
timeout. Expiry is evaluated lazily and never starts an automatic rollback. The
pending record and backup remain available so the application can offer an
explicit rollback.

To request that rollback while the pending version is running:

```cpp
auto launched = updater.rollbackLastUpdate();
if (launched) {
    // Exit before applyWaitTimeout so the detached helper can proceed.
}
```

When a matching pending update exists and a rollback request is created, success
confirms helper launch, not rollback completion. If no pending update remains,
the method succeeds as a no-op and launches no helper. The public method writes
no managed install file and supplies no rollback operations or restart command;
the helper derives both from the digest-bound terminal forward transaction
while holding the lock. Pending state is cleared only when a later library call
from the restored version reconciles a completed rollback. A custom
`IStateStore` must implement `commitHealthyVersion()` with the documented
compare-and-set semantics and must also implement
`IPendingUpdateCompareAndSet` for completed-rollback reconciliation. Without
the latter capability, reconciliation fails closed with `StateStoreError` and
retains pending state.

Forward backups are scoped by the complete manifest SHA-256, so a later update
does not overwrite the only backup for an earlier transaction. Successful
health confirmation clears the matching pending record but intentionally keeps
that backup. Once confirmation clears pending state, `rollbackLastUpdate()` is a
successful no-op and does not launch the helper; retained backups support only
an explicit product/operator recovery policy after confirmation. The library
performs no automatic backup or journal garbage collection. Define a
product-specific retention policy, and remove evidence only when no pending
update, active transaction, rollback request, or supported operator recovery
can still reference it.

## Mandatory Updates

When `mandatory=true` appears in the manifest, `CheckResult::mandatory` is true. The library does not directly disable a "later" UI action; callers should apply their own product policy.

## Package-Manager-Owned Installs

If an application is managed by the system package manager, self-replacement is usually the wrong model. Set `Config::installLayout` to `PackageManagerOwned`; the library rejects the self-update flow and the caller should direct users to the package manager.
