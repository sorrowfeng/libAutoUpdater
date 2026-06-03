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

## Release Tag

```sh
git tag -a vX.Y.Z -m "libAutoUpdater vX.Y.Z"
git push origin vX.Y.Z
```

The GitHub release workflow:

1. Builds Release on Linux, macOS, and Windows.
2. Runs `cmake --install`.
3. Packages the install tree as ZIP.
4. Generates SPDX SBOM files.
5. Extracts release notes from `CHANGELOG.md`.
6. Creates or updates the GitHub Release.

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
- SBOM files exist.
- CI, CodeQL, and the Release workflow passed.
