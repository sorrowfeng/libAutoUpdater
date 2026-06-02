#!/usr/bin/env python3
"""Extract a single release section from CHANGELOG.md."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def extract(text: str, tag: str) -> str:
    pattern = re.compile(rf"^##\s+{re.escape(tag)}\b.*$", re.M)
    match = pattern.search(text)
    if not match:
        raise SystemExit(f"CHANGELOG.md does not contain {tag}")

    next_match = re.search(r"^##\s+v[0-9]+\.[0-9]+\.[0-9]+.*$", text[match.end():], re.M)
    end = match.end() + next_match.start() if next_match else len(text)
    section = text[match.start():end].strip()
    return f"# libAutoUpdater {tag}\n\n{section}\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()

    notes = extract(args.changelog.read_text(encoding="utf-8"), args.tag)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(notes, encoding="utf-8")
    else:
        print(notes, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
