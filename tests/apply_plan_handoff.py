#!/usr/bin/env python3

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def run(updater: Path, plan_path: Path, digest: str, install_root: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            str(updater),
            "--plan",
            str(plan_path),
            "--plan-sha256",
            digest,
            "--install-root",
            str(install_root),
            "--pid",
            "0",
            "--wait",
            "1",
            *extra,
        ],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--updater", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    shutil.rmtree(args.work_dir, ignore_errors=True)
    install = args.work_dir / "install"
    staging = args.work_dir / "staging"
    backup = args.work_dir / "backup"
    install.mkdir(parents=True)
    staging.mkdir(parents=True)
    sentinel = install / "sentinel.txt"
    sentinel.write_text("must remain\n", encoding="utf-8")

    plan = {
        "schemaVersion": 2,
        "intent": "install",
        "installDir": str(install).replace("\\", "/"),
        "stagingDir": str(staging).replace("\\", "/"),
        "backupDir": str(backup).replace("\\", "/"),
        "restartCommand": [],
        "operations": [{"type": "remove", "target": "sentinel.txt", "size": 0}],
    }
    plan_bytes = json.dumps(plan, indent=2, sort_keys=True).encode("utf-8")
    plan_path = args.work_dir / "apply-plan.json"
    plan_path.write_bytes(plan_bytes)
    digest = hashlib.sha256(plan_bytes).hexdigest()

    wrong_digest = run(args.updater, plan_path, "0" * 64, install)
    if wrong_digest.returncode == 0 or sentinel.read_text(encoding="utf-8") != "must remain\n":
        raise RuntimeError("updater accepted a plan that did not match its launcher digest")

    wrong_root = run(args.updater, plan_path, digest, args.work_dir / "other-install")
    if wrong_root.returncode == 0 or sentinel.read_text(encoding="utf-8") != "must remain\n":
        raise RuntimeError("updater accepted a plan outside its launcher-bound install root")

    wrong_mode = run(args.updater, plan_path, digest, install, "--rollback")
    if wrong_mode.returncode == 0 or sentinel.read_text(encoding="utf-8") != "must remain\n":
        raise RuntimeError("updater accepted an install plan through rollback mode")

    installed = run(args.updater, plan_path, digest, install)
    if installed.returncode != 0 or sentinel.exists():
        raise RuntimeError(f"valid install handoff failed: {installed.stderr}")

    terminal_path = install / ".autoupdater" / "journal" / "terminal.json"
    terminal = json.loads(terminal_path.read_text(encoding="utf-8"))
    rollback = {
        "schemaVersion": 2,
        "intent": "rollback",
        "rollbackOf": {
            "transactionId": terminal["transactionId"],
            "planDigest": terminal["planDigest"],
        },
        "installDir": str(install).replace("\\", "/"),
        "stagingDir": str(backup).replace("\\", "/"),
        "backupDir": str(
            install / ".autoupdater" / "backup" / "rollback" / terminal["transactionId"]
        ).replace("\\", "/"),
        "restartCommand": [],
        "operations": [],
    }
    rollback_bytes = json.dumps(rollback, indent=2, sort_keys=True).encode("utf-8")
    rollback_path = args.work_dir / "rollback-plan.json"
    rollback_path.write_bytes(rollback_bytes)
    rollback_digest = hashlib.sha256(rollback_bytes).hexdigest()

    rolled_back = run(args.updater, rollback_path, rollback_digest, install, "--rollback")
    if rolled_back.returncode != 0 or sentinel.read_text(encoding="utf-8") != "must remain\n":
        raise RuntimeError(f"valid rollback handoff failed: {rolled_back.stderr}")

    replayed = run(args.updater, rollback_path, rollback_digest, install, "--rollback")
    if replayed.returncode != 0 or sentinel.read_text(encoding="utf-8") != "must remain\n":
        raise RuntimeError(f"idempotent rollback handoff failed: {replayed.stderr}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
