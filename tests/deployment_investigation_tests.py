#!/usr/bin/env python3
"""Regression tests for offline deployment-investigation evidence tools."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


PRIVILEGE_BOUNDARY = Path()
FEED_RETENTION = Path()
WORK_DIR = Path()


class PrivilegeBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def evidence(self) -> dict[str, object]:
        roles = (
            "installRoot",
            "tempRoot",
            "stateFile",
            "applyPlan",
            "journalRoot",
            "backupRoot",
            "helperExecutable",
        )
        return {
            "schemaVersion": 1,
            "deployment": {
                "id": "official-windows-x64",
                "environment": "production",
                "platform": "windows",
                "capturedAt": "2026-07-21T10:00:00Z",
                "configurationSha256": "a" * 64,
            },
            "principals": {
                "application": "application-user",
                "helper": "application-user",
                "applicationPrivilege": "standard",
                "helperPrivilege": "standard",
                "sameCredential": True,
                "lessTrusted": ["ordinary-local-users"],
            },
            "paths": [
                {
                    "role": role,
                    "path": f"C:/Program Files/Example/{role}",
                    "exists": True,
                    "ownerTrusted": True,
                    "aclComplete": True,
                    "lessTrustedWritable": False,
                    "lessTrustedReplaceable": False,
                    "unexpectedLink": False,
                }
                for role in roles
            ],
            "launch": {
                "kind": "stock",
                "authenticatedChannel": None,
                "trustedRootsBound": None,
                "ownerAclChecked": None,
                "oneTimeNonce": None,
                "intentAndDigestBound": None,
                "releaseAuthorizationVerified": None,
                "brokerOnlyPlanPublication": None,
            },
            "restart": {"policy": "disabled", "privilege": "same"},
            "stateStore": {
                "kind": "bundled-json",
                "accessControlVerified": True,
                "atomicCasVerified": True,
                "crashDurabilityVerified": True,
            },
            "attestation": {
                "productionSnapshot": True,
                "reviewedBy": "deployment-owner@example.test",
                "reviewedAt": "2026-07-21T10:10:00Z",
            },
        }

    def run_evidence(
        self, evidence: dict[str, object]
    ) -> subprocess.CompletedProcess[str]:
        path = self.root / "evidence.json"
        path.write_text(json.dumps(evidence), encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(PRIVILEGE_BOUNDARY), str(path)],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_complete_same_privilege_production_evidence_passes(self) -> None:
        result = self.run_evidence(self.evidence())
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_fixture_or_unknown_observation_remains_open(self) -> None:
        evidence = self.evidence()
        evidence["attestation"]["productionSnapshot"] = False
        evidence["paths"][0]["aclComplete"] = None
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

    def test_missing_ephemeral_path_is_incomplete_not_a_violation(self) -> None:
        evidence = self.evidence()
        evidence["paths"][3]["exists"] = False
        evidence["paths"][3]["aclComplete"] = False
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

    def test_lower_trust_write_fails_without_disclosing_the_path(self) -> None:
        evidence = self.evidence()
        evidence["paths"][1]["lessTrustedWritable"] = True
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(
            any("Program Files" in item["reason"] for item in report["checks"])
        )

    def test_stock_launch_requires_same_credential_and_cannot_elevate(self) -> None:
        evidence = self.evidence()
        evidence["principals"]["sameCredential"] = False
        evidence["principals"]["helper"] = "different-user"
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 2)

        evidence["principals"]["helperPrivilege"] = "elevated"
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 1)

    def test_privileged_restart_requires_a_protected_executable(self) -> None:
        evidence = self.evidence()
        evidence["principals"]["applicationPrivilege"] = "elevated"
        evidence["principals"]["helperPrivilege"] = "elevated"
        evidence["restart"] = {"policy": "allowlist", "privilege": "retained"}
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 3)

        restart = dict(evidence["paths"][0])
        restart["role"] = "restartExecutable"
        restart["path"] = "C:/Program Files/Example/app.exe"
        evidence["paths"].append(restart)
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)

    def test_explicit_state_store_control_failure_is_not_ignored(self) -> None:
        evidence = self.evidence()
        evidence["stateStore"]["crashDurabilityVerified"] = False
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 1)

        evidence["stateStore"]["kind"] = "custom"
        evidence["paths"] = [
            item for item in evidence["paths"] if item["role"] != "stateFile"
        ]
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 1)

    def test_broker_requires_every_authorization_control(self) -> None:
        evidence = self.evidence()
        evidence["launch"]["kind"] = "authenticated-broker"
        for key in tuple(evidence["launch"])[1:]:
            evidence["launch"][key] = True
        evidence["launch"]["releaseAuthorizationVerified"] = False
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 1)
        self.assertIn("releaseAuthorizationVerified", result.stdout)

    def test_schema_is_strict_and_rejects_duplicate_keys(self) -> None:
        path = self.root / "duplicate.json"
        path.write_text(
            '{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8"
        )
        result = subprocess.run(
            [sys.executable, str(PRIVILEGE_BOUNDARY), str(path)],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 3)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

    def test_review_cannot_predate_capture(self) -> None:
        evidence = self.evidence()
        evidence["attestation"]["reviewedAt"] = "2026-07-21T09:59:59Z"
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 3)

    def test_target_paths_must_be_absolute(self) -> None:
        evidence = self.evidence()
        evidence["paths"][0]["path"] = "relative/install-root"
        result = self.run_evidence(evidence)
        self.assertEqual(result.returncode, 3)


class FeedRetentionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def policy(self) -> dict[str, object]:
        return {
            "schemaVersion": 1,
            "policyId": "official-stable-v1",
            "canonicalTimestampProfile": "libAutoUpdater-rfc3339-nanoseconds-v1",
            "maxManifestLifetimeSeconds": 604800,
            "maxDownloadableAgeSeconds": 2592000,
            "maxIndexAgeSeconds": 86400,
            "maxSnapshotAgeSeconds": 86400,
            "minimumDownloadableReleaseCount": 1,
            "expiredMetadataMustBeUnavailable": True,
            "obsoleteIndexesMustBeUnavailable": True,
            "approvedBy": "release-owner@example.test",
            "approvedAt": "2026-07-01T00:00:00Z",
        }

    def snapshot(self, policy_digest: str) -> dict[str, object]:
        return {
            "schemaVersion": 1,
            "deployment": "official-static-feed",
            "capturedAt": "2026-07-21T10:00:00Z",
            "policyId": "official-stable-v1",
            "policySha256": policy_digest,
            "inventoryComplete": True,
            "collector": {"name": "hosting-inventory", "version": "1.0"},
            "client": {
                "configurationSha256": "b" * 64,
                "rejectExpiredManifest": True,
                "routingMode": "index",
            },
            "releases": [
                {
                    "resourceId": "release-sha256-001",
                    "version": "1.2.3",
                    "releaseId": "release-123",
                    "publishedAt": "2026-07-20T10:00:00Z",
                    "expiresAt": "2026-07-27T10:00:00Z",
                    "manifestStatus": "available",
                    "signatureStatus": "available",
                }
            ],
            "indexes": [
                {
                    "resourceId": "index-sha256-current",
                    "generatedAt": "2026-07-21T09:00:00Z",
                    "current": True,
                    "indexStatus": "available",
                    "signatureStatus": "available",
                }
            ],
            "attestation": {
                "productionSnapshot": True,
                "reviewedBy": "hosting-owner@example.test",
                "reviewedAt": "2026-07-21T10:05:00Z",
            },
        }

    def run_evidence(
        self,
        *,
        mutate_policy=None,
        mutate_snapshot=None,
        evaluated_at: str = "2026-07-21T11:00:00Z",
    ) -> subprocess.CompletedProcess[str]:
        policy = self.policy()
        if mutate_policy is not None:
            mutate_policy(policy)
        policy_path = self.root / "policy.json"
        policy_path.write_text(json.dumps(policy, sort_keys=True), encoding="utf-8")
        digest = hashlib.sha256(policy_path.read_bytes()).hexdigest()
        snapshot = self.snapshot(digest)
        if mutate_snapshot is not None:
            mutate_snapshot(snapshot)
        snapshot_path = self.root / "snapshot.json"
        snapshot_path.write_text(json.dumps(snapshot), encoding="utf-8")
        return subprocess.run(
            [
                sys.executable,
                str(FEED_RETENTION),
                "--policy",
                str(policy_path),
                "--snapshot",
                str(snapshot_path),
                "--evaluated-at",
                evaluated_at,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_complete_fresh_production_snapshot_passes(self) -> None:
        result = self.run_evidence()
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_expired_or_mismatched_signed_metadata_fails(self) -> None:
        result = self.run_evidence(
            mutate_snapshot=lambda value: value["releases"][0].update(
                {"expiresAt": "2026-07-21T10:00:00Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("expired signed metadata remains available", result.stdout)

        result = self.run_evidence(
            mutate_snapshot=lambda value: value["releases"][0].update(
                {"signatureStatus": "unavailable"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("availability differ", result.stdout)

    def test_unknown_or_incomplete_inventory_remains_open(self) -> None:
        def mutate(value):
            value["inventoryComplete"] = False
            value["attestation"]["productionSnapshot"] = False
            value["releases"][0]["manifestStatus"] = "unknown"

        result = self.run_evidence(mutate_snapshot=mutate)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

    def test_known_unavailable_half_cannot_satisfy_minimum_retention(self) -> None:
        def mutate(value):
            value["releases"][0]["manifestStatus"] = "unknown"
            value["releases"][0]["signatureStatus"] = "unavailable"

        result = self.run_evidence(mutate_snapshot=mutate)
        self.assertEqual(result.returncode, 1)
        self.assertIn("too few signed releases", result.stdout)

    def test_policy_digest_and_bounded_lifetime_are_enforced(self) -> None:
        result = self.run_evidence(
            mutate_snapshot=lambda value: value.update({"policySha256": "0" * 64})
        )
        self.assertEqual(result.returncode, 1)

        result = self.run_evidence(
            mutate_snapshot=lambda value: value["releases"][0].update(
                {"expiresAt": "2026-08-20T10:00:00Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("lifetime exceeds policy", result.stdout)

    def test_old_signed_indexes_and_disabled_client_expiry_fail(self) -> None:
        def add_old_index(value):
            value["indexes"].append(
                {
                    "resourceId": "index-sha256-obsolete",
                    "generatedAt": "2026-07-19T09:00:00Z",
                    "current": False,
                    "indexStatus": "available",
                    "signatureStatus": "available",
                }
            )

        result = self.run_evidence(mutate_snapshot=add_old_index)
        self.assertEqual(result.returncode, 1)
        self.assertIn("obsolete signed index remains available", result.stdout)

        result = self.run_evidence(
            mutate_snapshot=lambda value: value["client"].update(
                {"rejectExpiredManifest": False}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("client permits expired", result.stdout)

    def test_future_snapshot_fails_and_stale_snapshot_remains_open(self) -> None:
        result = self.run_evidence(evaluated_at="2026-07-21T09:59:59Z")
        self.assertEqual(result.returncode, 1)
        self.assertIn("after evaluation time", result.stdout)

        result = self.run_evidence(evaluated_at="2026-07-23T10:00:01Z")
        self.assertEqual(result.returncode, 2)
        self.assertIn("older than policy allows", result.stdout)

    def test_boundaries_crossed_after_capture_require_a_new_snapshot(self) -> None:
        result = self.run_evidence(
            mutate_snapshot=lambda value: value["releases"][0].update(
                {"expiresAt": "2026-07-21T10:30:00Z"}
            )
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("crossed its expiry boundary", result.stdout)

        result = self.run_evidence(
            mutate_policy=lambda value: value.update(
                {"maxDownloadableAgeSeconds": 86400}
            ),
            mutate_snapshot=lambda value: value["releases"][0].update(
                {"publishedAt": "2026-07-20T10:30:00Z"}
            ),
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("manifest crossed its maximum age", result.stdout)

        result = self.run_evidence(
            mutate_snapshot=lambda value: value["indexes"][0].update(
                {"generatedAt": "2026-07-20T10:30:00Z"}
            )
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("current index crossed its maximum age", result.stdout)

    def test_policy_approval_must_precede_capture_and_evaluation(self) -> None:
        result = self.run_evidence(
            mutate_policy=lambda value: value.update(
                {"approvedAt": "2099-01-01T00:00:00Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("approval is after evaluation", result.stdout)

        result = self.run_evidence(
            mutate_policy=lambda value: value.update(
                {"approvedAt": "2026-07-21T10:30:00Z"}
            )
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("approved after snapshot capture", result.stdout)

    def test_expiry_boundary_uses_nanoseconds_and_offsets(self) -> None:
        result = self.run_evidence(
            mutate_snapshot=lambda value: value["releases"][0].update(
                {
                    "publishedAt": "2026-07-20T18:00:00.000000001+08:00",
                    "expiresAt": "2026-07-21T18:00:00+08:00",
                }
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("expired signed metadata remains available", result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--privilege-boundary", type=Path, required=True)
    parser.add_argument("--feed-retention", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    global PRIVILEGE_BOUNDARY, FEED_RETENTION, WORK_DIR
    PRIVILEGE_BOUNDARY = args.privilege_boundary.resolve()
    FEED_RETENTION = args.feed_retention.resolve()
    WORK_DIR = args.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    suite = unittest.TestSuite()
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(PrivilegeBoundaryTests))
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(FeedRetentionTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
