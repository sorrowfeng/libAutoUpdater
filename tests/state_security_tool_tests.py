#!/usr/bin/env python3
"""Regression tests for the offline state-security evidence inspector."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path, PurePosixPath, PureWindowsPath


STATE_SECURITY = Path()
WORK_DIR = Path()


def target_platform() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


class StateSecurityToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)
        self.install_dir = self.root / "install"
        self.state_root = self.install_dir / ".autoupdater"
        self.state_root.mkdir(parents=True)
        self.permission_bundle = self.root / "permissions.txt"
        self.raw_permission_capture = self.root / "permissions.raw.txt"
        self.raw_permission_capture.write_bytes(
            b"numeric platform owner, ACL, and ancestor evidence\n"
        )
        self.sample_bytes = b"https://updates.example.test/objects/release\n"
        self.artifact_paths = {
            "primary": self.state_root / "state.json",
            "lastKnownGood": self.state_root / "state.json.lkg",
            "resume": self.state_root / "state.json.resume",
        }
        for role, path in self.artifact_paths.items():
            path.write_text(
                json.dumps(
                    {
                        "role": role,
                        "resource": "https://updates.example.test/objects/release",
                    }
                ),
                encoding="utf-8",
            )

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def path_observation(self, path: Path) -> dict[str, object]:
        return {
            "path": str(path.resolve()),
            "exists": True,
            "ownerTrusted": True,
            "aclComplete": True,
            "lessTrustedReadable": False,
            "lessTrustedWritable": False,
            "unexpectedLink": False,
        }

    def evidence(self) -> dict[str, object]:
        artifacts = []
        for role, path in self.artifact_paths.items():
            artifacts.append(
                {
                    "role": role,
                    **self.path_observation(path),
                    "contentSha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                }
            )
        return {
            "schemaVersion": 1,
            "deployment": {
                "id": "official-desktop-profile",
                "environment": "production",
                "platform": target_platform(),
                "installDir": str(self.install_dir.resolve()),
                "capturedAt": "2026-07-21T10:00:00Z",
                "validUntil": "2026-07-22T10:00:00Z",
                "configurationSha256": "a" * 64,
            },
            "storage": {
                "kind": "bundled-json",
                "permissionEvidenceSha256": None,
                "permissionEvidenceComplete": True,
                "root": self.path_observation(self.state_root),
                "artifacts": artifacts,
                "customInspection": {
                    "complete": None,
                    "credentialUrlsStored": None,
                    "accessControlVerified": None,
                },
            },
            "urlSampling": {
                "credentialMode": "none",
                "transportMode": "network",
                "credentialQueryNames": ["token", "x-amz-signature"],
                "complete": True,
                "sampleCount": 1,
                "sampleSetSha256": hashlib.sha256(self.sample_bytes).hexdigest(),
                "shortLivedAndScoped": None,
                "telemetryRedactionVerified": True,
            },
            "attestation": {
                "productionSnapshot": True,
                "reviewedBy": "deployment-owner@example.test",
                "reviewedAt": "2026-07-21T10:05:00Z",
            },
        }

    def write_permission_bundle(self, evidence: dict[str, object]) -> None:
        platform = evidence["deployment"]["platform"]
        path_type = PureWindowsPath if platform == "windows" else PurePosixPath

        def digest_path(value: str) -> str:
            normalized = str(path_type(value))
            if platform == "windows":
                normalized = normalized.casefold()
            return hashlib.sha256(normalized.encode("utf-8")).hexdigest()

        entries = []
        storage = evidence["storage"]
        if storage["kind"] == "bundled-json":
            for role, value in [
                ("root", storage["root"]),
                *((item["role"], item) for item in storage["artifacts"]),
            ]:
                entries.append(
                    {
                        "role": role,
                        "pathSha256": digest_path(value["path"]),
                        **{
                            name: value[name]
                            for name in (
                                "exists",
                                "ownerTrusted",
                                "aclComplete",
                                "lessTrustedReadable",
                                "lessTrustedWritable",
                                "unexpectedLink",
                            )
                        },
                    }
                )
            custom_store = None
        else:
            custom_store = dict(storage["customInspection"])
        bundle = {
            "schemaVersion": 1,
            "deployment": {
                "id": evidence["deployment"]["id"],
                "platform": platform,
                "installDirSha256": digest_path(
                    evidence["deployment"]["installDir"]
                ),
                "capturedAt": evidence["deployment"]["capturedAt"],
                "configurationSha256": evidence["deployment"][
                    "configurationSha256"
                ],
                "storageKind": storage["kind"],
            },
            "collector": {"name": "platform-acl-collector", "version": "1.0"},
            "complete": storage["permissionEvidenceComplete"],
            "rawEvidenceSha256": hashlib.sha256(
                self.raw_permission_capture.read_bytes()
            ).hexdigest(),
            "entries": entries,
            "customStore": custom_store,
        }
        self.permission_bundle.write_text(json.dumps(bundle), encoding="utf-8")
        storage["permissionEvidenceSha256"] = hashlib.sha256(
            self.permission_bundle.read_bytes()
        ).hexdigest()

    def run_evidence(
        self,
        evidence: dict[str, object],
        *,
        samples: bytes | None = None,
        permission_bundle: bool = True,
        regenerate_permissions: bool = True,
        evaluated_at: str = "2026-07-21T11:00:00Z",
    ) -> subprocess.CompletedProcess[str]:
        if regenerate_permissions:
            self.write_permission_bundle(evidence)
        evidence_path = self.root / "evidence.json"
        evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
        command = [
            sys.executable,
            str(STATE_SECURITY),
            str(evidence_path),
            "--evaluated-at",
            evaluated_at,
        ]
        if permission_bundle:
            command.extend(
                (
                    "--permission-evidence",
                    str(self.permission_bundle),
                    "--raw-permission-evidence",
                    str(self.raw_permission_capture),
                )
            )
        if samples is not None:
            command.append("--url-samples-stdin")
        return subprocess.run(
            command,
            input=samples,
            capture_output=True,
            check=False,
        )

    def refresh_artifact_digest(
        self, evidence: dict[str, object], role: str
    ) -> None:
        path = self.artifact_paths[role]
        for artifact in evidence["storage"]["artifacts"]:
            if artifact["role"] == role:
                artifact["contentSha256"] = hashlib.sha256(path.read_bytes()).hexdigest()

    def test_clean_bound_state_and_samples_pass(self) -> None:
        result = self.run_evidence(self.evidence(), samples=self.sample_bytes)
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_legacy_url_key_with_credential_fails_without_echoing_secret(self) -> None:
        secret = "sentinel-secret-value"
        self.artifact_paths["primary"].write_text(
            json.dumps(
                {
                    "downloadResume": {
                        f"https://updates.example.test/object?token={secret}": {
                            "bytes": 10
                        }
                    }
                }
            ),
            encoding="utf-8",
        )
        evidence = self.evidence()
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"credential-bearing URL is persisted", result.stdout)
        self.assertNotIn(secret.encode(), result.stdout + result.stderr)

    def test_unknown_query_is_open_and_insecure_permissions_fail(self) -> None:
        self.artifact_paths["resume"].write_text(
            json.dumps({"url": "https://updates.example.test/object?campaign=summer"}),
            encoding="utf-8",
        )
        evidence = self.evidence()
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"needs credential classification", result.stdout)

        evidence = self.evidence()
        evidence["storage"]["root"]["lessTrustedReadable"] = True
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 1)

    def test_signed_url_samples_are_stdin_only_and_bound(self) -> None:
        secret = b"sample-secret-sentinel"
        samples = (
            b"https://updates.example.test/object?X-Amz-Signature=" + secret + b"\n"
        )
        evidence = self.evidence()
        evidence["urlSampling"].update(
            {
                "credentialMode": "short-lived-signed",
                "sampleSetSha256": hashlib.sha256(samples).hexdigest(),
                "shortLivedAndScoped": True,
            }
        )
        result = self.run_evidence(evidence, samples=samples)
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertNotIn(secret, result.stdout + result.stderr)

        evidence["urlSampling"]["sampleSetSha256"] = "0" * 64
        result = self.run_evidence(evidence, samples=samples)
        self.assertEqual(result.returncode, 1)

        userinfo_secret = b"userinfo-secret-sentinel"
        userinfo = b"https://user:" + userinfo_secret + b"@updates.example.test/object\n"
        evidence = self.evidence()
        evidence["urlSampling"].update(
            {
                "sampleSetSha256": hashlib.sha256(userinfo).hexdigest(),
            }
        )
        result = self.run_evidence(evidence, samples=userinfo)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"userinfo credentials are forbidden", result.stdout)
        self.assertNotIn(userinfo_secret, result.stdout + result.stderr)

        mixed = (
            b"https://updates.example.test/object?X-Amz-Signature="
            + secret
            + b"&opaqueBearer=unclassified-secret\n"
        )
        evidence = self.evidence()
        evidence["urlSampling"].update(
            {
                "credentialMode": "short-lived-signed",
                "sampleCount": 1,
                "sampleSetSha256": hashlib.sha256(mixed).hexdigest(),
                "shortLivedAndScoped": True,
            }
        )
        result = self.run_evidence(evidence, samples=mixed)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"need credential classification", result.stdout)
        self.assertNotIn(b"unclassified-secret", result.stdout + result.stderr)

    def test_missing_or_stale_evidence_remains_open(self) -> None:
        result = self.run_evidence(
            self.evidence(),
            samples=None,
            permission_bundle=False,
            evaluated_at="2026-07-23T00:00:00Z",
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

    def test_custom_store_requires_explicit_content_and_access_review(self) -> None:
        evidence = self.evidence()
        evidence["storage"].update(
            {
                "kind": "custom",
                "root": None,
                "artifacts": [],
                "customInspection": {
                    "complete": True,
                    "credentialUrlsStored": False,
                    "accessControlVerified": True,
                },
            }
        )
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)

        evidence["storage"]["customInspection"]["credentialUrlsStored"] = True
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 1)

    def test_artifact_roles_are_bound_to_fixed_state_paths(self) -> None:
        clean_substitute = self.root / "clean-substitute.json"
        clean_substitute.write_text("{}", encoding="utf-8")
        evidence = self.evidence()
        evidence["storage"]["artifacts"][0]["path"] = str(
            clean_substitute.resolve()
        )
        evidence["storage"]["artifacts"][0]["contentSha256"] = hashlib.sha256(
            clean_substitute.read_bytes()
        ).hexdigest()
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 3)

    def test_complete_network_sampling_cannot_omit_or_empty_the_sample_set(self) -> None:
        evidence = self.evidence()
        evidence["urlSampling"].update(
            {
                "sampleCount": 0,
                "sampleSetSha256": hashlib.sha256(b"").hexdigest(),
            }
        )
        result = self.run_evidence(evidence, samples=b"")
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"network deployment has no validated URL sample", result.stdout)

        result = self.run_evidence(evidence, samples=None)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"not supplied on stdin", result.stdout)

    def test_permission_decisions_must_match_the_bound_structured_bundle(self) -> None:
        evidence = self.evidence()
        self.write_permission_bundle(evidence)
        evidence["storage"]["root"]["lessTrustedReadable"] = True
        result = self.run_evidence(
            evidence,
            samples=self.sample_bytes,
            regenerate_permissions=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"not bound to the evidence bundle", result.stdout)

        self.raw_permission_capture.write_bytes(b"tampered raw ACL capture\n")
        evidence = self.evidence()
        self.write_permission_bundle(evidence)
        self.raw_permission_capture.write_bytes(b"changed after binding\n")
        result = self.run_evidence(
            evidence,
            samples=self.sample_bytes,
            regenerate_permissions=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"raw platform permission capture digest", result.stdout)

    def test_bundled_state_requires_the_declared_host_platform(self) -> None:
        evidence = self.evidence()
        if target_platform() == "windows":
            other_platform = "linux"
            install_dir = "/opt/example-app"
            separator = "/"
        else:
            other_platform = "windows"
            install_dir = "C:/Program Files/Example"
            separator = "/"
        evidence["deployment"]["platform"] = other_platform
        evidence["deployment"]["installDir"] = install_dir
        state_root = install_dir + separator + ".autoupdater"
        evidence["storage"]["root"]["path"] = state_root
        names = {
            "primary": "state.json",
            "lastKnownGood": "state.json.lkg",
            "resume": "state.json.resume",
        }
        for artifact in evidence["storage"]["artifacts"]:
            artifact["path"] = state_root + separator + names[artifact["role"]]
        result = self.run_evidence(evidence, samples=self.sample_bytes)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"different host platform", result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-security", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    global STATE_SECURITY, WORK_DIR
    STATE_SECURITY = args.state_security.resolve()
    WORK_DIR = args.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(StateSecurityToolTests)
    )
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
