#!/usr/bin/env python3
"""Exercise the full libAutoUpdater static-file update flow."""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path


def platform_name() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "macos"
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    return system


def arch_name() -> str:
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64"}:
        return "x64"
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86", "i386", "i686"}:
        return "x86"
    return machine


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def path_uri(path: Path) -> str:
    return path.resolve().as_uri()


def assert_file(path: Path, expected: str) -> None:
    actual = path.read_text(encoding="utf-8")
    if actual != expected:
        raise AssertionError(f"{path} expected {expected!r}, got {actual!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--updater", type=Path, required=True)
    parser.add_argument("--make-manifest", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    root = args.work_dir.resolve()
    if root.exists():
        shutil.rmtree(root, ignore_errors=True)
    install_dir = root / "install"
    release_dir = root / "release"
    install_dir.mkdir(parents=True)
    release_dir.mkdir(parents=True)

    write_text(install_dir / "bin" / "app.txt", "version 1\n")
    write_text(install_dir / "config" / "unchanged.txt", "keep me\n")
    write_text(install_dir / "obsolete.txt", "remove me\n")

    write_text(release_dir / "bin" / "app.txt", "version 2\n")
    write_text(release_dir / "config" / "unchanged.txt", "keep me\n")
    write_text(release_dir / "data" / "new.txt", "new payload\n")

    subprocess.run(
        [
            sys.executable,
            str(args.make_manifest),
            str(release_dir),
            "--app-id",
            "libAutoUpdater.example",
            "--channel",
            "stable",
            "--platform",
            platform_name(),
            "--arch",
            arch_name(),
            "--version",
            "2.0.0",
            "--release-date",
            "2026-06-01T10:00:00Z",
            "--base-url",
            path_uri(release_dir) + "/",
            "--remove",
            "obsolete.txt",
        ],
        check=True,
        text=True,
    )

    result = subprocess.run(
        [
            str(args.cli),
            "--manifest",
            path_uri(release_dir / "manifest.json"),
            "--version",
            "1.0.0",
            "--install",
            str(install_dir),
            "--updater",
            str(args.updater),
            "--apply",
        ],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        return result.returncode

    if "updateAvailable=true" not in result.stdout or "readyToApply=true" not in result.stdout:
        print(result.stdout)
        raise AssertionError("CLI did not report an available update that was ready to apply")
    if "config/unchanged.txt" in result.stdout:
        print(result.stdout)
        raise AssertionError("unchanged file was unexpectedly downloaded")

    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            assert_file(install_dir / "bin" / "app.txt", "version 2\n")
            assert_file(install_dir / "config" / "unchanged.txt", "keep me\n")
            assert_file(install_dir / "data" / "new.txt", "new payload\n")
            if not (install_dir / "obsolete.txt").exists():
                return 0
        except (AssertionError, FileNotFoundError):
            pass
        time.sleep(0.2)

    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    journal_dir = install_dir / ".autoupdater" / "journal"
    if journal_dir.exists():
        for journal in journal_dir.glob("*.json"):
            print(f"{journal}:\n{journal.read_text(encoding='utf-8')}")
    raise AssertionError("update was not applied before timeout")


if __name__ == "__main__":
    raise SystemExit(main())
