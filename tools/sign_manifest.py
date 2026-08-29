#!/usr/bin/env python3
"""Sign a manifest with the OpenSSL CLI and write a detached .sig file.

The default output is base64 text, which libAutoUpdater's OpenSSL verifier
accepts in addition to raw binary signatures.
"""

from __future__ import annotations

import argparse
import base64
import subprocess
import sys
from pathlib import Path


def sign_ed25519(manifest: Path, private_key: Path) -> bytes:
    return subprocess.check_output(
        [
            "openssl",
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            str(private_key),
            "-in",
            str(manifest),
        ]
    )


def sign_rsa_sha256(manifest: Path, private_key: Path) -> bytes:
    return subprocess.check_output(
        [
            "openssl",
            "dgst",
            "-sha256",
            "-sign",
            str(private_key),
            str(manifest),
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--algorithm", choices=["ed25519", "rsa-sha256"], default="ed25519")
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--raw", action="store_true", help="Write raw binary signature instead of base64 text")
    args = parser.parse_args()

    if not args.manifest.is_file():
        print(f"error: manifest file does not exist: {args.manifest}", file=sys.stderr)
        sys.exit(1)
    if not args.private_key.is_file():
        print(f"error: private key file does not exist: {args.private_key}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.algorithm == "ed25519":
            signature = sign_ed25519(args.manifest, args.private_key)
        else:
            signature = sign_rsa_sha256(args.manifest, args.private_key)
    except subprocess.CalledProcessError as exc:
        print(f"error: openssl signing failed (exit code {exc.returncode})", file=sys.stderr)
        sys.exit(1)

    output = args.output or args.manifest.with_suffix(args.manifest.suffix + ".sig")
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.raw:
        output.write_bytes(signature)
    else:
        output.write_text(base64.b64encode(signature).decode("ascii") + "\n", encoding="ascii")

    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
