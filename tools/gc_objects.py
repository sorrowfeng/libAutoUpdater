#!/usr/bin/env python3
"""Remove unreferenced content-addressed objects from a libAutoUpdater object store."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def normalize(path: Path) -> str:
    return path.as_posix()


def referenced_objects(manifest_paths: list[Path], object_prefix: str) -> set[str]:
    referenced: set[str] = set()
    prefix = object_prefix.strip("/") + "/"
    for manifest_path in manifest_paths:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        for item in data.get("files", []):
            path = item.get("path", "")
            if not isinstance(path, str) or not path.startswith(prefix):
                continue
            rel = path[len(prefix):]
            parts = rel.split("/")
            if len(parts) == 2 and parts[0] and parts[1]:
                referenced.add(rel)
    return referenced


def object_files(object_root: Path) -> list[Path]:
    if not object_root.exists():
        raise SystemExit(f"Object root does not exist: {object_root}")
    return sorted(path for path in object_root.rglob("*") if path.is_file())


def prune_empty_dirs(root: Path) -> None:
    for path in sorted((p for p in root.rglob("*") if p.is_dir()), reverse=True):
        try:
            path.rmdir()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object_root", type=Path)
    parser.add_argument("--manifest", action="append", type=Path, required=True)
    parser.add_argument("--object-prefix", default="objects/sha256")
    parser.add_argument("--delete", action="store_true", help="Delete unreferenced objects; default is dry-run")
    args = parser.parse_args()

    object_root = args.object_root.resolve()
    referenced = referenced_objects(args.manifest, args.object_prefix)

    stale: list[Path] = []
    for path in object_files(object_root):
        rel = normalize(path.relative_to(object_root))
        if rel not in referenced:
            stale.append(path)

    action = "Deleting" if args.delete else "Would delete"
    for path in stale:
        print(f"{action} {path}")
        if args.delete:
            path.unlink()

    if args.delete:
        prune_empty_dirs(object_root)

    print(f"Referenced objects: {len(referenced)}")
    print(f"Unreferenced objects: {len(stale)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
