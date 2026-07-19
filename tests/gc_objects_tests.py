#!/usr/bin/env python3
"""Regression tests for fail-closed content-addressed object garbage collection."""

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
from typing import Any


GC_SCRIPT = Path()
JSON_CORPUS = Path()
GC_MODULE: Any = None
WORK_DIR = Path()
OBJECT_PREFIX = "objects/sha256"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class GcObjectsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def write_object(self, object_root: Path, contents: bytes) -> tuple[str, Path]:
        sha256 = digest(contents)
        path = object_root / sha256[:2] / sha256
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return sha256, path

    def valid_manifest(self, sha256: str, size: int) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "appId": "com.example.app",
            "channel": "stable",
            "platform": "windows",
            "arch": "x64",
            "version": "1.2.3",
            "mandatory": False,
            "allowDowngrade": False,
            "notes": "Valid Unicode scalar: 😀",
            "baseUrl": "https://updates.example.test/",
            "files": [
                {
                    "path": f"{OBJECT_PREFIX}/{sha256[:2]}/{sha256}",
                    "localPath": "bin/app.bin",
                    "sha256": sha256,
                    "size": size,
                }
            ],
            "remove": ["bin/obsolete.bin"],
        }

    def run_gc(
        self,
        object_root: Path,
        manifests: list[Path],
        *,
        delete: bool = True,
        object_prefix: str = OBJECT_PREFIX,
        allow_empty: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(GC_SCRIPT), str(object_root)]
        for manifest in manifests:
            command.extend(["--manifest", str(manifest)])
        command.extend(["--object-prefix", object_prefix])
        if delete:
            command.append("--delete")
        if allow_empty:
            command.append("--allow-empty")
        return subprocess.run(command, text=True, capture_output=True, check=False)

    def write_json(self, path: Path, value: Any) -> None:
        path.write_text(json.dumps(value, ensure_ascii=True, separators=(",", ":")), encoding="utf-8")

    def make_store(self, case_root: Path) -> tuple[Path, str, Path, Path]:
        object_root = case_root / "objects"
        referenced_digest, referenced_path = self.write_object(object_root, b"referenced payload")
        _, stale_path = self.write_object(object_root, b"stale payload")
        return object_root, referenced_digest, referenced_path, stale_path

    def assert_validation_failure_preserves_store(
        self,
        name: str,
        manifest_bytes: bytes,
        *,
        object_prefix: str = OBJECT_PREFIX,
    ) -> None:
        case_root = self.root / name
        case_root.mkdir()
        object_root, _, referenced_path, stale_path = self.make_store(case_root)
        manifest = case_root / "manifest.json"
        manifest.write_bytes(manifest_bytes)

        # Permit an intentional zero-reference result so these assertions prove
        # the input itself was rejected rather than tripping the empty-set guard.
        result = self.run_gc(object_root, [manifest], object_prefix=object_prefix, allow_empty=True)

        self.assertNotEqual(result.returncode, 0, msg=f"stdout={result.stdout}\nstderr={result.stderr}")
        self.assertTrue(referenced_path.exists(), msg=name)
        self.assertTrue(stale_path.exists(), msg=name)

    def test_valid_manifests_delete_only_stale_objects(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        # Content-addressed publishing commonly preserves an old source mtime;
        # Windows path stat and handle fstat expose different ctime semantics.
        os.utime(referenced_path, ns=(1_600_000_000_000_000_000, 1_600_000_000_000_000_000))
        manifest = self.root / "manifest.json"
        value = self.valid_manifest(sha256, len(b"referenced payload"))
        value["version"] = "1.2.3+build.007"
        value["files"].append(
            {
                "path": f"other/sha256/{digest(b'ordinary release file')[:2]}/{digest(b'ordinary release file')}",
                "localPath": "",
                "sha256": digest(b"ordinary release file"),
                "size": len(b"ordinary release file"),
            }
        )
        self.write_json(manifest, value)

        result = self.run_gc(object_root, [manifest])

        self.assertEqual(result.returncode, 0, msg=f"stdout={result.stdout}\nstderr={result.stderr}")
        self.assertTrue(referenced_path.exists())
        self.assertFalse(stale_path.exists())
        self.assertIn("Referenced objects: 1", result.stdout)
        self.assertIn("Unreferenced objects: 1", result.stdout)

    def test_zero_object_references_require_explicit_delete_confirmation(self) -> None:
        object_root, _, referenced_path, stale_path = self.make_store(self.root)
        manifest = self.root / "manifest.json"
        value = {
            "schemaVersion": 1,
            "version": "1.0.0",
            "files": [
                {
                    "path": "bin/app.bin",
                    "localPath": "",
                    "sha256": digest(b"ordinary release file"),
                    "size": len(b"ordinary release file"),
                }
            ],
        }
        self.write_json(manifest, value)

        refused = self.run_gc(object_root, [manifest])

        self.assertNotEqual(refused.returncode, 0)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())

        confirmed = self.run_gc(object_root, [manifest], allow_empty=True)

        self.assertEqual(confirmed.returncode, 0, msg=confirmed.stderr)
        self.assertFalse(referenced_path.exists())
        self.assertFalse(stale_path.exists())

    def test_dry_run_never_deletes(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        manifest = self.root / "manifest.json"
        self.write_json(manifest, self.valid_manifest(sha256, len(b"referenced payload")))

        result = self.run_gc(object_root, [manifest], delete=False)

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())
        self.assertIn("Would delete", result.stdout)

    def test_invalid_json_and_schema_fail_closed(self) -> None:
        seed = digest(b"referenced payload")
        valid = self.valid_manifest(seed, len(b"referenced payload"))
        file_entry = valid["files"][0]
        other_digest = digest(b"different payload")
        wrong_shard = "00" if seed[:2] != "00" else "ff"

        cases: dict[str, bytes] = {
            "duplicate_key": b'{"schemaVersion":1,"schemaVersion":1,"version":"1.0.0"}',
            "nan": b'{"schemaVersion":NaN,"version":"1.0.0"}',
            "infinity": b'{"schemaVersion":Infinity,"version":"1.0.0"}',
            "invalid_utf8": b'{"schemaVersion":1,"version":"1.0.0","notes":"\xff"}',
            "lone_surrogate": b'{"schemaVersion":1,"version":"1.0.0","notes":"\\ud800"}',
            "raw_control": b'{"schemaVersion":1,"version":"1.0.0","notes":"bad\x01value"}',
            "wrong_schema": json.dumps({**valid, "schemaVersion": 2}).encode(),
            "schema_float": json.dumps({**valid, "schemaVersion": 1.0}).encode(),
            "root_wrong_type": b"[]",
            "missing_version": json.dumps({key: value for key, value in valid.items() if key != "version"}).encode(),
            "unknown_root_key": json.dumps({**valid, "unexpected": True}).encode(),
            "optional_string_wrong_type": json.dumps({**valid, "notes": 123}).encode(),
            "version_non_ascii_prerelease": json.dumps({**valid, "version": "1.2.3-ª"}).encode(),
            "version_non_ascii_build": json.dumps({**valid, "version": "1.2.3+ª"}).encode(),
            "version_non_ascii_core": json.dumps({**valid, "version": "ª.2.3"}).encode(),
            "version_huge_core": json.dumps({**valid, "version": "9" * 100000 + ".2.3"}).encode(),
            "boolean_wrong_type": json.dumps({**valid, "mandatory": 1}).encode(),
            "files_wrong_type": json.dumps({**valid, "files": {}}).encode(),
            "unknown_file_key": json.dumps(
                {**valid, "files": [{**file_entry, "unexpected": True}]}
            ).encode(),
            "hash_format": json.dumps({**valid, "files": [{**file_entry, "sha256": seed.upper()}]}).encode(),
            "path_wrong_type": json.dumps({**valid, "files": [{**file_entry, "path": 123}]}).encode(),
            "local_path_wrong_type": json.dumps(
                {**valid, "files": [{**file_entry, "localPath": 123}]}
            ).encode(),
            "size_string": json.dumps({**valid, "files": [{**file_entry, "size": "18"}]}).encode(),
            "size_boolean": json.dumps({**valid, "files": [{**file_entry, "size": True}]}).encode(),
            "size_fractional": json.dumps({**valid, "files": [{**file_entry, "size": 1.5}]}).encode(),
            "size_negative": json.dumps({**valid, "files": [{**file_entry, "size": -1}]}).encode(),
            "size_over_limit": json.dumps(
                {**valid, "files": [{**file_entry, "size": 8 * 1024 * 1024 * 1024 + 1}]}
            ).encode(),
            "remove_item_wrong_type": json.dumps({**valid, "remove": [123]}).encode(),
            "unsafe_local_path": json.dumps(
                {**valid, "files": [{**file_entry, "localPath": "../escape.bin"}]}
            ).encode(),
            "malformed_object_path": json.dumps(
                {**valid, "files": [{**file_entry, "path": f"{OBJECT_PREFIX}/{seed}"}]}
            ).encode(),
            "object_prefix_without_suffix": json.dumps(
                {**valid, "files": [{**file_entry, "path": OBJECT_PREFIX}]}
            ).encode(),
            "shard_mismatch": json.dumps(
                {**valid, "files": [{**file_entry, "path": f"{OBJECT_PREFIX}/{wrong_shard}/{seed}"}]}
            ).encode(),
            "digest_mismatch": json.dumps(
                {
                    **valid,
                    "files": [
                        {
                            **file_entry,
                            "path": f"{OBJECT_PREFIX}/{other_digest[:2]}/{other_digest}",
                        }
                    ],
                }
            ).encode(),
            "conflicting_shared_source": json.dumps(
                {
                    **valid,
                    "files": [file_entry]
                    + [
                        {
                            "path": "shared/payload.bin",
                            "localPath": "bin/first.bin",
                            "sha256": seed,
                            "size": 1,
                        },
                        {
                            "path": "shared/payload.bin",
                            "localPath": "bin/second.bin",
                            "sha256": other_digest,
                            "size": 2,
                        },
                    ],
                }
            ).encode(),
            "ancestor_source_with_interleaved_sibling": json.dumps(
                {
                    **valid,
                    "files": [file_entry]
                    + [
                        {
                            "path": source,
                            "localPath": f"bin/source-{index}.bin",
                            "sha256": other_digest,
                            "size": 2,
                        }
                        for index, source in enumerate(("a", "a-", "a/child"))
                    ],
                }
            ).encode(),
        }

        for name, manifest_bytes in cases.items():
            with self.subTest(name=name):
                self.assert_validation_failure_preserves_store(name, manifest_bytes)

    def test_shared_json_conformance_corpus(self) -> None:
        for line_number, raw_line in enumerate(JSON_CORPUS.read_text(encoding="ascii").splitlines(), 1):
            if not raw_line or raw_line.startswith("#"):
                continue
            parts = raw_line.split("|")
            self.assertEqual(len(parts), 3, msg=f"corpus line {line_number}")
            expectation, name, encoded = parts
            self.assertIn(expectation, {"accept", "reject"}, msg=f"corpus line {line_number}")
            payload = bytes.fromhex(encoded)
            path = self.root / f"corpus-{line_number}.json"
            path.write_bytes(payload)
            accepted = True
            try:
                parsed = GC_MODULE.read_json_object(path)
                self.assertIsInstance(parsed, dict)
            except GC_MODULE.ValidationError:
                accepted = False
            self.assertEqual(accepted, expectation == "accept", msg=name)

    def test_json_resource_limits_match_library_defaults(self) -> None:
        cases = {
            "depth": b'{"v":' + b"[" * 64 + b"0" + b"]" * 64 + b"}",
            "container": b'{"v":[' + b",".join([b"0"] * 10001) + b"]}",
            "nodes": json.dumps({"v": [[None] * 1000 for _ in range(100)]}, separators=(",", ":")).encode(),
            "string": b'{"v":"' + b"a" * (1024 * 1024 + 1) + b'"}',
            "number": b'{"v":' + b"1" * 129 + b"}",
        }
        for name, payload in cases.items():
            with self.subTest(name=name):
                path = self.root / f"resource-{name}.json"
                path.write_bytes(payload)
                with self.assertRaises(GC_MODULE.ValidationError):
                    GC_MODULE.read_json_object(path)

    def test_all_manifests_are_validated_before_delete(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        valid_manifest = self.root / "valid.json"
        invalid_manifest = self.root / "invalid.json"
        self.write_json(valid_manifest, self.valid_manifest(sha256, len(b"referenced payload")))
        invalid_manifest.write_bytes(b'{"schemaVersion":1,"version":"1.0.0","version":"2.0.0"}')

        result = self.run_gc(object_root, [valid_manifest, invalid_manifest])

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())

    def test_invalid_prefix_fails_before_delete(self) -> None:
        case_root = self.root / "invalid_prefix"
        case_root.mkdir()
        object_root, sha256, referenced_path, stale_path = self.make_store(case_root)
        manifest = case_root / "manifest.json"
        self.write_json(manifest, self.valid_manifest(sha256, len(b"referenced payload")))

        result = self.run_gc(object_root, [manifest], object_prefix="../objects/sha256")

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())

    def test_object_prefix_case_variant_fails_before_delete(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        manifest = self.root / "manifest.json"
        value = self.valid_manifest(sha256, len(b"referenced payload"))
        other = digest(b"case-variant object path")
        value["files"].append(
            {
                "path": f"Objects/sha256/{other[:2]}/{other}",
                "localPath": "bin/case-variant.bin",
                "sha256": other,
                "size": 1,
            }
        )
        self.write_json(manifest, value)

        result = self.run_gc(object_root, [manifest])

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())

    def test_invalid_object_store_layout_fails_before_delete(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        malformed = object_root / "not-an-object.txt"
        malformed.write_bytes(b"metadata")
        (object_root / "not-a-shard").mkdir()
        manifest = self.root / "manifest.json"
        self.write_json(manifest, self.valid_manifest(sha256, len(b"referenced payload")))

        result = self.run_gc(object_root, [manifest])

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())
        self.assertTrue(malformed.exists())
        self.assertTrue((object_root / "not-a-shard").exists())

    def test_missing_referenced_object_fails_before_delete(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        manifest = self.root / "manifest.json"
        self.write_json(manifest, self.valid_manifest(sha256, len(b"referenced payload")))
        referenced_path.unlink()

        result = self.run_gc(object_root, [manifest])

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(stale_path.exists())

    def test_referenced_object_size_and_digest_are_verified_before_delete(self) -> None:
        for name, corrupt_contents, declared_size in (
            ("size", b"referenced payload", len(b"referenced payload") + 1),
            ("digest", b"X" * len(b"referenced payload"), len(b"referenced payload")),
        ):
            with self.subTest(name=name):
                case_root = self.root / name
                case_root.mkdir()
                object_root, sha256, referenced_path, stale_path = self.make_store(case_root)
                referenced_path.write_bytes(corrupt_contents)
                manifest = case_root / "manifest.json"
                self.write_json(manifest, self.valid_manifest(sha256, declared_size))

                result = self.run_gc(object_root, [manifest])

                self.assertNotEqual(result.returncode, 0, msg=result.stdout + result.stderr)
                self.assertTrue(referenced_path.exists())
                self.assertTrue(stale_path.exists())

    def test_symlink_or_junction_shard_cannot_escape_object_root(self) -> None:
        object_root, sha256, referenced_path, stale_path = self.make_store(self.root)
        manifest = self.root / "manifest.json"
        self.write_json(manifest, self.valid_manifest(sha256, len(b"referenced payload")))
        outside = self.root / "outside"
        outside.mkdir()
        outside_object = outside / ("aa" + "0" * 62)
        outside_object.write_bytes(b"must survive")
        link = object_root / "aa"
        if os.name == "nt":
            created = subprocess.run(
                ["cmd.exe", "/c", "mklink", "/J", str(link), str(outside)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(created.returncode, 0, msg=created.stdout + created.stderr)
        else:
            os.symlink(outside, link, target_is_directory=True)

        result = self.run_gc(object_root, [manifest])

        self.assertNotEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertTrue(outside_object.exists())
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())

    def test_object_identity_is_rechecked_before_delete(self) -> None:
        object_root, _, _, stale_path = self.make_store(self.root)
        with GC_MODULE.ObjectStore(object_root.absolute()) as store:
            objects = store.scan()
            stale_relative = stale_path.relative_to(object_root).as_posix()
            record = objects[stale_relative]
            stale_path.unlink()
            stale_path.write_bytes(b"replacement with a different identity and size")

            with self.assertRaises(GC_MODULE.ValidationError):
                store.delete(record)

        self.assertTrue(stale_path.exists())

    def test_object_replacement_between_check_and_delete_is_not_deleted(self) -> None:
        object_root, _, _, stale_path = self.make_store(self.root)
        replacement = b"replacement published after the identity check"
        with GC_MODULE.ObjectStore(object_root.absolute()) as store:
            objects = store.scan()
            stale_relative = stale_path.relative_to(object_root).as_posix()
            record = objects[stale_relative]
            current_info = store._current_info
            swapped = False

            def swap_after_check(candidate: Any, guard: Any) -> os.stat_result:
                nonlocal swapped
                info = current_info(candidate, guard)
                if not swapped:
                    stale_path.unlink()
                    stale_path.write_bytes(replacement)
                    swapped = True
                return info

            store._current_info = swap_after_check
            with self.assertRaises(GC_MODULE.ValidationError):
                store.delete(record)

        self.assertEqual(stale_path.read_bytes(), replacement)

    def test_quarantine_delete_failure_restores_original_object(self) -> None:
        object_root, _, _, stale_path = self.make_store(self.root)
        original_contents = stale_path.read_bytes()
        unlink = GC_MODULE.os.unlink

        def fail_quarantine_unlink(path: Any, *args: Any, **kwargs: Any) -> None:
            if Path(path).name.startswith(".gc-delete-"):
                raise OSError("injected quarantine deletion failure")
            unlink(path, *args, **kwargs)

        with GC_MODULE.ObjectStore(object_root.absolute()) as store:
            objects = store.scan()
            stale_relative = stale_path.relative_to(object_root).as_posix()
            GC_MODULE.os.unlink = fail_quarantine_unlink
            try:
                with self.assertRaises(GC_MODULE.ValidationError):
                    store.delete(objects[stale_relative])
            finally:
                GC_MODULE.os.unlink = unlink

        self.assertEqual(stale_path.read_bytes(), original_contents)
        self.assertEqual(list(stale_path.parent.glob(".gc-delete-*")), [])

    def test_shard_replacement_between_check_and_open_is_rejected(self) -> None:
        object_root, _, _, stale_path = self.make_store(self.root)
        stale_shard = stale_path.parent.name
        replacement_parent = self.root / "replacement"
        replacement_shard = replacement_parent / stale_shard
        replacement_shard.mkdir(parents=True)
        original_shard = object_root / stale_shard
        displaced_shard = self.root / "displaced-shard"

        with GC_MODULE.ObjectStore(object_root.absolute()) as store:
            new_shard_guard = store._new_shard_guard
            replaced = False

            def replace_before_open(shard: str) -> Any:
                nonlocal replaced
                if shard == stale_shard and not replaced:
                    original_shard.rename(displaced_shard)
                    replacement_shard.rename(original_shard)
                    replaced = True
                return new_shard_guard(shard)

            store._new_shard_guard = replace_before_open
            with self.assertRaises(GC_MODULE.ValidationError):
                store.scan()

    def test_root_replacement_between_check_and_open_is_rejected(self) -> None:
        object_root, _, _, _ = self.make_store(self.root)
        original_root = object_root.absolute()
        displaced_root = self.root / "displaced-root"
        replacement_root = self.root / "replacement-root"
        replacement_root.mkdir()
        store = GC_MODULE.ObjectStore(original_root)
        open_root_guard = store.root_guard.open

        def replace_before_open() -> None:
            original_root.rename(displaced_root)
            replacement_root.rename(original_root)
            open_root_guard()

        store.root_guard.open = replace_before_open
        with self.assertRaises(GC_MODULE.ValidationError):
            store.__enter__()
        self.assertIsNone(store.root_guard.fd)
        self.assertIsNone(store.root_guard.handle)

    def test_all_sha256_shards_do_not_exhaust_directory_handles(self) -> None:
        object_root = self.root / "objects"
        object_root.mkdir()
        for value in range(256):
            (object_root / f"{value:02x}").mkdir()

        with GC_MODULE.ObjectStore(object_root.absolute()) as store:
            self.assertEqual(store.scan(), {})

        renamed_root = self.root / "objects-after-scan"
        object_root.rename(renamed_root)
        renamed_root.rename(object_root)

    @unittest.skipUnless(os.name == "nt", "ancestor junction rebinding is Windows-specific")
    def test_ancestor_junction_is_rejected(self) -> None:
        real_parent = self.root / "real-parent"
        real_parent.mkdir()
        object_root, sha256, referenced_path, stale_path = self.make_store(real_parent)
        manifest = self.root / "manifest.json"
        self.write_json(manifest, self.valid_manifest(sha256, len(b"referenced payload")))
        junction = self.root / "store-link"
        created = subprocess.run(
            ["cmd.exe", "/c", "mklink", "/J", str(junction), str(real_parent)],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(created.returncode, 0, msg=created.stdout + created.stderr)

        result = self.run_gc(junction / object_root.name, [manifest])

        self.assertNotEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        self.assertTrue(referenced_path.exists())
        self.assertTrue(stale_path.exists())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gc", required=True, type=Path)
    parser.add_argument("--json-corpus", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    global GC_SCRIPT, GC_MODULE, JSON_CORPUS, WORK_DIR
    GC_SCRIPT = args.gc.resolve()
    JSON_CORPUS = args.json_corpus.resolve()
    WORK_DIR = args.work_dir.resolve()
    module_spec = importlib.util.spec_from_file_location("gc_objects_under_test", GC_SCRIPT)
    if module_spec is None or module_spec.loader is None:
        raise RuntimeError("cannot load gc_objects.py")
    GC_MODULE = importlib.util.module_from_spec(module_spec)
    sys.modules[module_spec.name] = GC_MODULE
    module_spec.loader.exec_module(GC_MODULE)
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    WORK_DIR.mkdir(parents=True)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(GcObjectsTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
