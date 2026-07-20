# Security Model

`libAutoUpdater` aims to reduce the risk of malicious updates, path traversal, downgrade attacks, replayed manifests, and failed file replacement, even when update files are hosted on a static HTTP/HTTPS server.

## Threat Model

Primary concerns:

- A manifest or update file is tampered with.
- A CDN, object store, or static server is misconfigured.
- An old manifest is replayed.
- An attacker attempts to write outside the installation root through manifest paths.
- A crash or power loss leaves the installation directory inconsistent during apply.

Not directly solved:

- The host is already controlled by administrator-level malware.
- The caller disables `verifyTls` or signature verification and still trusts an untrusted network.
- A package-manager-owned install directory is replaced directly by the application.

## Manifest Signature

Recommended for production:

```cpp
config.security.requireManifestSignature = true;
config.security.manifestSignatureUrl = "https://example.com/updates/releases/1.2.3/manifest.json.sig";
config.security.publicKeyPem = "... built-in public key ...";
```

Guidance:

- Keep the private key only in an offline or protected release environment.
- Embed the public key in the client, or distribute it through another trusted application channel.
- Perform key rotation by shipping a new client version.
- Sign the original `manifest.json` bytes. Do not reformat JSON between signing and verification.

## HTTPS and TLS

`NetworkOptions::verifyTls` defaults to `true`. Do not disable TLS verification in production.

Signatures and HTTPS are complementary:

- HTTPS protects the transport path and privacy.
- Manifest signatures protect update integrity if the static server is compromised.

## Base URL Allowlist

`SecurityOptions::allowedBaseUrls` restricts manifest URLs and release `baseUrl`
values to trusted prefixes. Prefix matching respects parsed origin and path
boundaries to prevent bypasses such as
`https://trusted.example.com.evil.com`. URL paths and queries are validated as
separate URI components, repeated path separators are preserved, and fragments
are rejected.

## Path Safety

All manifest file paths must be managed relative paths:

- Absolute paths are rejected.
- `..` is rejected.
- Windows drive prefixes are rejected.
- Empty paths are rejected.
- Paths use forward slashes.
- The top-level `.autoupdater` namespace and alias-ambiguous variants are
  reserved for updater state.
- The complete target set is rejected when two operations have the same
  portable target, differ only by ASCII case, or have an ancestor/descendant
  conflict. This applies to replace/replace, remove/remove, and replace/remove
  combinations before any version or local-file shortcut is taken.

Files are downloaded into the staging directory first. They are only written into the apply plan after SHA-256 verification succeeds.

The portable collision check deliberately does not rewrite input paths. Full
Unicode case-folding and normalization are not part of the current C++17 path
contract; integrations that rely on filesystem-specific Unicode aliases must
apply a stricter platform policy.

## Downgrade and Replay

Enabled by default:

- `rejectDowngrade=true`
- `rejectExpiredManifest=true`

The manifest may include:

- `publishedAt`
- `expiresAt`
- `releaseId`
- `allowDowngrade`

Timestamp fields use the documented nanosecond-capable RFC 3339 profile.
`expiresAt` is an exclusive upper bound: equality is expired. Invalid timestamp
syntax is rejected even when `rejectExpiredManifest` is disabled. Expiry still
depends on the local wall clock and does not by itself prevent clock rollback or
replay of a signed manifest that omits `expiresAt`.

Downgrades are accepted only when all of the following are true:

- The exact release manifest has a valid detached signature.
- The release manifest sets `allowDowngrade=true`.
- The local application explicitly sets `rejectDowngrade=false`.

An index-manifest signature does not authorize a downgrade of the selected
release manifest. The selected release manifest must be verified separately.

## Download Resume State

The core never uses the request URL itself as a persistence key. It derives an
opaque SHA-256 resource identity from release and signed artifact context while
excluding URL query parameters. The query string is therefore never serialized
as the resource key, and rotating signed-URL credentials do not invalidate a
matching partial download. The bundled JSON store keeps resume metadata in a
separate atomic sidecar, under the same inter-process lock as authoritative
state. It retains only the active release, rejects timestamps more than seven
days from the current wall clock in either direction, and enforces both
entry-count and byte limits. Legacy URL-keyed resume maps are advisory: they
are read for compatibility and scrubbed from the primary and last-known-good
state on the next successful resume mutation after the authoritative primary
state validates. A missing or corrupt primary continues to fail closed instead
of trusting or rewriting its last-known-good snapshot in isolation.

Resume metadata never authorizes content. A resumed or restarted artifact must
still match the signed size and SHA-256 before it can enter an apply plan.

## Diagnostic Data

`Error::message` is an in-process troubleshooting field, not a log-safe
contract. Applications should use `formatDiagnostic()` when recording errors;
it emits only the stable operation phase and error code. Bundled command-line
frontends and the external updater follow that rule, and the transaction
journal does not persist arbitrary adapter messages.

Custom adapters must not copy request URLs, authorization values, response
bodies, signatures, public or private key material, environment secrets, or
process arguments into error messages. Operational displays must omit URL
userinfo, query strings, and fragments because signed URLs commonly carry
credentials there.

## Apply and Rollback

Replacement is performed by `autoupdater_apply`:

1. Wait for the main process to exit.
2. Acquire the installation update lock.
3. Back up files that will be replaced or removed.
4. Copy staged files into the installation directory.
5. Verify installed files by SHA-256.
6. Roll back backups on failure.

Callers should invoke `markCurrentVersionHealthy()` after the new version
starts successfully. The configured health deadline is measured from the
external updater's durable completion receipt and is evaluated against the
local wall clock. Deadline expiry fails closed and retains both pending state
and rollback evidence. A successful confirmation atomically clears only the
matching pending record. Backups are deliberately retained in
manifest-specific directories and are not garbage-collected by the library;
deployments may remove them later only under an explicit retention policy.
Legacy schema-v2 receipts have no authoritative completion time, so they
remain confirmable without enforcing a guessed deadline. Legacy pending state
that predates apply-plan digests is never treated as a wildcard: confirmation
or completed-rollback reconciliation must first match the digest-verified
immutable transaction snapshot against all persisted version, release, and
path metadata.

## Security Review Checklist

Add tests and PR notes when changing:

- Manifest parser.
- Path validation / `safeJoin`.
- URL allowlist.
- Signature verifier.
- Download resume.
- Apply-plan schema.
- Updater backup / rollback.
- Process launch arguments.
