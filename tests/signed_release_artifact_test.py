#!/usr/bin/env python3
"""Generate, sign, and independently verify a release manifest artifact."""

from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(command: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        text=True,
        timeout=30,
    )


def require_success(result: subprocess.CompletedProcess[str], operation: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{operation} failed ({result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--make-manifest", required=True, type=Path)
    parser.add_argument("--sign-manifest", required=True, type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    root = args.work_dir.resolve()
    shutil.rmtree(root, ignore_errors=True)
    release_dir = root / "release"
    shutil.copytree(args.fixture.resolve(), release_dir)
    private_key = root / "private.pem"
    public_key = root / "public.pem"
    manifest = release_dir / "manifest.json"
    signature = release_dir / "manifest.json.sig"

    openssl = str(args.openssl.resolve())
    require_success(
        run([openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(private_key)]),
        "RSA key generation",
    )
    require_success(
        run([openssl, "pkey", "-in", str(private_key), "-pubout", "-out", str(public_key)]),
        "public-key export",
    )

    require_success(
        run(
            [
                sys.executable,
                str(args.make_manifest.resolve()),
                str(release_dir),
                "--app-id",
                "com.example.signed-artifact",
                "--platform",
                "linux",
                "--arch",
                "x64",
                "--version",
                "2.0.0",
                "--release-date",
                "2026-07-20T00:00:00Z",
                "--base-url",
                "https://updates.example.test/releases/2.0.0/",
            ]
        ),
        "manifest generation",
    )

    signing_env = os.environ.copy()
    signing_env["PATH"] = str(args.openssl.resolve().parent) + os.pathsep + signing_env.get("PATH", "")
    require_success(
        run(
            [
                sys.executable,
                str(args.sign_manifest.resolve()),
                str(manifest),
                "--private-key",
                str(private_key),
                "--algorithm",
                "rsa-sha256",
            ],
            env=signing_env,
        ),
        "manifest signing",
    )

    document = json.loads(manifest.read_text(encoding="utf-8"))
    paths = {entry["path"] for entry in document["files"]}
    if paths != {"bin/app.txt", "config/unchanged.txt", "data/new.txt"}:
        raise AssertionError(f"generated signed manifest has unexpected files: {sorted(paths)}")

    signature_bytes = base64.b64decode(signature.read_text(encoding="ascii").strip(), validate=True)
    raw_signature = root / "manifest.sig.bin"
    raw_signature.write_bytes(signature_bytes)
    require_success(
        run(
            [
                openssl,
                "dgst",
                "-sha256",
                "-verify",
                str(public_key),
                "-signature",
                str(raw_signature),
                str(manifest),
            ]
        ),
        "detached signature verification",
    )

    tampered = root / "tampered-manifest.json"
    tampered.write_bytes(manifest.read_bytes() + b" ")
    rejected = run(
        [
            openssl,
            "dgst",
            "-sha256",
            "-verify",
            str(public_key),
            "-signature",
            str(raw_signature),
            str(tampered),
        ]
    )
    if rejected.returncode == 0:
        raise AssertionError("tampered signed manifest unexpectedly verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
