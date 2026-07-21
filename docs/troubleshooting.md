# Troubleshooting

## No HTTP Network Adapter Is Available

Cause: the current build has no usable HTTP/HTTPS backend.

Fix:

- Linux/macOS: install a libcurl development package, or use CFNetwork on macOS.
- Windows: enable `LIBAUTOUPDATER_WITH_WINHTTP=ON`.
- vcpkg: use `cmake --preset vcpkg-debug`.

## Manifest baseUrl Is Not Allowed

Cause: `SecurityOptions::allowedBaseUrls` does not include the manifest `baseUrl` or the manifest URL selected from an index manifest.

Fix: add the full trusted prefix to the allowlist and keep host-boundary matching in mind.

## security.allowedBaseUrls Must Not Be Empty

Cause: every HTTP(S) update requires at least one trusted, query-free scope; the
allowlist is not optional, even for an unsigned development feed.

Fix: add the narrow HTTPS directory roots that contain the initial manifest,
any selected release manifest, detached signatures, and artifacts. Do not use a
host-wide scope when a release path is sufficient.

## ManifestSignatureInvalid: No Verifier Is Available

Cause: `requireManifestSignature=true`, but the build has no OpenSSL-backed
verifier and the application did not inject a working custom verifier.

Fix: configure with `LIBAUTOUPDATER_WITH_OPENSSL=ON` and
`LIBAUTOUPDATER_REQUIRE_OPENSSL=ON`, or inject and test an
`ISignatureVerifier`. Do not disable signature requirements to make a
production feed pass.

## Redirect Was Rejected

The core rejects destinations outside `allowedBaseUrls`, HTTPS-to-HTTP
downgrades, loops, excessive hops, ambiguous `Location`, and adapters that
report an effective URL different from the requested hop. Add a destination
scope only after verifying
that the redirect is an intended production path. Custom adapters must return a
single-hop response with automatic redirect handling disabled.

## PathTraversalRejected

The manifest contains an invalid path:

- Absolute path.
- `..`.
- Windows drive prefix.
- Empty path.

Fix the manifest or the packaging script input.

## Server Ignored Range Request

During resume, the server returned 200 instead of 206. The library refuses to combine an existing partial file with a full response.

Fix:

- Configure the server to support Range requests.
- Or disable `NetworkOptions::enableResume`.

## HashMismatch

The downloaded file SHA-256 does not match the manifest.

Check:

- The manifest matches the uploaded files.
- The CDN is not serving stale files.
- Object files were not edited manually.
- The manifest signature covers the intended manifest.

## External Updater Did Not Replace Files

Check:

- The main application is not still running.
- `updaterExecutable` points to `autoupdater_apply`.
- `apply-plan.json` was written.
- The installation directory allows replacement.
- No other updater process is actively operating on the same installation.

The default launcher does not elevate; it inherits the caller's current
credentials. Do not retry the helper manually as administrator or root against
plans/state from a lower-privileged writable directory. Correct
the installation ownership/ACL, or use a separately authenticated privileged
broker that validates trusted roots, ownership, a one-time request, and the
restart command. The broker must also independently verify signed release
authorization and rebuild or validate the complete plan, or require a plan
signed by a trusted release authority; a caller-supplied digest is not
authorization.

If a privileged deployment reports untrusted state or apply-plan ownership,
stop rather than changing permissions ad hoc. Verify the actual ACLs for the
install root, `.autoupdater`, custom `tempDir`, helper and restart executables,
then recreate the request through the trusted application channel.

An ordinary `.autoupdater/update.lock` file is expected to remain on disk. Its
presence does not mean the lock is held, and its contents are not used as PID
evidence. Do not delete or replace this regular file to clear contention;
doing so while a POSIX updater is active can break mutual exclusion. The
kernel releases the actual lock automatically when its process exits.

Very old releases created `update.lock` as a directory. The current updater
fails closed when that legacy marker exists because it has no trustworthy PID
or process-start identity. Confirm that no old updater is running before
manually moving or removing such a directory, then retry with the current
updater.

## An Apply or Rollback Was Interrupted

The external updater checks
`installDir/.autoupdater/journal/active.json` while holding the installation
lock on every invocation. It uses the immutable plan snapshot, per-operation
records, and durable backups to reconcile the interrupted transaction before
considering the newly requested plan.

Action:

- Preserve `active.json`, the referenced transaction files, and its backup
  directory.
- Correct the reported disk-space, permission, or storage error.
- Invoke the external updater again through the normal application API with an
  authorized plan. Merely starting the application does not run recovery.
- If diagnostics report that restart may already have begun or that journal
  evidence is inconsistent, stop all application instances and investigate the
  retained records. The updater intentionally fails closed instead of guessing.

Do not delete journal files, backups, or the regular `update.lock` marker to
force progress. That destroys the evidence required for safe recovery and can
invalidate mutual exclusion.

## Health Confirmation Deadline Expired

Cause: the running version still matches a pending update, and the current local
wall-clock time is at or beyond the terminal apply completion time plus
`healthConfirmationTimeout`. A zero timeout disables this deadline; non-zero
values are limited to 24 hours.

The library does not automatically roll back. It rejects health confirmation
and update checks while retaining pending state and rollback evidence. Decide
whether to call `rollbackLastUpdate()` or recover the current version through a
trusted operator workflow. Because the deadline uses the local wall clock,
also check for an incorrect or recently adjusted system time.

## rollbackLastUpdate Returned Success but Files Did Not Change

When a matching pending update exists and a rollback request is created,
success means the detached helper was launched, not that rollback completed.
If no pending update remains, the method succeeds as a no-op and launches no
helper. Otherwise, check that:

- The application exited before `applyWaitTimeout`.
- The pending state describes the currently running version.
- The latest terminal transaction matches that pending update.
- The manifest-scoped backup still exists and is readable.
- No other updater holds the installation lock.

The helper derives inverse operations and the restart command from the terminal
forward transaction; the caller-supplied rollback request cannot override them.
A later library operation clears pending state only after it verifies a
completed terminal rollback. Custom state stores must implement
`IPendingUpdateCompareAndSet` or this reconciliation fails closed and leaves the
pending record intact.

## Backup Directory Keeps Growing

This is expected: health confirmation clears pending state but intentionally
retains manifest-scoped rollback backups, and the library performs no automatic
garbage collection. After confirmation, `rollbackLastUpdate()` no longer has a
pending update to roll back and is a successful no-op; retained evidence is for
an explicit product/operator recovery policy. Apply an application- or
installer-owned retention policy only after confirming that no pending update,
active journal, supported operator recovery, or rollback request references the
candidate transaction.

## Python Command Fails on Windows

On Windows, `python` may point to the Microsoft Store alias. Use:

```powershell
uv run python --version
uv run python tools/make_manifest.py --help
```
