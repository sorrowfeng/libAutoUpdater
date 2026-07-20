#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def run(updater: pathlib.Path, work_dir: pathlib.Path, extra: list[str]) -> subprocess.CompletedProcess[str]:
    command = [
        str(updater),
        "--plan",
        str(work_dir / "missing-apply-plan.json"),
        "--plan-sha256",
        "0" * 64,
        "--install-root",
        str(work_dir / "install"),
        *extra,
    ]
    return subprocess.run(command, capture_output=True, text=True, timeout=5, check=False)


def require_exit(result: subprocess.CompletedProcess[str], expected: int, case: list[str]) -> None:
    if result.returncode != expected:
        raise RuntimeError(
            f"argument case {case!r} returned {result.returncode}, expected {expected}; "
            f"stdout={result.stdout!r}, stderr={result.stderr!r}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--updater", type=pathlib.Path, required=True)
    parser.add_argument("--work-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)

    invalid_cases = [
        ["--pid", ""],
        ["--pid", "-1"],
        ["--pid", "+1"],
        ["--pid", "1x"],
        ["--pid", "18446744073709551615"],
        ["--pid"],
        ["--pid", "0", "--pid", "0"],
        ["--wait", ""],
        ["--wait", "-1"],
        ["--wait", "+1"],
        ["--wait", "1x"],
        ["--wait", "86401"],
        ["--wait", "18446744073709551615"],
        ["--wait"],
        ["--wait", "0", "--wait", "0"],
        ["--rollback", "--rollback"],
        ["--plan", str(args.work_dir / "other-plan.json")],
        ["--plan-sha256", "0" * 64],
        ["--install-root", str(args.work_dir / "other-install")],
        ["--unknown"],
    ]
    for case in invalid_cases:
        require_exit(run(args.updater, args.work_dir, case), 2, case)

    # Exit 4 proves that parsing and the zero-PID wait completed and execution
    # advanced to opening the intentionally absent plan.
    operation_cases = (
        ([], "Apply"),
        (["--pid", "0", "--wait", "0"], "Apply"),
        (["--wait", "86400"], "Apply"),
        (["--rollback"], "Rollback"),
    )
    for case, phase in operation_cases:
        result = run(args.updater, args.work_dir, case)
        require_exit(result, 4, case)
        expected = f"phase={phase} code=ApplyFailed"
        if result.stderr.strip() != expected:
            raise RuntimeError(
                f"argument case {case!r} emitted {result.stderr!r}, expected {expected!r}"
            )
        if str(args.work_dir) in result.stderr:
            raise RuntimeError(f"argument case {case!r} leaked its working path: {result.stderr!r}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # pragma: no cover - CTest reports the message.
        print(error, file=sys.stderr)
        raise
