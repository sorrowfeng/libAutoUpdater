#!/usr/bin/env python3
"""Validate that release version metadata is consistent."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def cmake_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\s*\(\s*libAutoUpdater\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text, re.S)
    if not match:
        raise SystemExit("Could not find project VERSION in CMakeLists.txt")
    return match.group(1)


def vcpkg_version() -> str:
    data = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))
    return data["version-string"]


def changelog_has(tag: str) -> bool:
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    return re.search(rf"^##\s+{re.escape(tag)}\b", text, re.M) is not None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default="", help="Expected release tag, for example v1.2.3")
    parser.add_argument("--version-only", action="store_true", help="Only compare CMake and vcpkg versions")
    args = parser.parse_args()

    project = cmake_version()
    manifest = vcpkg_version()
    if project != manifest:
        raise SystemExit(f"Version mismatch: CMake={project}, vcpkg={manifest}")

    if not args.version_only:
        tag = args.tag or f"v{project}"
        if tag != f"v{project}":
            raise SystemExit(f"Tag {tag} does not match project version {project}")
        if not changelog_has(tag):
            raise SystemExit(f"CHANGELOG.md does not contain a {tag} section")

    print(f"Release metadata OK: v{project}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
