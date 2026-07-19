#!/usr/bin/env python3
"""Regression tests for release metadata producer timestamp and routing contracts."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


MAKE_MANIFEST = Path()
MAKE_INDEX = Path()
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--make-manifest", required=True, type=Path)
    parser.add_argument("--make-index", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    global MAKE_MANIFEST, MAKE_INDEX, WORK_DIR
    MAKE_MANIFEST = args.make_manifest.resolve()
    MAKE_INDEX = args.make_index.resolve()
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
