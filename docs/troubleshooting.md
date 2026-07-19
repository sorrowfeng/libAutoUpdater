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

## Python Command Fails on Windows

On Windows, `python` may point to the Microsoft Store alias. Use:

```powershell
uv run python --version
uv run python tools/make_manifest.py --help
```
