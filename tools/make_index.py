#!/usr/bin/env python3
"""Generate an index manifest that routes platform/arch pairs to release manifests."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

try:
    from .metadata_contract import is_rfc3339_timestamp
except ImportError:
    from metadata_contract import is_rfc3339_timestamp


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
    target = {"manifestUrl": url}
    if platform != "*":
        target["platform"] = platform
    if arch != "*":
        target["arch"] = arch
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("index.json"))
    parser.add_argument("--app-id", required=True)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--generated-at", required=True, help="RFC 3339 timestamp")
    parser.add_argument(
        "--target",
        action="append",
        type=parse_target,
        required=True,
        help="Target mapping like windows/x64=url; use * as a platform or architecture wildcard",
    )
    args = parser.parse_args()
    if not is_rfc3339_timestamp(args.generated_at):
        parser.error("--generated-at must use the documented RFC 3339 timestamp profile")
    selectors: set[tuple[str, str]] = set()
    for target in args.target:
        selector = (target.get("platform", ""), target.get("arch", ""))
        if selector in selectors:
            parser.error("--target selectors must be unique")
        selectors.add(selector)

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
