#!/usr/bin/env python3
"""Generate a libAutoUpdater release manifest from a release directory."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import shutil
from pathlib import Path
from typing import Iterable


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize(path: Path) -> str:
    return path.as_posix()


def included(path: str, patterns: Iterable[str]) -> bool:
    patterns = list(patterns)
    if not patterns:
        return True
    return any(fnmatch.fnmatch(path, pattern) or path.startswith(pattern.rstrip("/") + "/") for pattern in patterns)


def excluded(path: str, patterns: Iterable[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) or path.startswith(pattern.rstrip("/") + "/") for pattern in patterns)


def object_manifest_path(sha256: str, object_prefix: str) -> str:
    return f"{object_prefix.strip('/')}/{sha256[:2]}/{sha256}"


def copy_object(source: Path, sha256: str, object_root: Path) -> Path:
    target = object_root / sha256[:2] / sha256
    if target.exists():
        if sha256_file(target) != sha256:
            raise SystemExit(f"Object hash collision or corrupted object: {target}")
        return target

    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return target


def build_manifest(args: argparse.Namespace) -> dict:
    release_dir = args.release_dir.resolve()
    if not release_dir.is_dir():
        raise SystemExit(f"Release directory does not exist: {release_dir}")

    object_root = args.object_root
    if args.content_addressed:
        object_root = (object_root or (release_dir.parent / "objects" / "sha256")).resolve()
        object_root.mkdir(parents=True, exist_ok=True)

    files = []
    for root, _, names in os.walk(release_dir):
        for name in names:
            path = Path(root) / name
            rel = normalize(path.relative_to(release_dir))
            if rel == "manifest.json" or rel == "manifest.sig":
                continue
            if not included(rel, args.include):
                continue
            if excluded(rel, args.exclude):
                continue
            digest = sha256_file(path)
            size = path.stat().st_size
            if args.content_addressed:
                object_path = copy_object(path, digest, object_root)
                files.append(
                    {
                        "path": object_manifest_path(digest, args.object_prefix),
                        "localPath": rel,
                        "sha256": digest,
                        "size": size,
                    }
                )
                continue

            files.append(
                {
                    "path": rel,
                    "sha256": digest,
                    "size": size,
                }
            )

    files.sort(key=lambda item: item["path"])

    manifest = {
        "schemaVersion": 1,
        "appId": args.app_id,
        "channel": args.channel,
        "platform": args.platform,
        "arch": args.arch,
        "version": args.version,
        "releaseId": args.release_id or args.version,
        "releaseDate": args.release_date,
        "publishedAt": args.published_at or args.release_date,
        "expiresAt": args.expires_at,
        "minVersion": args.min_version,
        "minClientVersion": args.min_client_version,
        "mandatory": args.mandatory,
        "allowDowngrade": args.allow_downgrade,
        "notes": args.notes,
        "baseUrl": args.base_url,
        "files": files,
        "remove": args.remove,
    }
    return {key: value for key, value in manifest.items() if value not in (None, "", [])}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_dir", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--app-id", required=True)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--platform", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--release-id", default="")
    parser.add_argument("--release-date", required=True, help="UTC ISO-8601 timestamp")
    parser.add_argument("--published-at", default="")
    parser.add_argument("--expires-at", default="")
    parser.add_argument("--min-version", default="")
    parser.add_argument("--min-client-version", default="0.1.0")
    parser.add_argument("--mandatory", action="store_true")
    parser.add_argument("--allow-downgrade", action="store_true")
    parser.add_argument("--notes", default="")
    parser.add_argument("--base-url", required=True)
    parser.add_argument(
        "--content-addressed",
        action="store_true",
        help="Copy files into an object store and emit manifest path/localPath pairs",
    )
    parser.add_argument(
        "--object-root",
        type=Path,
        default=None,
        help="Object store root for --content-addressed; defaults to <release_dir>/../objects/sha256",
    )
    parser.add_argument(
        "--object-prefix",
        default="objects/sha256",
        help="URL path prefix for object files in generated manifest entries",
    )
    parser.add_argument("--include", action="append", default=[], help="Glob or directory prefix to include")
    parser.add_argument("--exclude", action="append", default=[], help="Glob or directory prefix to exclude")
    parser.add_argument("--remove", action="append", default=[], help="Managed path to remove during apply")
    args = parser.parse_args()

    manifest = build_manifest(args)
    output = args.output or (args.release_dir / "manifest.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"Wrote {output}")
    print("Upload the release directory to:")
    print(f"  {args.base_url}")
    print("Client manifest URL should point to:")
    print(f"  {args.base_url.rstrip('/')}/manifest.json")
    if args.content_addressed:
        print("Object store root:")
        print(f"  {(args.object_root or (args.release_dir.resolve().parent / 'objects' / 'sha256')).resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
