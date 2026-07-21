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

## Production Deployment Baseline

The runtime keeps some controls configurable for tests and compatibility. A
production deployment is secure only when the integration enforces all of the
following:

- Every HTTP(S) update uses HTTPS, keeps `NetworkOptions::verifyTls=true`, and
  supplies a non-empty, query-free `allowedBaseUrls` narrowed to the release
  paths the application actually needs.
- Every production network feed sets `requireManifestSignature=true`, embeds a
  trusted public key, and signs the exact bytes of both the index (when used)
  and the selected release manifest. SHA-256 artifact hashes do not replace
  manifest authentication.
- Release manifests carry a bounded `expiresAt`, clients keep
  `rejectExpiredManifest=true`, and the publisher removes or denies obsolete
  release metadata according to a documented retention policy. Clients that
  require resistance to local clock rollback need a trusted-time or
  server-assisted policy outside this library.
- The installation root, `.autoupdater` state, custom staging directory, apply
  plans, journals, backups, updater executable, and restart executable are not
  writable by a principal less trusted than the application/helper processes
  that consume them.
- The default launcher inherits the calling process's existing credentials and
  performs no elevation. The helper is not an authorization boundary. A
  privileged broker requires a separate authenticated protocol and the controls
  described below.
- Long-lived bearer tokens, passwords, and reusable credentials are never put
  in update URLs. Short-lived signed URLs must use narrowly scoped credentials
  and still be treated as secrets by surrounding telemetry and infrastructure.

## Manifest Signature

The runtime default is `false`. Detached signatures are mandatory for production
HTTP(S) feeds:

```cpp
config.security.requireManifestSignature = true;
config.security.manifestSignatureUrl = "https://example.com/updates/releases/1.2.3/manifest.json.sig";
config.security.publicKeyPem = "... built-in public key ...";
```

Guidance:

- Keep the private key only in an offline or protected release environment.
- Embed the public key in the client, or distribute it through another trusted application channel.
- Sign the original `manifest.json` bytes. Do not reformat JSON between signing and verification.

### Bundled Verifier Policy

The OpenSSL-backed verifier accepts exactly these public-key and signature
combinations:

- Ed25519, which provides approximately 128 bits of security.
- RSA PKCS#1 v1.5 with SHA-256. RSA keys must be at least 2048 bits; use 3072
  bits for newly generated long-lived release keys.

ECDSA, DSA, Ed448, RSA-PSS keys, RSA keys below 2048 bits, and other algorithms
are rejected even if the linked OpenSSL version could verify them. Detached
signatures may be raw bytes or base64 text. `tools/sign_manifest.py` exposes
the matching `ed25519` and `rsa-sha256` signing modes.

Applications that inject a custom `ISignatureVerifier` define their own
algorithm policy and must document and test it separately.

### Key Rotation and Recovery

`SecurityOptions` is deliberately a single-key trust model. One
`publicKeyPem` verifies both index and release manifests; there is no key ID,
keyring, dual-signature window, remotely supplied trust root, or revocation
mechanism.

For a planned rotation:

1. While the old key is still trusted, publish an old-key-signed client update
   that embeds the new public key.
2. Keep signing the feed with the old key for the full client migration
   window.
3. After the supported client population has migrated, switch the feed to the
   new private key. Clients that missed the transition require a new installer
   or another trusted distribution channel.

If the old private key is compromised, an update authorized by that same key
cannot establish a trustworthy replacement key. Recover through an independent
trusted channel such as a signed installer, managed software distribution, or
an application store.

## HTTPS and TLS

`NetworkOptions::verifyTls` defaults to `true`. The code permits an initial
`http:` URL when it is explicitly allowlisted, but production scopes must use
`https:` and must not disable certificate verification.

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

For every HTTP(S) update the allowlist is mandatory and must contain only
query-free absolute HTTP(S) URLs. Configuration fails before network access when
it is empty, malformed, or does not contain the initial manifest URL. Keep each
scope as narrow as practical. Local `file:` manifests use a different boundary
and require the explicit `allowLocalFileUrls=true` opt-in.

## Redirect Handling

Redirects are coordinated by the core rather than delegated to a network
backend. Only 301, 302, 303, 307, and 308 are followed, up to
`NetworkOptions::maxRedirects` (five by default). Every hop is resolved and
checked again against the allowlist. Cross-origin redirects work only when both
origins are explicitly allowed; HTTPS-to-HTTP downgrade, file/network mode
switches, loops, missing or duplicate `Location`, and an effective URL reported
by the adapter as different from the requested hop fail closed.

Bundled adapters disable automatic redirect following. A custom
`INetworkClient` must do the same and must report the requested URL as its
effective URL for a single hop. Range validators are not forwarded across a
redirect boundary.

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

The implemented downgrade baseline is the greater of the running version and
the persisted `lastAcceptedVersion`. Release `publishedAt` and index
`generatedAt` are syntax-validated but are not compared monotonically, and the
persisted `lastAcceptedReleaseId` is not a planner rejection condition. An index
has no client-enforced expiry of its own. Therefore this is a version rollback
barrier plus an optional release-manifest expiry check, not a total
release-sequence anti-replay protocol. An old signed index or release manifest
that remains downloadable can still be accepted when it selects an unexpired
release above the client's baseline. Production publishers must use bounded
release expiry, control old-metadata availability, protect local state, and
document any stronger freshness service they require.

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

Excluding query parameters from the resume-state key prevents one persistence
path from storing signed-URL credentials; it is not an end-to-end secrecy
guarantee. Network libraries, proxies, crash dumps, and application telemetry
may still observe a requested URL. Never use long-lived URL credentials. Use
short-lived, least-privilege signed URLs only when required, and redact them in
all surrounding systems.

## Local Filesystem and Privilege Boundary

The default state lives at `installDir/.autoupdater/state.json`; apply plans,
staging files, journals, and backups are likewise security-sensitive inputs or
evidence. Newly created POSIX private directories use `0700`, and ordinary
private payload files start as `0600`, but rollback backups may deliberately
preserve source permission bits. Their confidentiality therefore also depends
on the private parent directory and deployment ACL. The library does not repair
an already existing permissive ancestor. Windows private paths inherit the
deployment's ACL; the library does not synthesize or audit a restrictive DACL.
Installers must create and verify the production ownership and ACLs on every
supported platform, including any custom `tempDir`.

An injected `IStateStore` must provide equivalent access control, bounded
storage, inter-process synchronization, atomic replace/compare-and-set
semantics, and crash durability. Moving state to a database or service does not
remove its role in downgrade, pending-update, and rollback decisions.

The default launcher uses `CreateProcessW` or `execv` with the current process's
credentials; it does not invoke UAC, `sudo`, a privileged service, or an
authenticated broker. The helper's
`--plan-sha256`, `--install-root`, and intent checks bind command-line values to
one another and detect accidental/tampered plan changes, but the digest is not
an authorization credential. A principal able to invoke a privileged helper
could otherwise provide its own plan and matching digest.

Do not expose the stock command line directly as an elevated entry point. A
privileged integration must authenticate the caller, restrict install/staging/
plan roots, verify owner and ACLs, bind digest, intent, install root, and a
single-use nonce to one authorization session, and generate or allowlist the
restart command on the trusted side. In addition, the privileged side must
independently authorize update content: verify the signed release metadata and
rebuild or validate every plan field, or require a plan signed by a trusted
release authority. It must publish the accepted plan into a broker-only writable
root and must not treat a caller-supplied digest, nonce, or file owner as content
authorization. It must also define whether restart drops privileges. Without
those controls, use a launcher that keeps the helper at the same privilege level
as the application and fail with a permission error when the installation is
not writable.

The offline [deployment investigation procedure](deployment-investigations.md)
defines the production evidence required to verify this boundary. Repository
tests and example evidence are not a production attestation.

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
