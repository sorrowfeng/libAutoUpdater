#!/usr/bin/env python3
"""Regression tests for release metadata producer timestamp and routing contracts."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import shutil
import stat
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


MAKE_MANIFEST = Path()
MAKE_INDEX = Path()
SIGN_MANIFEST = Path()
WORK_DIR = Path()


class MetadataToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def run_tool(self, script: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(script), *arguments], text=True, capture_output=True, check=False
        )

    def load_make_manifest_module(self):
        spec = importlib.util.spec_from_file_location("make_manifest_under_test", MAKE_MANIFEST)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(MAKE_MANIFEST.parent))
        try:
            spec.loader.exec_module(module)
        finally:
            sys.path.pop(0)
        return module

    def load_sign_manifest_module(self):
        spec = importlib.util.spec_from_file_location("sign_manifest_under_test", SIGN_MANIFEST)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def manifest_command(self, output: Path, timestamp: str) -> list[str]:
        release = self.root / "release"
        release.mkdir(exist_ok=True)
        (release / "app.bin").write_bytes(b"payload")
        return [
            str(release),
            "--output",
            str(output),
            "--app-id",
            "com.example.app",
            "--platform",
            "windows",
            "--arch",
            "x64",
            "--version",
            "1.2.3",
            "--release-date",
            timestamp,
            "--base-url",
            "https://updates.example.test/releases/1.2.3/",
        ]

    def test_make_manifest_validates_timestamps_before_writing(self) -> None:
        invalid_output = self.root / "invalid.json"
        invalid = self.run_tool(
            MAKE_MANIFEST, *self.manifest_command(invalid_output, "2026-02-30T00:00:00Z")
        )
        self.assertNotEqual(invalid.returncode, 0)
        self.assertFalse(invalid_output.exists())

        valid_output = self.root / "valid.json"
        valid = self.run_tool(
            MAKE_MANIFEST,
            *self.manifest_command(valid_output, "2026-07-19T20:34:56.123456789+08:00"),
        )
        self.assertEqual(valid.returncode, 0, msg=valid.stdout + valid.stderr)
        document = json.loads(valid_output.read_text(encoding="utf-8"))
        self.assertEqual(document["releaseDate"], "2026-07-19T20:34:56.123456789+08:00")

    def test_make_manifest_redacts_operational_urls(self) -> None:
        output = self.root / "manifest.json"
        command = self.manifest_command(output, "2026-07-19T12:00:00Z")
        base_url = command.index("--base-url") + 1
        command[base_url] = (
            "https://sentinel-user:sentinel-password@updates.example.test:8443/"
            "releases/1.2.3/?token=sentinel-token#sentinel-fragment"
        )

        result = self.run_tool(MAKE_MANIFEST, *command)

        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertIn("https://updates.example.test:8443/releases/1.2.3/", result.stdout)
        for secret in (
            "sentinel-user",
            "sentinel-password",
            "sentinel-token",
            "sentinel-fragment",
        ):
            self.assertNotIn(secret, result.stdout + result.stderr)

    def test_make_manifest_excludes_default_and_custom_generated_outputs(self) -> None:
        release = self.root / "release"
        release.mkdir()
        (release / "app.bin").write_bytes(b"payload")
        (release / "manifest.json").write_text("stale manifest", encoding="utf-8")
        (release / "manifest.json.sig").write_text("stale signature", encoding="utf-8")

        default = self.run_tool(
            MAKE_MANIFEST,
            *self.manifest_command(release / "manifest.json", "2026-07-19T12:00:00Z"),
        )
        self.assertEqual(default.returncode, 0, msg=default.stdout + default.stderr)
        default_paths = {
            item["path"] for item in json.loads((release / "manifest.json").read_text(encoding="utf-8"))["files"]
        }
        self.assertEqual(default_paths, {"app.bin"})

        implicit_manifest = release / "metadata" / "release-v1.json"
        implicit_signature = implicit_manifest.with_suffix(implicit_manifest.suffix + ".sig")
        implicit_manifest.parent.mkdir()
        implicit_manifest.write_text("stale implicit manifest", encoding="utf-8")
        implicit_signature.write_text("stale implicit signature", encoding="utf-8")
        implicit = self.run_tool(
            MAKE_MANIFEST,
            *self.manifest_command(implicit_manifest, "2026-07-19T12:00:00Z"),
        )
        self.assertEqual(implicit.returncode, 0, msg=implicit.stdout + implicit.stderr)
        implicit_paths = {
            item["path"] for item in json.loads(implicit_manifest.read_text(encoding="utf-8"))["files"]
        }
        self.assertNotIn("metadata/release-v1.json", implicit_paths)
        self.assertNotIn("metadata/release-v1.json.sig", implicit_paths)

        custom_manifest = release / "metadata" / "release-v2.json"
        custom_signature = release / "signatures" / "release-v2.detached"
        custom_signature.parent.mkdir()
        custom_manifest.write_text("stale custom manifest", encoding="utf-8")
        custom_signature.write_text("stale custom signature", encoding="utf-8")
        custom = self.run_tool(
            MAKE_MANIFEST,
            *self.manifest_command(custom_manifest, "2026-07-19T12:00:00Z"),
            "--signature-output",
            str(custom_signature),
        )
        self.assertEqual(custom.returncode, 0, msg=custom.stdout + custom.stderr)
        custom_paths = {
            item["path"] for item in json.loads(custom_manifest.read_text(encoding="utf-8"))["files"]
        }
        self.assertNotIn("metadata/release-v2.json", custom_paths)
        self.assertNotIn("signatures/release-v2.detached", custom_paths)
        self.assertNotIn("manifest.json", custom_paths)
        self.assertNotIn("manifest.json.sig", custom_paths)

        same_output = self.run_tool(
            MAKE_MANIFEST,
            *self.manifest_command(self.root / "same-output.json", "2026-07-19T12:00:00Z"),
            "--signature-output",
            str(self.root / "same-output.json"),
        )
        self.assertNotEqual(same_output.returncode, 0)
        self.assertIn("must refer to different files", same_output.stdout + same_output.stderr)

    def test_content_addressed_copy_revalidates_source_bytes(self) -> None:
        module = self.load_make_manifest_module()

        source = self.root / "source.bin"
        source.write_bytes(b"changed-after-hash")
        source.chmod(stat.S_IREAD)
        original_digest = hashlib.sha256(b"original").hexdigest()
        object_root = self.root / "objects"
        try:
            with self.assertRaisesRegex(SystemExit, "Source changed while copying release object"):
                module.copy_object(source, original_digest, object_root)
        finally:
            source.chmod(stat.S_IREAD | stat.S_IWRITE)
        shard = object_root / original_digest[:2]
        self.assertFalse((shard / original_digest).exists())
        self.assertEqual(list(shard.glob(".*.tmp")), [])

    def test_content_addressed_copy_is_concurrency_safe(self) -> None:
        module = self.load_make_manifest_module()
        source = self.root / "source.bin"
        contents = b"one immutable object"
        source.write_bytes(contents)
        digest = hashlib.sha256(contents).hexdigest()
        object_root = self.root / "objects"
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(lambda _: module.copy_object(source, digest, object_root), range(32)))
        target = object_root / digest[:2] / digest
        self.assertTrue(all(result == target for result in results))
        self.assertEqual(target.read_bytes(), contents)
        self.assertEqual(list(target.parent.glob(".*.tmp")), [])

    @unittest.skipIf(sys.platform == "win32", "POSIX permission contract")
    def test_content_addressed_copy_preserves_publishable_mode(self) -> None:
        module = self.load_make_manifest_module()
        source = self.root / "executable.bin"
        contents = b"portable executable"
        source.write_bytes(contents)
        source.chmod(0o777)
        digest = hashlib.sha256(contents).hexdigest()

        target = module.copy_object(source, digest, self.root / "objects")

        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o755)

    def test_make_index_emits_wildcards_and_rejects_duplicate_routes(self) -> None:
        output = self.root / "index.json"
        valid = self.run_tool(
            MAKE_INDEX,
            "--output",
            str(output),
            "--app-id",
            "com.example.app",
            "--generated-at",
            "2026-07-19T12:34:56Z",
            "--target",
            "*/*=fallback/manifest.json",
            "--target",
            "windows/x64=windows/manifest.json",
        )
        self.assertEqual(valid.returncode, 0, msg=valid.stdout + valid.stderr)
        targets = json.loads(output.read_text(encoding="utf-8"))["targets"]
        self.assertEqual(targets[0], {"manifestUrl": "fallback/manifest.json"})
        self.assertEqual(targets[1]["platform"], "windows")
        self.assertEqual(targets[1]["arch"], "x64")

        duplicate_output = self.root / "duplicate.json"
        duplicate = self.run_tool(
            MAKE_INDEX,
            "--output",
            str(duplicate_output),
            "--app-id",
            "com.example.app",
            "--generated-at",
            "2026-07-19T12:34:56Z",
            "--target",
            "windows/x64=first.json",
            "--target",
            "windows/x64=second.json",
        )
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertFalse(duplicate_output.exists())

        invalid_time_output = self.root / "invalid-time.json"
        invalid_time = self.run_tool(
            MAKE_INDEX,
            "--output",
            str(invalid_time_output),
            "--app-id",
            "com.example.app",
            "--generated-at",
            "not-a-time",
            "--target",
            "windows/x64=manifest.json",
        )
        self.assertNotEqual(invalid_time.returncode, 0)
        self.assertFalse(invalid_time_output.exists())

    def test_sign_manifest_pins_rsa_pkcs1_padding(self) -> None:
        module = self.load_sign_manifest_module()
        manifest = self.root / "manifest.json"
        private_key = self.root / "private.pem"

        with mock.patch.object(module.subprocess, "check_output", return_value=b"signature") as run:
            signature = module.sign_rsa_sha256(manifest, private_key)

        self.assertEqual(signature, b"signature")
        run.assert_called_once_with(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-sigopt",
                "rsa_padding_mode:pkcs1",
                "-sign",
                str(private_key),
                str(manifest),
            ]
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--make-manifest", required=True, type=Path)
    parser.add_argument("--make-index", required=True, type=Path)
    parser.add_argument("--sign-manifest", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    global MAKE_MANIFEST, MAKE_INDEX, SIGN_MANIFEST, WORK_DIR
    MAKE_MANIFEST = args.make_manifest.resolve()
    MAKE_INDEX = args.make_index.resolve()
    SIGN_MANIFEST = args.sign_manifest.resolve()
    WORK_DIR = args.work_dir.resolve()
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    WORK_DIR.mkdir(parents=True)
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(MetadataToolTests)
    )
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
