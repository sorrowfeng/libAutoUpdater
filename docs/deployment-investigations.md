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
