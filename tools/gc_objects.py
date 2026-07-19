#!/usr/bin/env python3
"""Remove unreferenced content-addressed objects from a libAutoUpdater object store."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import secrets
import stat
import sys
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Iterable, Iterator, Optional

try:
    from .metadata_contract import is_rfc3339_timestamp
except ImportError:
    from metadata_contract import is_rfc3339_timestamp


MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_ARTIFACT_BYTES = 8 * 1024 * 1024 * 1024
MAX_TOTAL_ARTIFACT_BYTES = 32 * 1024 * 1024 * 1024
MAX_VERSION_COMPONENT = 2**31 - 1
MAX_VERSION_COMPONENT_TEXT = str(MAX_VERSION_COMPONENT)
MAX_JSON_DEPTH = 64
MAX_JSON_NODES = 100000
MAX_JSON_STRING_BYTES = 1024 * 1024
MAX_JSON_NUMBER_BYTES = 128
MAX_JSON_CONTAINER_ENTRIES = 10000

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHARD_RE = re.compile(r"^[0-9a-f]{2}$")
IDENTIFIER_RE = re.compile(r"^[0-9A-Za-z-]+$")

ROOT_KEYS = {
    "schemaVersion",
    "appId",
    "channel",
    "platform",
    "arch",
    "version",
    "releaseId",
    "releaseDate",
    "publishedAt",
    "expiresAt",
    "minVersion",
    "minClientVersion",
    "mandatory",
    "allowDowngrade",
    "notes",
    "baseUrl",
    "files",
    "remove",
}
STRING_KEYS = {
    "appId",
    "channel",
    "platform",
    "arch",
    "releaseId",
    "releaseDate",
    "publishedAt",
    "expiresAt",
    "notes",
    "baseUrl",
}
VERSION_KEYS = {"version", "minVersion", "minClientVersion"}
BOOL_KEYS = {"mandatory", "allowDowngrade"}
TIMESTAMP_KEYS = {"releaseDate", "publishedAt", "expiresAt"}
FILE_KEYS = {"path", "localPath", "sha256", "size"}
WINDOWS_DEVICE_NAMES = {"CON", "PRN", "AUX", "NUL", "CLOCK$"}


class ValidationError(ValueError):
    """An input cannot safely be used to decide which objects to delete."""


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_non_finite(value: str) -> None:
    raise ValidationError(f"non-finite JSON number is not allowed: {value}")


def parse_json_integer(value: str) -> int:
    if len(value.encode("ascii")) > MAX_JSON_NUMBER_BYTES:
        raise ValidationError("JSON number exceeds the token byte limit")
    parsed = int(value, 10)
    if parsed < -(2**63) or parsed > 2**64 - 1:
        raise ValidationError("JSON integer is outside the signed/unsigned 64-bit range")
    return parsed


def parse_json_float(value: str) -> float:
    if len(value.encode("ascii")) > MAX_JSON_NUMBER_BYTES:
        raise ValidationError("JSON number exceeds the token byte limit")
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValidationError("JSON floating-point number is out of range")
    significand = value.split("e", 1)[0].split("E", 1)[0]
    if parsed == 0.0 and any(character in "123456789" for character in significand):
        raise ValidationError("JSON floating-point number underflows")
    return parsed


def validate_unicode_scalars(value: Any) -> None:
    pending = [value]
    while pending:
        current = pending.pop()
        if isinstance(current, str):
            if any(0xD800 <= ord(character) <= 0xDFFF for character in current):
                raise ValidationError("JSON strings must not contain lone UTF-16 surrogates")
        elif isinstance(current, list):
            pending.extend(current)
        elif isinstance(current, dict):
            pending.extend(current.keys())
            pending.extend(current.values())


def validate_json_resources(value: Any) -> None:
    pending: list[tuple[Any, int]] = [(value, 1)]
    nodes = 0
    while pending:
        current, depth = pending.pop()
        if depth > MAX_JSON_DEPTH:
            raise ValidationError("JSON depth limit exceeded")
        nodes += 1
        if nodes > MAX_JSON_NODES:
            raise ValidationError("JSON node limit exceeded")

        if isinstance(current, str):
            if len(current.encode("utf-8")) > MAX_JSON_STRING_BYTES:
                raise ValidationError("JSON string byte limit exceeded")
        elif isinstance(current, list):
            if len(current) > MAX_JSON_CONTAINER_ENTRIES:
                raise ValidationError("JSON array entry limit exceeded")
            pending.extend((item, depth + 1) for item in current)
        elif isinstance(current, dict):
            if len(current) > MAX_JSON_CONTAINER_ENTRIES:
                raise ValidationError("JSON object entry limit exceeded")
            for key, item in current.items():
                if len(key.encode("utf-8")) > MAX_JSON_STRING_BYTES:
                    raise ValidationError("JSON string byte limit exceeded")
                pending.append((item, depth + 1))


def read_json_object(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as handle:
            raw = handle.read(MAX_MANIFEST_BYTES + 1)
    except OSError as error:
        raise ValidationError(f"cannot read manifest: {error}") from error
    if len(raw) > MAX_MANIFEST_BYTES:
        raise ValidationError(f"manifest exceeds {MAX_MANIFEST_BYTES} bytes")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ValidationError(f"manifest is not valid UTF-8: {error}") from error
    try:
        value = json.loads(
            text,
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_non_finite,
            parse_int=parse_json_integer,
            parse_float=parse_json_float,
            strict=True,
        )
    except (json.JSONDecodeError, RecursionError) as error:
        raise ValidationError(f"invalid JSON: {error}") from error
    validate_unicode_scalars(value)
    validate_json_resources(value)
    if not isinstance(value, dict):
        raise ValidationError("manifest root must be an object")
    return value


def validate_version(value: str, field: str) -> None:
    if not value:
        raise ValidationError(f"{field} must not be empty")
    core_and_prerelease, separator, build = value.partition("+")
    if separator:
        if "+" in build:
            raise ValidationError(f"{field} has invalid build metadata")
        validate_version_identifiers(build, field, "build metadata", reject_numeric_leading_zeroes=False)

    core, separator, prerelease = core_and_prerelease.partition("-")
    if separator:
        validate_version_identifiers(prerelease, field, "prerelease", reject_numeric_leading_zeroes=True)
    components = core.split(".")
    if len(components) != 3:
        raise ValidationError(f"{field} must contain major.minor.patch")
    for component in components:
        if not component.isascii() or not component.isdigit():
            raise ValidationError(f"{field} contains a non-numeric core component")
        if len(component) > 1 and component.startswith("0"):
            raise ValidationError(f"{field} contains a leading-zero core component")
        if len(component) > len(MAX_VERSION_COMPONENT_TEXT) or (
            len(component) == len(MAX_VERSION_COMPONENT_TEXT) and component > MAX_VERSION_COMPONENT_TEXT
        ):
            raise ValidationError(f"{field} core component is out of range")


def validate_version_identifiers(
    value: str, field: str, kind: str, *, reject_numeric_leading_zeroes: bool
) -> None:
    if not value:
        raise ValidationError(f"{field} has empty {kind}")
    for identifier in value.split("."):
        if not IDENTIFIER_RE.fullmatch(identifier):
            raise ValidationError(f"{field} has invalid {kind}")
        if (
            reject_numeric_leading_zeroes
            and identifier.isdigit()
            and len(identifier) > 1
            and identifier.startswith("0")
        ):
            raise ValidationError(f"{field} has a leading-zero numeric {kind} identifier")


def is_windows_device_name(segment: str) -> bool:
    base = segment.split(".", 1)[0].upper()
    if base in WINDOWS_DEVICE_NAMES:
        return True
    return len(base) == 4 and base[:3] in {"COM", "LPT"} and base[3] in "123456789"


def validate_managed_path(path: str, field: str, *, target: bool = False) -> None:
    if not path:
        raise ValidationError(f"{field} must not be empty")
    if path.startswith(("/", "\\")) or "\\" in path:
        raise ValidationError(f"{field} must be a relative path using forward slashes")
    if len(path) >= 2 and path[0].isascii() and path[0].isalpha() and path[1] == ":":
        raise ValidationError(f"{field} must not contain a drive prefix")

    segments = path.split("/")
    for segment in segments:
        if (
            not segment
            or segment in {".", ".."}
            or segment.endswith((".", " "))
            or ":" in segment
            or is_windows_device_name(segment)
            or any(ord(character) < 0x20 or ord(character) == 0x7F for character in segment)
        ):
            raise ValidationError(f"{field} contains an unsafe path segment")
    if target and (segments[0].lower() == ".autoupdater" or "~" in segments[0]):
        raise ValidationError(f"{field} uses the reserved updater namespace")


def portable_collision_key(path: str) -> str:
    return "\0".join(
        "".join(character.lower() if "A" <= character <= "Z" else character for character in segment)
        for segment in path.split("/")
    )


def ascii_fold(value: str) -> str:
    return "".join(character.lower() if "A" <= character <= "Z" else character for character in value)


def validate_unique_targets(paths: Iterable[str]) -> None:
    keys = sorted(portable_collision_key(path) for path in paths)
    for previous, current in zip(keys, keys[1:]):
        if previous == current:
            raise ValidationError("managed targets collide under portable filesystem semantics")
        if current.startswith(previous + "\0"):
            raise ValidationError("managed targets have an ancestor/descendant conflict")


def validate_object_prefix(object_prefix: str) -> str:
    validate_managed_path(object_prefix, "object prefix")
    if object_prefix != object_prefix.strip("/"):
        raise ValidationError("object prefix must be canonical and must not start or end with a slash")
    return object_prefix


def validate_object_entry_path(path: str, sha256: str, object_prefix: str, field: str) -> str:
    validate_managed_path(path, field)
    prefix = object_prefix + "/"
    if not path.startswith(prefix):
        raise ValidationError(f"{field} does not use the configured object prefix")
    relative = path[len(prefix) :]
    parts = relative.split("/")
    if len(parts) != 2 or not SHARD_RE.fullmatch(parts[0]) or not SHA256_RE.fullmatch(parts[1]):
        raise ValidationError(f"{field} must have the form <prefix>/<shard>/<sha256>")
    shard, digest = parts
    if digest != sha256:
        raise ValidationError(f"{field} digest does not match files[].sha256")
    if shard != digest[:2]:
        raise ValidationError(f"{field} shard does not match the digest prefix")
    return relative


def validate_manifest(data: dict[str, Any], object_prefix: str) -> dict[str, int]:
    unknown = set(data) - ROOT_KEYS
    if unknown:
        raise ValidationError(f"unknown manifest field(s): {', '.join(sorted(unknown))}")

    schema = data.get("schemaVersion")
    if type(schema) is not int or schema != 1:
        raise ValidationError("schemaVersion must be the integer 1")
    if "version" not in data or not isinstance(data["version"], str):
        raise ValidationError("version must be a string")

    for key in STRING_KEYS:
        if key in data and not isinstance(data[key], str):
            raise ValidationError(f"{key} must be a string")
    for key in TIMESTAMP_KEYS:
        if key in data and not is_rfc3339_timestamp(data[key]):
            raise ValidationError(f"{key} must use the documented RFC 3339 timestamp profile")
    for key in VERSION_KEYS:
        if key not in data:
            continue
        value = data[key]
        if key != "version" and value is None:
            continue
        if not isinstance(value, str):
            raise ValidationError(f"{key} must be a string or null")
        validate_version(value, key)
    for key in BOOL_KEYS:
        if key in data and type(data[key]) is not bool:
            raise ValidationError(f"{key} must be a boolean")

    files = data.get("files", [])
    if not isinstance(files, list):
        raise ValidationError("files must be an array")
    remove = data.get("remove", [])
    if not isinstance(remove, list):
        raise ValidationError("remove must be an array")

    references: dict[str, int] = {}
    source_evidence: dict[str, tuple[str, int]] = {}
    sources: list[str] = []
    targets: list[str] = []
    total_size = 0
    for index, item in enumerate(files):
        field = f"files[{index}]"
        if not isinstance(item, dict):
            raise ValidationError(f"{field} must be an object")
        unknown = set(item) - FILE_KEYS
        if unknown:
            raise ValidationError(f"unknown {field} field(s): {', '.join(sorted(unknown))}")
        for required in ("path", "sha256", "size"):
            if required not in item:
                raise ValidationError(f"{field}.{required} is required")
        if not isinstance(item["path"], str):
            raise ValidationError(f"{field}.path must be a string")
        if not isinstance(item["sha256"], str) or not SHA256_RE.fullmatch(item["sha256"]):
            raise ValidationError(f"{field}.sha256 must be a lowercase hexadecimal SHA-256 digest")
        if type(item["size"]) is not int or item["size"] < 0 or item["size"] > MAX_ARTIFACT_BYTES:
            raise ValidationError(f"{field}.size must be a non-negative integer within the artifact limit")

        sha256 = item["sha256"]
        validate_managed_path(item["path"], f"{field}.path")
        source_key = portable_collision_key(item["path"])
        evidence = (sha256, item["size"])
        previous_evidence = source_evidence.get(source_key)
        if previous_evidence is not None and previous_evidence != evidence:
            raise ValidationError(f"{field} gives conflicting evidence for the same source path")
        if previous_evidence is None:
            sources.append(item["path"])
        source_evidence[source_key] = evidence
        prefix = object_prefix + "/"
        canonical_prefix_match = item["path"] == object_prefix or item["path"].startswith(prefix)
        folded_path = ascii_fold(item["path"])
        folded_prefix = ascii_fold(object_prefix)
        folded_prefix_match = folded_path == folded_prefix or folded_path.startswith(folded_prefix + "/")
        if folded_prefix_match and not canonical_prefix_match:
            raise ValidationError(f"{field}.path uses non-canonical object-prefix casing")
        if canonical_prefix_match:
            relative = validate_object_entry_path(item["path"], sha256, object_prefix, f"{field}.path")
            previous_size = references.get(relative)
            if previous_size is not None and previous_size != item["size"]:
                raise ValidationError(f"{field} gives a conflicting size for the same digest")
            references[relative] = item["size"]
        total_size += item["size"]
        if total_size > MAX_TOTAL_ARTIFACT_BYTES:
            raise ValidationError("manifest artifacts exceed the total byte limit")

        if "localPath" in item:
            if not isinstance(item["localPath"], str):
                raise ValidationError(f"{field}.localPath must be a string")
            if item["localPath"]:
                validate_managed_path(item["localPath"], f"{field}.localPath", target=True)
                targets.append(item["localPath"])
            else:
                validate_managed_path(item["path"], f"{field}.path", target=True)
                targets.append(item["path"])
        else:
            validate_managed_path(item["path"], f"{field}.path", target=True)
            targets.append(item["path"])

    for index, path in enumerate(remove):
        if not isinstance(path, str):
            raise ValidationError(f"remove[{index}] must be a string")
        validate_managed_path(path, f"remove[{index}]", target=True)
        targets.append(path)
    validate_unique_targets(sources)
    validate_unique_targets(targets)
    return references


def referenced_objects(manifest_paths: list[Path], object_prefix: str) -> dict[str, int]:
    referenced: dict[str, int] = {}
    for manifest_path in manifest_paths:
        try:
            manifest_references = validate_manifest(read_json_object(manifest_path), object_prefix)
        except ValidationError as error:
            raise ValidationError(f"{manifest_path}: {error}") from error
        for relative, size in manifest_references.items():
            previous_size = referenced.get(relative)
            if previous_size is not None and previous_size != size:
                raise ValidationError(f"conflicting sizes for object {relative} across manifests")
            referenced[relative] = size
    return referenced


REPARSE_POINT_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    _CREATE_FILE = ctypes.WinDLL("kernel32", use_last_error=True).CreateFileW
    _CREATE_FILE.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    )
    _CREATE_FILE.restype = wintypes.HANDLE
    _CLOSE_HANDLE = ctypes.WinDLL("kernel32", use_last_error=True).CloseHandle
    _CLOSE_HANDLE.argtypes = (wintypes.HANDLE,)
    _CLOSE_HANDLE.restype = wintypes.BOOL
    _INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value


def is_reparse_point(info: os.stat_result) -> bool:
    return bool(getattr(info, "st_file_attributes", 0) & REPARSE_POINT_ATTRIBUTE)


def stable_object_identity(info: os.stat_result) -> tuple[int, int, int, int]:
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def object_identity(info: os.stat_result) -> tuple[int, ...]:
    identity = stable_object_identity(info)
    # On Windows, CRT fstat reports st_ctime using last-write semantics while
    # path lstat reports creation time. They are not comparable for the same
    # handle. POSIX ctime is a useful additional inode-generation check.
    return identity if os.name == "nt" else identity + (info.st_ctime_ns,)


def directory_identity(info: os.stat_result) -> tuple[int, int]:
    return (info.st_dev, info.st_ino)


@dataclass(frozen=True)
class ObjectRecord:
    relative: str
    path: Path
    identity: tuple[int, ...]
    size: int


class DirectoryGuard:
    """Hold a directory identity so a validated shard cannot be replaced."""

    def __init__(self, path: Path, *, parent_fd: Optional[int] = None, entry_name: Optional[str] = None) -> None:
        self.path = path
        self.parent_fd = parent_fd
        self.entry_name = entry_name
        self.fd: Optional[int] = None
        self.handle: Any = None
        self.identity: Optional[tuple[int, int]] = None

    def open(self) -> None:
        if os.name == "nt":
            # Do not grant FILE_SHARE_DELETE. OPEN_REPARSE_POINT ensures a
            # junction itself is opened and rejected instead of its target.
            handle = _CREATE_FILE(
                str(self.path),
                0x0080,  # FILE_READ_ATTRIBUTES
                0x0001 | 0x0002,  # FILE_SHARE_READ | FILE_SHARE_WRITE
                None,
                3,  # OPEN_EXISTING
                0x02000000 | 0x00200000,  # BACKUP_SEMANTICS | OPEN_REPARSE_POINT
                None,
            )
            if handle == _INVALID_HANDLE_VALUE:
                raise OSError(ctypes.get_last_error(), f"cannot lock directory identity: {self.path}")
            self.handle = handle
        else:
            no_follow = getattr(os, "O_NOFOLLOW", None)
            directory = getattr(os, "O_DIRECTORY", None)
            if no_follow is None or directory is None:
                raise ValidationError("platform cannot securely open object-store directories")
            flags = os.O_RDONLY | no_follow | directory | getattr(os, "O_CLOEXEC", 0)
            target: Any = self.entry_name if self.entry_name is not None else self.path
            self.fd = os.open(target, flags, dir_fd=self.parent_fd)

        try:
            info = os.lstat(self.path) if os.name == "nt" else os.fstat(self.fd)
            if is_reparse_point(info) or not stat.S_ISDIR(info.st_mode):
                raise ValidationError(f"object-store directory is a link or reparse point: {self.path}")
            self.identity = directory_identity(info)
        except Exception:
            self.close()
            raise

    def close(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        if self.handle is not None:
            _CLOSE_HANDLE(self.handle)
            self.handle = None


class ObjectStore:
    """Validated two-level object-store snapshot with guarded shard identities."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.root_guard = DirectoryGuard(root)
        self.ancestor_guards: list[DirectoryGuard] = []
        self.shard_identities: dict[str, tuple[int, int]] = {}
        self.objects: dict[str, ObjectRecord] = {}

    def __enter__(self) -> "ObjectStore":
        try:
            root_info = os.lstat(self.root)
        except OSError as error:
            raise ValidationError(f"cannot inspect object root: {error}") from error
        if is_reparse_point(root_info) or not stat.S_ISDIR(root_info.st_mode):
            raise ValidationError(f"object root must be a real directory, not a link: {self.root}")
        try:
            if os.name == "nt":
                # Windows has no Python handle-relative scandir/unlink. Lock
                # the complete absolute directory chain and reject every
                # ancestor reparse point so later pathname operations cannot
                # be rebound through a junction above the guarded root.
                chain = list(reversed(self.root.parents)) + [self.root]
                for component in chain:
                    guard = self.root_guard if component == self.root else DirectoryGuard(component)
                    guard.open()
                    self.ancestor_guards.append(guard)
            else:
                self.root_guard.open()
            if self.root_guard.identity != directory_identity(root_info):
                raise ValidationError("object root changed while securing its identity")
        except (OSError, ValidationError) as error:
            if self.ancestor_guards:
                for guard in reversed(self.ancestor_guards):
                    guard.close()
                self.ancestor_guards.clear()
            else:
                self.root_guard.close()
            raise ValidationError(f"cannot secure object root path: {error}") from error
        return self

    def __exit__(self, _error_type: Any, _error: Any, _traceback: Any) -> None:
        if self.ancestor_guards:
            for guard in reversed(self.ancestor_guards):
                guard.close()
            self.ancestor_guards.clear()
        else:
            self.root_guard.close()

    def _new_shard_guard(self, shard: str) -> DirectoryGuard:
        guard = DirectoryGuard(self.root / shard, parent_fd=self.root_guard.fd, entry_name=shard)
        try:
            guard.open()
        except OSError as error:
            raise ValidationError(f"cannot secure object shard {shard}: {error}") from error
        expected = self.shard_identities.get(shard)
        if expected is not None and guard.identity != expected:
            guard.close()
            raise ValidationError(f"object shard changed after validation: {shard}")
        return guard

    def scan(self) -> dict[str, ObjectRecord]:
        try:
            scan_root: Any = self.root if os.name == "nt" else self.root_guard.fd
            with os.scandir(scan_root) as iterator:
                shards = sorted(iterator, key=lambda entry: entry.name)
        except OSError as error:
            raise ValidationError(f"cannot enumerate object root: {error}") from error

        for shard_entry in shards:
            shard = shard_entry.name
            shard_path = self.root / shard
            try:
                if os.name == "nt":
                    shard_info = os.lstat(shard_path)
                else:
                    shard_info = os.stat(shard, dir_fd=self.root_guard.fd, follow_symlinks=False)
            except OSError as error:
                raise ValidationError(f"cannot inspect object-store entry {shard_path}: {error}") from error
            if is_reparse_point(shard_info):
                raise ValidationError(f"object store must not contain reparse points: {shard_path}")
            if not SHARD_RE.fullmatch(shard) or not stat.S_ISDIR(shard_info.st_mode):
                raise ValidationError(f"object store contains an invalid shard directory: {shard}")

            guard = self._new_shard_guard(shard)
            try:
                if guard.identity is None:
                    raise ValidationError(f"cannot identify object shard: {shard}")
                if guard.identity != directory_identity(shard_info):
                    raise ValidationError(f"object shard changed while securing its identity: {shard}")
                self.shard_identities[shard] = guard.identity
                scan_shard: Any = shard_path if os.name == "nt" else guard.fd
                try:
                    with os.scandir(scan_shard) as iterator:
                        entries = sorted(iterator, key=lambda entry: entry.name)
                except OSError as error:
                    raise ValidationError(f"cannot enumerate object shard {shard}: {error}") from error
                for entry in entries:
                    path = shard_path / entry.name
                    relative = f"{shard}/{entry.name}"
                    try:
                        if os.name == "nt":
                            info = os.lstat(path)
                        else:
                            info = os.stat(entry.name, dir_fd=guard.fd, follow_symlinks=False)
                    except OSError as error:
                        raise ValidationError(f"cannot inspect object {relative}: {error}") from error
                    if is_reparse_point(info) or not stat.S_ISREG(info.st_mode):
                        raise ValidationError(f"object store contains a non-regular entry: {relative}")
                    if not SHA256_RE.fullmatch(entry.name) or shard != entry.name[:2]:
                        raise ValidationError(f"object has an invalid store path: {relative}")
                    self.objects[relative] = ObjectRecord(relative, path, object_identity(info), info.st_size)
            finally:
                guard.close()
        return self.objects

    def _current_info(self, record: ObjectRecord, guard: DirectoryGuard) -> os.stat_result:
        try:
            if os.name == "nt":
                info = os.lstat(record.path)
            else:
                info = os.stat(record.path.name, dir_fd=guard.fd, follow_symlinks=False)
        except OSError as error:
            raise ValidationError(f"object changed after validation: {record.relative}: {error}") from error
        if is_reparse_point(info) or not stat.S_ISREG(info.st_mode) or object_identity(info) != record.identity:
            raise ValidationError(f"object changed after validation: {record.relative}")
        return info

    @contextmanager
    def open_for_read(self, record: ObjectRecord) -> Iterator[BinaryIO]:
        shard = record.relative.split("/", 1)[0]
        guard = self._new_shard_guard(shard)
        fd: Optional[int] = None
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
        try:
            try:
                self._current_info(record, guard)
                if os.name == "nt":
                    fd = os.open(record.path, flags)
                else:
                    fd = os.open(record.path.name, flags | os.O_NOFOLLOW, dir_fd=guard.fd)
            except OSError as error:
                raise ValidationError(f"cannot open referenced object {record.relative}: {error}") from error
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or object_identity(info) != record.identity:
                raise ValidationError(f"object changed while opening: {record.relative}")
            with os.fdopen(fd, "rb", closefd=True) as handle:
                fd = None
                yield handle
        finally:
            if fd is not None:
                os.close(fd)
            guard.close()

    def delete(self, record: ObjectRecord) -> None:
        shard = record.relative.split("/", 1)[0]
        guard = self._new_shard_guard(shard)
        quarantine_name = f".gc-delete-{secrets.token_hex(16)}"
        quarantine_path = record.path.with_name(quarantine_name)
        quarantined = False
        try:
            self._current_info(record, guard)
            if os.name == "nt":
                os.rename(record.path, quarantine_path)
            else:
                os.rename(
                    record.path.name,
                    quarantine_name,
                    src_dir_fd=guard.fd,
                    dst_dir_fd=guard.fd,
                )
            quarantined = True

            if os.name == "nt":
                quarantined_info = os.lstat(quarantine_path)
            else:
                quarantined_info = os.stat(quarantine_name, dir_fd=guard.fd, follow_symlinks=False)
            if (
                is_reparse_point(quarantined_info)
                or not stat.S_ISREG(quarantined_info.st_mode)
                # POSIX rename may update ctime on the same inode. Compare the
                # stable binding and content metadata here; the pre-rename
                # check still uses ctime as an extra generation signal.
                or stable_object_identity(quarantined_info) != record.identity[:4]
            ):
                raise ValidationError(f"object changed while being quarantined: {record.relative}")

            if os.name == "nt":
                os.unlink(quarantine_path)
            else:
                os.unlink(quarantine_name, dir_fd=guard.fd)
            quarantined = False
        except (OSError, ValidationError) as error:
            if quarantined:
                try:
                    if os.name == "nt":
                        try:
                            os.lstat(record.path)
                            source_missing = False
                        except FileNotFoundError:
                            source_missing = True
                        if source_missing:
                            os.rename(quarantine_path, record.path)
                    else:
                        try:
                            os.stat(record.path.name, dir_fd=guard.fd, follow_symlinks=False)
                            source_missing = False
                        except FileNotFoundError:
                            source_missing = True
                        if source_missing:
                            os.rename(
                                quarantine_name,
                                record.path.name,
                                src_dir_fd=guard.fd,
                                dst_dir_fd=guard.fd,
                            )
                    if source_missing:
                        quarantined = False
                except OSError as restore_error:
                    raise ValidationError(
                        f"cannot restore object {record.relative} from {quarantine_path}: {restore_error}"
                    ) from error
            if quarantined:
                raise ValidationError(
                    f"object {record.relative} changed concurrently and remains quarantined at {quarantine_path}"
                ) from error
            if isinstance(error, ValidationError):
                raise
            raise ValidationError(f"cannot delete object {record.relative}: {error}") from error
        finally:
            guard.close()


def verify_referenced_objects(
    store: ObjectStore, objects: dict[str, ObjectRecord], references: dict[str, int], *, verify_digest: bool
) -> None:
    for relative, declared_size in sorted(references.items()):
        record = objects[relative]
        if record.size != declared_size:
            raise ValidationError(
                f"referenced object size mismatch: {relative}: expected {declared_size}, got {record.size}"
            )
        if not verify_digest:
            continue
        hasher = hashlib.sha256()
        with store.open_for_read(record) as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                hasher.update(chunk)
        expected_digest = relative.split("/", 1)[1]
        if hasher.hexdigest() != expected_digest:
            raise ValidationError(f"referenced object content digest mismatch: {relative}")


def run(args: argparse.Namespace) -> int:
    object_prefix = validate_object_prefix(args.object_prefix)

    # Fail closed: every manifest and every object-store entry is validated,
    # and every referenced object is confirmed present, before the first unlink.
    referenced = referenced_objects(args.manifest, object_prefix)
    object_root = Path(os.path.abspath(os.fspath(args.object_root)))
    with ObjectStore(object_root) as store:
        objects = store.scan()
        missing = sorted(set(referenced) - set(objects))
        if missing:
            raise ValidationError(f"referenced object is missing: {missing[0]}")
        if args.delete and objects and not referenced and not args.allow_empty:
            raise ValidationError(
                "refusing to delete from a non-empty object store with zero references; "
                "pass --allow-empty only when deleting every object is intentional"
            )
        verify_referenced_objects(store, objects, referenced, verify_digest=args.delete)

        stale = [objects[relative] for relative in sorted(set(objects) - set(referenced))]
        action = "Deleting" if args.delete else "Would delete"
        for record in stale:
            print(f"{action} {record.path}")
            if args.delete:
                store.delete(record)

    print(f"Referenced objects: {len(referenced)}")
    print(f"Unreferenced objects: {len(stale)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object_root", type=Path)
    parser.add_argument("--manifest", action="append", type=Path, required=True)
    parser.add_argument("--object-prefix", default="objects/sha256")
    parser.add_argument("--delete", action="store_true", help="Delete unreferenced objects; default is dry-run")
    parser.add_argument(
        "--allow-empty",
        action="store_true",
        help="Allow --delete to remove every object when validated manifests contain zero object references",
    )
    args = parser.parse_args()

    try:
        return run(args)
    except (ValidationError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
