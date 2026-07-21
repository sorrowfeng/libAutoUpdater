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
