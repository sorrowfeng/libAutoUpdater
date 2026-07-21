#!/usr/bin/env python3
"""Regression tests for the offline dependency advisory evidence validator."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Callable


VALIDATOR = Path()
INVENTORY_EXAMPLE = Path()
REVIEW_EXAMPLE = Path()
WORK_DIR = Path()


class DependencyReviewToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)
        self.source_commit = "a" * 40
        self.os_coordinate = (
            "cpe:2.3:o:microsoft:windows_11:10.0.26100.0:*:*:*:*:*:*:*"
        )
        self.actions = [
            (
                "actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd",
                "de0fac2e4500dabe0009e67214ff5f5447ce83dd",
            ),
            (
                "actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f",
                "bbbca2ddaa5d8feaa63e36b76fdaad77386f024f",
            ),
            (
                "actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3",
                "70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3",
            ),
            (
                "github/codeql-action/init@f52b05f4acaaa234e44466e66d29050e135ea9ef",
                "f52b05f4acaaa234e44466e66d29050e135ea9ef",
            ),
            (
                "github/codeql-action/analyze@f52b05f4acaaa234e44466e66d29050e135ea9ef",
                "f52b05f4acaaa234e44466e66d29050e135ea9ef",
            ),
        ]
        build_configuration = {
            "schemaVersion": 1,
            "deploymentId": "official-windows-x64",
            "platform": "windows",
            "architecture": "x64",
            "sourceCommit": self.source_commit,
            "complete": True,
            "scopeApplicability": [
                {
                    "scope": scope,
                    "applicable": scope not in {"qt", "native-http"},
                }
                for scope in (
                    "openssl",
                    "curl",
                    "qt",
                    "native-http",
                    "github-actions",
                    "package-manager",
                    "os-runtime",
                    "build-tools",
                )
            ],
        }
        os_version = {
            "schemaVersion": 1,
            "deploymentId": "official-windows-x64",
            "platform": "windows",
            "architecture": "x64",
            "sourceCommit": self.source_commit,
            "complete": True,
            "components": [
                {
                    "componentId": "windows-runtime",
                    "version": "10.0.26100.0",
                    "coordinate": self.os_coordinate,
                }
            ],
        }
        workflow_capture = {
            "schemaVersion": 1,
            "complete": True,
            "actions": [
                {"coordinate": coordinate, "version": version}
                for coordinate, version in self.actions
            ],
        }
        self.evidence_files: dict[str, Path] = {}
        for evidence_id, contents in {
            "build-config": json.dumps(build_configuration).encode("utf-8"),
            "dependency-lock": b"exact direct and transitive package lock\n",
            "workflows": json.dumps(workflow_capture).encode("utf-8"),
            "os-build": json.dumps(os_version).encode("utf-8"),
            "toolchain": b"exact compiler and build tool inventory\n",
            "openssl-advisories": b"current OpenSSL project advisory export\n",
            "curl-advisories": b"current curl project advisory export\n",
            "vcpkg-advisories": b"current vcpkg project advisory export\n",
            "cmake-advisories": b"current CMake project advisory export\n",
            "platform-advisories": b"current platform advisory export\n",
            "github-advisories": b"current GitHub advisory export\n",
        }.items():
            path = self.root / f"{evidence_id}.capture"
            path.write_bytes(contents)
            self.evidence_files[evidence_id] = path

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def digest(self, evidence_id: str) -> str:
        return hashlib.sha256(self.evidence_files[evidence_id].read_bytes()).hexdigest()

    def capture(self, evidence_id: str, kind: str) -> dict[str, Any]:
        return {
            "id": evidence_id,
            "kind": kind,
            "sha256": self.digest(evidence_id),
            "capturedAt": "2026-07-21T09:00:00Z",
            "complete": True,
        }

    def component(
        self,
        component_id: str,
        name: str,
        scope: str,
        kind: str,
        version: str,
        coordinate: str,
        capture_id: str,
    ) -> dict[str, Any]:
        return {
            "id": component_id,
            "name": name,
            "scope": scope,
            "kind": kind,
            "direct": True,
            "version": version,
            "resolution": "EXACT",
            "coordinate": coordinate,
            "captureId": capture_id,
        }

    def inventory(self) -> dict[str, Any]:
        components = [
            self.component(
                "openssl",
                "OpenSSL",
                "openssl",
                "runtime",
                "3.0.16",
                "pkg:generic/openssl@3.0.16",
                "dependency-lock",
            ),
            self.component(
                "curl",
                "CURL",
                "curl",
                "runtime",
                "8.12.1",
                "pkg:generic/curl@8.12.1",
                "dependency-lock",
            ),
            self.component(
                "vcpkg",
                "vcpkg",
                "package-manager",
                "build",
                "2025.06.13",
                "pkg:generic/vcpkg@2025.06.13",
                "dependency-lock",
            ),
            self.component(
                "windows-runtime",
                "Windows",
                "os-runtime",
                "system",
                "10.0.26100.0",
                self.os_coordinate,
                "os-build",
            ),
            self.component(
                "cmake",
                "CMake",
                "build-tools",
                "build",
                "4.0.1",
                "pkg:generic/cmake@4.0.1",
                "toolchain",
            ),
        ]
        components.extend(
            self.component(
                "action-" + coordinate.split("@", 1)[0].replace("/", "-"),
                coordinate.split("@", 1)[0],
                "github-actions",
                "ci-action",
                version,
                coordinate,
                "workflows",
            )
            for coordinate, version in self.actions
        )
        coverage = [
            {
                "scope": "openssl",
                "status": "EXACT",
                "reason": None,
                "captureIds": ["dependency-lock"],
            },
            {
                "scope": "curl",
                "status": "EXACT",
                "reason": None,
                "captureIds": ["dependency-lock"],
            },
            {
                "scope": "qt",
                "status": "NOT_APPLICABLE",
                "reason": "Qt adapter is disabled in this release profile",
                "captureIds": ["build-config"],
            },
            {
                "scope": "native-http",
                "status": "NOT_APPLICABLE",
                "reason": "This profile uses the CURL adapter",
                "captureIds": ["build-config"],
            },
            {
                "scope": "github-actions",
                "status": "EXACT",
                "reason": None,
                "captureIds": ["workflows"],
            },
            {
                "scope": "package-manager",
                "status": "EXACT",
                "reason": None,
                "captureIds": ["dependency-lock"],
            },
            {
                "scope": "os-runtime",
                "status": "EXACT",
                "reason": None,
                "captureIds": ["os-build"],
            },
            {
                "scope": "build-tools",
                "status": "EXACT",
                "reason": None,
                "captureIds": ["toolchain"],
            },
        ]
        return {
            "schemaVersion": 1,
            "deployment": {
                "id": "official-windows-x64",
                "environment": "production",
                "platform": "windows",
                "architecture": "x64",
                "sourceCommit": self.source_commit,
                "capturedAt": "2026-07-21T10:00:00Z",
                "validUntil": "2026-07-22T10:00:00Z",
                "buildConfigurationCaptureId": "build-config",
            },
            "coverage": coverage,
            "captures": [
                self.capture("build-config", "build-configuration"),
                self.capture("dependency-lock", "dependency-lock"),
                self.capture("workflows", "workflow-definition"),
                self.capture("os-build", "os-version"),
                self.capture("toolchain", "toolchain"),
            ],
            "components": components,
            "inventoryComplete": True,
            "attestation": {
                "productionSnapshot": True,
                "reviewedBy": "build-owner@example.test",
                "reviewedAt": "2026-07-21T10:10:00Z",
            },
        }

    def source(
        self,
        source_id: str,
        kind: str,
        authority: str,
        inventory_digest: str,
        covered_components: list[dict[str, str]],
        remediations: list[dict[str, str]] | None = None,
    ) -> dict[str, Any]:
        return {
            "id": source_id,
            "kind": kind,
            "authority": authority,
            "sha256": self.digest(source_id),
            "inventorySha256": inventory_digest,
            "capturedAt": "2026-07-21T10:30:00Z",
            "validUntil": "2026-07-22T10:30:00Z",
            "complete": True,
            "coveredComponents": covered_components,
            "remediations": remediations or [],
        }

    def review(self, inventory: dict[str, Any], inventory_bytes: bytes) -> dict[str, Any]:
        authority_by_component = {
            "openssl": "openssl-advisories",
            "curl": "curl-advisories",
            "github-actions": "github-advisories",
            "vcpkg": "vcpkg-advisories",
            "windows-runtime": "platform-advisories",
            "cmake": "cmake-advisories",
        }
        inventory_digest = hashlib.sha256(inventory_bytes).hexdigest()
        exact_components = {
            component["id"]: component
            for component in inventory["components"]
            if component["resolution"] == "EXACT"
        }

        def coverage(*component_ids: str) -> list[dict[str, str]]:
            return [
                {
                    "componentId": component_id,
                    "version": exact_components[component_id]["version"],
                    "coordinate": exact_components[component_id]["coordinate"],
                }
                for component_id in component_ids
                if component_id in exact_components
            ]

        component_reviews = []
        for component in exact_components.values():
            authority_id = (
                authority_by_component["github-actions"]
                if component["scope"] == "github-actions"
                else authority_by_component["windows-runtime"]
                if component["scope"] in {"native-http", "os-runtime"}
                else authority_by_component[component["id"]]
            )
            component_reviews.append(
                {
                    "componentId": component["id"],
                    "version": component["version"],
                    "sourceIds": [authority_id],
                    "complete": True,
                    "advisories": [],
                }
            )
        sources = [
            self.source(
                "openssl-advisories",
                "upstream-security",
                "OpenSSL project",
                inventory_digest,
                coverage("openssl"),
            ),
            self.source(
                "curl-advisories",
                "upstream-security",
                "curl project",
                inventory_digest,
                coverage("curl"),
            ),
            self.source(
                "vcpkg-advisories",
                "upstream-security",
                "Microsoft vcpkg project",
                inventory_digest,
                coverage("vcpkg"),
            ),
            self.source(
                "cmake-advisories",
                "upstream-security",
                "Kitware CMake project",
                inventory_digest,
                coverage("cmake"),
            ),
            self.source(
                "platform-advisories",
                "platform-security",
                "Microsoft Security Response Center",
                inventory_digest,
                coverage(
                    *[
                        component_id
                        for component_id, component in exact_components.items()
                        if component["scope"] in {"native-http", "os-runtime"}
                    ]
                ),
            ),
            self.source(
                "github-advisories",
                "github-security",
                "GitHub Advisory Database",
                inventory_digest,
                coverage(
                    *[
                        component_id
                        for component_id, component in exact_components.items()
                        if component["scope"] == "github-actions"
                    ]
                ),
            ),
        ]
        sources = [source for source in sources if source["coveredComponents"]]
        return {
            "schemaVersion": 1,
            "inventorySha256": inventory_digest,
            "capturedAt": "2026-07-21T11:00:00Z",
            "validUntil": "2026-07-22T10:00:00Z",
            "sources": sources,
            "componentReviews": component_reviews,
            "reviewComplete": True,
            "attestation": {
                "productionSnapshot": True,
                "reviewedBy": "security-owner@example.test",
                "reviewedAt": "2026-07-21T11:10:00Z",
            },
        }

    def run_documents(
        self,
        *,
        mutate_inventory: Callable[[dict[str, Any]], None] | None = None,
        mutate_review: Callable[[dict[str, Any]], None] | None = None,
        omit_evidence: set[str] | None = None,
        extra_evidence: dict[str, Path] | None = None,
        after_snapshot: Callable[[], None] | None = None,
        evaluated_at: str = "2026-07-21T12:00:00Z",
    ) -> subprocess.CompletedProcess[str]:
        inventory = self.inventory()
        if mutate_inventory is not None:
            mutate_inventory(inventory)
        inventory_bytes = json.dumps(
            inventory, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
        review = self.review(inventory, inventory_bytes)
        if mutate_review is not None:
            mutate_review(review)
        inventory_path = self.root / "inventory.json"
        review_path = self.root / "review.json"
        inventory_path.write_bytes(inventory_bytes)
        review_path.write_text(json.dumps(review), encoding="utf-8")
        if after_snapshot is not None:
            after_snapshot()
        command = [
            sys.executable,
            str(VALIDATOR),
            "--inventory",
            str(inventory_path),
            "--review",
            str(review_path),
            "--evaluated-at",
            evaluated_at,
        ]
        evidence = dict(self.evidence_files)
        evidence.update(extra_evidence or {})
        declared_ids = {
            capture["id"] for capture in inventory["captures"]
        } | {source["id"] for source in review["sources"]}
        for evidence_id, path in sorted(evidence.items()):
            if (
                evidence_id in declared_ids
                and (omit_evidence is None or evidence_id not in omit_evidence)
            ):
                command.extend(["--evidence", f"{evidence_id}={path}"])
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def test_complete_current_review_passes(self) -> None:
        result = self.run_documents()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_repository_examples_remain_open(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                "--inventory",
                str(INVENTORY_EXAMPLE),
                "--review",
                str(REVIEW_EXAMPLE),
                "--evaluated-at",
                "2026-07-21T12:00:00Z",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

    def test_unresolved_versions_and_ranges_remain_open(self) -> None:
        for value in ("NOASSERTION", "[>=8.0 <9]", "latest", "builtin-baseline-only"):
            with self.subTest(value=value):
                def unresolved(inventory: dict[str, Any]) -> None:
                    component = next(
                        item for item in inventory["components"] if item["id"] == "curl"
                    )
                    component.update(
                        {
                            "version": value,
                            "resolution": "OPEN",
                            "coordinate": None,
                        }
                    )
                    coverage = next(
                        item for item in inventory["coverage"] if item["scope"] == "curl"
                    )
                    coverage.update(
                        {
                            "status": "OPEN",
                            "reason": "exact deployed version is not resolved",
                        }
                    )

                result = self.run_documents(mutate_inventory=unresolved)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_false_exact_claim_and_unpinned_action_fail(self) -> None:
        result = self.run_documents(
            mutate_inventory=lambda value: next(
                item for item in value["components"] if item["id"] == "curl"
            ).update({"version": "NOASSERTION"})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("marked exact uses an unresolved version", result.stdout)

        def unpin_action(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "action-actions-checkout"
            )
            component.update(
                {
                    "version": "v6.0.2",
                    "coordinate": "actions/checkout@v6.0.2",
                }
            )

        result = self.run_documents(mutate_inventory=unpin_action)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not pinned to one full commit SHA", result.stdout)

        result = self.run_documents(
            mutate_inventory=lambda value: next(
                item
                for item in value["components"]
                if item["id"] == "action-actions-checkout"
            ).update({"name": "unrelated/action-name"})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("component name does not match its coordinate", result.stdout)

        result = self.run_documents(
            mutate_inventory=lambda value: value["components"][-1].update(
                {
                    "version": "0" * 40,
                    "coordinate": "github/codeql-action/analyze@" + "0" * 40,
                }
            )
        )
        self.assertEqual(result.returncode, 1)

    def test_hidden_ranges_and_coordinate_mismatches_fail(self) -> None:
        for version in (
            "8.x",
            "builtin-baseline-only",
            "8 or newer",
            "1.2,1.3",
            "1.2 to 1.3",
            "8 and later",
            "8andlater",
            "vNext",
            "vNext.1",
            "current",
            "current.1",
            "latest-1",
        ):
            with self.subTest(version=version):
                def hidden_range(inventory: dict[str, Any]) -> None:
                    component = next(
                        item for item in inventory["components"] if item["id"] == "curl"
                    )
                    component["version"] = version
                    component["coordinate"] = f"pkg:generic/curl@{version}"

                result = self.run_documents(mutate_inventory=hidden_range)
                self.assertEqual(result.returncode, 1)
                self.assertIn("unresolved version expression", result.stdout)

        for coordinate in (
            "pkg:generic/other-package@3.0.16",
            "pkg:generic/openssl@0.0.0",
            "pkg:npm/openssl@3.0.16",
        ):
            with self.subTest(coordinate=coordinate):
                result = self.run_documents(
                    mutate_inventory=lambda value, coordinate=coordinate: next(
                        item
                        for item in value["components"]
                        if item["id"] == "openssl"
                    ).update({"coordinate": coordinate})
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("coordinate name or version does not match", result.stdout)

        def package_name_contains_alias(inventory: dict[str, Any]) -> None:
            component = next(
                item for item in inventory["components"] if item["id"] == "cmake"
            )
            component["name"] = "latest-cmake"
            component["coordinate"] = "pkg:generic/latest-cmake@4.0.1"

        result = self.run_documents(mutate_inventory=package_name_contains_alias)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def scoped_npm_build_tool(inventory: dict[str, Any]) -> None:
            component = next(
                item for item in inventory["components"] if item["id"] == "cmake"
            )
            component.update(
                {
                    "name": "core",
                    "version": "18.0.0",
                    "coordinate": "pkg:npm/%40angular/core@18.0.0",
                }
            )

        result = self.run_documents(mutate_inventory=scoped_npm_build_tool)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def valid_qualified_purl(inventory: dict[str, Any]) -> None:
            component = next(
                item for item in inventory["components"] if item["id"] == "curl"
            )
            component["coordinate"] = (
                "pkg:generic/curl@8.12.1?arch=x64&distro=windows"
            )

        result = self.run_documents(mutate_inventory=valid_qualified_purl)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for coordinate in (
            "pkg:generic/curl@8.12.1?not-a-qualifier",
            "pkg:generic/curl@8.12.1?distro=windows&arch=x64",
            "pkg:generic/curl@8.12.1?arch=arm64&distro=windows",
            "pkg:generic/curl@8.12.1?arch=-&distro=windows",
            "pkg:generic/curl@8.12.1?arch=%2A&distro=windows",
            "pkg:generic/curl@8.12.1?arch=x%2A64&distro=windows",
            "pkg:generic/curl@8.12.1?arch=any&distro=windows",
            "pkg:generic/curl@8.12.1?arch=universal2&distro=windows",
            "pkg:generic/curl@8.12.1?arch=x64&distro=ubuntu-24.04",
            "pkg:generic/curl@8.12.1?arch=x64&distro=solaris",
            "pkg:generic/curl@8.12.1?arch=x64&distro=windowsmalware",
            "pkg:generic/curl@8.12.1?arch=x64&os=win64evil",
            "pkg:generic/curl@8.12.1?arch=x64&platform=windowsunknown",
            "pkg:generic/curl@8.12.1?target_arch=arm64",
            "pkg:generic/curl@8.12.1?target_hw=arm64",
            "pkg:generic/curl@8.12.1?cpu=arm64",
            "pkg:generic/curl@8.12.1?distro_name=ubuntu-24.04",
            "pkg:generic/curl@8.12.1?target_os=linux",
            "pkg:generic/curl@8.12.1%ZZ",
            "pkg:generic/%2F/curl@8.12.1",
        ):
            with self.subTest(coordinate=coordinate):
                result = self.run_documents(
                    mutate_inventory=lambda value, coordinate=coordinate: next(
                        item
                        for item in value["components"]
                        if item["id"] == "curl"
                    ).update({"coordinate": coordinate})
                )
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("coordinate name or version does not match", result.stdout)

        result = self.run_documents(
            mutate_inventory=lambda value: next(
                item for item in value["components"] if item["id"] == "vcpkg"
            ).update(
                {
                    "coordinate": (
                        "pkg:vcpkg/vcpkg@2025.06.13?triplet=arm64-windows"
                    )
                }
            )
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        result = self.run_documents(
            mutate_inventory=lambda value: next(
                item for item in value["components"] if item["id"] == "vcpkg"
            ).update(
                {
                    "coordinate": (
                        "pkg:vcpkg/vcpkg@2025.06.13?triplet=x64-windows"
                    )
                }
            )
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        result = self.run_documents(
            mutate_inventory=lambda value: next(
                item
                for item in value["components"]
                if item["id"] == "windows-runtime"
            ).update(
                {"coordinate": "cpe:2.3:o:microsoft:windows_11:10.0.26100.0"}
            )
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        def cpe_substring_name(inventory: dict[str, Any]) -> None:
            component = next(
                item for item in inventory["components"] if item["id"] == "openssl"
            )
            component.update(
                {
                    "name": "SSL",
                    "coordinate": (
                        "cpe:2.3:a:openssl:openssl:3.0.16:*:*:*:*:*:*:*"
                    ),
                }
            )

        result = self.run_documents(mutate_inventory=cpe_substring_name)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

    def test_component_and_scope_require_compatible_capture_kinds(self) -> None:
        def wrong_capture_kind(inventory: dict[str, Any]) -> None:
            component = next(
                item for item in inventory["components"] if item["id"] == "openssl"
            )
            component["captureId"] = "workflows"
            coverage = next(
                item for item in inventory["coverage"] if item["scope"] == "openssl"
            )
            coverage["captureIds"] = ["workflows"]

        result = self.run_documents(mutate_inventory=wrong_capture_kind)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("capture kind that cannot establish", result.stdout)
        self.assertIn("references an incompatible capture kind", result.stdout)

    def test_native_http_primary_identity_is_distinct_from_os_identity(self) -> None:
        winhttp_coordinate = (
            "cpe:2.3:a:microsoft:winhttp:10.0.26100.0:*:*:*:*:*:*:*"
        )
        build_document = json.loads(
            self.evidence_files["build-config"].read_text()
        )
        native_scope = next(
            item
            for item in build_document["scopeApplicability"]
            if item["scope"] == "native-http"
        )
        native_scope["applicable"] = True
        self.evidence_files["build-config"].write_text(json.dumps(build_document))
        os_document = json.loads(self.evidence_files["os-build"].read_text())
        os_document["components"].append(
            {
                "componentId": "winhttp",
                "version": "10.0.26100.0",
                "coordinate": winhttp_coordinate,
            }
        )
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def add_native_component(inventory: dict[str, Any]) -> None:
            inventory["components"].append(
                self.component(
                    "winhttp",
                    "WinHTTP",
                    "native-http",
                    "system",
                    "10.0.26100.0",
                    winhttp_coordinate,
                    "os-build",
                )
            )
            coverage = next(
                item
                for item in inventory["coverage"]
                if item["scope"] == "native-http"
            )
            coverage.update(
                {"status": "EXACT", "reason": None, "captureIds": ["os-build"]}
            )

        result = self.run_documents(mutate_inventory=add_native_component)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_scopes_require_a_direct_primary_identity(self) -> None:
        replacements = (
            ("openssl", "CURL", "8.12.1", "pkg:generic/curl@8.12.1"),
            ("vcpkg", "zlib", "1.3.1", "pkg:generic/zlib@1.3.1"),
        )
        for component_id, name, version, coordinate in replacements:
            with self.subTest(component_id=component_id):
                def replace_primary(inventory: dict[str, Any]) -> None:
                    component = next(
                        item
                        for item in inventory["components"]
                        if item["id"] == component_id
                    )
                    component.update(
                        {"name": name, "version": version, "coordinate": coordinate}
                    )

                result = self.run_documents(mutate_inventory=replace_primary)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("lacks a direct primary component identity", result.stdout)

        result = self.run_documents(
            mutate_inventory=lambda value: next(
                item for item in value["components"] if item["id"] == "openssl"
            ).update({"direct": False})
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("lacks a direct primary component identity", result.stdout)

        primary_aliases = (
            (
                "openssl",
                "openssl@3",
                "3.0.16",
                "pkg:generic/openssl%403@3.0.16",
            ),
            (
                "curl",
                "libcurl-devel",
                "8.12.1",
                "pkg:generic/libcurl-devel@8.12.1",
            ),
        )
        for component_id, name, version, coordinate in primary_aliases:
            with self.subTest(primary_alias=component_id):
                def use_primary_alias(inventory: dict[str, Any]) -> None:
                    component = next(
                        item
                        for item in inventory["components"]
                        if item["id"] == component_id
                    )
                    component.update(
                        {"name": name, "version": version, "coordinate": coordinate}
                    )

                result = self.run_documents(mutate_inventory=use_primary_alias)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_distribution_coordinates_require_distribution_authority(self) -> None:
        linux_os_coordinate = (
            "cpe:2.3:o:debian:debian_linux:12.0:*:*:*:*:*:x86_64:*"
        )
        build_document = json.loads(
            self.evidence_files["build-config"].read_text()
        )
        build_document.update(
            {"deploymentId": "official-debian-x64", "platform": "linux"}
        )
        self.evidence_files["build-config"].write_text(json.dumps(build_document))
        os_document = json.loads(self.evidence_files["os-build"].read_text())
        os_document.update(
            {
                "deploymentId": "official-debian-x64",
                "platform": "linux",
                "components": [
                    {
                        "componentId": "windows-runtime",
                        "version": "12.0",
                        "coordinate": linux_os_coordinate,
                    }
                ],
            }
        )
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def debian_openssl(inventory: dict[str, Any]) -> None:
            inventory["deployment"].update(
                {"id": "official-debian-x64", "platform": "linux"}
            )
            component = next(
                item for item in inventory["components"] if item["id"] == "openssl"
            )
            component.update(
                {
                    "version": "3.0.16-1+deb12u1",
                    "coordinate": (
                        "pkg:deb/debian/openssl@3.0.16-1%2Bdeb12u1"
                        "?arch=x64&distro=debian-12"
                    ),
                }
            )
            os_component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            os_component.update(
                {
                    "name": "Debian",
                    "version": "12.0",
                    "coordinate": linux_os_coordinate,
                }
            )

        result = self.run_documents(mutate_inventory=debian_openssl)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("lacks an authoritative advisory source", result.stdout)

        def use_distribution_authority(review: dict[str, Any]) -> None:
            for source_id in ("openssl-advisories", "platform-advisories"):
                source = next(
                    item
                    for item in review["sources"]
                    if item["id"] == source_id
                )
                source.update(
                    {
                        "kind": "distribution-security",
                        "authority": "Debian Security Tracker",
                    }
                )

        result = self.run_documents(
            mutate_inventory=debian_openssl,
            mutate_review=use_distribution_authority,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def debian_cpe_openssl(inventory: dict[str, Any]) -> None:
            debian_openssl(inventory)
            component = next(
                item for item in inventory["components"] if item["id"] == "openssl"
            )
            component.update(
                {
                    "version": "3.0.16",
                    "coordinate": (
                        "cpe:2.3:a:debian:openssl:3.0.16:*:*:*:*:*:*:*"
                    ),
                }
            )

        result = self.run_documents(mutate_inventory=debian_cpe_openssl)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("lacks an authoritative advisory source", result.stdout)

        result = self.run_documents(
            mutate_inventory=debian_cpe_openssl,
            mutate_review=use_distribution_authority,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def rpm_primary_aliases(inventory: dict[str, Any]) -> None:
            debian_openssl(inventory)
            openssl = next(
                item for item in inventory["components"] if item["id"] == "openssl"
            )
            openssl.update(
                {
                    "name": "openssl-libs",
                    "version": "3.0.16-1",
                    "coordinate": "pkg:rpm/openssl-libs@3.0.16-1",
                }
            )
            curl = next(
                item for item in inventory["components"] if item["id"] == "curl"
            )
            curl.update(
                {
                    "name": "curl-minimal",
                    "version": "8.12.1-1",
                    "coordinate": "pkg:rpm/curl-minimal@8.12.1-1",
                }
            )

        def use_all_distribution_authorities(review: dict[str, Any]) -> None:
            use_distribution_authority(review)
            source = next(
                item
                for item in review["sources"]
                if item["id"] == "curl-advisories"
            )
            source.update(
                {
                    "kind": "distribution-security",
                    "authority": "Distribution Security Tracker",
                }
            )

        result = self.run_documents(
            mutate_inventory=rpm_primary_aliases,
            mutate_review=use_all_distribution_authorities,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_workflow_action_capture_is_bidirectional_and_supports_subpaths(self) -> None:
        result = self.run_documents()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("workflow Action capture matches", result.stdout)

        def omit_action(inventory: dict[str, Any]) -> None:
            inventory["components"] = [
                component
                for component in inventory["components"]
                if component["name"] != "github/codeql-action/init"
            ]

        result = self.run_documents(mutate_inventory=omit_action)
        self.assertEqual(result.returncode, 1)
        self.assertIn("workflow Action capture and component inventory differ", result.stdout)

        checkout_commit = self.actions[0][1]
        invalid_coordinate = f"actions/checkout/..@{checkout_commit}"
        workflow = json.loads(self.evidence_files["workflows"].read_text())
        workflow["actions"][0]["coordinate"] = invalid_coordinate
        self.evidence_files["workflows"].write_text(json.dumps(workflow))

        def invalid_action_path(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "action-actions-checkout"
            )
            component["coordinate"] = invalid_coordinate

        result = self.run_documents(mutate_inventory=invalid_action_path)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_inventory_review_and_raw_evidence_digests_are_bound(self) -> None:
        result = self.run_documents(
            mutate_review=lambda value: value.update({"inventorySha256": "f" * 64})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("inventory digest does not match", result.stdout)

        result = self.run_documents(
            after_snapshot=lambda: self.evidence_files["dependency-lock"].write_bytes(
                b"changed after capture\n"
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("dependency capture digest does not match", result.stdout)

        result = self.run_documents(
            after_snapshot=lambda: self.evidence_files[
                "openssl-advisories"
            ].write_bytes(b"changed advisory export\n")
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("advisory source capture digest does not match", result.stdout)

    def test_missing_raw_evidence_remains_open(self) -> None:
        result = self.run_documents(omit_evidence={"dependency-lock"})
        self.assertEqual(result.returncode, 2)
        self.assertIn("dependency capture was not supplied", result.stdout)

        result = self.run_documents(omit_evidence={"openssl-advisories"})
        self.assertEqual(result.returncode, 2)
        self.assertIn("advisory source capture was not supplied", result.stdout)

    def test_inventory_and_review_completeness_claims_are_enforced(self) -> None:
        def remove_scope(inventory: dict[str, Any]) -> None:
            inventory["coverage"] = [
                item for item in inventory["coverage"] if item["scope"] != "qt"
            ]

        result = self.run_documents(mutate_inventory=remove_scope)
        self.assertEqual(result.returncode, 1)
        self.assertIn("required dependency scope is missing", result.stdout)

        def incomplete_inventory(inventory: dict[str, Any]) -> None:
            remove_scope(inventory)
            inventory["inventoryComplete"] = False

        result = self.run_documents(mutate_inventory=incomplete_inventory)
        self.assertEqual(result.returncode, 2)

        def remove_review(review: dict[str, Any]) -> None:
            review["componentReviews"].pop()

        result = self.run_documents(mutate_review=remove_review)
        self.assertEqual(result.returncode, 1)
        self.assertIn("lacks an advisory review", result.stdout)

        def incomplete_review(review: dict[str, Any]) -> None:
            remove_review(review)
            review["reviewComplete"] = False

        result = self.run_documents(mutate_review=incomplete_review)
        self.assertEqual(result.returncode, 2)

    def test_version_and_authority_mismatches_fail(self) -> None:
        result = self.run_documents(
            mutate_review=lambda value: value["componentReviews"][0].update(
                {"version": "0.0.0"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("version does not match inventory", result.stdout)

        def wrong_authority(review: dict[str, Any]) -> None:
            component_review = next(
                item
                for item in review["componentReviews"]
                if item["componentId"] == "windows-runtime"
            )
            component_review["sourceIds"] = ["openssl-advisories"]

        result = self.run_documents(mutate_review=wrong_authority)
        self.assertEqual(result.returncode, 1)
        self.assertIn("lacks an authoritative advisory source", result.stdout)

        def source_does_not_cover_component(review: dict[str, Any]) -> None:
            component_review = next(
                item
                for item in review["componentReviews"]
                if item["componentId"] == "curl"
            )
            component_review["sourceIds"] = ["openssl-advisories"]

        result = self.run_documents(mutate_review=source_does_not_cover_component)
        self.assertEqual(result.returncode, 1)
        self.assertIn("lacks an authoritative advisory source", result.stdout)

        result = self.run_documents(
            mutate_review=lambda value: value["sources"][0].update(
                {"inventorySha256": "f" * 64}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("source is not bound to this inventory", result.stdout)

        def source_coordinate_mismatch(review: dict[str, Any]) -> None:
            source = next(
                item for item in review["sources"] if item["id"] == "curl-advisories"
            )
            source["coveredComponents"][0]["coordinate"] = (
                "pkg:generic/curl@0.0.0"
            )

        result = self.run_documents(mutate_review=source_coordinate_mismatch)
        self.assertEqual(result.returncode, 1)
        self.assertIn("component coverage does not match inventory", result.stdout)

    def test_advisory_assessments_do_not_hide_known_risk(self) -> None:
        for remediation in ("none", "planned", "accepted-risk"):
            with self.subTest(remediation=remediation):
                def affected(review: dict[str, Any]) -> None:
                    review["componentReviews"][0]["advisories"] = [
                        {
                            "id": "CVE-2099-0001",
                            "sourceId": "openssl-advisories",
                            "assessment": "affected",
                            "remediation": remediation,
                            "rationale": "affected production component",
                            "remediationEvidenceId": None,
                        }
                    ]

                result = self.run_documents(mutate_review=affected)
                self.assertEqual(result.returncode, 1)
                self.assertIn("not verifiably remediated", result.stdout)

        def unknown(review: dict[str, Any]) -> None:
            review["componentReviews"][0]["advisories"] = [
                {
                    "id": "CVE-2099-0002",
                    "sourceId": "openssl-advisories",
                    "assessment": "unknown",
                    "remediation": "none",
                    "rationale": None,
                    "remediationEvidenceId": None,
                }
            ]

        result = self.run_documents(mutate_review=unknown)
        self.assertEqual(result.returncode, 2)
        self.assertIn("applicability is unknown", result.stdout)

        def contradictory_unknown(review: dict[str, Any]) -> None:
            unknown(review)
            review["componentReviews"][0]["advisories"][0]["remediation"] = (
                "verified"
            )

        result = self.run_documents(mutate_review=contradictory_unknown)
        self.assertEqual(result.returncode, 1)
        self.assertIn("contradictory remediation", result.stdout)

    def test_not_affected_and_verified_remediation_paths(self) -> None:
        def not_affected(review: dict[str, Any]) -> None:
            review["componentReviews"][0]["advisories"] = [
                {
                    "id": "CVE-2099-0003",
                    "sourceId": "openssl-advisories",
                    "assessment": "not-affected",
                    "remediation": "not-applicable",
                    "rationale": "the reviewed exact version is outside the affected range",
                    "remediationEvidenceId": None,
                }
            ]

        result = self.run_documents(mutate_review=not_affected)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        remediation_path = self.root / "remediation.capture"
        remediation_path.write_bytes(b"verified compensating control evidence\n")
        self.evidence_files["remediation"] = remediation_path

        def add_remediation(review: dict[str, Any], bound_advisory: str) -> None:
            component_review = review["componentReviews"][0]
            review["sources"].append(
                {
                    "id": "remediation",
                    "kind": "remediation-evidence",
                    "authority": "independent remediation reviewer",
                    "sha256": hashlib.sha256(remediation_path.read_bytes()).hexdigest(),
                    "inventorySha256": review["inventorySha256"],
                    "capturedAt": "2026-07-21T10:30:00Z",
                    "validUntil": "2026-07-22T10:30:00Z",
                    "complete": True,
                    "coveredComponents": [
                        {
                            "componentId": component_review["componentId"],
                            "version": component_review["version"],
                            "coordinate": "pkg:generic/openssl@3.0.16",
                        }
                    ],
                    "remediations": [
                        {
                            "componentId": component_review["componentId"],
                            "version": component_review["version"],
                            "coordinate": "pkg:generic/openssl@3.0.16",
                            "advisoryId": bound_advisory,
                        }
                    ],
                }
            )
            component_review["sourceIds"].append("remediation")
            component_review["advisories"] = [
                {
                    "id": "CVE-2099-0004",
                    "sourceId": "openssl-advisories",
                    "assessment": "affected",
                    "remediation": "verified",
                    "rationale": "the protected control was independently verified",
                    "remediationEvidenceId": "remediation",
                }
            ]

        def remediated(review: dict[str, Any]) -> None:
            add_remediation(review, "CVE-2099-0004")

        result = self.run_documents(mutate_review=remediated)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def wrong_advisory_binding(review: dict[str, Any]) -> None:
            add_remediation(review, "CVE-2099-DIFFERENT")

        result = self.run_documents(mutate_review=wrong_advisory_binding)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not verifiably remediated", result.stdout)

    def test_future_and_expired_evidence_never_passes(self) -> None:
        def stale_capture(inventory: dict[str, Any]) -> None:
            inventory["captures"][0]["capturedAt"] = "2026-07-01T10:00:00Z"

        result = self.run_documents(mutate_inventory=stale_capture)
        self.assertEqual(result.returncode, 1)
        self.assertIn("predates the inventory by more than seven days", result.stdout)

        def future_inventory(inventory: dict[str, Any]) -> None:
            inventory["deployment"].update(
                {
                    "capturedAt": "2026-07-21T12:00:00.000000001Z",
                    "validUntil": "2026-07-22T12:00:00Z",
                }
            )
            inventory["attestation"]["reviewedAt"] = (
                "2026-07-21T12:00:00.000000002Z"
            )

        result = self.run_documents(mutate_inventory=future_inventory)
        self.assertEqual(result.returncode, 1)
        self.assertIn("capture is after evaluation time", result.stdout)

        result = self.run_documents(evaluated_at="2026-07-22T11:00:00.000000001Z")
        self.assertEqual(result.returncode, 2)
        self.assertIn("validity has elapsed", result.stdout)

        result = self.run_documents(
            mutate_review=lambda value: value["sources"][0].update(
                {"capturedAt": "2026-07-21T11:00:00.000000001Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("source is newer than the review", result.stdout)

        result = self.run_documents(
            mutate_review=lambda value: value["sources"][0].update(
                {"capturedAt": "2026-07-21T09:59:59.999999999Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("source predates the dependency inventory", result.stdout)

    def test_self_declared_long_validity_windows_fail(self) -> None:
        result = self.run_documents(
            mutate_inventory=lambda value: value["deployment"].update(
                {"validUntil": "2027-07-21T10:00:00Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("seven-day maximum", result.stdout)

        result = self.run_documents(
            mutate_review=lambda value: value.update(
                {"validUntil": "2027-07-21T11:00:00Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("seven-day maximum", result.stdout)

        result = self.run_documents(
            mutate_review=lambda value: value["sources"][0].update(
                {"validUntil": "2027-07-21T10:30:00Z"}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("source validity exceeds", result.stdout)

    def test_build_configuration_binds_commit_and_non_applicability(self) -> None:
        result = self.run_documents(
            mutate_inventory=lambda value: value["deployment"].update(
                {"sourceCommit": "0" * 40}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("source commit", result.stdout)

        def unrelated_not_applicable_capture(inventory: dict[str, Any]) -> None:
            coverage = next(
                item for item in inventory["coverage"] if item["scope"] == "qt"
            )
            coverage["captureIds"] = ["dependency-lock"]

        result = self.run_documents(
            mutate_inventory=unrelated_not_applicable_capture
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("not bound to the build configuration", result.stdout)

        def impossible_not_applicable(inventory: dict[str, Any]) -> None:
            inventory["components"] = [
                component
                for component in inventory["components"]
                if component["scope"] != "os-runtime"
            ]
            coverage = next(
                item
                for item in inventory["coverage"]
                if item["scope"] == "os-runtime"
            )
            coverage.update(
                {
                    "status": "NOT_APPLICABLE",
                    "reason": "incorrectly omitted",
                    "captureIds": ["build-config"],
                }
            )

        result = self.run_documents(mutate_inventory=impossible_not_applicable)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot be not applicable", result.stdout)

    def test_profile_and_os_capture_bind_platform_architecture_and_coordinate(
        self,
    ) -> None:
        result = self.run_documents(
            mutate_inventory=lambda value: value["deployment"].update(
                {"platform": "linux"}
            )
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("deployment profile does not match", result.stdout)

        result = self.run_documents(
            mutate_inventory=lambda value: value["deployment"].update(
                {"architecture": "arm64"}
            )
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("deployment profile does not match", result.stdout)

        arm64_coordinate = (
            "cpe:2.3:o:microsoft:windows_11:10.0.26100.0:*:*:*:*:*:arm64:*"
        )
        os_document = json.loads(self.evidence_files["os-build"].read_text())

        def os_coordinate_in_native_scope(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component["scope"] = "native-http"
            native_coverage = next(
                item
                for item in inventory["coverage"]
                if item["scope"] == "native-http"
            )
            native_coverage.update(
                {"status": "EXACT", "reason": None, "captureIds": ["os-build"]}
            )

        result = self.run_documents(mutate_inventory=os_coordinate_in_native_scope)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        winhttp_as_os = (
            "cpe:2.3:o:microsoft:winhttp:10.0.26100.0:*:*:*:*:*:*:*"
        )
        os_document["components"][0]["coordinate"] = winhttp_as_os
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def native_coordinate_in_os_scope(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component.update({"name": "WinHTTP", "coordinate": winhttp_as_os})

        result = self.run_documents(mutate_inventory=native_coordinate_in_os_scope)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        os_document["components"][0]["coordinate"] = arm64_coordinate
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def contradictory_target_hardware(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component["coordinate"] = arm64_coordinate

        result = self.run_documents(mutate_inventory=contradictory_target_hardware)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        spaced_vendor_coordinate = (
            "cpe:2.3:o:micro soft:windows_11:10.0.26100.0:*:*:*:*:*:*:*"
        )
        os_document["components"][0]["coordinate"] = spaced_vendor_coordinate
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def invalid_cpe_field(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component["coordinate"] = spaced_vendor_coordinate

        result = self.run_documents(mutate_inventory=invalid_cpe_field)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        evil_coordinate = (
            "cpe:2.3:o:microsoft:windowsmalware:10.0.26100.0:*:*:*:*:*:*:*"
        )
        os_document["components"][0]["coordinate"] = evil_coordinate
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def structurally_wrong_windows_product(inventory: dict[str, Any]) -> None:
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component["name"] = "windowsmalware"
            component["coordinate"] = evil_coordinate

        result = self.run_documents(
            mutate_inventory=structurally_wrong_windows_product
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("coordinate name or version does not match", result.stdout)

        macos_coordinate = "cpe:2.3:o:apple:macos:14.5:*:*:*:*:*:x86_64:*"
        build_document = json.loads(
            self.evidence_files["build-config"].read_text()
        )
        build_document.update(
            {"deploymentId": "official-macos-x64", "platform": "macos"}
        )
        self.evidence_files["build-config"].write_text(json.dumps(build_document))
        os_document.update(
            {
                "deploymentId": "official-macos-x64",
                "platform": "macos",
                "components": [
                    {
                        "componentId": "windows-runtime",
                        "version": "14.5",
                        "coordinate": macos_coordinate,
                    }
                ],
            }
        )
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def coherent_macos_profile(inventory: dict[str, Any]) -> None:
            inventory["deployment"].update(
                {"id": "official-macos-x64", "platform": "macos"}
            )
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component.update(
                {"name": "macOS", "version": "14.5", "coordinate": macos_coordinate}
            )

        result = self.run_documents(mutate_inventory=coherent_macos_profile)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def homebrew_components(inventory: dict[str, Any]) -> None:
            coherent_macos_profile(inventory)
            build_tool = next(
                item for item in inventory["components"] if item["id"] == "cmake"
            )
            build_tool.update(
                {
                    "name": "qt@6",
                    "version": "6.8.0",
                    "coordinate": "pkg:brew/qt%406@6.8.0?arch=universal2",
                }
            )
            openssl = next(
                item for item in inventory["components"] if item["id"] == "openssl"
            )
            openssl.update(
                {
                    "name": "openssl@3",
                    "coordinate": "pkg:brew/openssl%403@3.0.16?arch=universal2",
                }
            )

        def use_homebrew_authority(review: dict[str, Any]) -> None:
            for source_id in ("cmake-advisories", "openssl-advisories"):
                source = next(
                    item
                    for item in review["sources"]
                    if item["id"] == source_id
                )
                source.update(
                    {"kind": "distribution-security", "authority": "Homebrew"}
                )

        result = self.run_documents(
            mutate_inventory=homebrew_components,
            mutate_review=use_homebrew_authority,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        linux_coordinate = (
            "cpe:2.3:o:canonical:ubuntu_linux:24.04:*:*:*:*:*:x86_64:*"
        )
        build_document.update(
            {"deploymentId": "official-linux-x64", "platform": "linux"}
        )
        self.evidence_files["build-config"].write_text(json.dumps(build_document))
        os_document.update(
            {
                "deploymentId": "official-linux-x64",
                "platform": "linux",
                "components": [
                    {
                        "componentId": "windows-runtime",
                        "version": "24.04",
                        "coordinate": linux_coordinate,
                    }
                ],
            }
        )
        self.evidence_files["os-build"].write_text(json.dumps(os_document))

        def coherent_linux_profile(inventory: dict[str, Any]) -> None:
            inventory["deployment"].update(
                {"id": "official-linux-x64", "platform": "linux"}
            )
            component = next(
                item
                for item in inventory["components"]
                if item["id"] == "windows-runtime"
            )
            component.update(
                {
                    "name": "Ubuntu",
                    "version": "24.04",
                    "coordinate": linux_coordinate,
                }
            )

        def use_linux_distribution_authority(review: dict[str, Any]) -> None:
            source = next(
                item
                for item in review["sources"]
                if item["id"] == "platform-advisories"
            )
            source.update(
                {
                    "kind": "distribution-security",
                    "authority": "Ubuntu Security Notices",
                }
            )

        result = self.run_documents(
            mutate_inventory=coherent_linux_profile,
            mutate_review=use_linux_distribution_authority,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_strict_schema_errors_return_three_without_secrets(self) -> None:
        inventory = self.inventory()
        inventory["unexpectedSecretField"] = "secret-internal-registry.example.test"
        inventory_path = self.root / "invalid-inventory.json"
        inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
        review_path = self.root / "unused-review.json"
        review_path.write_text("{}", encoding="utf-8")
        command = [
            sys.executable,
            str(VALIDATOR),
            "--inventory",
            str(inventory_path),
            "--review",
            str(review_path),
            "--evaluated-at",
            "2026-07-21T12:00:00Z",
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 3)
        self.assertNotIn("secret-internal-registry", result.stdout)
        self.assertNotIn("Traceback", result.stderr)

        inventory_path.write_bytes(
            b'{"schemaVersion":1,"schemaVersion":1}'
        )
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 3)

        inventory_path.write_bytes(b'{"schemaVersion":' + b"1" * 5000 + b"}")
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 3)
        self.assertNotIn("Traceback", result.stderr)
        self.assertNotIn("1111111111", result.stdout)

    def test_reports_do_not_expose_component_or_capture_contents(self) -> None:
        def unresolved_secret(inventory: dict[str, Any]) -> None:
            component = inventory["components"][0]
            component["name"] = "secret-internal-component-name"
            component["resolution"] = "OPEN"
            component["version"] = "secret-build-version"
            component["coordinate"] = None
            inventory["coverage"][0].update(
                {"status": "OPEN", "reason": "internal repository details"}
            )

        result = self.run_documents(mutate_inventory=unresolved_secret)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("secret-internal", result.stdout)
        self.assertNotIn("secret-build-version", result.stdout)
        self.assertNotIn("release build configuration", result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--inventory-example", type=Path, required=True)
    parser.add_argument("--review-example", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    global VALIDATOR, INVENTORY_EXAMPLE, REVIEW_EXAMPLE, WORK_DIR
    VALIDATOR = args.validator.resolve()
    INVENTORY_EXAMPLE = args.inventory_example.resolve()
    REVIEW_EXAMPLE = args.review_example.resolve()
    work_base = args.work_dir.resolve()
    work_base.mkdir(parents=True, exist_ok=True)
    WORK_DIR = Path(tempfile.mkdtemp(prefix="run-", dir=work_base))
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(DependencyReviewToolTests)
    )
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
