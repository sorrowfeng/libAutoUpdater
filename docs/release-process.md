# Release Process

## Versioning

The project uses SemVer.

Before release, keep these values in sync:

- `project(... VERSION X.Y.Z)` in `CMakeLists.txt`.
- `version-string` in `vcpkg.json`.
- The `## vX.Y.Z` section in `CHANGELOG.md`.

Preflight check:

```sh
python tools/check_release_ready.py --tag vX.Y.Z
```

## Local Checks

```sh
git diff --check
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

## Repository Release Controls

The workflow assumes these GitHub repository controls are configured. The
workflow file cannot create or verify these settings by itself:

- Protect `main` with a ruleset that requires pull requests, at least one
  approval, and successful CI and CodeQL checks before merge.
- Protect `v*` tags with a ruleset that restricts creation to release
  maintainers and prevents tag updates or deletion.
- Configure the `release` environment with required reviewers. The publishing
  job targets this environment and cannot publish until an authorized reviewer
  approves it.

Keep workflow Actions pinned to full commit SHAs. Dependabot is configured to
propose reviewed GitHub Actions updates.

## Release Tag

Wait for the `main` push CI on the exact release commit to finish successfully
before creating the tag. The release workflow fails closed when it cannot find
that exact successful run or when the tag does not resolve to a commit on
`main`.

```sh
git tag -a vX.Y.Z -m "libAutoUpdater vX.Y.Z"
git push origin vX.Y.Z
```

The GitHub release workflow:

1. Verifies the tag, version metadata, `main` ancestry, and successful CI for
   the exact tagged commit.
2. Builds and tests the Release configuration on Linux, macOS, and Windows.
3. Runs `cmake --install` and validates the installed package contract.
4. Packages the install tree as ZIP.
5. Generates SPDX SBOM files containing installed-file hashes, selected HTTP
   and crypto dependency components, relationships, platform, source commit,
   and unique workflow-generation identity.
6. Extracts release notes from `CHANGELOG.md`.
7. Waits for approval through the `release` environment and publishes the
   GitHub Release from the only job with `contents: write`.

## Release Artifacts

Each platform should include:

- `libAutoUpdater-vX.Y.Z-Linux.zip`
- `libAutoUpdater-vX.Y.Z-macOS.zip`
- `libAutoUpdater-vX.Y.Z-Windows.zip`
- Matching `.spdx.json` files

## Post-release

After publishing, verify:

- The Release page is not a draft.
- All three platform ZIPs exist.
- Each platform SBOM exists and contains the expected dependency components.
- The Release workflow passed. Its source gate independently verified exact
  `main` CI; the repository ruleset required CodeQL before that commit merged.
