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
