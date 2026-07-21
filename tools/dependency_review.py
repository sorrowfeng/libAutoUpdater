#!/usr/bin/env python3
"""Validate offline RISK-005 dependency inventory and advisory evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import unquote_to_bytes

from evidence_contract import (
    EvidenceError,
    Status,
    combine_statuses,
    exit_code,
    parse_json_bytes,
    read_limited_bytes,
    require_array,
    require_bool,
    require_enum,
    require_exact_keys,
    require_integer,
    require_nullable_bool,
    require_object,
    require_sha256,
    require_text,
    timestamp_nanoseconds,
)


RISK_ID = "RISK-005"
SCOPES = (
    "openssl",
    "curl",
    "qt",
    "native-http",
    "github-actions",
    "package-manager",
    "os-runtime",
    "build-tools",
)
SCOPE_SET = set(SCOPES)
CAPTURE_KINDS = {
    "build-configuration",
    "dependency-lock",
    "package-list",
    "runtime-scan",
    "workflow-definition",
    "toolchain",
    "os-version",
    "other",
}
COMPONENT_KINDS = {"runtime", "build", "test", "ci-action", "system"}
AUTHORITY_KINDS = {
    "upstream-security",
    "platform-security",
    "distribution-security",
    "github-security",
}
SOURCE_KINDS = AUTHORITY_KINDS | {"remediation-evidence"}
AUTHORITIES_BY_SCOPE = {
    "openssl": {"upstream-security", "distribution-security"},
    "curl": {"upstream-security", "distribution-security"},
    "qt": {"upstream-security", "distribution-security"},
    "native-http": {"platform-security"},
    "github-actions": {"github-security", "upstream-security"},
    "package-manager": {
        "upstream-security",
        "distribution-security",
        "github-security",
    },
    "os-runtime": {
        "platform-security",
        "distribution-security",
        "upstream-security",
    },
    "build-tools": {
        "upstream-security",
        "distribution-security",
        "github-security",
    },
}
IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$", re.ASCII)
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$", re.ASCII)
PURL_PATTERN = re.compile(
    r"^pkg:(?P<type>[a-z][a-z0-9.+-]*)/(?P<path>[^@?#]+)@(?P<version>[^?#]+)(?:\?(?P<qualifiers>[^#]+))?(?:#(?P<subpath>.+))?$",
    re.ASCII,
)
PURL_SEGMENT_PATTERN = re.compile(
    r"^(?:[A-Za-z0-9._~:+-]|%[0-9A-Fa-f]{2})+$", re.ASCII
)
PURL_QUALIFIER_KEY_PATTERN = re.compile(r"^[a-z][a-z0-9._-]*$", re.ASCII)
PURL_PROFILE_VALUE_PATTERN = re.compile(
    r"^[A-Za-z0-9]+(?:[._-][A-Za-z0-9]+)*$", re.ASCII
)
CPE_FIELD_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._~-]*$", re.ASCII)
ACTION_OWNER_PATTERN = re.compile(
    r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$", re.ASCII
)
EXACT_VERSION_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:+~-]{0,127}$", re.ASCII)
MAX_VALIDITY_NANOSECONDS = 7 * 24 * 60 * 60 * 1_000_000_000
MAX_EVIDENCE_ARGUMENTS = 512
MAX_CAPTURES = 128
MAX_COMPONENTS = 1024
MAX_SOURCES = 256
MAX_COMPONENT_REVIEWS = 1024
MAX_ADVISORIES_PER_COMPONENT = 256
NON_OPTIONAL_SCOPES = {"os-runtime", "build-tools"}
PRIMARY_IDENTITY_SCOPES = {
    "openssl",
    "curl",
    "qt",
    "native-http",
    "package-manager",
    "os-runtime",
}
CAPTURE_KINDS_BY_SCOPE = {
    "openssl": {"dependency-lock", "package-list", "runtime-scan"},
    "curl": {"dependency-lock", "package-list", "runtime-scan"},
    "qt": {"dependency-lock", "package-list", "runtime-scan"},
    "native-http": {
        "os-version",
        "package-list",
        "runtime-scan",
    },
    "github-actions": {"workflow-definition"},
    "package-manager": {
        "dependency-lock",
        "package-list",
        "toolchain",
    },
    "os-runtime": {"os-version", "package-list", "runtime-scan"},
    "build-tools": {
        "dependency-lock",
        "package-list",
        "toolchain",
    },
}
PURL_TYPES_BY_SCOPE = {
    "openssl": {"apk", "brew", "conan", "deb", "generic", "github", "rpm", "vcpkg"},
    "curl": {"apk", "brew", "conan", "deb", "generic", "github", "rpm", "vcpkg"},
    "qt": {"apk", "brew", "conan", "deb", "generic", "github", "rpm", "vcpkg"},
    "native-http": set(),
    "package-manager": {
        "brew",
        "cargo",
        "conan",
        "generic",
        "github",
        "golang",
        "npm",
        "nuget",
        "pypi",
        "vcpkg",
    },
    "os-runtime": set(),
    "build-tools": {
        "apk",
        "brew",
        "cargo",
        "deb",
        "generic",
        "github",
        "golang",
        "npm",
        "nuget",
        "pypi",
        "rpm",
    },
}
CPE_PARTS_BY_SCOPE = {
    "openssl": {"a"},
    "curl": {"a"},
    "qt": {"a"},
    "native-http": {"a"},
    "package-manager": {"a"},
    "os-runtime": {"o"},
    "build-tools": {"a"},
}
WINDOWS_OS_CPE_PRODUCTS = {
    "windows7",
    "windows8",
    "windows81",
    "windows10",
    "windows11",
    "windowsembedded",
    "windowsiot",
    "windowsphone",
    "windowsrt",
    "windowsserver",
    "windowsserver2003",
    "windowsserver2008",
    "windowsserver2008r2",
    "windowsserver2012",
    "windowsserver2012r2",
    "windowsserver2016",
    "windowsserver2019",
    "windowsserver2022",
    "windowsserver2025",
    "windowsvista",
    "windowsxp",
}
WINDOWS_NATIVE_HTTP_CPE_PRODUCTS = {"winhttp"}
MACOS_OS_CPE_PRODUCTS = {"darwin", "macos", "macosserver", "macosx"}
MACOS_NATIVE_HTTP_CPE_PRODUCTS = {"cfnetwork"}
LINUX_CPE_PRODUCTS_BY_VENDOR = {
    "almalinux": {"almalinux"},
    "alpinelinux": {"alpinelinux"},
    "amazon": {"amazonlinux"},
    "canonical": {"ubuntulinux"},
    "centos": {"centos"},
    "debian": {"debianlinux"},
    "fedoraproject": {"fedora"},
    "linux": {"linuxkernel"},
    "opensuse": {"leap", "opensuse"},
    "oracle": {"linux", "oraclelinux"},
    "redhat": {"enterpriselinux", "redhatenterpriselinux"},
    "rocky": {"rockylinux"},
    "suse": {"linuxenterprisedesktop", "linuxenterpriseserver", "suse"},
}
ARCHITECTURE_ALIASES = {
    "x8664": {"amd64", "x64", "x8664"},
    "arm64": {"aarch64", "arm64", "armv8"},
    "x86": {"i386", "i486", "i586", "i686", "ia32", "x86"},
    "armv7": {"arm", "armhf", "armv7", "armv7l"},
    "ppc64le": {"powerpc64le", "ppc64le"},
    "s390x": {"s390x"},
}
PLATFORM_QUALIFIER_BASES = {
    "windows": {
        "win32",
        "win64",
        "windows",
        "windows-server",
        "windowsserver",
    },
    "macos": {"darwin", "macos", "macosx", "osx"},
    "linux": {
        "alma-linux",
        "almalinux",
        "alpine",
        "alpine-linux",
        "amazon-linux",
        "amazonlinux",
        "centos",
        "debian",
        "fedora",
        "linux",
        "open-suse",
        "opensuse",
        "oracle-linux",
        "oraclelinux",
        "red-hat",
        "redhat",
        "rhel",
        "rocky-linux",
        "rockylinux",
        "suse",
        "ubuntu",
    },
}
PLATFORM_VERSION_SUFFIX_PATTERN = re.compile(
    r"^(?:[._-]?[0-9]+(?:[._-][0-9]+)*)?$", re.ASCII
)
VCPKG_TRIPLET_PATTERN = re.compile(
    r"^(?P<architecture>[A-Za-z0-9]+(?:[._][A-Za-z0-9]+)*)-"
    r"(?P<target>[A-Za-z0-9]+)(?:-[A-Za-z0-9]+)*$",
    re.ASCII,
)
OPENSSL_IDENTITY_PATTERN = re.compile(
    r"^(?:openssl(?:[0-9]+|devel|libs)?|libssl(?:[0-9]+(?:t64)?|dev)?)$", re.ASCII
)
CURL_IDENTITY_PATTERN = re.compile(
    r"^(?:curl(?:devel|minimal)?|libcurl(?:[0-9]+)?(?:gnutls|openssl)?(?:dev|devel|minimal)?)$",
    re.ASCII,
)
QT_IDENTITY_PATTERN = re.compile(
    r"^(?:qt|qtbase|qtbase[56](?:dev(?:tools)?)?|qt[56](?:qtbase(?:common|dev|devel|gui|private)?|(?:(?:base|core|gui|network|widgets)(?:dev)?))?|libqt[56](?:core|gui|network|widgets)[0-9]+[a-z0-9]*)$",
    re.ASCII,
)
PACKAGE_MANAGER_IDENTITIES = {"brew", "conan", "homebrew", "vcpkg"}
UNRESOLVED_VERSIONS = {
    "noassertion",
    "unknown",
    "unresolved",
    "latest",
    "stable",
    "rolling",
    "head",
    "main",
    "master",
    "builtin-baseline-only",
    "baseline-only",
}
ROLLING_VERSION_TOKENS = {
    "current",
    "default",
    "dev",
    "development",
    "head",
    "latest",
    "main",
    "master",
    "next",
    "nightly",
    "noassertion",
    "rolling",
    "snapshot",
    "stable",
    "tip",
    "trunk",
    "unknown",
    "unresolved",
    "unversioned",
    "vnext",
}


@dataclass(frozen=True)
class Capture:
    identifier: str
    kind: str
    digest: str
    captured_at: int
    complete: bool


@dataclass(frozen=True)
class Component:
    identifier: str
    name: str
    scope: str
    kind: str
    direct: bool
    version: str | None
    resolution: str
    coordinate: str | None
    capture_id: str


@dataclass(frozen=True)
class Inventory:
    environment: str
    platform: str
    captured_at: int
    valid_until: int
    captures: dict[str, Capture]
    components: dict[str, Component]
    declared_evidence: set[str]


@dataclass(frozen=True)
class Source:
    identifier: str
    kind: str
    authority: str
    digest: str
    inventory_digest: str
    captured_at: int
    valid_until: int
    complete: bool
    covered_components: dict[str, tuple[str, str]]
    remediations: set[tuple[str, str, str, str]]


def check(identifier: str, status: Status, reason: str) -> dict[str, str]:
    return {"id": identifier, "status": status.value, "reason": reason}


def identifier(value: Any, context: str) -> str:
    text = require_text(value, context, maximum=128)
    if IDENTIFIER_PATTERN.fullmatch(text) is None:
        raise EvidenceError(f"{context} is not a safe evidence identifier")
    return text


def nullable_text(value: Any, context: str, *, maximum: int = 512) -> str | None:
    if value is None:
        return None
    return require_text(value, context, maximum=maximum)


def nullable_identifier(value: Any, context: str) -> str | None:
    if value is None:
        return None
    return identifier(value, context)


def nullable_sha256(value: Any, context: str) -> str | None:
    if value is None:
        return None
    return require_sha256(value, context)


def schema_version(document: dict[str, Any], context: str) -> None:
    if require_integer(document["schemaVersion"], f"{context}.schemaVersion") != 1:
        raise EvidenceError(f"{context}.schemaVersion must be integer 1")


def bounded_array(value: Any, context: str, maximum: int) -> list[Any]:
    array = require_array(value, context)
    if len(array) > maximum:
        raise EvidenceError(f"{context} exceeds the {maximum}-item limit")
    return array


def parse_evidence_paths(values: list[str]) -> dict[str, Path]:
    if len(values) > MAX_EVIDENCE_ARGUMENTS:
        raise EvidenceError("too many evidence arguments")
    paths: dict[str, Path] = {}
    for index, value in enumerate(values):
        if "=" not in value:
            raise EvidenceError(f"evidence argument {index} must use id=path")
        raw_identifier, raw_path = value.split("=", 1)
        evidence_id = identifier(raw_identifier, f"evidence argument {index} id")
        if evidence_id in paths:
            raise EvidenceError("duplicate evidence argument identifier")
        if not raw_path:
            raise EvidenceError(f"evidence argument {index} path is empty")
        paths[evidence_id] = Path(raw_path)
    return paths


def binding_result(
    evidence_id: str,
    expected_digest: str,
    paths: dict[str, Path],
    *,
    check_id: str,
    description: str,
) -> tuple[dict[str, str], bytes | None]:
    path = paths.get(evidence_id)
    if path is None:
        return check(check_id, Status.OPEN, f"{description} was not supplied"), None
    try:
        contents = read_limited_bytes(path)
    except EvidenceError:
        return check(check_id, Status.OPEN, f"{description} is unreadable"), None
    if hashlib.sha256(contents).hexdigest() != expected_digest:
        return check(check_id, Status.FAIL, f"{description} digest does not match"), None
    return (
        check(check_id, Status.PASS, f"{description} bytes match the snapshot"),
        contents,
    )


def binding_check(
    evidence_id: str,
    expected_digest: str,
    paths: dict[str, Path],
    *,
    check_id: str,
    description: str,
) -> dict[str, str]:
    result, _ = binding_result(
        evidence_id,
        expected_digest,
        paths,
        check_id=check_id,
        description=description,
    )
    return result


def window_checks(
    captured_at: int,
    valid_until: int,
    evaluated_at: int,
    *,
    prefix: str,
    description: str,
) -> list[dict[str, str]]:
    if valid_until <= captured_at:
        raise EvidenceError(f"{description} validity must end after capture")
    checks: list[dict[str, str]] = []
    if valid_until - captured_at > MAX_VALIDITY_NANOSECONDS:
        checks.append(
            check(
                f"{prefix}.window",
                Status.FAIL,
                f"{description} validity exceeds the seven-day maximum",
            )
        )
    if captured_at > evaluated_at:
        checks.append(
            check(
                f"{prefix}.age",
                Status.FAIL,
                f"{description} capture is after evaluation time",
            )
        )
    elif evaluated_at > valid_until:
        checks.append(
            check(
                f"{prefix}.age",
                Status.OPEN,
                f"{description} validity has elapsed",
            )
        )
    else:
        checks.append(
            check(
                f"{prefix}.age",
                Status.PASS,
                f"{description} evidence is current",
            )
        )
    return checks


def attestation_checks(
    value: Any,
    *,
    context: str,
    prefix: str,
    environment: str,
    captured_at: int,
    evaluated_at: int,
) -> list[dict[str, str]]:
    attestation = require_object(value, context)
    require_exact_keys(
        attestation,
        context,
        {"productionSnapshot", "reviewedBy", "reviewedAt"},
    )
    production = require_bool(
        attestation["productionSnapshot"], f"{context}.productionSnapshot"
    )
    reviewed_by = nullable_text(attestation["reviewedBy"], f"{context}.reviewedBy")
    reviewed_at_text = nullable_text(
        attestation["reviewedAt"], f"{context}.reviewedAt", maximum=40
    )
    reviewed_at = None
    if reviewed_at_text is not None:
        reviewed_at = timestamp_nanoseconds(reviewed_at_text, f"{context}.reviewedAt")
        if reviewed_at < captured_at:
            raise EvidenceError(f"{context}.reviewedAt must not precede capture")
    checks: list[dict[str, str]] = []
    if reviewed_at is not None and reviewed_at > evaluated_at:
        checks.append(check(f"{prefix}.time", Status.FAIL, "review is after evaluation time"))
    if (
        environment == "production"
        and production
        and reviewed_by is not None
        and reviewed_at is not None
    ):
        checks.append(check(f"{prefix}.production", Status.PASS, "production snapshot is signed off"))
    else:
        checks.append(check(f"{prefix}.production", Status.OPEN, "production snapshot is not signed off"))
    return checks


def looks_unresolved_version(value: str) -> bool:
    lowered = value.casefold()
    if lowered in UNRESOLVED_VERSIONS:
        return True
    tokens = set(filter(None, re.split(r"[^a-z0-9]+", lowered, flags=re.ASCII)))
    if tokens & ROLLING_VERSION_TOKENS:
        return True
    if EXACT_VERSION_PATTERN.fullmatch(value) is None:
        return True
    if value.startswith(("^", "~")) or value.endswith("+"):
        return True
    if re.search(r"(?:^|[.\s_-])x(?:$|[.\s_-])", lowered, re.ASCII):
        return True
    if re.search(
        r"(?:^|[^a-z0-9])(?:and|any|later|newer|older|or|range|to)(?:$|[^a-z0-9])",
        lowered,
        re.ASCII,
    ):
        return True
    if re.search(r"(?:and|or)(?:later|newer|older)$", lowered, re.ASCII):
        return True
    return False


def normalized_identity(value: str) -> str:
    return "".join(character.casefold() for character in value if character.isalnum())


def decode_purl_token(value: str) -> str | None:
    if PURL_SEGMENT_PATTERN.fullmatch(value) is None:
        return None
    try:
        decoded = unquote_to_bytes(value).decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    if (
        not decoded
        or decoded != decoded.strip()
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in decoded)
    ):
        return None
    return decoded


def parse_purl_coordinate(
    value: str,
) -> tuple[str, str, str, dict[str, str]] | None:
    match = PURL_PATTERN.fullmatch(value)
    if match is None:
        return None
    path = match.group("path").split("/")
    decoded_path = [decode_purl_token(segment) for segment in path]
    if (
        any(
            segment is None
            or segment in {".", ".."}
            or any(character in segment for character in "/?#")
            for segment in decoded_path
        )
        or not decoded_path
    ):
        return None
    version = decode_purl_token(match.group("version"))
    if version is None:
        return None
    qualifiers = match.group("qualifiers")
    decoded_qualifiers: dict[str, str] = {}
    if qualifiers is not None:
        keys: list[str] = []
        for pair in qualifiers.split("&"):
            key, separator, raw_value = pair.partition("=")
            decoded_value = decode_purl_token(raw_value)
            if (
                separator != "="
                or PURL_QUALIFIER_KEY_PATTERN.fullmatch(key) is None
                or decoded_value is None
            ):
                return None
            keys.append(key)
            decoded_qualifiers[key] = decoded_value
        if len(keys) != len(set(keys)) or keys != sorted(keys):
            return None
    subpath = match.group("subpath")
    if subpath is not None:
        for raw_segment in subpath.split("/"):
            segment = decode_purl_token(raw_segment)
            if (
                segment is None
                or segment in {".", ".."}
                or any(character in segment for character in "/?#")
            ):
                return None
    name = decoded_path[-1]
    if name is None or any(character in name for character in "/?#"):
        return None
    return match.group("type"), name, version, decoded_qualifiers


def action_coordinate_commit(value: str) -> str | None:
    path, separator, commit = value.rpartition("@")
    if separator != "@" or COMMIT_PATTERN.fullmatch(commit) is None or commit == "0" * 40:
        return None
    segments = path.split("/")
    if len(segments) < 2:
        return None
    for index, segment in enumerate(segments):
        if (
            not segment
            or segment in {".", ".."}
            or re.fullmatch(r"[A-Za-z0-9_.-]+", segment, re.ASCII) is None
            or not normalized_identity(segment)
            or (index == 0 and ACTION_OWNER_PATTERN.fullmatch(segment) is None)
        ):
            return None
    return commit


def coordinate_identity(component: Component) -> str | None:
    if component.coordinate is None:
        return None
    purl = parse_purl_coordinate(component.coordinate)
    if purl is not None:
        return normalized_identity(purl[1])
    if component.coordinate.startswith("cpe:2.3:"):
        fields = component.coordinate.split(":")
        if len(fields) == 13:
            return normalized_identity(fields[4])
    return None


def is_primary_scope_identity(component: Component) -> bool:
    identity = coordinate_identity(component)
    if identity is None:
        return False
    if component.scope == "openssl":
        return OPENSSL_IDENTITY_PATTERN.fullmatch(identity) is not None
    if component.scope == "curl":
        return CURL_IDENTITY_PATTERN.fullmatch(identity) is not None
    if component.scope == "qt":
        return QT_IDENTITY_PATTERN.fullmatch(identity) is not None
    if component.scope == "package-manager":
        return identity in PACKAGE_MANAGER_IDENTITIES
    if component.scope == "native-http":
        return identity in (
            WINDOWS_NATIVE_HTTP_CPE_PRODUCTS | MACOS_NATIVE_HTTP_CPE_PRODUCTS
        )
    if component.scope == "os-runtime":
        os_products = WINDOWS_OS_CPE_PRODUCTS | MACOS_OS_CPE_PRODUCTS
        os_products.update(
            product
            for products in LINUX_CPE_PRODUCTS_BY_VENDOR.values()
            for product in products
        )
        return identity in os_products
    return False


def authority_kinds_for_component(component: Component, *, platform: str) -> set[str]:
    if component.scope == "github-actions":
        return {"github-security", "upstream-security"}
    if component.coordinate is None:
        return set()
    purl = parse_purl_coordinate(component.coordinate)
    if purl is not None:
        package_type = purl[0]
        if package_type in {"apk", "brew", "deb", "rpm"}:
            return {"distribution-security"}
        if package_type in {"cargo", "github", "golang", "npm", "nuget", "pypi"}:
            return {"github-security", "upstream-security"}
        return {"upstream-security"}
    if component.coordinate.startswith("cpe:2.3:"):
        fields = component.coordinate.split(":")
        if len(fields) != 13:
            return set()
        vendor = normalized_identity(fields[3])
        if component.scope in {"native-http", "os-runtime"}:
            if platform in {"windows", "macos"}:
                return {"platform-security"}
            if vendor == "linux":
                return {"upstream-security"}
            return {"distribution-security"}
        if vendor in LINUX_CPE_PRODUCTS_BY_VENDOR:
            return (
                {"upstream-security"}
                if vendor == "linux"
                else {"distribution-security"}
            )
        return {"upstream-security"}
    return set()


def cpe_matches_platform(
    vendor: str, product: str, platform: str, *, scope: str
) -> bool:
    normalized_vendor = normalized_identity(vendor)
    normalized_product = normalized_identity(product)
    if scope == "native-http":
        if platform == "windows":
            return (
                normalized_vendor == "microsoft"
                and normalized_product in WINDOWS_NATIVE_HTTP_CPE_PRODUCTS
            )
        if platform == "macos":
            return (
                normalized_vendor == "apple"
                and normalized_product in MACOS_NATIVE_HTTP_CPE_PRODUCTS
            )
        return False
    if platform == "windows":
        return (
            normalized_vendor == "microsoft"
            and normalized_product in WINDOWS_OS_CPE_PRODUCTS
        )
    if platform == "macos":
        return normalized_vendor == "apple" and normalized_product in MACOS_OS_CPE_PRODUCTS
    products = LINUX_CPE_PRODUCTS_BY_VENDOR.get(normalized_vendor, set())
    return normalized_product in products


def cpe_target_software_matches(value: str, platform: str) -> bool:
    if value in {"*", "-"}:
        return True
    normalized = normalized_identity(value)
    if platform == "windows":
        return normalized in (
            WINDOWS_OS_CPE_PRODUCTS | WINDOWS_NATIVE_HTTP_CPE_PRODUCTS
        ) or normalized == "windows"
    if platform == "macos":
        return normalized in (MACOS_OS_CPE_PRODUCTS | MACOS_NATIVE_HTTP_CPE_PRODUCTS)
    return normalized == "linux"


def cpe_target_hardware_matches(value: str, architecture: str) -> bool:
    if value in {"*", "-"}:
        return True
    normalized_capture = normalized_identity(value)
    normalized_profile = normalized_identity(architecture)
    for aliases in ARCHITECTURE_ALIASES.values():
        if normalized_profile in aliases:
            return normalized_capture in aliases
    return normalized_capture == normalized_profile


def qualifier_platform(value: str) -> str | None:
    if PURL_PROFILE_VALUE_PATTERN.fullmatch(value) is None:
        return None
    folded = value.casefold()
    for platform, bases in PLATFORM_QUALIFIER_BASES.items():
        for base in sorted(bases, key=len, reverse=True):
            if folded.startswith(base) and PLATFORM_VERSION_SUFFIX_PATTERN.fullmatch(
                folded[len(base) :]
            ):
                return platform
    return None


def vcpkg_triplet_matches_profile(
    value: str, *, platform: str, architecture: str
) -> bool:
    match = VCPKG_TRIPLET_PATTERN.fullmatch(value)
    if match is None or not cpe_target_hardware_matches(
        match.group("architecture"), architecture
    ):
        return False
    target_platform = {
        "linux": "linux",
        "macos": "macos",
        "mingw": "windows",
        "osx": "macos",
        "uwp": "windows",
        "windows": "windows",
    }.get(match.group("target").casefold())
    return target_platform == platform


def purl_matches_profile(
    package_type: str,
    qualifiers: dict[str, str],
    *,
    platform: str,
    architecture: str,
) -> bool:
    if package_type in {"apk", "deb", "rpm"} and platform != "linux":
        return False
    if package_type == "brew" and platform not in {"linux", "macos"}:
        return False
    for key in (
        "arch",
        "architecture",
        "cpu",
        "target_arch",
        "target_cpu",
        "target_hw",
    ):
        value = qualifiers.get(key)
        if value is not None:
            if PURL_PROFILE_VALUE_PATTERN.fullmatch(value) is None:
                return False
            normalized = normalized_identity(value)
            neutral_architectures = {
                "apk": {"noarch"},
                "deb": {"all"},
                "rpm": {"noarch"},
            }.get(package_type, set())
            if normalized in neutral_architectures:
                continue
            if normalized in {"universal", "universal2"}:
                profile_architecture = normalized_identity(architecture)
                if package_type != "brew" or platform != "macos" or not any(
                    profile_architecture in aliases
                    for aliases in (
                        ARCHITECTURE_ALIASES["x8664"],
                        ARCHITECTURE_ALIASES["arm64"],
                    )
                ):
                    return False
            else:
                if not cpe_target_hardware_matches(value, architecture):
                    return False
    for key in (
        "distro",
        "distro_name",
        "os",
        "platform",
        "target_os",
        "target_platform",
        "target_sw",
    ):
        value = qualifiers.get(key)
        if value is not None and qualifier_platform(value) != platform:
            return False
    triplet = qualifiers.get("triplet")
    if triplet is None:
        return True
    return package_type == "vcpkg" and vcpkg_triplet_matches_profile(
        triplet, platform=platform, architecture=architecture
    )


def cpe_component_name_matches(
    component_name: str, product: str, *, scope: str, platform: str
) -> bool:
    name = normalized_identity(component_name)
    normalized_product = normalized_identity(product)
    if name == normalized_product:
        return True
    if scope != "os-runtime":
        return False
    if platform == "windows":
        if name == "windows":
            return normalized_product in WINDOWS_OS_CPE_PRODUCTS
        return name == "windowsserver" and normalized_product.startswith("windowsserver")
    if platform == "macos":
        return name == "macos" and normalized_product in {"macos", "macosx"}
    aliases = {
        "almalinux": {"alma", "almalinux"},
        "alpinelinux": {"alpine", "alpinelinux"},
        "amazonlinux": {"amazonlinux"},
        "ubuntulinux": {"ubuntu", "ubuntulinux"},
        "centos": {"centos"},
        "debianlinux": {"debian", "debianlinux"},
        "fedora": {"fedora"},
        "linuxkernel": {"linux", "linuxkernel"},
        "leap": {"opensuse", "opensuseleap"},
        "opensuse": {"opensuse"},
        "linux": {"oraclelinux"},
        "oraclelinux": {"oraclelinux"},
        "enterpriselinux": {"redhatenterpriselinux"},
        "redhatenterpriselinux": {"redhatenterpriselinux"},
        "rockylinux": {"rocky", "rockylinux"},
        "linuxenterprisedesktop": {"suselinuxenterprisedesktop"},
        "linuxenterpriseserver": {"suselinuxenterpriseserver"},
        "suse": {"suse"},
    }
    return name in aliases.get(normalized_product, set())


def coordinate_matches_component(
    component: Component, *, platform: str, architecture: str
) -> bool:
    if component.coordinate is None or component.version is None:
        return False
    purl = parse_purl_coordinate(component.coordinate)
    if purl is not None:
        coordinate_type, coordinate_name, coordinate_version, qualifiers = purl
        return (
            coordinate_type in PURL_TYPES_BY_SCOPE[component.scope]
            and coordinate_name.casefold() == component.name.casefold()
            and coordinate_version == component.version
            and not looks_unresolved_version(coordinate_version)
            and purl_matches_profile(
                coordinate_type,
                qualifiers,
                platform=platform,
                architecture=architecture,
            )
        )
    if component.coordinate.startswith("cpe:2.3:"):
        fields = component.coordinate.split(":")
        if (
            len(fields) != 13
            or fields[2] not in CPE_PARTS_BY_SCOPE[component.scope]
            or any(
                not field
                or (field not in {"*", "-"} and CPE_FIELD_PATTERN.fullmatch(field) is None)
                for field in fields[2:13]
            )
            or fields[3] in {"", "*", "-"}
            or fields[4] in {"", "*", "-"}
            or looks_unresolved_version(fields[5])
            or (
                normalized_identity(fields[3])
                in LINUX_CPE_PRODUCTS_BY_VENDOR
                and platform != "linux"
            )
            or (
                component.scope in {"native-http", "os-runtime"}
                and not cpe_matches_platform(
                    fields[3], fields[4], platform, scope=component.scope
                )
            )
            or not cpe_target_software_matches(fields[10], platform)
            or not cpe_target_hardware_matches(fields[11], architecture)
        ):
            return False
        return (
            fields[5] == component.version
            and cpe_component_name_matches(
                component.name,
                fields[4],
                scope=component.scope,
                platform=platform,
            )
        )
    return False


def parse_capture(
    value: Any,
    *,
    context: str,
) -> Capture:
    capture = require_object(value, context)
    require_exact_keys(
        capture,
        context,
        {"id", "kind", "sha256", "capturedAt", "complete"},
    )
    return Capture(
        identifier=identifier(capture["id"], f"{context}.id"),
        kind=require_enum(capture["kind"], f"{context}.kind", CAPTURE_KINDS),
        digest=require_sha256(capture["sha256"], f"{context}.sha256"),
        captured_at=timestamp_nanoseconds(
            capture["capturedAt"], f"{context}.capturedAt"
        ),
        complete=require_bool(capture["complete"], f"{context}.complete"),
    )


def parse_profile_binding(
    document: dict[str, Any], *, context: str
) -> tuple[str, str, str, str]:
    deployment_id = identifier(document["deploymentId"], f"{context}.deploymentId")
    platform = require_enum(
        document["platform"], f"{context}.platform", {"windows", "macos", "linux"}
    )
    architecture = require_text(
        document["architecture"], f"{context}.architecture", maximum=64
    )
    source_commit = require_text(
        document["sourceCommit"], f"{context}.sourceCommit", maximum=40
    )
    if COMMIT_PATTERN.fullmatch(source_commit) is None or source_commit == "0" * 40:
        raise EvidenceError(f"{context}.sourceCommit is invalid")
    return deployment_id, platform, architecture, source_commit


def parse_build_configuration_capture(
    contents: bytes,
) -> tuple[str, str, str, str, bool, dict[str, bool | None]]:
    document = require_object(parse_json_bytes(contents), "build configuration capture")
    require_exact_keys(
        document,
        "build configuration capture",
        {
            "schemaVersion",
            "deploymentId",
            "platform",
            "architecture",
            "sourceCommit",
            "complete",
            "scopeApplicability",
        },
    )
    schema_version(document, "build configuration capture")
    deployment_id, platform, architecture, source_commit = parse_profile_binding(
        document, context="build configuration capture"
    )
    complete = require_bool(
        document["complete"], "build configuration capture.complete"
    )
    applicability: dict[str, bool | None] = {}
    for index, raw_scope in enumerate(
        bounded_array(
            document["scopeApplicability"],
            "build configuration capture.scopeApplicability",
            len(SCOPES),
        )
    ):
        context = f"build configuration capture.scopeApplicability[{index}]"
        scope = require_object(raw_scope, context)
        require_exact_keys(scope, context, {"scope", "applicable"})
        name = require_enum(scope["scope"], f"{context}.scope", SCOPE_SET)
        if name in applicability:
            raise EvidenceError("build configuration contains duplicate scope applicability")
        applicability[name] = require_nullable_bool(
            scope["applicable"], f"{context}.applicable"
        )
    return (
        deployment_id,
        platform,
        architecture,
        source_commit,
        complete,
        applicability,
    )


def parse_os_version_capture(
    contents: bytes,
) -> tuple[str, str, str, str, bool, dict[str, tuple[str, str]]]:
    document = require_object(parse_json_bytes(contents), "OS version capture")
    require_exact_keys(
        document,
        "OS version capture",
        {
            "schemaVersion",
            "deploymentId",
            "platform",
            "architecture",
            "sourceCommit",
            "complete",
            "components",
        },
    )
    schema_version(document, "OS version capture")
    deployment_id, platform, architecture, source_commit = parse_profile_binding(
        document, context="OS version capture"
    )
    complete = require_bool(document["complete"], "OS version capture.complete")
    components: dict[str, tuple[str, str]] = {}
    for index, raw_component in enumerate(
        bounded_array(document["components"], "OS version capture.components", MAX_COMPONENTS)
    ):
        context = f"OS version capture.components[{index}]"
        component = require_object(raw_component, context)
        require_exact_keys(component, context, {"componentId", "version", "coordinate"})
        component_id = identifier(component["componentId"], f"{context}.componentId")
        if component_id in components:
            raise EvidenceError("OS version capture contains a duplicate component")
        components[component_id] = (
            require_text(component["version"], f"{context}.version", maximum=128),
            require_text(component["coordinate"], f"{context}.coordinate", maximum=1024),
        )
    return deployment_id, platform, architecture, source_commit, complete, components


def parse_workflow_capture(contents: bytes) -> tuple[bool, set[tuple[str, str]]]:
    document = require_object(parse_json_bytes(contents), "workflow capture")
    require_exact_keys(
        document,
        "workflow capture",
        {"schemaVersion", "complete", "actions"},
    )
    schema_version(document, "workflow capture")
    complete = require_bool(document["complete"], "workflow capture.complete")
    actions: set[tuple[str, str]] = set()
    for index, raw_action in enumerate(
        bounded_array(document["actions"], "workflow capture.actions", MAX_COMPONENTS)
    ):
        context = f"workflow capture.actions[{index}]"
        action = require_object(raw_action, context)
        require_exact_keys(action, context, {"coordinate", "version"})
        coordinate = require_text(
            action["coordinate"], f"{context}.coordinate", maximum=1024
        )
        version = require_text(action["version"], f"{context}.version", maximum=40)
        if action_coordinate_commit(coordinate) != version:
            raise EvidenceError("workflow capture contains an unpinned Action")
        entry = (coordinate, version)
        if entry in actions:
            raise EvidenceError("workflow capture contains a duplicate Action")
        actions.add(entry)
    return complete, actions


def parse_component(value: Any, *, context: str) -> Component:
    component = require_object(value, context)
    require_exact_keys(
        component,
        context,
        {
            "id",
            "name",
            "scope",
            "kind",
            "direct",
            "version",
            "resolution",
            "coordinate",
            "captureId",
        },
    )
    name = require_text(component["name"], f"{context}.name")
    direct = require_bool(component["direct"], f"{context}.direct")
    return Component(
        identifier=identifier(component["id"], f"{context}.id"),
        name=name,
        scope=require_enum(component["scope"], f"{context}.scope", SCOPE_SET),
        kind=require_enum(component["kind"], f"{context}.kind", COMPONENT_KINDS),
        direct=direct,
        version=nullable_text(component["version"], f"{context}.version"),
        resolution=require_enum(
            component["resolution"], f"{context}.resolution", {"EXACT", "OPEN"}
        ),
        coordinate=nullable_text(
            component["coordinate"], f"{context}.coordinate", maximum=1024
        ),
        capture_id=identifier(component["captureId"], f"{context}.captureId"),
    )


def validate_inventory(
    document: Any,
    *,
    evaluated_at: int,
    evidence_paths: dict[str, Path],
) -> tuple[list[dict[str, str]], Inventory]:
    root = require_object(document, "inventory")
    require_exact_keys(
        root,
        "inventory",
        {
            "schemaVersion",
            "deployment",
            "coverage",
            "captures",
            "components",
            "inventoryComplete",
            "attestation",
        },
    )
    schema_version(root, "inventory")
    deployment = require_object(root["deployment"], "inventory.deployment")
    require_exact_keys(
        deployment,
        "inventory.deployment",
        {
            "id",
            "environment",
            "platform",
            "architecture",
            "sourceCommit",
            "capturedAt",
            "validUntil",
            "buildConfigurationCaptureId",
        },
    )
    deployment_id = identifier(deployment["id"], "inventory.deployment.id")
    environment = require_enum(
        deployment["environment"],
        "inventory.deployment.environment",
        {"production", "staging", "test", "development"},
    )
    platform = require_enum(
        deployment["platform"],
        "inventory.deployment.platform",
        {"windows", "macos", "linux"},
    )
    architecture = require_text(
        deployment["architecture"],
        "inventory.deployment.architecture",
        maximum=64,
    )
    source_commit = require_text(
        deployment["sourceCommit"], "inventory.deployment.sourceCommit", maximum=40
    )
    if COMMIT_PATTERN.fullmatch(source_commit) is None:
        raise EvidenceError("inventory.deployment.sourceCommit must be a full lowercase commit SHA")
    captured_at = timestamp_nanoseconds(
        deployment["capturedAt"], "inventory.deployment.capturedAt"
    )
    valid_until = timestamp_nanoseconds(
        deployment["validUntil"], "inventory.deployment.validUntil"
    )
    build_configuration_id = nullable_identifier(
        deployment["buildConfigurationCaptureId"],
        "inventory.deployment.buildConfigurationCaptureId",
    )
    inventory_complete = require_bool(
        root["inventoryComplete"], "inventory.inventoryComplete"
    )

    checks = window_checks(
        captured_at,
        valid_until,
        evaluated_at,
        prefix="inventory",
        description="dependency inventory",
    )
    checks.extend(
        attestation_checks(
            root["attestation"],
            context="inventory.attestation",
            prefix="inventory.attestation",
            environment=environment,
            captured_at=captured_at,
            evaluated_at=evaluated_at,
        )
    )
    checks.append(
        check(
            "inventory.complete",
            Status.PASS if inventory_complete else Status.OPEN,
            "dependency inventory is declared complete"
            if inventory_complete
            else "dependency inventory is not declared complete",
        )
    )
    if source_commit == "0" * 40:
        checks.append(
            check(
                "inventory.sourceCommit",
                Status.FAIL if environment == "production" or inventory_complete else Status.OPEN,
                "source commit is an unresolved zero identifier",
            )
        )

    captures: dict[str, Capture] = {}
    capture_contents: dict[str, bytes] = {}
    for index, raw_capture in enumerate(
        bounded_array(root["captures"], "inventory.captures", MAX_CAPTURES)
    ):
        capture = parse_capture(raw_capture, context=f"inventory.captures[{index}]")
        if capture.identifier in captures:
            raise EvidenceError("inventory contains a duplicate capture id")
        captures[capture.identifier] = capture
        if capture.captured_at > captured_at:
            checks.append(check(f"inventory.capture.{index}.time", Status.FAIL, "capture is newer than the inventory"))
        elif captured_at - capture.captured_at > MAX_VALIDITY_NANOSECONDS:
            checks.append(check(f"inventory.capture.{index}.age", Status.FAIL, "capture predates the inventory by more than seven days"))
        checks.append(
            check(
                f"inventory.capture.{index}.complete",
                Status.PASS if capture.complete else Status.OPEN,
                "dependency capture is complete" if capture.complete else "dependency capture is incomplete",
            )
        )
        binding, contents = binding_result(
            capture.identifier,
            capture.digest,
            evidence_paths,
            check_id=f"inventory.capture.{index}.binding",
            description="dependency capture",
        )
        checks.append(binding)
        if contents is not None:
            capture_contents[capture.identifier] = contents

    build_applicability: dict[str, bool | None] = {}
    build_capture_complete = False
    if build_configuration_id is None:
        checks.append(check("inventory.buildConfiguration", Status.OPEN, "build configuration capture is absent"))
    elif build_configuration_id not in captures:
        raise EvidenceError("build configuration references an unknown capture")
    elif captures[build_configuration_id].kind != "build-configuration":
        checks.append(check("inventory.buildConfiguration", Status.FAIL, "build configuration references the wrong capture kind"))
    else:
        checks.append(check("inventory.buildConfiguration", Status.PASS, "build configuration capture is identified"))
        build_contents = capture_contents.get(build_configuration_id)
        if build_contents is not None:
            (
                captured_deployment_id,
                captured_platform,
                captured_architecture,
                captured_source_commit,
                build_capture_complete,
                build_applicability,
            ) = parse_build_configuration_capture(build_contents)
            if (
                captured_deployment_id,
                captured_platform,
                captured_architecture,
            ) != (deployment_id, platform, architecture):
                checks.append(check("inventory.buildConfiguration.profile", Status.FAIL, "build configuration deployment profile does not match inventory"))
            else:
                checks.append(check("inventory.buildConfiguration.profile", Status.PASS, "build configuration binds the deployment profile"))
            if captured_source_commit != source_commit:
                checks.append(check("inventory.buildConfiguration.commit", Status.FAIL, "build configuration source commit does not match inventory"))
            else:
                checks.append(check("inventory.buildConfiguration.commit", Status.PASS, "build configuration binds the inventory source commit"))
            if build_capture_complete and set(build_applicability) != SCOPE_SET:
                checks.append(check("inventory.buildConfiguration.scopes", Status.FAIL, "complete build configuration omits dependency scopes"))
            elif not build_capture_complete:
                checks.append(check("inventory.buildConfiguration.complete", Status.OPEN, "build configuration capture is incomplete"))
            else:
                checks.append(check("inventory.buildConfiguration.complete", Status.PASS, "build configuration capture is complete"))

    os_capture_components: dict[str, tuple[bool, dict[str, tuple[str, str]]]] = {}
    for os_index, capture in enumerate(captures.values()):
        if capture.kind != "os-version":
            continue
        contents = capture_contents.get(capture.identifier)
        if contents is None:
            continue
        (
            captured_deployment_id,
            captured_platform,
            captured_architecture,
            captured_source_commit,
            complete,
            captured_components,
        ) = parse_os_version_capture(contents)
        os_capture_components[capture.identifier] = (complete, captured_components)
        if (
            captured_deployment_id,
            captured_platform,
            captured_architecture,
        ) != (deployment_id, platform, architecture):
            checks.append(check(f"inventory.osCapture.{os_index}.profile", Status.FAIL, "OS capture deployment profile does not match inventory"))
        else:
            checks.append(check(f"inventory.osCapture.{os_index}.profile", Status.PASS, "OS capture binds the deployment profile"))
        if captured_source_commit != source_commit:
            checks.append(check(f"inventory.osCapture.{os_index}.commit", Status.FAIL, "OS capture source commit does not match inventory"))
        else:
            checks.append(check(f"inventory.osCapture.{os_index}.commit", Status.PASS, "OS capture binds the inventory source commit"))
        checks.append(
            check(
                f"inventory.osCapture.{os_index}.complete",
                Status.PASS if complete else Status.OPEN,
                "OS capture is complete" if complete else "OS capture is incomplete",
            )
        )

    workflow_actions: set[tuple[str, str]] = set()
    workflow_capture_seen = False
    workflow_captures_complete = True
    for capture in captures.values():
        if capture.kind != "workflow-definition":
            continue
        contents = capture_contents.get(capture.identifier)
        if contents is None:
            workflow_captures_complete = False
            continue
        complete, actions = parse_workflow_capture(contents)
        workflow_capture_seen = True
        workflow_captures_complete = workflow_captures_complete and complete
        workflow_actions.update(actions)
    if workflow_capture_seen and not workflow_captures_complete:
        checks.append(check("inventory.actions.capture", Status.OPEN, "workflow Action capture is incomplete"))

    components: dict[str, Component] = {}
    for index, raw_component in enumerate(
        bounded_array(root["components"], "inventory.components", MAX_COMPONENTS)
    ):
        component = parse_component(raw_component, context=f"inventory.components[{index}]")
        if component.identifier in components:
            raise EvidenceError("inventory contains a duplicate component id")
        if component.capture_id not in captures:
            raise EvidenceError("component references an unknown dependency capture")
        components[component.identifier] = component
        if component.resolution == "OPEN":
            checks.append(check(f"inventory.component.{index}.resolution", Status.OPEN, "component version is unresolved"))
            continue
        if component.version is None or component.coordinate is None:
            checks.append(check(f"inventory.component.{index}.resolution", Status.FAIL, "exact component lacks a version or coordinate"))
            continue
        if looks_unresolved_version(component.version):
            checks.append(check(f"inventory.component.{index}.resolution", Status.FAIL, "component marked exact uses an unresolved version expression"))
            continue
        capture_kind = captures[component.capture_id].kind
        if capture_kind not in CAPTURE_KINDS_BY_SCOPE[component.scope]:
            checks.append(check(f"inventory.component.{index}.capture", Status.FAIL, "component uses a capture kind that cannot establish its dependency scope"))
            continue
        if component.kind == "ci-action" or component.scope == "github-actions":
            action_commit = action_coordinate_commit(component.coordinate)
            action_name = component.coordinate.rpartition("@")[0] if action_commit else None
            if component.kind != "ci-action" or component.scope != "github-actions":
                checks.append(check(f"inventory.component.{index}.action", Status.FAIL, "GitHub Action component has an inconsistent scope or kind"))
            elif action_commit != component.version:
                checks.append(check(f"inventory.component.{index}.action", Status.FAIL, "GitHub Action is not pinned to one full commit SHA"))
            elif action_name is None or component.name.casefold() != action_name.casefold():
                checks.append(check(f"inventory.component.{index}.action", Status.FAIL, "GitHub Action component name does not match its coordinate"))
            else:
                checks.append(check(f"inventory.component.{index}.action", Status.PASS, "GitHub Action is pinned to one full commit SHA"))
        elif not coordinate_matches_component(
            component, platform=platform, architecture=architecture
        ):
            checks.append(check(f"inventory.component.{index}.coordinate", Status.FAIL, "component coordinate name or version does not match"))
        else:
            checks.append(check(f"inventory.component.{index}.resolution", Status.PASS, "component has an exact reviewed coordinate"))

    for os_index, (capture_id, (complete, captured_components)) in enumerate(
        os_capture_components.items()
    ):
        if not complete:
            continue
        expected_components = {
            component.identifier: (component.version, component.coordinate)
            for component in components.values()
            if component.capture_id == capture_id
            and component.scope in {"native-http", "os-runtime"}
            and component.resolution == "EXACT"
            and component.version is not None
            and component.coordinate is not None
        }
        if captured_components != expected_components:
            checks.append(check(f"inventory.osCapture.{os_index}.components", Status.FAIL, "OS capture and exact platform component inventory differ"))
        else:
            checks.append(check(f"inventory.osCapture.{os_index}.components", Status.PASS, "OS capture matches exact platform components"))

    inventory_actions = {
        (component.coordinate, component.version)
        for component in components.values()
        if component.scope == "github-actions"
        and component.coordinate is not None
        and component.version is not None
    }
    if workflow_capture_seen and workflow_captures_complete:
        if workflow_actions != inventory_actions:
            checks.append(check("inventory.actions.coverage", Status.FAIL, "workflow Action capture and component inventory differ"))
        else:
            checks.append(check("inventory.actions.coverage", Status.PASS, "workflow Action capture matches the component inventory"))
    elif inventory_actions:
        checks.append(check("inventory.actions.coverage", Status.OPEN, "complete workflow Action capture is unavailable"))

    exact_component_count = sum(
        component.resolution == "EXACT" for component in components.values()
    )
    if exact_component_count == 0:
        checks.append(check("inventory.components", Status.OPEN, "no exact dependency component was captured"))

    coverage: dict[str, tuple[str, list[str]]] = {}
    for index, raw_coverage in enumerate(
        bounded_array(root["coverage"], "inventory.coverage", len(SCOPES))
    ):
        context = f"inventory.coverage[{index}]"
        item = require_object(raw_coverage, context)
        require_exact_keys(item, context, {"scope", "status", "reason", "captureIds"})
        scope = require_enum(item["scope"], f"{context}.scope", SCOPE_SET)
        if scope in coverage:
            raise EvidenceError("inventory contains a duplicate coverage scope")
        status = require_enum(
            item["status"], f"{context}.status", {"EXACT", "NOT_APPLICABLE", "OPEN"}
        )
        reason = nullable_text(item["reason"], f"{context}.reason")
        capture_ids: list[str] = []
        for capture_index, raw_id in enumerate(
            bounded_array(item["captureIds"], f"{context}.captureIds", MAX_CAPTURES)
        ):
            capture_id = identifier(raw_id, f"{context}.captureIds[{capture_index}]")
            if capture_id in capture_ids:
                raise EvidenceError("coverage contains a duplicate capture id")
            if capture_id not in captures:
                raise EvidenceError("coverage references an unknown dependency capture")
            capture_ids.append(capture_id)
        coverage[scope] = (status, capture_ids)
        scoped_components = [
            component for component in components.values() if component.scope == scope
        ]
        if status == "OPEN":
            if reason is None:
                raise EvidenceError("open coverage must explain the unresolved evidence")
            checks.append(check(f"inventory.coverage.{index}", Status.OPEN, "dependency scope remains unresolved"))
            continue
        if status == "EXACT" and reason is not None:
            raise EvidenceError("exact coverage reason must be null")
        if status == "NOT_APPLICABLE" and reason is None:
            raise EvidenceError("not-applicable coverage must include a reason")
        if not capture_ids:
            checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "resolved dependency scope has no capture evidence"))
            continue
        if any(not captures[capture_id].complete for capture_id in capture_ids):
            checks.append(check(f"inventory.coverage.{index}.captures", Status.OPEN, "dependency scope uses an incomplete capture"))
        if status == "NOT_APPLICABLE":
            if scope in NON_OPTIONAL_SCOPES:
                checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "required runtime or build scope cannot be not applicable"))
            elif build_configuration_id is None or build_configuration_id not in capture_ids:
                checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "not-applicable scope is not bound to the build configuration"))
            elif build_capture_complete and build_applicability.get(scope) is not False:
                checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "build configuration does not prove scope non-applicability"))
            elif not build_capture_complete:
                checks.append(check(f"inventory.coverage.{index}", Status.OPEN, "scope non-applicability uses an incomplete build configuration"))
            elif scoped_components:
                checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "not-applicable scope contains dependency components"))
            else:
                checks.append(check(f"inventory.coverage.{index}", Status.PASS, "scope non-applicability has capture evidence"))
            continue
        if any(
            captures[capture_id].kind not in CAPTURE_KINDS_BY_SCOPE[scope]
            for capture_id in capture_ids
        ):
            checks.append(check(f"inventory.coverage.{index}.captureKinds", Status.FAIL, "exact dependency scope references an incompatible capture kind"))
        if build_capture_complete and build_applicability.get(scope) is not True:
            checks.append(check(f"inventory.coverage.{index}.applicability", Status.FAIL, "build configuration does not mark the exact scope applicable"))
        elif not build_capture_complete:
            checks.append(check(f"inventory.coverage.{index}.applicability", Status.OPEN, "exact scope lacks a complete build configuration"))
        if scope == "github-actions" and not any(
            captures[capture_id].kind == "workflow-definition"
            for capture_id in capture_ids
        ):
            checks.append(check(f"inventory.coverage.{index}.workflow", Status.FAIL, "Action scope lacks a workflow-definition capture"))
        if not scoped_components:
            checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "exact dependency scope contains no components"))
        elif any(component.resolution != "EXACT" for component in scoped_components):
            checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "exact dependency scope contains unresolved components"))
        elif any(component.capture_id not in capture_ids for component in scoped_components):
            checks.append(check(f"inventory.coverage.{index}", Status.FAIL, "exact dependency component is not bound to scope evidence"))
        elif scope in PRIMARY_IDENTITY_SCOPES and not any(
            component.direct
            and component.resolution == "EXACT"
            and is_primary_scope_identity(component)
            for component in scoped_components
        ):
            checks.append(check(f"inventory.coverage.{index}.identity", Status.FAIL, "exact dependency scope lacks a direct primary component identity"))
        else:
            checks.append(check(f"inventory.coverage.{index}", Status.PASS, "dependency scope has exact component coverage"))

    for scope_index, scope in enumerate(SCOPES):
        if scope not in coverage:
            checks.append(
                check(
                    f"inventory.coverage.missing.{scope_index}",
                    Status.FAIL if inventory_complete else Status.OPEN,
                    "required dependency scope is missing",
                )
            )

    return checks, Inventory(
        environment=environment,
        platform=platform,
        captured_at=captured_at,
        valid_until=valid_until,
        captures=captures,
        components=components,
        declared_evidence=set(captures),
    )


def parse_source(value: Any, *, context: str) -> Source:
    source = require_object(value, context)
    require_exact_keys(
        source,
        context,
        {
            "id",
            "kind",
            "authority",
            "sha256",
            "inventorySha256",
            "capturedAt",
            "validUntil",
            "complete",
            "coveredComponents",
            "remediations",
        },
    )
    kind = require_enum(source["kind"], f"{context}.kind", SOURCE_KINDS)
    captured_at = timestamp_nanoseconds(source["capturedAt"], f"{context}.capturedAt")
    valid_until = timestamp_nanoseconds(source["validUntil"], f"{context}.validUntil")
    if valid_until <= captured_at:
        raise EvidenceError(f"{context}.validUntil must be after capturedAt")
    covered_components: dict[str, tuple[str, str]] = {}
    for index, raw_component in enumerate(
        bounded_array(
            source["coveredComponents"],
            f"{context}.coveredComponents",
            MAX_COMPONENTS,
        )
    ):
        item_context = f"{context}.coveredComponents[{index}]"
        item = require_object(raw_component, item_context)
        require_exact_keys(item, item_context, {"componentId", "version", "coordinate"})
        component_id = identifier(item["componentId"], f"{item_context}.componentId")
        if component_id in covered_components:
            raise EvidenceError("advisory source contains duplicate component coverage")
        covered_components[component_id] = (
            require_text(item["version"], f"{item_context}.version"),
            require_text(item["coordinate"], f"{item_context}.coordinate", maximum=1024),
        )
    remediations: set[tuple[str, str, str, str]] = set()
    for index, raw_remediation in enumerate(
        bounded_array(
            source["remediations"],
            f"{context}.remediations",
            MAX_ADVISORIES_PER_COMPONENT,
        )
    ):
        item_context = f"{context}.remediations[{index}]"
        item = require_object(raw_remediation, item_context)
        require_exact_keys(
            item,
            item_context,
            {"componentId", "version", "coordinate", "advisoryId"},
        )
        entry = (
            identifier(item["componentId"], f"{item_context}.componentId"),
            require_text(item["version"], f"{item_context}.version"),
            require_text(item["coordinate"], f"{item_context}.coordinate", maximum=1024),
            identifier(item["advisoryId"], f"{item_context}.advisoryId"),
        )
        if entry in remediations:
            raise EvidenceError("remediation source contains a duplicate target")
        remediations.add(entry)
    if kind in AUTHORITY_KINDS:
        if not covered_components or remediations:
            raise EvidenceError("authority source must cover components and no remediations")
    elif (
        not covered_components
        or not remediations
        or any(
            covered_components.get(component_id) != (version, coordinate)
            for component_id, version, coordinate, _ in remediations
        )
    ):
        raise EvidenceError("remediation source must bind components and advisory targets")
    return Source(
        identifier=identifier(source["id"], f"{context}.id"),
        kind=kind,
        authority=require_text(source["authority"], f"{context}.authority"),
        digest=require_sha256(source["sha256"], f"{context}.sha256"),
        inventory_digest=require_sha256(
            source["inventorySha256"], f"{context}.inventorySha256"
        ),
        captured_at=captured_at,
        valid_until=valid_until,
        complete=require_bool(source["complete"], f"{context}.complete"),
        covered_components=covered_components,
        remediations=remediations,
    )


def validate_review(
    document: Any,
    *,
    inventory: Inventory,
    inventory_digest: str,
    evaluated_at: int,
    evidence_paths: dict[str, Path],
) -> tuple[list[dict[str, str]], set[str]]:
    root = require_object(document, "review")
    require_exact_keys(
        root,
        "review",
        {
            "schemaVersion",
            "inventorySha256",
            "capturedAt",
            "validUntil",
            "sources",
            "componentReviews",
            "reviewComplete",
            "attestation",
        },
    )
    schema_version(root, "review")
    expected_inventory_digest = nullable_sha256(
        root["inventorySha256"], "review.inventorySha256"
    )
    captured_at = timestamp_nanoseconds(root["capturedAt"], "review.capturedAt")
    valid_until = timestamp_nanoseconds(root["validUntil"], "review.validUntil")
    review_complete = require_bool(root["reviewComplete"], "review.reviewComplete")
    checks = window_checks(
        captured_at,
        valid_until,
        evaluated_at,
        prefix="review",
        description="dependency advisory review",
    )
    if captured_at < inventory.captured_at:
        checks.append(check("review.sequence", Status.FAIL, "advisory review predates the bound inventory"))
    if valid_until > inventory.valid_until:
        checks.append(check("review.window", Status.FAIL, "advisory review outlives the dependency inventory"))
    if expected_inventory_digest is None:
        checks.append(check("review.inventory", Status.OPEN, "advisory review lacks an inventory digest"))
    elif expected_inventory_digest != inventory_digest:
        checks.append(check("review.inventory", Status.FAIL, "advisory review inventory digest does not match"))
    else:
        checks.append(check("review.inventory", Status.PASS, "advisory review binds the exact inventory bytes"))
    checks.append(
        check(
            "review.complete",
            Status.PASS if review_complete else Status.OPEN,
            "advisory review is declared complete"
            if review_complete
            else "advisory review is not declared complete",
        )
    )
    checks.extend(
        attestation_checks(
            root["attestation"],
            context="review.attestation",
            prefix="review.attestation",
            environment=inventory.environment,
            captured_at=captured_at,
            evaluated_at=evaluated_at,
        )
    )

    sources: dict[str, Source] = {}
    for index, raw_source in enumerate(
        bounded_array(root["sources"], "review.sources", MAX_SOURCES)
    ):
        source = parse_source(raw_source, context=f"review.sources[{index}]")
        if source.identifier in sources or source.identifier in inventory.declared_evidence:
            raise EvidenceError("review contains a duplicate evidence id")
        sources[source.identifier] = source
        if source.inventory_digest != inventory_digest:
            checks.append(check(f"review.source.{index}.inventory", Status.FAIL, "advisory source is not bound to this inventory"))
        if source.valid_until - source.captured_at > MAX_VALIDITY_NANOSECONDS:
            checks.append(check(f"review.source.{index}.window", Status.FAIL, "advisory source validity exceeds the seven-day maximum"))
        if source.captured_at < inventory.captured_at:
            checks.append(check(f"review.source.{index}.sequence", Status.FAIL, "advisory source predates the dependency inventory"))
        elif source.captured_at > captured_at:
            checks.append(check(f"review.source.{index}.time", Status.FAIL, "advisory source is newer than the review"))
        elif evaluated_at > source.valid_until:
            checks.append(check(f"review.source.{index}.age", Status.OPEN, "advisory source validity has elapsed"))
        else:
            checks.append(check(f"review.source.{index}.age", Status.PASS, "advisory source is current"))
        checks.append(
            check(
                f"review.source.{index}.complete",
                Status.PASS if source.complete else Status.OPEN,
                "advisory source is complete" if source.complete else "advisory source is incomplete",
            )
        )
        for coverage_index, (component_id, (version, coordinate)) in enumerate(
            source.covered_components.items()
        ):
            component = inventory.components.get(component_id)
            if (
                component is None
                or component.version != version
                or component.coordinate != coordinate
            ):
                checks.append(check(f"review.source.{index}.coverage.{coverage_index}", Status.FAIL, "advisory source component coverage does not match inventory"))
        for remediation_index, (component_id, version, coordinate, _) in enumerate(
            sorted(source.remediations)
        ):
            component = inventory.components.get(component_id)
            if (
                component is None
                or component.version != version
                or component.coordinate != coordinate
            ):
                checks.append(check(f"review.source.{index}.remediation.{remediation_index}", Status.FAIL, "remediation target does not match inventory"))
        checks.append(
            binding_check(
                source.identifier,
                source.digest,
                evidence_paths,
                check_id=f"review.source.{index}.binding",
                description="advisory source capture",
            )
        )

    if sources and valid_until > min(source.valid_until for source in sources.values()):
        checks.append(check("review.sources.window", Status.FAIL, "advisory review outlives a cited source"))

    reviewed_components: set[str] = set()
    for index, raw_component_review in enumerate(
        bounded_array(
            root["componentReviews"],
            "review.componentReviews",
            MAX_COMPONENT_REVIEWS,
        )
    ):
        context = f"review.componentReviews[{index}]"
        component_review = require_object(raw_component_review, context)
        require_exact_keys(
            component_review,
            context,
            {"componentId", "version", "sourceIds", "complete", "advisories"},
        )
        component_id = identifier(component_review["componentId"], f"{context}.componentId")
        if component_id in reviewed_components:
            raise EvidenceError("review contains a duplicate component review")
        reviewed_components.add(component_id)
        component = inventory.components.get(component_id)
        if component is None:
            checks.append(check(f"review.component.{index}.inventory", Status.FAIL, "review references an unknown inventory component"))
        version = nullable_text(component_review["version"], f"{context}.version")
        if component is not None:
            if version is None:
                checks.append(check(f"review.component.{index}.version", Status.OPEN, "component review lacks an exact version"))
            elif component.version != version:
                checks.append(check(f"review.component.{index}.version", Status.FAIL, "component review version does not match inventory"))
            else:
                checks.append(check(f"review.component.{index}.version", Status.PASS, "component review version matches inventory"))
        complete = require_bool(component_review["complete"], f"{context}.complete")
        checks.append(
            check(
                f"review.component.{index}.complete",
                Status.PASS if complete else Status.OPEN,
                "component advisory review is complete" if complete else "component advisory review is incomplete",
            )
        )
        source_ids: list[str] = []
        for source_index, raw_source_id in enumerate(
            bounded_array(
                component_review["sourceIds"],
                f"{context}.sourceIds",
                MAX_SOURCES,
            )
        ):
            source_id = identifier(raw_source_id, f"{context}.sourceIds[{source_index}]")
            if source_id in source_ids:
                raise EvidenceError("component review contains a duplicate source id")
            source_ids.append(source_id)
        known_authorities = [
            source_id
            for source_id in source_ids
            if source_id in sources
            and component is not None
            and sources[source_id].kind
            in (
                authority_kinds_for_component(
                    component, platform=inventory.platform
                )
                & AUTHORITIES_BY_SCOPE[component.scope]
            )
            and sources[source_id].covered_components.get(component.identifier)
            == (component.version, component.coordinate)
        ]
        unknown_sources = [source_id for source_id in source_ids if source_id not in sources]
        if unknown_sources:
            checks.append(
                check(
                    f"review.component.{index}.sources",
                    Status.FAIL if review_complete or complete else Status.OPEN,
                    "component review references an undeclared advisory source",
                )
            )
        elif not known_authorities:
            checks.append(
                check(
                    f"review.component.{index}.sources",
                    Status.FAIL if complete else Status.OPEN,
                    "component review lacks an authoritative advisory source",
                )
            )
        else:
            checks.append(check(f"review.component.{index}.sources", Status.PASS, "component review cites an authoritative source"))

        advisory_ids: set[str] = set()
        for advisory_index, raw_advisory in enumerate(
            bounded_array(
                component_review["advisories"],
                f"{context}.advisories",
                MAX_ADVISORIES_PER_COMPONENT,
            )
        ):
            advisory_context = f"{context}.advisories[{advisory_index}]"
            advisory = require_object(raw_advisory, advisory_context)
            require_exact_keys(
                advisory,
                advisory_context,
                {
                    "id",
                    "sourceId",
                    "assessment",
                    "remediation",
                    "rationale",
                    "remediationEvidenceId",
                },
            )
            advisory_id = identifier(advisory["id"], f"{advisory_context}.id")
            if advisory_id in advisory_ids:
                raise EvidenceError("component review contains a duplicate advisory id")
            advisory_ids.add(advisory_id)
            source_id = identifier(advisory["sourceId"], f"{advisory_context}.sourceId")
            assessment = require_enum(
                advisory["assessment"],
                f"{advisory_context}.assessment",
                {"affected", "not-affected", "unknown"},
            )
            remediation = require_enum(
                advisory["remediation"],
                f"{advisory_context}.remediation",
                {"none", "planned", "accepted-risk", "verified", "not-applicable"},
            )
            rationale = nullable_text(advisory["rationale"], f"{advisory_context}.rationale")
            remediation_evidence_id = nullable_identifier(
                advisory["remediationEvidenceId"],
                f"{advisory_context}.remediationEvidenceId",
            )
            decision_id = f"review.component.{index}.advisory.{advisory_index}"
            if (
                source_id not in sources
                or source_id not in source_ids
                or component is None
                or sources[source_id].kind
                not in (
                    authority_kinds_for_component(
                        component, platform=inventory.platform
                    )
                    & AUTHORITIES_BY_SCOPE[component.scope]
                )
                or sources[source_id].covered_components.get(component.identifier)
                != (component.version, component.coordinate)
            ):
                checks.append(check(decision_id, Status.FAIL, "advisory is not bound to a cited authoritative source"))
                continue
            if assessment == "unknown":
                if remediation != "none" or remediation_evidence_id is not None:
                    checks.append(check(decision_id, Status.FAIL, "unknown advisory assessment has contradictory remediation"))
                else:
                    checks.append(check(decision_id, Status.OPEN, "advisory applicability is unknown"))
                continue
            if assessment == "not-affected":
                if remediation != "not-applicable" or rationale is None or remediation_evidence_id is not None:
                    checks.append(check(decision_id, Status.FAIL, "not-affected advisory assessment is inconsistent"))
                else:
                    checks.append(check(decision_id, Status.PASS, "advisory was reviewed as not affecting this version"))
                continue
            remediation_source = (
                sources.get(remediation_evidence_id)
                if remediation_evidence_id is not None
                else None
            )
            if (
                remediation == "verified"
                and rationale is not None
                and remediation_source is not None
                and remediation_source.kind == "remediation-evidence"
                and remediation_evidence_id in source_ids
                and component is not None
                and component.version is not None
                and (
                    component.identifier,
                    component.version,
                    component.coordinate,
                    advisory_id,
                )
                in remediation_source.remediations
            ):
                checks.append(check(decision_id, Status.PASS, "affected advisory has verified remediation evidence"))
            else:
                checks.append(check(decision_id, Status.FAIL, "affected advisory is not verifiably remediated"))

    exact_components = {
        component.identifier
        for component in inventory.components.values()
        if component.resolution == "EXACT"
    }
    for component_index, component_id in enumerate(sorted(exact_components - reviewed_components)):
        checks.append(
            check(
                f"review.missing.{component_index}",
                Status.FAIL if review_complete else Status.OPEN,
                "exact inventory component lacks an advisory review",
            )
        )
    return checks, set(sources)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--evidence", action="append", default=[])
    parser.add_argument("--evaluated-at", required=True)
    args = parser.parse_args()
    try:
        evaluated_at = timestamp_nanoseconds(args.evaluated_at, "evaluated-at")
        evidence_paths = parse_evidence_paths(args.evidence)
        inventory_bytes = read_limited_bytes(args.inventory)
        inventory_checks, inventory = validate_inventory(
            parse_json_bytes(inventory_bytes),
            evaluated_at=evaluated_at,
            evidence_paths=evidence_paths,
        )
        review_checks, review_evidence = validate_review(
            parse_json_bytes(read_limited_bytes(args.review)),
            inventory=inventory,
            inventory_digest=hashlib.sha256(inventory_bytes).hexdigest(),
            evaluated_at=evaluated_at,
            evidence_paths=evidence_paths,
        )
        declared_evidence = inventory.declared_evidence | review_evidence
        if evidence_paths.keys() - declared_evidence:
            raise EvidenceError("an evidence argument does not match a declared capture")
        checks = inventory_checks + review_checks
    except EvidenceError:
        print(
            json.dumps(
                {
                    "risk": RISK_ID,
                    "status": "OPEN",
                    "error": "evidence is invalid, incomplete, or unreadable",
                },
                sort_keys=True,
            )
        )
        return 3
    status = combine_statuses(Status(item["status"]) for item in checks)
    print(
        json.dumps(
            {"risk": RISK_ID, "status": status.value, "checks": checks},
            indent=2,
            sort_keys=True,
        )
    )
    return exit_code(status)


if __name__ == "__main__":
    raise SystemExit(main())
