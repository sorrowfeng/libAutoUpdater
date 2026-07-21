#!/usr/bin/env python3
"""Strict, dependency-free helpers for deployment evidence validators."""

from __future__ import annotations

import json
import re
from datetime import date
from enum import Enum
from pathlib import Path
from typing import Any, Iterable

from metadata_contract import is_rfc3339_timestamp


MAX_EVIDENCE_BYTES = 1024 * 1024
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
TIMESTAMP_COMPONENTS = re.compile(
    r"(?P<year>[0-9]{4})-(?P<month>[0-9]{2})-(?P<day>[0-9]{2})"
    r"T(?P<hour>[0-9]{2}):(?P<minute>[0-9]{2}):(?P<second>[0-9]{2})"
    r"(?:\.(?P<fraction>[0-9]{1,9}))?"
    r"(?P<zone>Z|(?P<sign>[+-])(?P<offset_hour>[0-9]{2}):(?P<offset_minute>[0-9]{2}))",
    re.ASCII,
)


class EvidenceError(ValueError):
    """Raised when evidence does not satisfy its declared schema."""


class Status(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    OPEN = "OPEN"


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_nonfinite(value: str) -> None:
    raise EvidenceError(f"non-finite JSON number is not allowed: {value}")


def read_limited_bytes(path: Path, maximum: int = MAX_EVIDENCE_BYTES) -> bytes:
    try:
        with path.open("rb") as handle:
            contents = handle.read(maximum + 1)
    except OSError as error:
        raise EvidenceError(f"cannot read evidence file: {error}") from error
    if len(contents) > maximum:
        raise EvidenceError(f"evidence file exceeds the {maximum}-byte limit")
    return contents


def load_json(path: Path) -> Any:
    contents = read_limited_bytes(path)
    return parse_json_bytes(contents)


def parse_json_bytes(contents: bytes) -> Any:
    if len(contents) > MAX_EVIDENCE_BYTES:
        raise EvidenceError(
            f"evidence file exceeds the {MAX_EVIDENCE_BYTES}-byte limit"
        )
    try:
        return json.loads(
            contents.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_nonfinite,
        )
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise EvidenceError(f"cannot parse evidence JSON: {error}") from error


def require_object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceError(f"{context} must be an object")
    return value


def require_array(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list):
        raise EvidenceError(f"{context} must be an array")
    return value


def require_exact_keys(
    value: dict[str, Any], context: str, required: Iterable[str]
) -> None:
    expected = set(required)
    missing = expected - value.keys()
    unknown = value.keys() - expected
    if missing:
        raise EvidenceError(
            f"{context} is missing field(s): {', '.join(sorted(missing))}"
        )
    if unknown:
        raise EvidenceError(
            f"{context} contains unknown field(s): {', '.join(sorted(unknown))}"
        )


def require_text(value: Any, context: str, *, maximum: int = 512) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
        raise EvidenceError(f"{context} must be a non-empty, trimmed string")
    if len(value) > maximum:
        raise EvidenceError(f"{context} exceeds the {maximum}-character limit")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise EvidenceError(f"{context} must not contain control characters")
    return value


def require_enum(value: Any, context: str, choices: Iterable[str]) -> str:
    text = require_text(value, context)
    allowed = set(choices)
    if text not in allowed:
        raise EvidenceError(
            f"{context} must be one of: {', '.join(sorted(allowed))}"
        )
    return text


def require_bool(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        raise EvidenceError(f"{context} must be a boolean")
    return value


def require_nullable_bool(value: Any, context: str) -> bool | None:
    if value is not None and not isinstance(value, bool):
        raise EvidenceError(f"{context} must be true, false, or null")
    return value


def require_integer(value: Any, context: str, *, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise EvidenceError(f"{context} must be an integer >= {minimum}")
    return value


def require_sha256(value: Any, context: str) -> str:
    text = require_text(value, context, maximum=64)
    if SHA256_PATTERN.fullmatch(text) is None:
        raise EvidenceError(f"{context} must be a lowercase SHA-256 digest")
    return text


def require_timestamp(value: Any, context: str) -> str:
    text = require_text(value, context, maximum=40)
    if not is_rfc3339_timestamp(text):
        raise EvidenceError(f"{context} must use the updater's RFC 3339 profile")
    return text


def timestamp_nanoseconds(value: Any, context: str) -> int:
    """Return a validated RFC 3339 timestamp as integer Unix nanoseconds."""

    text = require_timestamp(value, context)
    match = TIMESTAMP_COMPONENTS.fullmatch(text)
    if match is None:
        raise EvidenceError(f"{context} must use the updater's RFC 3339 profile")
    day_count = date(
        int(match.group("year")),
        int(match.group("month")),
        int(match.group("day")),
    ).toordinal() - date(1970, 1, 1).toordinal()
    seconds = (
        day_count * 86400
        + int(match.group("hour")) * 3600
        + int(match.group("minute")) * 60
        + int(match.group("second"))
    )
    if match.group("zone") != "Z":
        offset = int(match.group("offset_hour")) * 3600 + int(
            match.group("offset_minute")
        ) * 60
        seconds += -offset if match.group("sign") == "+" else offset
    fraction = (match.group("fraction") or "").ljust(9, "0")
    return seconds * 1_000_000_000 + int(fraction or "0")


def combine_statuses(statuses: Iterable[Status]) -> Status:
    materialized = list(statuses)
    if Status.FAIL in materialized:
        return Status.FAIL
    if Status.OPEN in materialized:
        return Status.OPEN
    return Status.PASS


def exit_code(status: Status) -> int:
    return {Status.PASS: 0, Status.FAIL: 1, Status.OPEN: 2}[status]
