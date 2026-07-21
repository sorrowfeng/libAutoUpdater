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
4. Packages the install tree as ZIP. The workflow does not set
   `BUILD_SHARED_LIBS`, so these official packages use the default static
   `libAutoUpdater` configuration.
5. Generates SPDX SBOM files containing installed-file hashes, selected HTTP
   and crypto dependency components, relationships, platform, source commit,
   and unique workflow-generation identity.
6. Extracts release notes from `CHANGELOG.md`.
7. Waits for approval through the `release` environment and publishes the
   GitHub Release from the only job with `contents: write`.

The generated SBOM is not an advisory clearance. Before describing an
application artifact as production-ready, capture its exact direct, transitive,
system, build, and CI dependencies and run the protected offline review in
[the dependency investigation](deployment-investigations.md#risk-005-exact-dependencies-and-authoritative-advisories).
That review is intentionally not satisfied by repository fixtures or wired to
the public release job without real production evidence.

## Release Artifacts

Each platform should include:

- `libAutoUpdater-vX.Y.Z-Linux.zip`
- `libAutoUpdater-vX.Y.Z-macOS.zip`
- `libAutoUpdater-vX.Y.Z-Windows.zip`
- Matching `.spdx.json` files

Shared-library builds are supported on Windows, macOS, and Linux through
`BUILD_SHARED_LIBS=ON`, but the current workflow does not publish a separate
shared package variant.

## Application Update Feed Artifacts

The GitHub Release ZIPs above distribute this library; they are distinct from
an application's static update feed. A signed production feed publishes:

- `manifest.json` and the default detached `manifest.json.sig` for every
  release target.
- `index.json` and `index.json.sig` when multi-platform/channel routing uses an
  index.
- Every artifact referenced by the release manifest.

Run `tools/make_manifest.py` first, then sign the final bytes with
`tools/sign_manifest.py`. For an index, generate it with `tools/make_index.py`
and sign it separately. `--base-url` is the artifact base stored in a release
manifest; it is not the public manifest URL. Never place the release private key
in the published tree or reformat JSON after signing.

## Post-release

After publishing, verify:

- The Release page is not a draft.
- All three platform ZIPs exist.
- Each platform SBOM exists and contains the expected dependency components.
- The separate exact dependency/advisory review is current for the released
  platform profile, or its remaining status is explicitly recorded as `OPEN`.
- The Release workflow passed. Its source gate independently verified exact
  `main` CI; the repository ruleset required CodeQL before that commit merged.
