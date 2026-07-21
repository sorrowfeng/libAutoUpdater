#!/usr/bin/env python3
"""Apply and roll back an update with the installed updater executable."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path


def sha256_bytes(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def write_text(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(contents)


def write_plan(path: Path, document: dict[str, object]) -> str:
    contents = (json.dumps(document, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)
    return sha256_bytes(contents)


def run_helper(updater: Path, plan: Path, digest: str, install_dir: Path, rollback: bool = False) -> None:
    command = [
        str(updater),
        "--plan",
        str(plan),
        "--plan-sha256",
        digest,
        "--install-root",
        str(install_dir),
        "--pid",
        "0",
        "--wait",
        "0",
    ]
    if rollback:
        command.append("--rollback")
    result = subprocess.run(
        command, capture_output=True, encoding="utf-8", errors="replace", text=True, timeout=30
    )
    if result.returncode != 0:
        raise AssertionError(
            f"installed updater returned {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def wait_for_layout(install_dir: Path, expected: dict[str, str], absent: set[str]) -> None:
    deadline = time.monotonic() + 5
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            for relative, contents in expected.items():
                actual = (install_dir / relative).read_text(encoding="utf-8")
                if actual != contents:
                    raise AssertionError(f"{relative}: expected {contents!r}, got {actual!r}")
            for relative in absent:
                if (install_dir / relative).exists():
                    raise AssertionError(f"{relative} unexpectedly exists")
            return
        except (AssertionError, FileNotFoundError, PermissionError) as error:
            last_error = error
            time.sleep(0.05)
    raise AssertionError(f"installed update layout did not settle: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--updater", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    root = args.work_dir.resolve()
    shutil.rmtree(root, ignore_errors=True)
    install_dir = root / "install"
    staging_dir = root / "staging"
    backup_dir = install_dir / ".autoupdater" / "backup" / "forward"
    request_dir = root / "requests"
    install_dir.mkdir(parents=True)
    shutil.copytree(args.fixture.resolve(), staging_dir)

    write_text(install_dir / "bin" / "app.txt", "version 1\n")
    write_text(install_dir / "config" / "unchanged.txt", "keep me\n")
    write_text(install_dir / "obsolete.txt", "remove me\n")

    app_contents = (staging_dir / "bin" / "app.txt").read_bytes()
    new_contents = (staging_dir / "data" / "new.txt").read_bytes()
    release_id = "installed-package-release-2"
    install_plan = {
        "schemaVersion": 2,
        "intent": "install",
        "appId": "com.example.installed-package",
        "fromVersion": "1.0.0",
        "toVersion": "2.0.0",
        "releaseId": release_id,
        "manifestSha256": sha256_bytes(b"installed-package-manifest"),
        "installDir": str(install_dir),
        "stagingDir": str(staging_dir),
        "backupDir": str(backup_dir),
        "restartCommand": [],
        "operations": [
            {
                "type": "replace",
                "source": "bin/app.txt",
                "target": "bin/app.txt",
                "sha256": sha256_bytes(app_contents),
                "size": len(app_contents),
            },
            {
                "type": "replace",
                "source": "data/new.txt",
                "target": "data/new.txt",
                "sha256": sha256_bytes(new_contents),
                "size": len(new_contents),
            },
            {"type": "remove", "target": "obsolete.txt"},
        ],
    }
    install_plan_path = request_dir / "install-plan.json"
    install_digest = write_plan(install_plan_path, install_plan)
    run_helper(args.updater.resolve(), install_plan_path, install_digest, install_dir)

    wait_for_layout(
        install_dir,
        {
            "bin/app.txt": "version 2\n",
            "config/unchanged.txt": "keep me\n",
            "data/new.txt": "new payload\n",
        },
        {"obsolete.txt"},
    )

    terminal_path = install_dir / ".autoupdater" / "journal" / "terminal.json"
    terminal = json.loads(terminal_path.read_text(encoding="utf-8"))
    transaction_id = terminal["transactionId"]
    terminal_digest = terminal["planDigest"]
    plan_snapshot = install_dir / ".autoupdater" / "journal" / f"{transaction_id}.plan.json"
    if terminal_digest != sha256_bytes(plan_snapshot.read_bytes()):
        raise AssertionError("terminal receipt is not bound to its immutable plan snapshot")

    rollback_plan = {
        "schemaVersion": 2,
        "intent": "rollback",
        "rollbackOf": {"transactionId": transaction_id, "planDigest": terminal_digest},
        "appId": "com.example.installed-package",
        "fromVersion": "2.0.0",
        "releaseId": release_id,
        "installDir": str(install_dir),
        "stagingDir": str(backup_dir),
        "backupDir": str(install_dir / ".autoupdater" / "backup" / "rollback" / transaction_id),
        "restartCommand": [],
        "operations": [],
    }
    rollback_plan_path = request_dir / "rollback-plan.json"
    rollback_digest = write_plan(rollback_plan_path, rollback_plan)
    run_helper(args.updater.resolve(), rollback_plan_path, rollback_digest, install_dir, rollback=True)

    wait_for_layout(
        install_dir,
        {
            "bin/app.txt": "version 1\n",
            "config/unchanged.txt": "keep me\n",
            "obsolete.txt": "remove me\n",
        },
        {"data/new.txt"},
    )
    if (install_dir / ".autoupdater" / "journal" / "active.json").exists():
        raise AssertionError("installed rollback left an active transaction")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
