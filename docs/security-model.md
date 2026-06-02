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

`SecurityOptions::allowedBaseUrls` restricts manifest URLs and release `baseUrl` values to trusted prefixes. Prefix matching must respect host boundaries to prevent bypasses such as `https://trusted.example.com.evil.com`.

## Path Safety

All manifest file paths must be managed relative paths:

- Absolute paths are rejected.
- `..` is rejected.
- Windows drive prefixes are rejected.
- Empty paths are rejected.
- Paths use forward slashes.

Files are downloaded into the staging directory first. They are only written into the apply plan after SHA-256 verification succeeds.

## Downgrade and Replay

Enabled by default:

- `rejectDowngrade=true`
- `rejectExpiredManifest=true`

The manifest may include:

- `publishedAt`
- `expiresAt`
- `releaseId`
- `allowDowngrade`

Downgrades should only be accepted when explicitly allowed.

## Apply and Rollback

Replacement is performed by `autoupdater_apply`:

1. Wait for the main process to exit.
2. Acquire the installation update lock.
3. Back up files that will be replaced or removed.
4. Copy staged files into the installation directory.
5. Verify installed files by SHA-256.
6. Roll back backups on failure.

Callers should invoke `markCurrentVersionHealthy()` after the new version starts successfully so the previous version can be considered healthy.

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
