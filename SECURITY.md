# Security Policy

`libAutoUpdater` processes remote manifests, downloaded files, and local file replacement. It is a security-sensitive component. Please do not disclose exploitable vulnerability details in public issues.

## Supported Versions

Current support policy:

| Version | Supported |
| --- | --- |
| Latest minor / patch | Yes |
| Older versions | Best effort |

Before `1.0.0`, API and manifest details may still change. Security fixes are prioritized for the latest version.

## Reporting a Vulnerability

Please report vulnerabilities through GitHub Security Advisory, or contact the repository owner privately through the security contact listed in the owner profile.

Include as much detail as possible:

- Affected version or commit.
- Affected platform.
- Reproduction steps or a minimal proof of concept.
- Expected impact, such as arbitrary file write, downgrade attack, signature bypass, path traversal, or rollback breakage.
- Whether the issue has already been publicly disclosed.

We will acknowledge reports as soon as possible and provide a fix plan and release timeline when practical.

## Security Scope

Important security boundaries include:

- Manifest signature verification.
- HTTPS/TLS verification.
- `allowedBaseUrls` restrictions.
- Path traversal and absolute path rejection.
- SHA-256 verification after download.
- External updater backup, replacement, verification, and rollback behavior.
- Anti-downgrade and anti-replay behavior.

See [docs/security-model.md](docs/security-model.md) for details.
