#!/usr/bin/env python3
"""Regression tests for dependency-aware, fail-closed SPDX SBOM generation."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


MAKE_SBOM = Path()
CONFIGURED_DEPENDENCIES = Path()
WORK_DIR = Path()


def dependency(
    name: str = "CURL",
    spdx_id: str = "SPDXRef-Package-CURL",
) -> dict[str, str]:
    return {
        "name": name,
        "SPDXID": spdx_id,
        "versionInfo": "8.8.0",
        "downloadLocation": "https://curl.se/",
        "licenseDeclared": "curl",
        "supplier": "Organization: curl",
    }


class SbomToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.install_root = self.root / "install"
        self.install_root.mkdir(parents=True)
        (self.install_root / "bin").mkdir()
        (self.install_root / "bin" / "libAutoUpdater.bin").write_bytes(b"installed payload")
        self.output = self.root / "result.spdx.json"

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def run_tool(self, *extra: str, output: Path | None = None) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(MAKE_SBOM),
            str(self.install_root),
            "--version",
            "1.2.3",
            "--output",
            str(output or self.output),
            *extra,
        ]
        return subprocess.run(command, text=True, capture_output=True, check=False)

    def write_dependencies(self, value: object, path: Path | None = None) -> Path:
        target = path or (self.root / "dependencies.json")
        target.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return target

    def valid_metadata(self) -> dict[str, object]:
        return {"schemaVersion": 1, "dependencies": [dependency()]}

    def test_emits_dependency_components_and_complete_relationship_graph(self) -> None:
        (self.install_root / "include").mkdir()
        (self.install_root / "include" / "api.hpp").write_text("// API\n", encoding="utf-8")
        openssl = dependency("OpenSSL", "SPDXRef-Package-OpenSSL")
        openssl.update(
            {
                "versionInfo": "3.3.1",
                "downloadLocation": "https://www.openssl.org/",
                "licenseDeclared": "Apache-2.0",
                "supplier": "Organization: OpenSSL Software Foundation",
            }
        )
        metadata = self.write_dependencies(
            {"schemaVersion": 1, "dependencies": [dependency(), openssl]}
        )

        result = self.run_tool(
            "--dependencies",
            str(metadata),
            "--platform",
            "Windows x64",
            "--commit",
            "0123456789abcdef0123456789abcdef01234567",
            "--generation-id",
            "release-run-7",
        )

        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        document = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        self.assertEqual(
            document["documentNamespace"],
            "https://github.com/sorrowfeng/libAutoUpdater/sbom/libAutoUpdater/1.2.3/"
            "platform/Windows%20x64/commit/0123456789abcdef0123456789abcdef01234567/"
            "generation/release-run-7",
        )
        package_ids = {package["SPDXID"] for package in document["packages"]}
        self.assertEqual(
            package_ids,
            {
                "SPDXRef-Package-libAutoUpdater",
                "SPDXRef-Package-CURL",
                "SPDXRef-Package-OpenSSL",
            },
        )
        self.assertEqual(len(package_ids), len(document["packages"]))
        dependencies = document["packages"][1:]
        self.assertTrue(all(package["filesAnalyzed"] is False for package in dependencies))
        root_package = document["packages"][0]
        file_sha1_values = sorted(
            next(
                checksum["checksumValue"]
                for checksum in file_entry["checksums"]
                if checksum["algorithm"] == "SHA1"
            )
            for file_entry in document["files"]
        )
        expected_verification_code = hashlib.sha1(
            "".join(file_sha1_values).encode("ascii")
        ).hexdigest()
        self.assertEqual(
            root_package["packageVerificationCode"]["packageVerificationCodeValue"],
            expected_verification_code,
        )
        self.assertTrue(
            all(
                {checksum["algorithm"] for checksum in file_entry["checksums"]}
                == {"SHA1", "SHA256"}
                for file_entry in document["files"]
            )
        )

        relationships = document["relationships"]
        describes = [item for item in relationships if item["relationshipType"] == "DESCRIBES"]
        contains = [item for item in relationships if item["relationshipType"] == "CONTAINS"]
        depends_on = [item for item in relationships if item["relationshipType"] == "DEPENDS_ON"]
        self.assertEqual(
            describes,
            [
                {
                    "spdxElementId": "SPDXRef-DOCUMENT",
                    "relationshipType": "DESCRIBES",
                    "relatedSpdxElement": "SPDXRef-Package-libAutoUpdater",
                }
            ],
        )
        self.assertEqual(
            {item["relatedSpdxElement"] for item in contains},
            {item["SPDXID"] for item in document["files"]},
        )
        self.assertEqual(len(contains), 2)
        self.assertEqual(
            {item["relatedSpdxElement"] for item in depends_on},
            {"SPDXRef-Package-CURL", "SPDXRef-Package-OpenSSL"},
        )

    def test_allows_a_build_with_no_external_dependencies(self) -> None:
        result = self.run_tool("--platform", "source", "--commit", "local")

        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        document = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(len(document["packages"]), 1)
        self.assertFalse(
            any(
                relationship["relationshipType"] == "DEPENDS_ON"
                for relationship in document["relationships"]
            )
        )
        self.assertTrue(
            any(
                relationship["relationshipType"] == "DESCRIBES"
                for relationship in document["relationships"]
            )
        )

    def test_default_document_namespace_is_unique_for_each_generation(self) -> None:
        first_output = self.root / "first.spdx.json"
        second_output = self.root / "second.spdx.json"

        first = self.run_tool("--platform", "source", "--commit", "local", output=first_output)
        second = self.run_tool(
            "--platform", "source", "--commit", "local", output=second_output
        )

        self.assertEqual(first.returncode, 0, msg=first.stdout + first.stderr)
        self.assertEqual(second.returncode, 0, msg=second.stdout + second.stderr)
        first_namespace = json.loads(first_output.read_text(encoding="utf-8"))[
            "documentNamespace"
        ]
        second_namespace = json.loads(second_output.read_text(encoding="utf-8"))[
            "documentNamespace"
        ]
        self.assertNotEqual(first_namespace, second_namespace)
        self.assertIn("/generation/", first_namespace)
        self.assertIn("/generation/", second_namespace)

    def test_configured_dependency_metadata_is_accepted(self) -> None:
        configured = json.loads(CONFIGURED_DEPENDENCIES.read_text(encoding="utf-8"))

        result = self.run_tool("--dependencies", str(CONFIGURED_DEPENDENCIES))

        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        document = json.loads(self.output.read_text(encoding="utf-8"))
        expected_ids = {item["SPDXID"] for item in configured["dependencies"]}
        actual_ids = {
            package["SPDXID"]
            for package in document["packages"]
            if package["SPDXID"] != "SPDXRef-Package-libAutoUpdater"
        }
        self.assertEqual(actual_ids, expected_ids)

    def test_rejects_malformed_duplicate_and_incomplete_dependency_input(self) -> None:
        valid = self.valid_metadata()
        duplicate_name = dependency("curl", "SPDXRef-Package-curl-second")
        duplicate_id = dependency("OpenSSL", "SPDXRef-Package-CURL")
        cases: dict[str, str | object] = {
            "malformed-json": "{",
            "duplicate-json-key": '{"schemaVersion":1,"schemaVersion":1,"dependencies":[]}',
            "root-is-array": [],
            "missing-root-field": {"schemaVersion": 1},
            "unknown-root-field": {**valid, "unexpected": True},
            "boolean-schema": {"schemaVersion": True, "dependencies": []},
            "floating-point-schema": {"schemaVersion": 1.0, "dependencies": []},
            "wrong-schema": {"schemaVersion": 2, "dependencies": []},
            "dependencies-is-object": {"schemaVersion": 1, "dependencies": {}},
            "dependency-is-string": {"schemaVersion": 1, "dependencies": ["CURL"]},
            "missing-package-field": {
                "schemaVersion": 1,
                "dependencies": [
                    {key: value for key, value in dependency().items() if key != "versionInfo"}
                ],
            },
            "unknown-package-field": {
                "schemaVersion": 1,
                "dependencies": [{**dependency(), "checksum": "unsupported"}],
            },
            "duplicate-name": {
                "schemaVersion": 1,
                "dependencies": [dependency(), duplicate_name],
            },
            "duplicate-id": {
                "schemaVersion": 1,
                "dependencies": [dependency(), duplicate_id],
            },
            "reserved-root-id": {
                "schemaVersion": 1,
                "dependencies": [dependency("Other", "SPDXRef-Package-libAutoUpdater")],
            },
            "reserved-document-id": {
                "schemaVersion": 1,
                "dependencies": [dependency("Other", "SPDXRef-DOCUMENT")],
            },
            "invalid-id": {
                "schemaVersion": 1,
                "dependencies": [dependency("Other", "SPDXRef-Package Other")],
            },
            "reserved-file-id": {
                "schemaVersion": 1,
                "dependencies": [dependency("Other", "SPDXRef-File-1")],
            },
            "duplicate-root-name": {
                "schemaVersion": 1,
                "dependencies": [dependency("libautoupdater", "SPDXRef-Package-Other")],
            },
            "credential-url": {
                "schemaVersion": 1,
                "dependencies": [
                    {
                        **dependency(),
                        "downloadLocation": "https://user:password@example.test/package",
                    }
                ],
            },
            "relative-url": {
                "schemaVersion": 1,
                "dependencies": [{**dependency(), "downloadLocation": "downloads/package"}],
            },
            "query-url": {
                "schemaVersion": 1,
                "dependencies": [
                    {
                        **dependency(),
                        "downloadLocation": "https://example.test/package?token=secret",
                    }
                ],
            },
            "fragment-url": {
                "schemaVersion": 1,
                "dependencies": [
                    {
                        **dependency(),
                        "downloadLocation": "https://example.test/package#credential",
                    }
                ],
            },
            "invalid-supplier": {
                "schemaVersion": 1,
                "dependencies": [{**dependency(), "supplier": "curl project"}],
            },
            "untrimmed-name": {
                "schemaVersion": 1,
                "dependencies": [{**dependency(), "name": " CURL"}],
            },
        }
        for label, contents in cases.items():
            with self.subTest(label=label):
                metadata = self.root / f"{label}.json"
                if isinstance(contents, str):
                    metadata.write_text(contents, encoding="utf-8")
                else:
                    self.write_dependencies(contents, metadata)
                sentinel = f"unchanged-{label}".encode("utf-8")
                self.output.write_bytes(sentinel)

                result = self.run_tool("--dependencies", str(metadata))

                self.assertNotEqual(result.returncode, 0, msg=label)
                self.assertIn("SBOM generation failed", result.stdout + result.stderr)
                self.assertEqual(self.output.read_bytes(), sentinel)
                self.assertEqual(list(self.output.parent.glob(f".{self.output.name}.*.tmp")), [])

    def test_dependency_file_cannot_alias_output(self) -> None:
        self.write_dependencies(self.valid_metadata(), self.output)
        original = self.output.read_bytes()

        result = self.run_tool("--dependencies", str(self.output))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must refer to different files", result.stdout + result.stderr)
        self.assertEqual(self.output.read_bytes(), original)

    def test_output_inside_install_tree_does_not_describe_itself(self) -> None:
        output = self.install_root / "sbom.spdx.json"
        output.write_text("stale output", encoding="utf-8")

        result = self.run_tool(output=output)

        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual([item["fileName"] for item in document["files"]], ["bin/libAutoUpdater.bin"])

    def test_atomic_writer_preserves_existing_output_when_replace_fails(self) -> None:
        spec = importlib.util.spec_from_file_location("make_sbom_under_test", MAKE_SBOM)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.output.write_bytes(b"old-document")

        with mock.patch.object(module.os, "replace", side_effect=OSError("injected replace failure")):
            with self.assertRaisesRegex(OSError, "injected replace failure"):
                module.atomic_write_json(self.output, {"new": "document"})

        self.assertEqual(self.output.read_bytes(), b"old-document")
        self.assertEqual(list(self.output.parent.glob(f".{self.output.name}.*.tmp")), [])


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make-sbom", type=Path, required=True)
    parser.add_argument("--configured-dependencies", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_arguments()
    MAKE_SBOM = arguments.make_sbom.resolve()
    CONFIGURED_DEPENDENCIES = arguments.configured_dependencies.resolve()
    WORK_DIR = arguments.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0]])
