# Audit Optimization Backlog

This checklist is derived from the repository audit completed on 2026-07-19. It
separates verified defects from deployment-dependent risks and optional
improvements. Items are ordered by security impact, data-loss risk, and the
likelihood of affecting production update flows.

## Status and priority

- `[ ]` Not started
- `[x]` Completed and verified
- **P0**: Address before treating the updater as production-ready.
- **P1**: Address in the next reliability and security milestone.
- **P2**: Longer-term maintainability, performance, and release hardening.
- **Investigation**: Not a confirmed vulnerability; gather the listed evidence
  before deciding whether a code or deployment change is required.

## P0: Immediate security and integrity work

- [x] **AU-001 — Remove `releaseId` from journal path construction**
  - Classification: Verified security issue; high severity; CWE-22/CWE-73.
  - Key code: `src/Manifest.cpp:90`, `src/ApplyPlanWriter.cpp:35`,
    `src/ApplyPlan.cpp:65`, `updater/ApplyExecutor.cpp:91-95,273,276`.
  - Required work:
    - Generate a random transaction identifier or use a fixed-length digest for
      journal filenames.
    - Never use the remote `releaseId` as a filesystem path component.
    - Make journal creation and writes mandatory; abort apply on failure.
    - Write through a same-directory temporary file, flush it, and atomically
      replace the final journal.
  - Completion criteria:
    - The final journal handle is always beneath the configured journal root.
    - Empty apply plans cannot create or truncate files outside that root.
    - Tests cover traversal, absolute paths, UNC paths, separators, NUL,
      overlong values, and empty operation lists.

- [x] **AU-002 — Enforce filesystem containment without following links**
  - Classification: Verified security issue; medium severity, potentially high
    when the helper runs with elevated privileges; CWE-59.
  - Key code: `src/util/PathUtil.cpp:37-85`,
    `src/LocalSnapshotBuilder.cpp:20-38`,
    `updater/ApplyExecutor.cpp:225-266`.
  - Required work:
    - Perform install, staging, and backup operations relative to trusted root
      directory handles.
    - On POSIX, use `openat`/`openat2` with no-follow and beneath-root
      constraints where available.
    - On Windows, reject unexpected reparse points and verify final paths from
      opened handles.
    - Write and verify a temporary file in the target directory before an
      atomic replacement.
  - Completion criteria:
    - Intermediate and leaf symlinks, junctions, and reparse points cannot
      escape any trusted root.
    - Link swaps between check, copy, hash, and rollback cannot redirect an
      operation.
    - Cross-platform regression tests cover source, target, and backup links.

- [x] **AU-003 — Validate the initial URL and every redirect hop**
  - Classification: Verified security issue; medium severity; CWE-918.
  - Key code: `src/ManifestFetcher.cpp:64,80-84`,
    `src/UpdatePlanner.cpp:51-52`, `src/util/UrlUtil.cpp:60-73`,
    `src/default/CurlNetworkClient.cpp:90`,
    `src/default/WinHttpNetworkClient.cpp:216-217`,
    `src/default/CfNetworkClient.cpp:249-255`.
  - Required work:
    - Apply the allowlist to the initial manifest URL.
    - Disable automatic redirects and process redirects manually with a strict
      hop limit.
    - Parse and normalize scheme, host, port, and path before every request.
    - Reject HTTPS-to-HTTP downgrade and disallowed protocol changes.
    - Keep local `file:` update behavior behind an explicit separate policy.
  - Completion criteria:
    - Cross-origin, localhost, private-address, protocol-downgrade, dot-segment,
      relative, and redirect-loop tests are present.
    - The effective URL of every network request satisfies the same policy as
      the original URL.

- [x] **AU-004 — Add hard resource budgets to all untrusted input paths**
  - Classification: Verified security issue; medium severity; CWE-400/CWE-674.
  - Key code: `include/libAutoUpdater/Config.h:19-25`,
    `src/ManifestFetcher.cpp:12-29`, `src/util/Json.cpp:33-127`,
    `src/DownloadExecutor.cpp:79-127`, and all network backends.
  - Required work:
    - Define separate maximum sizes for index, manifest, signature, artifact,
      JSON depth, node count, string length, and container size.
    - Pass the signed artifact size to the network layer as a streaming hard
      limit.
    - Check declared and actual byte counts with overflow-safe arithmetic.
    - Cancel and remove partial data immediately after a limit is exceeded.
  - Completion criteria:
    - Tests cover missing or dishonest `Content-Length`, chunked unbounded
      bodies, one-byte-over-limit responses, excessive JSON depth, and resume
      counter overflow.

- [x] **AU-005 — Make apply, journal, and rollback crash-recoverable**
  - Classification: Verified high-impact correctness and integrity defect.
  - Key code: `updater/ApplyExecutor.cpp:64-123,225-279`.
  - Required work:
    - Store operation identity, intent, backup state, completion state, and
      errors in the journal.
    - Return and propagate rollback failures instead of discarding them.
    - Use atomic replacement and appropriate file/directory durability calls.
    - Recover or safely roll back incomplete transactions on the next start.
    - Record restart failure separately from successful file installation.
  - Completion criteria:
    - Failure injection at every backup, replace, remove, journal, and rollback
      boundary produces a deterministic recoverable state.
    - Forced termination at every transaction boundary is covered by tests.

- [x] **AU-006 — Delegate public rollback to the external updater**
  - Classification: Verified high-impact architecture and stability defect.
  - Key code: `src/Updater.cpp:523-574`.
  - Required work:
    - Represent rollback as an authenticated, validated external apply plan.
    - Use the same process wait, install lock, journal, containment, hash, and
      recovery guarantees as forward apply.
    - Remove direct install-file replacement from the running main process.
  - Completion criteria:
    - `rollbackLastUpdate()` never modifies managed install files in-process.
    - Partial rollback and rollback-of-rollback failures are recoverable.

- [x] **AU-007 — Repair the updater task/state machine**
  - Classification: Verified high-impact correctness and lifecycle defect.
  - Key code: `src/Updater.cpp:133-136,217-229,315-322,407-503`,
    `src/default/DirectDispatcher.cpp:9-12`.
  - Required work:
    - Reject or queue work while busy instead of synchronously joining an old
      worker.
    - Never join the current worker thread.
    - Catch user callback exceptions at the dispatcher boundary.
    - Bind each decision, download, and apply plan to a generation identifier.
    - Clear invalid cached plans and gate apply on the current Ready-to-Apply
      state and matching generation.
    - Persist pending state successfully before announcing readiness.
  - Completion criteria:
    - Callback re-entry, thrown callbacks, cancellation, destruction, stale
      plans, and overlapping API calls have deterministic tests and results.

- [x] **AU-008 — Make state persistence atomic, locked, and fail-closed**
  - Classification: Verified high-impact correctness issue with security
    implications for anti-replay state.
  - Key code: `src/default/JsonStateStore.cpp:17-225`,
    `src/Updater.cpp:200-206,315-322,506-520`.
  - Required work:
    - Add in-process and inter-process synchronization.
    - Write a same-directory temporary state file, verify write/flush/close,
      durably sync it where required, and atomically replace the old state.
    - Preserve a last-known-good snapshot.
    - Distinguish a missing state file from a corrupt or unreadable one.
    - Propagate failures from security-critical state operations.
  - Completion criteria:
    - Crash, short-write, disk-full, permission, and concurrent-writer tests do
      not lose the last valid state.
    - Anti-downgrade decisions fail closed on corrupt existing state.

## P1: Near-term security and reliability work

- [x] **AU-009 — Bind downgrade authorization to a verified signature and local policy**
  - Classification: Verified low-severity security policy defect; CWE-345.
  - Key code: `include/libAutoUpdater/Config.h:29`,
    `src/Manifest.cpp:96-97`, `src/ManifestFetcher.cpp:17-27`,
    `src/UpdateTypes.h:48-52`, `src/UpdatePlanner.cpp:79-90`.
  - Completion criteria:
    - Remote `allowDowngrade` is ineffective unless this exact manifest has a
      valid signature and a separate local policy permits downgrade.

- [x] **AU-010 — Reject duplicate, conflicting, and reserved operations**
  - Classification: Verified medium-impact correctness defect.
  - Key code: `src/Manifest.cpp:116-165`,
    `src/ApplyPlan.cpp:97-134`, `updater/ApplyExecutor.cpp:230-247`.
  - Completion criteria:
    - Manifest parsing, planning, and apply-plan parsing reject duplicate
      targets, replace/remove conflicts, normalized path collisions, and
      `.autoupdater` internal paths.

- [x] **AU-011 — Correct platform timeout and process-launch behavior**
  - Classification: Verified high-to-medium stability defects.
  - Key code: `src/default/CfNetworkClient.cpp:249-367`,
    `src/default/ProcessLauncher.cpp:36-46,94-118`,
    `updater/ApplyExecutor.cpp:186-205`, `updater/main.cpp:68-81`.
  - Required work:
    - Make CFNetwork timeout and cancellation interrupt blocking operations.
    - Prefer `posix_spawn`, or report child setup/exec errors through a
      close-on-exec pipe and avoid unsafe post-fork C++ work.
    - Handle Windows wait/open failures explicitly.
    - Strictly parse and bound PID and timeout arguments.
    - Implement Windows argument quoting according to the CRT rules.

- [x] **AU-012 — Replace the crash-persistent directory lock**
  - Classification: Verified medium-impact availability defect.
  - Key code: `updater/ApplyExecutor.cpp:157-175`.
  - Completion criteria:
    - Prefer an OS lock released automatically on process death, or use robust
      PID plus process-start identity checks for stale lock recovery.
    - Tests cover crash, stale lock, PID reuse, and an active competing updater.

- [x] **AU-013 — Enforce a strict JSON and schema contract**
  - Classification: Verified correctness defect; parser differentials remain a
    deployment-dependent security risk.
  - Key code: `src/util/Json.cpp:68-102,130-232,272-283,367-373`,
    plus manifest, apply-plan, and state parsers.
  - Required work:
    - Reject duplicate keys and raw control characters.
    - Correctly decode Unicode escapes and surrogate pairs and validate UTF-8.
    - Represent signed, unsigned, and floating-point numbers distinctly.
    - Use locale-independent integer parsing and serialization.
    - Validate every schema field for type, range, format, and uniqueness.
  - Completion criteria:
    - RFC 8259 conformance and cross-parser corpus tests pass.
    - Negative, fractional, non-finite, over-range, and greater-than-`2^53`
      integer cases never trigger undefined behavior or precision loss.

- [x] **AU-014 — Fix timestamp, index routing, and URL construction semantics**
  - Classification: Verified correctness defects; expiry impact requires policy
    confirmation.
  - Key code: `src/UpdatePlanner.cpp:14-49`,
    `src/ManifestFetcher.cpp:42-48`, `src/util/UrlUtil.cpp:47-58`.
  - Completion criteria:
    - Parse and compare a documented RFC 3339 profile as time values.
    - Reject malformed and expired timestamps at exact boundaries.
    - Prefer exact index matches over wildcards and reject ambiguous routes.
    - Construct URLs using URI-aware path, query, and fragment handling.

- [x] **AU-015 — Report file-read and file-write failures accurately**
  - Classification: Verified medium-impact integrity and diagnostics defect.
  - Key code: `src/util/Sha256.cpp:163-183`,
    `src/default/StdFileSystem.cpp:75-84,140-145`, and network file writers.
  - Completion criteria:
    - SHA-256 distinguishes EOF from read failure.
    - All stream write, flush, close, permission, rename, and cleanup errors are
      checked and returned.
    - Atomic replacement never deletes the old target before the new file is
      ready.

- [x] **AU-016 — Put the Qt network adapter on a valid Qt thread model**
  - Classification: Verified medium-impact example/integration defect.
  - Key code: `examples/qt/QtNetworkClient.cpp:12-105`.
  - Completion criteria:
    - `QNetworkAccessManager` and replies stay on their owning Qt thread.
    - Calls cross threads through queued invocations.
    - Timeout, cancellation, TLS verification, and write failures honor the
      core network interface contract.

- [x] **AU-017 — Complete health confirmation and backup retention behavior**
  - Classification: Verified implementation/documentation mismatch.
  - Key code: `include/libAutoUpdater/Config.h:61`,
    `src/Updater.cpp:506-520`.
  - Completion criteria:
    - `healthConfirmationTimeout` has an implemented and tested state
      transition.
    - Successful health confirmation applies a documented backup cleanup or
      retention policy.
    - Repeated updates do not overwrite the only usable rollback backup.

- [x] **AU-018 — Fix release-tool manifest/signature consistency**
  - Classification: Verified release reliability defect.
  - Key code: `tools/make_manifest.py:66-79`,
    `tools/sign_manifest.py:44-58`.
  - Completion criteria:
    - Manifest generation excludes the configured signature output, including
      the default `manifest.json.sig`.
    - Custom output files cannot be included in a later manifest generation.
    - Copied content is revalidated after copy or copied from the same verified
      file handle to close the hash/copy race.

## P2: Performance, maintainability, and release hardening

- [x] **AU-019 — Remove O(n²) snapshot lookup behavior**
  - Key code: `src/UpdateTypes.h:26-33`, `src/UpdatePlanner.cpp:102`.
  - Replace linear path lookup with a normalized path-to-record map and add a
    large-manifest performance regression test.

- [x] **AU-020 — Bound and batch download-resume state**
  - Key code: `src/default/JsonStateStore.cpp:113-131,185-221`.
  - Use a credential-free stable resource identifier, prune entries by age and
    release, and avoid rewriting the complete state for every small update.

- [x] **AU-021 — Improve hashing and progress reporting**
  - Key code: `src/util/Sha256.cpp:53-61`,
    `src/default/CurlNetworkClient.cpp:188-193`.
  - Process full SHA-256 blocks directly and ensure progress callbacks receive
    a useful total while transfer is active.

- [x] **AU-022 — Add structured, non-sensitive updater diagnostics**
  - Distinguish apply failure, rollback failure, recovery failure, state
    persistence failure, and restart failure.
  - Never log signatures, private material, or credential-bearing URLs.

- [x] **AU-023 — Complete installed-package and shared-library contracts**
  - Key code: `updater/CMakeLists.txt:21-24` and package export configuration.
  - Export the documented updater target, verify `find_package()` from a clean
    consumer project, and add Windows shared-library export support or document
    that only static builds are supported.

- [x] **AU-024 — Harden CI and release provenance**
  - Require release tags to refer to an approved, tested commit.
  - Run relevant tests in or before every release job.
  - Limit `contents: write` to the publishing job.
  - Pin third-party Actions to immutable revisions where practical.
  - Produce an SBOM containing dependency components and relationships, not
    only installed files.

- [ ] **AU-025 — Define supported dependency and crypto policy**
  - Document minimum supported OpenSSL/CURL/Qt versions, accepted signature
    algorithms, minimum key strength, and key rotation behavior.
  - Add a CI job where OpenSSL is mandatory and configuration fails if it is
    unavailable.

## Test backlog

- [ ] **TEST-001 — Add updater security-boundary regression tests**
  - Cover journal traversal, URL policy, redirect downgrade, resource limits,
    symlink/junction containment, duplicate operations, reserved paths, and
    unsigned downgrade.

- [ ] **TEST-002 — Add transaction and filesystem fault injection**
  - Cover short writes, read failures, permission changes, disk full, failed
    flush/rename/remove, rollback failure, process termination, and recovery.

- [ ] **TEST-003 — Add state-machine and concurrency tests**
  - Cover callback re-entry and exceptions, overlapping API calls, generation
    mismatch, cancellation, destructor behavior, multiple instances, and
    cross-process state/lock contention.

- [ ] **TEST-004 — Add real platform-adapter tests**
  - Cover Curl, WinHTTP, CFNetwork, Qt networking, POSIX launch, Windows launch
    and wait behavior, slow servers, redirect chains, timeout, and cancellation.

- [ ] **TEST-005 — Expand parser, hash, and crypto coverage**
  - Add RFC 8259 corpora, depth and numeric boundaries, Unicode and NUL cases,
    SHA-256 block/file/error cases, Ed25519 and RSA tests, malformed Base64, and
    invalid key material.
  - Replace the current no-op OpenSSL test pass with an explicit skip or a
    mandatory crypto test configuration.

- [ ] **TEST-006 — Add sanitizer, fuzz, packaging, and install tests**
  - Run ASan/UBSan where supported.
  - Replace shallow fuzz-smoke input with persistent coverage-guided fuzzing.
  - Test installed CMake targets, static/shared consumers, generated manifests,
    signed release artifacts, and rollback after installed updates.

## Documentation backlog

- [ ] **DOC-001 — Align recovery documentation with implemented guarantees**
  - Update `docs/architecture-plan.md`, `docs/integration.md`, and
    `docs/troubleshooting.md` when journal recovery, external rollback, health
    confirmation, and backup retention behavior are finalized.

- [ ] **DOC-002 — Document production security requirements**
  - State when signatures and URL allowlists are mandatory.
  - Document state/apply-plan ownership and ACL requirements.
  - Document redirect, TLS, expiry, anti-replay, and elevated-helper policies.
  - Warn against placing long-lived credentials in download URLs.

- [ ] **DOC-003 — Correct package and release documentation**
  - Ensure documented installed targets match the generated CMake package.
  - Document supported shared/static configurations and platform backends.
  - Keep release instructions consistent with manifest and signature filenames.

## Deployment-dependent investigations

These items are not confirmed vulnerabilities and must remain labelled as
investigations until the required deployment evidence is available.

- [ ] **RISK-001 — Verify apply-plan and state privilege boundaries**
  - Gather production ACLs, temp-directory ownership, helper launch method,
    elevation behavior, and restart-command policy.
  - If a lower-privileged user can modify plan/state consumed by an elevated
    helper, bind plans to trusted roots, ownership, a one-time nonce, and an
    authenticated invocation channel.

- [ ] **RISK-002 — Verify expiry and old-release replay conditions**
  - Confirm the canonical timestamp profile, release retention policy, and
    whether old signed manifests remain downloadable.

- [ ] **RISK-003 — Verify state URL credential exposure**
  - Determine whether production URLs contain signed query parameters and
    inspect actual Windows, macOS, and Linux state-file permissions.

- [ ] **RISK-004 — Verify JSON signing-service parser consistency**
  - Determine whether any external approval or signing system parses the JSON
    before signing and compare duplicate-key and Unicode behavior.

- [ ] **RISK-005 — Perform dependency advisory review**
  - Resolve the exact production versions of OpenSSL, CURL, Qt, build Actions,
    and package-manager dependencies, then check authoritative advisories.

## Release-readiness exit gate

The updater should not be described as production-ready until all P0 items are
closed and independently verified. A release candidate should additionally
meet the following conditions:

- All P1 items affecting the selected platform and integration mode are closed.
- Security-boundary and fault-injection tests pass on every supported platform.
- The real signature-verification implementation is exercised in CI.
- A forced-crash recovery test demonstrates that no install state leaves the
  application both unstartable and non-recoverable.
- The packaged CMake targets are consumed successfully from a clean external
  project.
- Documentation accurately states the remaining limitations and required
  deployment controls.
