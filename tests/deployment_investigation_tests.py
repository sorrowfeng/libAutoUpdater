#!/usr/bin/env python3
"""Regression tests for offline deployment-investigation evidence tools."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


PRIVILEGE_BOUNDARY = Path()
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--privilege-boundary", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    global PRIVILEGE_BOUNDARY, WORK_DIR
    PRIVILEGE_BOUNDARY = args.privilege_boundary.resolve()
    WORK_DIR = args.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(PrivilegeBoundaryTests)
    )
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
