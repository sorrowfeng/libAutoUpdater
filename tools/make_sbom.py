#!/usr/bin/env python3
"""Generate a minimal SPDX 2.3 JSON SBOM for an installed libAutoUpdater tree."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def spdx_id_for(index: int) -> str:
    return f"SPDXRef-File-{index}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--name", default="libAutoUpdater")
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    if not root.is_dir():
        raise SystemExit(f"SBOM root does not exist: {root}")

    files = []
    relationships = []
    for index, path in enumerate(sorted(p for p in root.rglob("*") if p.is_file()), start=1):
        file_id = spdx_id_for(index)
        files.append(
            {
                "SPDXID": file_id,
                "fileName": path.relative_to(root).as_posix(),
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256_file(path)}],
                "licenseConcluded": "NOASSERTION",
                "licenseInfoInFiles": ["NOASSERTION"],
                "copyrightText": "NOASSERTION",
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-Package-libAutoUpdater",
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": file_id,
            }
        )

    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{args.name}-{args.version}",
        "documentNamespace": f"https://github.com/sorrowfeng/libAutoUpdater/sbom/{args.version}",
        "creationInfo": {
            "created": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "creators": ["Tool: libAutoUpdater tools/make_sbom.py"],
        },
        "packages": [
            {
                "name": args.name,
                "SPDXID": "SPDXRef-Package-libAutoUpdater",
                "versionInfo": args.version,
                "downloadLocation": "https://github.com/sorrowfeng/libAutoUpdater",
                "filesAnalyzed": True,
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION",
            }
        ],
        "files": files,
        "relationships": relationships,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
