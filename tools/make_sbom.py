#!/usr/bin/env python3
"""Generate an SPDX 2.3 JSON SBOM for an installed libAutoUpdater tree."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import tempfile
import uuid
from pathlib import Path
from typing import Any
from urllib.parse import quote, urlsplit


DEPENDENCY_SCHEMA_VERSION = 1
MAX_DEPENDENCY_FILE_BYTES = 1024 * 1024
SPDX_ID_PATTERN = re.compile(r"^SPDXRef-[A-Za-z0-9.-]+$")
DEPENDENCY_REQUIRED_FIELDS = frozenset(
    {"name", "SPDXID", "versionInfo", "downloadLocation", "licenseDeclared"}
)
DEPENDENCY_OPTIONAL_FIELDS = frozenset({"supplier"})
DEPENDENCY_FIELDS = DEPENDENCY_REQUIRED_FIELDS | DEPENDENCY_OPTIONAL_FIELDS
ROOT_PACKAGE_ID = "SPDXRef-Package-libAutoUpdater"
DOCUMENT_ID = "SPDXRef-DOCUMENT"


class SbomInputError(ValueError):
    """Raised when command or dependency metadata input is invalid."""


def checksums_for_file(path: Path) -> list[dict[str, str]]:
    sha1 = hashlib.sha1()
    sha256 = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            sha1.update(chunk)
            sha256.update(chunk)
    return [
        {"algorithm": "SHA1", "checksumValue": sha1.hexdigest()},
        {"algorithm": "SHA256", "checksumValue": sha256.hexdigest()},
    ]


def spdx_id_for_file(index: int) -> str:
    return f"SPDXRef-File-{index}"


def validate_text(value: Any, field: str, *, max_length: int = 512) -> str:
    if not isinstance(value, str):
        raise SbomInputError(f"{field} must be a string")
    if not value or value != value.strip():
        raise SbomInputError(f"{field} must be a non-empty, trimmed string")
    if len(value) > max_length:
        raise SbomInputError(f"{field} exceeds the {max_length}-character limit")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise SbomInputError(f"{field} must not contain control characters")
    return value


def validate_download_location(value: Any, field: str) -> str:
    location = validate_text(value, field, max_length=2048)
    if location in {"NONE", "NOASSERTION"}:
        return location
    parsed = urlsplit(location)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise SbomInputError(f"{field} must be an absolute HTTP(S) URL, NONE, or NOASSERTION")
    if parsed.username is not None or parsed.password is not None:
        raise SbomInputError(f"{field} must not contain credentials")
    if parsed.query or parsed.fragment:
        raise SbomInputError(f"{field} must not contain a query or fragment")
    return location


def validate_supplier(value: Any, field: str) -> str:
    supplier = validate_text(value, field)
    if supplier == "NOASSERTION":
        return supplier
    if not supplier.startswith(("Organization: ", "Person: ")):
        raise SbomInputError(
            f"{field} must start with 'Organization: ' or 'Person: ', or be NOASSERTION"
        )
    if supplier.endswith(": "):
        raise SbomInputError(f"{field} must identify a supplier")
    return supplier


def reject_duplicate_json_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise SbomInputError(f"dependency metadata contains duplicate JSON key: {key}")
        value[key] = item
    return value


def reject_nonfinite_json(value: str) -> None:
    raise SbomInputError(f"dependency metadata contains invalid JSON number: {value}")


def load_dependencies(path: Path | None, root_name: str) -> list[dict[str, Any]]:
    if path is None:
        return []
    try:
        size = path.stat().st_size
    except OSError as error:
        raise SbomInputError(f"cannot read dependency metadata: {error}") from error
    if size > MAX_DEPENDENCY_FILE_BYTES:
        raise SbomInputError(
            f"dependency metadata exceeds the {MAX_DEPENDENCY_FILE_BYTES}-byte limit"
        )
    try:
        contents = path.read_text(encoding="utf-8")
        document = json.loads(
            contents,
            object_pairs_hook=reject_duplicate_json_keys,
            parse_constant=reject_nonfinite_json,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SbomInputError(f"invalid dependency metadata: {error}") from error

    if not isinstance(document, dict):
        raise SbomInputError("dependency metadata root must be an object")
    expected_root_fields = {"schemaVersion", "dependencies"}
    missing_root = expected_root_fields - document.keys()
    unknown_root = document.keys() - expected_root_fields
    if missing_root:
        raise SbomInputError(
            "dependency metadata is missing field(s): " + ", ".join(sorted(missing_root))
        )
    if unknown_root:
        raise SbomInputError(
            "dependency metadata has unknown field(s): " + ", ".join(sorted(unknown_root))
        )
    schema_version = document["schemaVersion"]
    if (
        not isinstance(schema_version, int)
        or isinstance(schema_version, bool)
        or schema_version != DEPENDENCY_SCHEMA_VERSION
    ):
        raise SbomInputError(
            f"dependency metadata schemaVersion must be {DEPENDENCY_SCHEMA_VERSION}"
        )
    raw_dependencies = document["dependencies"]
    if not isinstance(raw_dependencies, list):
        raise SbomInputError("dependency metadata dependencies must be an array")

    dependencies: list[dict[str, Any]] = []
    seen_names: set[str] = set()
    seen_ids = {DOCUMENT_ID, ROOT_PACKAGE_ID}
    root_name_key = root_name.casefold()
    for index, raw_dependency in enumerate(raw_dependencies):
        context = f"dependencies[{index}]"
        if not isinstance(raw_dependency, dict):
            raise SbomInputError(f"{context} must be an object")
        missing = DEPENDENCY_REQUIRED_FIELDS - raw_dependency.keys()
        unknown = raw_dependency.keys() - DEPENDENCY_FIELDS
        if missing:
            raise SbomInputError(f"{context} is missing field(s): " + ", ".join(sorted(missing)))
        if unknown:
            raise SbomInputError(f"{context} has unknown field(s): " + ", ".join(sorted(unknown)))

        name = validate_text(raw_dependency["name"], f"{context}.name")
        name_key = name.casefold()
        if name_key == root_name_key:
            raise SbomInputError(f"{context}.name duplicates the root package name")
        if name_key in seen_names:
            raise SbomInputError(f"duplicate dependency name: {name}")
        seen_names.add(name_key)

        spdx_id = validate_text(raw_dependency["SPDXID"], f"{context}.SPDXID")
        if not SPDX_ID_PATTERN.fullmatch(spdx_id):
            raise SbomInputError(f"{context}.SPDXID is not a valid SPDX element ID")
        if re.fullmatch(r"SPDXRef-File-[1-9][0-9]*", spdx_id):
            raise SbomInputError(f"{context}.SPDXID uses the reserved generated-file namespace")
        if spdx_id in seen_ids:
            raise SbomInputError(f"duplicate or reserved SPDXID: {spdx_id}")
        seen_ids.add(spdx_id)

        package: dict[str, Any] = {
            "name": name,
            "SPDXID": spdx_id,
            "versionInfo": validate_text(
                raw_dependency["versionInfo"], f"{context}.versionInfo"
            ),
            "downloadLocation": validate_download_location(
                raw_dependency["downloadLocation"], f"{context}.downloadLocation"
            ),
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": validate_text(
                raw_dependency["licenseDeclared"], f"{context}.licenseDeclared"
            ),
            "copyrightText": "NOASSERTION",
        }
        if "supplier" in raw_dependency:
            package["supplier"] = validate_supplier(
                raw_dependency["supplier"], f"{context}.supplier"
            )
        dependencies.append(package)
    return dependencies


def is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def collect_files(root: Path, output: Path) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    files: list[dict[str, Any]] = []
    relationships: list[dict[str, str]] = []
    output_absolute = output.resolve(strict=False)
    candidates = sorted(path for path in root.rglob("*") if path.is_file())
    for path in candidates:
        if path.resolve(strict=False) == output_absolute:
            continue
        if path.is_symlink():
            try:
                target = path.resolve(strict=True)
            except OSError as error:
                raise SbomInputError(f"cannot resolve installed symlink {path}: {error}") from error
            if not is_within(target, root):
                raise SbomInputError(f"installed symlink escapes the SBOM root: {path}")
        file_id = spdx_id_for_file(len(files) + 1)
        try:
            checksums = checksums_for_file(path)
        except OSError as error:
            raise SbomInputError(f"cannot hash installed file {path}: {error}") from error
        files.append(
            {
                "SPDXID": file_id,
                "fileName": path.relative_to(root).as_posix(),
                "checksums": checksums,
                "licenseConcluded": "NOASSERTION",
                "licenseInfoInFiles": ["NOASSERTION"],
                "copyrightText": "NOASSERTION",
            }
        )
        relationships.append(
            {
                "spdxElementId": ROOT_PACKAGE_ID,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": file_id,
            }
        )
    return files, relationships


def package_verification_code(files: list[dict[str, Any]]) -> str:
    sha1_values = []
    for file_entry in files:
        sha1_values.append(
            next(
                checksum["checksumValue"]
                for checksum in file_entry["checksums"]
                if checksum["algorithm"] == "SHA1"
            )
        )
    return hashlib.sha1("".join(sorted(sha1_values)).encode("ascii")).hexdigest()


def namespace_for(
    name: str,
    version: str,
    platform: str | None,
    commit: str | None,
    generation_id: str,
) -> str:
    parts = [quote(name, safe=""), quote(version, safe="")]
    if platform is not None:
        parts.extend(("platform", quote(platform, safe="")))
    if commit is not None:
        parts.extend(("commit", quote(commit, safe="")))
    parts.extend(("generation", quote(generation_id, safe="")))
    return "https://github.com/sorrowfeng/libAutoUpdater/sbom/" + "/".join(parts)


def atomic_write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(document, handle, indent=2, ensure_ascii=False, allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def build_document(
    root: Path,
    output: Path,
    name: str,
    version: str,
    platform: str | None,
    commit: str | None,
    generation_id: str,
    dependencies: list[dict[str, Any]],
) -> dict[str, Any]:
    files, relationships = collect_files(root, output)
    relationships.insert(
        0,
        {
            "spdxElementId": DOCUMENT_ID,
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": ROOT_PACKAGE_ID,
        },
    )
    relationships.extend(
        {
            "spdxElementId": ROOT_PACKAGE_ID,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": dependency["SPDXID"],
        }
        for dependency in dependencies
    )
    root_package = {
        "name": name,
        "SPDXID": ROOT_PACKAGE_ID,
        "versionInfo": version,
        "downloadLocation": "https://github.com/sorrowfeng/libAutoUpdater",
        "filesAnalyzed": True,
        "packageVerificationCode": {
            "packageVerificationCodeValue": package_verification_code(files)
        },
        "licenseConcluded": "MIT",
        "licenseDeclared": "MIT",
        "copyrightText": "NOASSERTION",
    }
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": DOCUMENT_ID,
        "name": f"{name}-{version}",
        "documentNamespace": namespace_for(name, version, platform, commit, generation_id),
        "creationInfo": {
            "created": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "creators": ["Tool: libAutoUpdater tools/make_sbom.py"],
        },
        "packages": [root_package, *dependencies],
        "files": files,
        "relationships": relationships,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--name", default="libAutoUpdater")
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform")
    parser.add_argument("--commit")
    parser.add_argument(
        "--generation-id",
        help="Unique build/run identifier; defaults to a random UUID",
    )
    parser.add_argument(
        "--dependencies",
        type=Path,
        help="CMake-generated dependency metadata JSON (schemaVersion 1)",
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    try:
        name = validate_text(args.name, "name")
        version = validate_text(args.version, "version")
        platform = (
            validate_text(args.platform, "platform") if args.platform is not None else None
        )
        commit = validate_text(args.commit, "commit") if args.commit is not None else None
        generation_id = (
            validate_text(args.generation_id, "generation-id")
            if args.generation_id is not None
            else str(uuid.uuid4())
        )
        root = args.root.resolve(strict=True)
        if not root.is_dir():
            raise SbomInputError(f"SBOM root is not a directory: {root}")
        output = args.output.resolve(strict=False)
        dependency_path = args.dependencies.resolve(strict=False) if args.dependencies else None
        if dependency_path is not None and dependency_path == output:
            raise SbomInputError("dependency metadata and output must refer to different files")
        dependencies = load_dependencies(dependency_path, name)
        document = build_document(
            root,
            output,
            name,
            version,
            platform,
            commit,
            generation_id,
            dependencies,
        )
        atomic_write_json(output, document)
    except (OSError, SbomInputError) as error:
        raise SystemExit(f"SBOM generation failed: {error}") from error
    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
