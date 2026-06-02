#!/usr/bin/env python3
"""Generate an index manifest that routes platform/arch pairs to release manifests."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_target(value: str) -> dict:
    parts = value.split("=", 1)
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("target must be platform/arch=url")
    platform_arch, url = parts
    platform_parts = platform_arch.split("/", 1)
    if len(platform_parts) != 2:
        raise argparse.ArgumentTypeError("target must be platform/arch=url")
    platform, arch = platform_parts
    if not platform or not arch or not url:
        raise argparse.ArgumentTypeError("target must be platform/arch=url")
    return {
        "platform": platform,
        "arch": arch,
        "manifestUrl": url,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("index.json"))
    parser.add_argument("--app-id", required=True)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--generated-at", required=True, help="UTC ISO-8601 timestamp")
    parser.add_argument(
        "--target",
        action="append",
        type=parse_target,
        required=True,
        help="Target mapping like windows/x64=https://example.com/releases/1.2.0/windows-x64/manifest.json",
    )
    args = parser.parse_args()

    manifest = {
        "schemaVersion": 1,
        "appId": args.app_id,
        "channel": args.channel,
        "generatedAt": args.generated_at,
        "targets": args.target,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
