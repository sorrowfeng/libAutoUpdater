# Deployment Investigation Evidence

The repository provides offline evidence validators for the deployment-dependent
items in the audit backlog. These validators do not contact production systems,
package registries, signing services, or advisory databases. A green repository
test proves only that the evidence contract and decision rules work.

For investigation items, a checked backlog item means that the repository-side
procedure, validator, and regression tests are complete. It does **not** mean a
production deployment has passed. The deployment status remains `OPEN` until a
snapshot from each real release profile is complete and signed off by its owner.

Validator exit codes are stable: `0` is `PASS`, `1` is `FAIL`, `2` is `OPEN`,
and `3` means malformed evidence (also not a pass). Reports intentionally omit
raw path values and credential material. Store source evidence in a protected
location: it can contain internal paths, principals, ACLs, and configuration
details.

## RISK-001: apply-plan and state privilege boundaries

Status: **OPEN — no production snapshot is committed to this repository.**

Copy `deployment-evidence/risk-001.example.json` outside the source tree,
replace every fixture value with observations from the target deployment, and
run:

```text
python tools/check_privilege_boundary.py <protected-evidence.json>
```

Capture every ancestor that can replace a protected path when determining
`lessTrustedReplaceable`; checking only the leaf mode is insufficient. On
Windows, record owner SIDs and numeric DACL rights with `Get-Acl`. On Linux,
record numeric `stat` data and, when present, `getfacl` data for each ancestor.
On macOS, also retain `ls -ldeO@` output. Network-share ACLs, integrity levels,
dynamic group membership, mount options, sandbox/authorization policy, and
post-capture changes require separate review.

The stock launcher is safe only as a same-credential launch. A genuinely
privileged broker must authenticate the caller, bind trusted roots and a
single-use nonce, verify ownership/ACLs, bind intent and digest, independently
authorize signed release content, publish the plan into a broker-only root,
and bound restart behavior. A caller-supplied digest is integrity evidence, not
authorization.

## RISK-002: expiry and old-release replay

Status: **OPEN — the production retention policy and hosting inventory have not
been supplied.**

Start with `deployment-evidence/risk-002-policy.example.json` and
`deployment-evidence/risk-002-snapshot.example.json`. The snapshot must contain
an opaque identifier rather than a URL for each resource, must cover both the
manifest and its detached signature, and must be a complete point-in-time
inventory of the real static host or CDN. Bind it to the exact policy bytes by
placing the policy file's lowercase SHA-256 in `policySha256`, then run:

```text
python tools/check_feed_retention.py --policy <policy.json> --snapshot <snapshot.json> \
  --evaluated-at <trusted-current-rfc3339-time>
```

The validator uses the same nanosecond-capable RFC 3339 profile as the runtime.
`expiresAt` is exclusive, so metadata available at the exact expiry instant is
a blocker. It also rejects manifest/signature availability mismatches, policy
lifetime overruns, future publication times, and complete inventories that
retain too few usable releases. `unknown` observations and incomplete
inventories remain `OPEN`. The snapshot also binds the effective client
configuration, including `rejectExpiredManifest` and whether it uses an index.
When index routing is active, exactly one fresh current `index.json`/signature
pair must be available and every obsolete signed index must be unavailable.
Snapshots older than `maxSnapshotAgeSeconds` become `OPEN`; future captures are
rejected as deployment blockers. A policy approved after capture, or an expiry
or maximum-age boundary crossed between capture and `--evaluated-at`, also
returns `OPEN` and requires a fresh hosting snapshot.

This is a hosting snapshot, not a total anti-replay proof. Local clock rollback,
cache behavior between the collector and clients, a stronger freshness service,
and changes after capture still require deployment review.

## RISK-003: state URL credentials and permissions

Status: **OPEN — production state files, ACL evidence, and URL samples have not
been supplied.**

Use `deployment-evidence/risk-003.example.json` for each deployed platform and
profile. Capture the state directory plus the fixed `state.json`,
`state.json.lkg`, and `state.json.resume` children; bind every existing file
with its SHA-256. The inspector rejects substitute paths outside this exact
contract. Put raw owner, mode, DACL, and ancestor output in a protected capture.
Normalize its effective-access conclusions with
`deployment-evidence/risk-003-permissions.example.json`. The main snapshot
binds that structured bundle, and the structured bundle binds the raw capture.

Supply raw URL samples only on standard input so credentials never enter the
process command line:

```text
python tools/inspect_state_security.py <evidence.json> \
  --permission-evidence <structured-permission-evidence.json> \
  --raw-permission-evidence <protected-raw-platform-output> \
  --url-samples-stdin \
  --evaluated-at <trusted-current-rfc3339-time> < <protected-url-samples>
```

The inspector parses JSON with duplicate-key rejection and scans both keys and
values. This catches legacy URL-keyed resume maps as well as ordinary fields.
It reports only verdicts: raw URLs, query values, paths, ACLs, ETags, and sample
contents are never emitted. Userinfo and query names declared as credential
keys are definite blockers when persisted. Unclassified query parameters or
fragments stay `OPEN`, not `FAIL`. A complete network sample set must be
non-empty, supplied on stdin, and bound by `sampleSetSha256`; a local-only empty
set must still be supplied and hash-bound.

For Windows, capture owner SIDs and numeric DACL rights with `Get-Acl`; for
Linux, capture numeric `stat` plus `getfacl` when available; for macOS, include
`stat` and `ls -ldeO@`. Determine read, write, delete, and replace access using
the real application/helper and lower-trust identities, including ancestor and
share ACLs. The tool verifies all byte, deployment, path, and decision-field
bindings but deliberately does not reinterpret platform-specific ACL text; the
named deployment reviewer remains responsible for that effective-access
translation. A custom `IStateStore` uses `customInspection` instead of
filesystem artifacts and must attest content scanning and access controls
separately.

A `PASS` is valid only through `deployment.validUntil`. URL credentials can
also appear in network-library logs, proxies, crash dumps, and application
telemetry; the evidence therefore records telemetry redaction and requires
signed URL credentials to be short-lived and narrowly scoped.

## RISK-004: JSON signing-service parser consistency

Status: **OPEN — no signed-off production approval/signing workflow snapshot or
external-parser result set has been supplied.**

Start with `deployment-evidence/risk-004.example.json`. Hash the exact deployed
workflow definition or immutable release of its configuration into
`workflowSha256`; a label or mutable branch name is not sufficient. First
determine whether any approval, policy, transformation, or signing step parses
the manifest bytes. A workflow that never parses JSON may set
`parserUsed=false`, but it passes only when it signs the exact supplied bytes;
the parser, result digest, and parse-failure fields must then be `null`, and
`--results` must be omitted.

When a step does parse JSON, copy
`deployment-evidence/risk-004-results.example.json`, record the exact parser
implementation, version, and a hash of all behavior-affecting configuration,
then run every case in `tests/json_conformance_corpus.txt` through that deployed
parser. Corpus rows contain the expected verdict, case name, and exact JSON
bytes as lowercase hexadecimal. For each result, record whether the bytes were
accepted. Also record the decoded UTF-8 bytes for a string-valued `v` and the
exact base-10 value for an integer-valued `v`; use `null` when that semantic
value is not present. Rejected cases must not report a decoded value. Set
`complete=true` only after every corpus row ran through the named configuration.

Retain the hashed workflow and parser-configuration captures in protected
storage. Bind the result file's SHA-256 in `flow.resultsSha256`, copy the same
parser identity into both JSON files, and run:

```text
python tools/check_external_json_parser.py <workflow-evidence.json> \
  --corpus tests/json_conformance_corpus.txt \
  --workflow <protected-workflow-capture> \
  --parser-configuration <protected-parser-configuration-capture> \
  --results <protected-parser-results.json> \
  --evaluated-at <trusted-current-rfc3339-time>
```

For a no-parser workflow, omit `--parser-configuration` and `--results`. The
validator strictly parses both JSON evidence files, requires the canonical
checked-in corpus, binds every supplied capture's exact bytes, requires current
workflow and result snapshots, and compares accept/reject, Unicode decoding,
and large-integer behavior. It fails on parser/configuration, corpus, or digest
mismatches; duplicate/unknown result cases; claimed-complete coverage gaps; a
workflow that reserializes instead of signing the supplied bytes; or parse
failures that do not stop approval/signing. Missing captures, identity, results,
completeness, current validity, or production sign-off remains `OPEN`.
Repository fixtures and a result generated by a different parser are not
production evidence. Set `validUntil` to the organization's approved short
review interval, and invalidate the snapshot immediately when the workflow,
parser executable, or behavior-affecting configuration changes; the validator
does not choose that policy interval for the deployment owner.

## RISK-005: exact dependencies and authoritative advisories

Status: **OPEN — no exact production inventory, authoritative advisory export,
or signed-off component review is committed to this repository.**

The repository cannot establish production dependency safety from source
constraints alone. CMake records selected CURL and OpenSSL versions when their
package metadata exposes them, but may emit `NOASSERTION`; WinHTTP and CFNetwork
depend on the deployed OS build; the optional Qt example is outside that CMake
dependency list. Conan uses version ranges, Homebrew resolves rolling formulae,
and a vcpkg baseline does not prove the installed triplet, features, or
transitive versions. GitHub Actions are pinned to full commit SHAs, but runner
images and installed system packages still require the exact build-run capture.
The release SBOM contains selected build observations, not a complete dependency
inventory or an advisory verdict.

Copy `deployment-evidence/risk-005-inventory.example.json` outside the source
tree. For every production platform and release profile, retain protected raw
captures for:

- the exact source commit and effective CMake/build configuration;
- package-manager locks and installed direct plus transitive package lists;
- runtime-linked libraries, OS build/revision, and vendor-backported packages;
- optional CURL, OpenSSL, Qt, WinHTTP, and CFNetwork applicability;
- every production-build GitHub Action at its full 40-character commit SHA;
- compiler, linker, CMake, packaging tools, and runner image identity.

List each protected capture in `captures`, bind its bytes with SHA-256, and pass
it as `--evidence <id>=<path>`. Use `EXACT` only for a fully resolved deployed
coordinate expressed as a matching package URL (purl) or CPE. The coordinate's
canonical package name and version must match the separate component fields,
and the purl package type must be valid for the declared dependency scope. Use
a complete, simple CPE 2.3 name when a CPE is supplied. Platform and native HTTP
components require a CPE whose part, vendor, and product are consistent with
the inventory's Windows, macOS, or supported Linux distribution identity; this
prevents Windows evidence from being relabeled as another platform. CPE product
families use explicit allowlists rather than name prefixes. An explicit
`target_sw` or `target_hw` must also match the deployment platform or normalized
architecture; wildcard values defer to the bound profile fields. Exact version
tokens use a conservative identifier syntax; whitespace, range
operators, comma-separated sets, wildcards, and rolling aliases such as
`latest`, `current`, or `vNext` cannot be promoted to `EXACT`. A package
baseline without installed results and missing transitive dependencies are also
`OPEN`. The validator evaluates only the parsed version segment, so an unrelated
package name containing text such as `latest` is not rejected.

Purl percent escapes must be valid UTF-8. Qualifiers use unique, sorted
lowercase `key=value` pairs, and namespace/subpath segments cannot be empty,
`.` or `..`. An `arch`/`architecture` qualifier must match the normalized
deployment architecture and cannot contain wildcard syntax. Debian `all` and
APK/RPM `noarch` are accepted as ecosystem-specific neutral values; Homebrew
`universal`/`universal2` is accepted only for x64 or arm64 macOS profiles.
The common `cpu`, `target_arch`, `target_cpu`, and `target_hw` aliases follow the
same architecture rule. Explicit `os`, `platform`, `target_os`,
`target_platform`, `target_sw`, `distro`, and `distro_name` values must use a
known platform name with an optional numeric release suffix and match the
deployment platform. A vcpkg `triplet` must bind the same architecture and
platform. APK, Debian, and RPM coordinates are
Linux-only, while Homebrew is limited to Linux and macOS. Simple CPE fields
allow only conservative unescaped identifier characters; whitespace and escaped
fields require a reviewed parser extension. Non-OS CPE component names must
match the product exactly after normalization; OS display names use only the
documented product aliases.
An exact OpenSSL, CURL, Qt, native HTTP, package-manager, or OS scope must also
contain at least one direct component whose canonical coordinate identifies the
scope's primary component. This prevents an unrelated package such as zlib or
CURL from occupying the OpenSSL scope while still allowing separately reviewed
transitive components. Windows native HTTP accepts only an application CPE for
WinHTTP, macOS accepts only CFNetwork, and the OS scope accepts OS products but
not those native API identities. Linux native HTTP remains `NOT_APPLICABLE`
unless a reviewed implementation and identity mapping are added.

The accepted Linux CPE vendors cover AlmaLinux, Alpine Linux, Amazon Linux,
Ubuntu/Canonical, CentOS, Debian, Fedora, the Linux kernel, openSUSE, Oracle
Linux, Red Hat, Rocky Linux, and SUSE. Add a reviewed validator change and
regression case before using a different vendor identity; do not relabel it as
`generic`.

The `build-configuration` capture is strict JSON containing `schemaVersion`,
`deploymentId`, `platform`, `architecture`, the same nonzero `sourceCommit`,
`complete`, and one `scopeApplicability` entry for each of the eight scopes. It
is the mandatory proof for `NOT_APPLICABLE`, and `os-runtime` plus `build-tools`
cannot be marked inapplicable. An `os-version` capture is also strict JSON with
the same profile and commit fields plus `complete` and a `components` array of
exact `{componentId, version, coordinate}` records. A complete OS capture is
compared in both directions with the exact platform components that cite it.

A `workflow-definition` capture is strict JSON with `schemaVersion`, `complete`,
and an `actions` array of `{coordinate, version}` records. Its complete Action
set is compared in both directions with the inventory. Coordinates may include
an Action subpath, such as `github/codeql-action/init@<commit>`, but every
version and coordinate must end in the same full nonzero 40-character commit;
the inventory component name must equal the coordinate's repository/subpath,
and empty, `.` and `..` path segments are rejected.
Generate this normalized capture from the exact bound workflow tree, not from a
hand-selected list.

Every exact component and exact scope must cite a capture kind capable of
establishing that scope: workflow definitions for Actions, OS/runtime evidence
for platform components, toolchain/package evidence for build tools, and
lock/package/runtime evidence for libraries. A build-configuration capture
proves applicability but cannot by itself prove an exact component version.

Next copy `deployment-evidence/risk-005-review.example.json`, hash the exact
inventory bytes into `inventorySha256`, and obtain current exports from the
authoritative upstream, platform vendor, distribution, or GitHub security
source appropriate to each component. Every source record names its authority,
binds the exact inventory digest, and explicitly lists the component IDs and
exact `{version, coordinate}` pairs it covers; a broad `upstream-security`
label alone is insufficient. `coveredComponents` entries contain
`componentId`, `version`, and `coordinate`. Remediation targets additionally
contain `advisoryId`, so changing a package ecosystem or revision invalidates
both the authority and remediation bindings.
Authority kind is derived from the exact coordinate, not only its broad scope:
APK, Homebrew, Debian, and RPM package revisions require
`distribution-security`; Windows and macOS platform CPEs require
`platform-security`; Linux distribution CPEs require `distribution-security`;
known distribution vendors in application CPEs also require
`distribution-security`; and generic upstream coordinates require
`upstream-security`. A distribution backport therefore cannot be cleared only
by an upstream version advisory.
Bind every source export as another `--evidence` item and cite it from the
component review. Record exact deployed package revisions: the validator
deliberately does not interpret OpenSSL letter releases, distribution backports,
Qt revisions, or ecosystem version ranges. That applicability decision remains
with the named reviewer.

Run the fully local check with no registry or advisory-network access:

```text
python tools/dependency_review.py \
  --inventory <protected-production-inventory.json> \
  --review <protected-advisory-review.json> \
  --evidence <capture-id>=<protected-capture> \
  --evidence <source-id>=<protected-authoritative-export> \
  --evaluated-at <trusted-current-rfc3339-time>
```

Repeat `--evidence` for every declared inventory capture, advisory source, and
verified remediation artifact. A `PASS` requires all eight scopes, at least one
exact component, complete byte-bound captures, current ecosystem-appropriate
authority sources, an exact review for every resolved component, and separate
production sign-off of the inventory and advisory review. Known affected
components with no remediation, planned remediation, or accepted risk return
`FAIL`. A verified mitigation requires a bound remediation-evidence source that
names the same inventory, component, exact version, and advisory ID; evidence
for another coordinate, component, or CVE cannot be reused. Unknown
applicability remains `OPEN`. Inventory, review, and source validity windows are
capped at seven days; raw captures more than seven days older than the inventory
are rejected. An advisory source must not predate the inventory, and a review
cannot outlive its inventory or earliest source. Profile, architecture,
hash/version/source, unpinned or omitted Action, future snapshot, and internally
contradictory completeness mismatches fail closed. Missing, incomplete, or
expired evidence remains `OPEN`;
malformed evidence returns exit code `3` and never emits raw component data,
capture paths, versions, or advisory contents.

The example files intentionally return `OPEN`. The validator does not download
or certify advisory data, parse arbitrary lock/package-list formats, prove that
a capture omitted no component, or authenticate a reviewer. Those content and
identity claims remain part of the protected, named attestation; repository CI
exercises only structural bindings and decision rules. Refresh both inventory
and advisory evidence whenever the build, runner, dependency graph, OS image,
or authority source changes, even before the seven-day limit.
