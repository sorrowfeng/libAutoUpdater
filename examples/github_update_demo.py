#!/usr/bin/env python3
"""Run a real update demo using this GitHub repository as the update server."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_REPO = "sorrowfeng/libAutoUpdater"
DEFAULT_BRANCH = "main"
RELEASE_PATH = "examples/update-server/releases/2.0.0"
APP_ID = "libAutoUpdater.example"

OLD_APP = """libAutoUpdater demo application
version=1.0.0
served-from=local-install
"""

NEW_APP = """libAutoUpdater demo application
version=2.0.0
served-from=github-raw
"""

UNCHANGED_SETTINGS = """{
  "channel": "stable",
  "theme": "system",
  "unchangedAcrossUpdate": true
}
"""


def default_manifest_url(repo: str, branch: str) -> str:
    return f"https://raw.githubusercontent.com/{repo}/{branch}/{RELEASE_PATH}/manifest.json"


def default_build_path(root: Path, *parts: str) -> Path:
    return root.joinpath("build", *parts)


def first_existing(candidates: list[Path]) -> Path:
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def default_cli(root: Path) -> Path:
    if sys.platform == "win32":
        return first_existing([
            default_build_path(root, "examples", "cli", "Debug", "libAutoUpdater_cli.exe"),
            default_build_path(root, "examples", "cli", "Release", "libAutoUpdater_cli.exe"),
        ])
    return default_build_path(root, "examples", "cli", "libAutoUpdater_cli")


def default_updater(root: Path) -> Path:
    if sys.platform == "win32":
        return first_existing([
            default_build_path(root, "updater", "Debug", "autoupdater_apply.exe"),
            default_build_path(root, "updater", "Release", "autoupdater_apply.exe"),
        ])
    return default_build_path(root, "updater", "autoupdater_apply")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def print_tree(root: Path) -> None:
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if rel.startswith(".autoupdater/") or rel == ".autoupdater":
            continue
        if path.is_file():
            print(f"  {rel}")


def print_file(root: Path, rel: str) -> None:
    path = root / rel
    print(f"\n--- {rel} ---")
    if path.exists():
        print(path.read_text(encoding="utf-8").rstrip())
    else:
        print("<missing>")


def assert_text(path: Path, expected: str) -> None:
    actual = path.read_text(encoding="utf-8")
    if actual != expected:
        raise AssertionError(f"{path} expected {expected!r}, got {actual!r}")


def create_old_install(install_dir: Path) -> None:
    if install_dir.exists():
        shutil.rmtree(install_dir)
    write_text(install_dir / "bin" / "demo_app.txt", OLD_APP)
    write_text(install_dir / "config" / "settings.json", UNCHANGED_SETTINGS)
    write_text(install_dir / "legacy" / "remove-me.txt", "This file should be removed by the update.\n")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub owner/repo used by raw.githubusercontent.com")
    parser.add_argument("--branch", default=DEFAULT_BRANCH)
    parser.add_argument("--manifest-url", default="")
    parser.add_argument("--work-dir", type=Path, default=repo_root / "demo-work" / "github-update")
    parser.add_argument("--cli", type=Path, default=default_cli(repo_root))
    parser.add_argument("--updater", type=Path, default=default_updater(repo_root))
    args = parser.parse_args()

    manifest_url = args.manifest_url or default_manifest_url(args.repo, args.branch)
    install_dir = args.work_dir.resolve() / "install"

    print("GitHub-hosted update demo")
    print(f"  manifest: {manifest_url}")
    print(f"  install:  {install_dir}")
    print(f"  cli:      {args.cli}")
    print(f"  updater:  {args.updater}")

    if not args.cli.exists():
        print("\nCLI executable was not found. Build the project first:")
        print("  cmake -S . -B build")
        print("  cmake --build build --config Debug")
        return 2
    if not args.updater.exists():
        print("\nUpdater executable was not found. Enable LIBAUTOUPDATER_BUILD_UPDATER and rebuild.")
        return 2

    create_old_install(install_dir)

    print("\nBefore update")
    print_tree(install_dir)
    print_file(install_dir, "bin/demo_app.txt")
    print_file(install_dir, "legacy/remove-me.txt")

    command = [
        str(args.cli),
        "--manifest",
        manifest_url,
        "--version",
        "1.0.0",
        "--install",
        str(install_dir),
        "--updater",
        str(args.updater),
        "--apply",
    ]

    print("\nRunning updater CLI")
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)
    combined_output = result.stdout + result.stderr
    if result.returncode != 0 or "state=Failed" in combined_output:
        if "No HTTP network adapter is available" in combined_output:
            print("This build does not include libcurl, so it cannot fetch GitHub HTTPS URLs.")
            print("Reconfigure with LIBAUTOUPDATER_WITH_CURL=ON and make sure CMake finds CURL.")
        return result.returncode if result.returncode != 0 else 1

    deadline = time.time() + 20
    last_error = "update results are not visible yet"
    while time.time() < deadline:
        try:
            assert_text(install_dir / "bin" / "demo_app.txt", NEW_APP)
            assert_text(install_dir / "config" / "settings.json", UNCHANGED_SETTINGS)
            assert_text(install_dir / "resources" / "feature.txt", "New file delivered by libAutoUpdater 2.0.0.\n")
            if (install_dir / "legacy" / "remove-me.txt").exists():
                last_error = "legacy/remove-me.txt still exists"
            else:
                break
        except (AssertionError, FileNotFoundError):
            last_error = sys.exc_info()[1]
            time.sleep(0.2)
    else:
        print("\nTimed out while waiting for the external updater. Current install tree:")
        print_tree(install_dir)
        print_file(install_dir, "bin/demo_app.txt")
        print_file(install_dir, "config/settings.json")
        print_file(install_dir, "resources/feature.txt")
        print_file(install_dir, "legacy/remove-me.txt")
        raise AssertionError(f"The external updater did not finish before timeout: {last_error}")

    print("\nAfter update")
    print_tree(install_dir)
    print_file(install_dir, "bin/demo_app.txt")
    print_file(install_dir, "resources/feature.txt")
    print_file(install_dir, "legacy/remove-me.txt")
    print("\nDemo completed: files were downloaded from GitHub Raw and applied locally.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
