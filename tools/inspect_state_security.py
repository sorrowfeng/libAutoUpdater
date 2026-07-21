#!/usr/bin/env python3
"""Inspect offline RISK-003 state, permission, and URL-sample evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any
from urllib.parse import parse_qsl, urlsplit

from evidence_contract import (
    EvidenceError,
    MAX_EVIDENCE_BYTES,
    Status,
    combine_statuses,
    exit_code,
    load_json,
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


RISK_ID = "RISK-003"
STATE_ROLES = {"primary", "lastKnownGood", "resume"}
MAX_URL_SAMPLES = 1000


def check(identifier: str, status: Status, reason: str) -> dict[str, str]:
    return {"id": identifier, "status": status.value, "reason": reason}


def absolute_path(value: Any, context: str, platform: str) -> str:
    text = require_text(value, context, maximum=4096)
    path_type = PureWindowsPath if platform == "windows" else PurePosixPath
    parsed = path_type(text)
    if not parsed.is_absolute():
        raise EvidenceError(f"{context} must be absolute for the target platform")
    if ".." in parsed.parts:
        raise EvidenceError(f"{context} must not contain parent traversal")
    return text


def path_key(value: str, platform: str) -> str:
    parsed = PureWindowsPath(value) if platform == "windows" else PurePosixPath(value)
    normalized = str(parsed)
    return normalized.casefold() if platform == "windows" else normalized


def path_digest(value: str, platform: str) -> str:
    return hashlib.sha256(path_key(value, platform).encode("utf-8")).hexdigest()


def host_platform() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def observation(
    identifier: str,
    value: bool | None,
    *,
    safe_when: bool,
    description: str,
    incomplete_is_open: bool = True,
) -> dict[str, str]:
    if value is None or (incomplete_is_open and value is False and safe_when):
        return check(identifier, Status.OPEN, f"{description} was not established")
    if value == safe_when:
        return check(identifier, Status.PASS, f"{description} satisfies policy")
    return check(identifier, Status.FAIL, f"{description} violates policy")


def url_characteristics(
    value: str, credential_names: set[str]
) -> tuple[bool, bool, bool, bool]:
    """Return URL/userinfo/known-query/unclassified flags without retaining values."""

    try:
        parsed = urlsplit(value)
    except ValueError:
        return False, False, False, False
    if parsed.scheme.casefold() not in {"http", "https"} or not parsed.netloc:
        return False, False, False, False
    has_userinfo = parsed.username is not None or parsed.password is not None
    query_pairs = parse_qsl(parsed.query, keep_blank_values=True)
    known_query = any(
        name.casefold() in credential_names for name, _ in query_pairs
    )
    unclassified = any(
        name.casefold() not in credential_names for name, _ in query_pairs
    ) or bool(parsed.fragment)
    return True, has_userinfo, known_query, unclassified


def scan_json_strings(document: Any, credential_names: set[str]) -> tuple[int, int, int]:
    stack = [document]
    strings = 0
    credential_urls = 0
    unknown_query_urls = 0
    nodes = 0
    while stack:
        value = stack.pop()
        nodes += 1
        if nodes > 100_000:
            raise EvidenceError("state JSON exceeds the inspection node limit")
        if isinstance(value, dict):
            stack.extend(value.values())
            stack.extend(value.keys())
        elif isinstance(value, list):
            stack.extend(value)
        elif isinstance(value, str):
            strings += 1
            is_url, userinfo, known_query, unclassified = url_characteristics(
                value, credential_names
            )
            if is_url and (userinfo or known_query):
                credential_urls += 1
            if is_url and unclassified:
                unknown_query_urls += 1
    return strings, credential_urls, unknown_query_urls


def load_permission_bundle(
    expected_digest: str | None, supplied_path: Path | None
) -> tuple[Any | None, list[dict[str, str]]]:
    if supplied_path is None:
        return None, [check("permissions.bundle", Status.OPEN, "permission evidence bundle was not supplied")]
    try:
        contents = read_limited_bytes(supplied_path)
    except EvidenceError:
        return None, [check("permissions.bundle", Status.OPEN, "permission evidence bundle is unreadable")]
    actual = hashlib.sha256(contents).hexdigest()
    checks: list[dict[str, str]] = []
    if expected_digest is None:
        checks.append(check("permissions.bundle", Status.OPEN, "permission evidence digest is absent"))
    elif actual != expected_digest:
        checks.append(check("permissions.bundle", Status.FAIL, "permission evidence digest does not match"))
    else:
        checks.append(check("permissions.bundle", Status.PASS, "permission evidence bytes match the snapshot"))
    try:
        return parse_json_bytes(contents), checks
    except EvidenceError:
        checks.append(check("permissions.schema", Status.OPEN, "permission evidence is not valid structured JSON"))
        return None, checks


def validate_path_observations(
    value: dict[str, Any], context: str, identifier: str
) -> list[dict[str, str]]:
    owner = require_nullable_bool(value["ownerTrusted"], f"{context}.ownerTrusted")
    acl = require_nullable_bool(value["aclComplete"], f"{context}.aclComplete")
    readable = require_nullable_bool(
        value["lessTrustedReadable"], f"{context}.lessTrustedReadable"
    )
    writable = require_nullable_bool(
        value["lessTrustedWritable"], f"{context}.lessTrustedWritable"
    )
    link = require_nullable_bool(value["unexpectedLink"], f"{context}.unexpectedLink")
    return [
        observation(f"{identifier}.owner", owner, safe_when=True, description="trusted ownership", incomplete_is_open=False),
        observation(f"{identifier}.acl", acl, safe_when=True, description="ACL evidence completeness"),
        observation(f"{identifier}.read", readable, safe_when=False, description="lower-trust read access", incomplete_is_open=False),
        observation(f"{identifier}.write", writable, safe_when=False, description="lower-trust write access", incomplete_is_open=False),
        observation(f"{identifier}.link", link, safe_when=False, description="unexpected link/reparse state", incomplete_is_open=False),
    ]


def validate_permission_bundle(
    document: Any | None,
    *,
    deployment: dict[str, Any],
    storage_kind: str,
    declared_complete: bool | None,
    raw_evidence_path: Path | None,
) -> tuple[dict[str, dict[str, Any]], dict[str, Any] | None, list[dict[str, str]]]:
    if document is None:
        return {}, None, []
    try:
        bundle = require_object(document, "permissionEvidence")
        require_exact_keys(
            bundle,
            "permissionEvidence",
            {
                "schemaVersion",
                "deployment",
                "collector",
                "complete",
                "rawEvidenceSha256",
                "entries",
                "customStore",
            },
        )
        if bundle["schemaVersion"] != 1 or isinstance(bundle["schemaVersion"], bool):
            raise EvidenceError("permissionEvidence.schemaVersion must be integer 1")
        binding = require_object(bundle["deployment"], "permissionEvidence.deployment")
        require_exact_keys(
            binding,
            "permissionEvidence.deployment",
            {
                "id",
                "platform",
                "installDirSha256",
                "capturedAt",
                "configurationSha256",
                "storageKind",
            },
        )
        require_text(binding["id"], "permissionEvidence.deployment.id")
        require_enum(
            binding["platform"],
            "permissionEvidence.deployment.platform",
            {"windows", "macos", "linux"},
        )
        require_sha256(
            binding["installDirSha256"],
            "permissionEvidence.deployment.installDirSha256",
        )
        timestamp_nanoseconds(
            binding["capturedAt"], "permissionEvidence.deployment.capturedAt"
        )
        require_sha256(
            binding["configurationSha256"],
            "permissionEvidence.deployment.configurationSha256",
        )
        require_enum(
            binding["storageKind"],
            "permissionEvidence.deployment.storageKind",
            {"bundled-json", "custom"},
        )
        collector = require_object(
            bundle["collector"], "permissionEvidence.collector"
        )
        require_exact_keys(
            collector, "permissionEvidence.collector", {"name", "version"}
        )
        require_text(collector["name"], "permissionEvidence.collector.name")
        require_text(collector["version"], "permissionEvidence.collector.version")
        complete = require_bool(bundle["complete"], "permissionEvidence.complete")
        raw_evidence_digest = require_sha256(
            bundle["rawEvidenceSha256"],
            "permissionEvidence.rawEvidenceSha256",
        )

        entries: dict[str, dict[str, Any]] = {}
        for index, raw_entry in enumerate(
            require_array(bundle["entries"], "permissionEvidence.entries")
        ):
            context = f"permissionEvidence.entries[{index}]"
            entry = require_object(raw_entry, context)
            require_exact_keys(
                entry,
                context,
                {
                    "role",
                    "pathSha256",
                    "exists",
                    "ownerTrusted",
                    "aclComplete",
                    "lessTrustedReadable",
                    "lessTrustedWritable",
                    "unexpectedLink",
                },
            )
            role = require_enum(entry["role"], f"{context}.role", {"root", *STATE_ROLES})
            if role in entries:
                raise EvidenceError("permissionEvidence.entries contains duplicate roles")
            require_sha256(entry["pathSha256"], f"{context}.pathSha256")
            for name in (
                "exists",
                "ownerTrusted",
                "aclComplete",
                "lessTrustedReadable",
                "lessTrustedWritable",
                "unexpectedLink",
            ):
                require_nullable_bool(entry[name], f"{context}.{name}")
            entries[role] = entry

        custom_value = bundle["customStore"]
        custom: dict[str, Any] | None = None
        if custom_value is not None:
            custom = require_object(custom_value, "permissionEvidence.customStore")
            require_exact_keys(
                custom,
                "permissionEvidence.customStore",
                {"complete", "credentialUrlsStored", "accessControlVerified"},
            )
            for name in ("complete", "credentialUrlsStored", "accessControlVerified"):
                require_nullable_bool(custom[name], f"permissionEvidence.customStore.{name}")
    except EvidenceError:
        return {}, None, [
            check(
                "permissions.schema",
                Status.OPEN,
                "permission evidence does not satisfy the structured contract",
            )
        ]

    checks: list[dict[str, str]] = []
    if raw_evidence_path is None:
        checks.append(
            check(
                "permissions.raw",
                Status.OPEN,
                "raw platform permission capture was not supplied",
            )
        )
    else:
        try:
            raw_bytes = read_limited_bytes(raw_evidence_path)
        except EvidenceError:
            checks.append(
                check(
                    "permissions.raw",
                    Status.OPEN,
                    "raw platform permission capture is unreadable",
                )
            )
        else:
            if hashlib.sha256(raw_bytes).hexdigest() != raw_evidence_digest:
                checks.append(
                    check(
                        "permissions.raw",
                        Status.FAIL,
                        "raw platform permission capture digest does not match",
                    )
                )
            else:
                checks.append(
                    check(
                        "permissions.raw",
                        Status.PASS,
                        "raw platform permission capture is bundle-bound",
                    )
                )
    for name in ("id", "platform", "capturedAt", "configurationSha256"):
        if binding[name] != deployment[name]:
            checks.append(
                check(
                    f"permissions.binding.{name}",
                    Status.FAIL,
                    "permission evidence deployment binding does not match",
                )
            )
    if binding["installDirSha256"] != path_digest(
        deployment["installDir"], deployment["platform"]
    ):
        checks.append(
            check(
                "permissions.binding.installDir",
                Status.FAIL,
                "permission evidence install root binding does not match",
            )
        )
    if binding["storageKind"] != storage_kind:
        checks.append(
            check(
                "permissions.binding.storageKind",
                Status.FAIL,
                "permission evidence storage kind does not match",
            )
        )
    if declared_complete != complete:
        checks.append(
            check(
                "permissions.complete",
                Status.FAIL,
                "permission completeness declaration is not bundle-bound",
            )
        )
    elif complete:
        checks.append(
            check(
                "permissions.complete",
                Status.PASS,
                "structured permission evidence is declared complete",
            )
        )
    else:
        checks.append(
            check(
                "permissions.complete",
                Status.OPEN,
                "structured permission evidence is incomplete",
            )
        )
    expected_roles = {"root", *STATE_ROLES} if storage_kind == "bundled-json" else set()
    if set(entries) != expected_roles:
        checks.append(
            check(
                "permissions.entries",
                Status.OPEN,
                "permission evidence does not cover the required roles exactly",
            )
        )
    if storage_kind == "bundled-json" and custom is not None:
        checks.append(
            check(
                "permissions.custom",
                Status.OPEN,
                "bundled storage includes an unexpected custom-store attestation",
            )
        )
    if storage_kind == "custom" and custom is None:
        checks.append(
            check(
                "permissions.custom",
                Status.OPEN,
                "custom storage lacks a structured access attestation",
            )
        )
    return entries, custom, checks


def bind_permission_entry(
    declared: dict[str, Any],
    bundle_entry: dict[str, Any] | None,
    *,
    role: str,
    expected_path_digest: str,
    context: str,
) -> tuple[bool | None, list[dict[str, str]]]:
    names = (
        "exists",
        "ownerTrusted",
        "aclComplete",
        "lessTrustedReadable",
        "lessTrustedWritable",
        "unexpectedLink",
    )
    declared_values = {
        name: require_nullable_bool(declared[name], f"{context}.{name}")
        for name in names
    }
    if bundle_entry is None:
        return declared_values["exists"], [
            check(
                f"permissions.{role}",
                Status.OPEN,
                "structured permission entry is missing",
            ),
            *validate_path_observations(declared, context, f"permissions.{role}"),
        ]
    checks: list[dict[str, str]] = []
    if bundle_entry["pathSha256"] != expected_path_digest:
        checks.append(
            check(
                f"permissions.{role}.path",
                Status.FAIL,
                "permission entry is bound to a different path",
            )
        )
    for name in names:
        if declared_values[name] != bundle_entry[name]:
            checks.append(
                check(
                    f"permissions.{role}.{name}",
                    Status.FAIL,
                    "permission observation is not bound to the evidence bundle",
                )
            )
    checks.extend(
        validate_path_observations(
            bundle_entry, f"permissionEvidence.{role}", f"permissions.{role}"
        )
    )
    return bundle_entry["exists"], checks


def validate_document(
    document: Any,
    *,
    permission_evidence: Path | None,
    raw_permission_evidence: Path | None,
    url_sample_bytes: bytes | None,
    evaluated_at: int,
) -> list[dict[str, str]]:
    root = require_object(document, "evidence")
    require_exact_keys(
        root,
        "evidence",
        {"schemaVersion", "deployment", "storage", "urlSampling", "attestation"},
    )
    if root["schemaVersion"] != 1 or isinstance(root["schemaVersion"], bool):
        raise EvidenceError("evidence.schemaVersion must be integer 1")

    deployment = require_object(root["deployment"], "deployment")
    require_exact_keys(
        deployment,
        "deployment",
        {
            "id",
            "environment",
            "platform",
            "installDir",
            "capturedAt",
            "validUntil",
            "configurationSha256",
        },
    )
    require_text(deployment["id"], "deployment.id")
    environment = require_enum(
        deployment["environment"],
        "deployment.environment",
        {"production", "staging", "test", "development"},
    )
    platform = require_enum(
        deployment["platform"], "deployment.platform", {"windows", "macos", "linux"}
    )
    install_dir = absolute_path(
        deployment["installDir"], "deployment.installDir", platform
    )
    captured_at = timestamp_nanoseconds(deployment["capturedAt"], "deployment.capturedAt")
    valid_until = timestamp_nanoseconds(deployment["validUntil"], "deployment.validUntil")
    if valid_until <= captured_at:
        raise EvidenceError("deployment.validUntil must be after capturedAt")
    require_sha256(deployment["configurationSha256"], "deployment.configurationSha256")

    storage = require_object(root["storage"], "storage")
    require_exact_keys(
        storage,
        "storage",
        {"kind", "permissionEvidenceSha256", "permissionEvidenceComplete", "root", "artifacts", "customInspection"},
    )
    storage_kind = require_enum(storage["kind"], "storage.kind", {"bundled-json", "custom"})
    permission_digest = storage["permissionEvidenceSha256"]
    if permission_digest is not None:
        require_sha256(permission_digest, "storage.permissionEvidenceSha256")
    permission_complete = require_nullable_bool(
        storage["permissionEvidenceComplete"], "storage.permissionEvidenceComplete"
    )

    url_sampling = require_object(root["urlSampling"], "urlSampling")
    require_exact_keys(
        url_sampling,
        "urlSampling",
        {
            "credentialMode",
            "transportMode",
            "credentialQueryNames",
            "complete",
            "sampleCount",
            "sampleSetSha256",
            "shortLivedAndScoped",
            "telemetryRedactionVerified",
        },
    )
    credential_mode = require_enum(
        url_sampling["credentialMode"],
        "urlSampling.credentialMode",
        {"none", "short-lived-signed", "unknown"},
    )
    transport_mode = require_enum(
        url_sampling["transportMode"],
        "urlSampling.transportMode",
        {"network", "local-only", "unknown"},
    )
    raw_names = require_array(
        url_sampling["credentialQueryNames"], "urlSampling.credentialQueryNames"
    )
    credential_names = {
        require_text(item, f"urlSampling.credentialQueryNames[{index}]").casefold()
        for index, item in enumerate(raw_names)
    }
    if len(credential_names) != len(raw_names):
        raise EvidenceError("urlSampling.credentialQueryNames contains duplicates")
    sampling_complete = require_nullable_bool(
        url_sampling["complete"], "urlSampling.complete"
    )
    sample_count = require_integer(
        url_sampling["sampleCount"], "urlSampling.sampleCount"
    )
    if sample_count > MAX_URL_SAMPLES:
        raise EvidenceError("urlSampling.sampleCount exceeds the entry limit")
    sample_digest = url_sampling["sampleSetSha256"]
    if sample_digest is not None:
        require_sha256(sample_digest, "urlSampling.sampleSetSha256")
    short_lived = require_nullable_bool(
        url_sampling["shortLivedAndScoped"], "urlSampling.shortLivedAndScoped"
    )
    telemetry = require_nullable_bool(
        url_sampling["telemetryRedactionVerified"],
        "urlSampling.telemetryRedactionVerified",
    )

    attestation = require_object(root["attestation"], "attestation")
    require_exact_keys(
        attestation, "attestation", {"productionSnapshot", "reviewedBy", "reviewedAt"}
    )
    production_snapshot = require_bool(
        attestation["productionSnapshot"], "attestation.productionSnapshot"
    )
    reviewed_by = attestation["reviewedBy"]
    reviewed_at = attestation["reviewedAt"]
    if reviewed_by is not None:
        require_text(reviewed_by, "attestation.reviewedBy")
    reviewed_at_ns = None
    if reviewed_at is not None:
        reviewed_at_ns = timestamp_nanoseconds(reviewed_at, "attestation.reviewedAt")
        if reviewed_at_ns < captured_at:
            raise EvidenceError("attestation.reviewedAt must not precede capture")

    permission_document, permission_load_checks = load_permission_bundle(
        permission_digest, permission_evidence
    )
    permission_entries, permission_custom, permission_contract_checks = (
        validate_permission_bundle(
            permission_document,
            deployment=deployment,
            storage_kind=storage_kind,
            declared_complete=permission_complete,
            raw_evidence_path=raw_permission_evidence,
        )
    )
    results = [*permission_load_checks, *permission_contract_checks]
    platform_matches_host = platform == host_platform()
    if storage_kind == "bundled-json" and platform_matches_host:
        results.append(
            check(
                "storage.platform",
                Status.PASS,
                "bundled state is inspected on the declared target platform",
            )
        )
    elif storage_kind == "bundled-json":
        results.append(
            check(
                "storage.platform",
                Status.OPEN,
                "bundled state cannot be inspected on a different host platform",
            )
        )
    if captured_at > evaluated_at:
        results.append(check("snapshot.age", Status.FAIL, "snapshot capture is after evaluation time"))
    elif evaluated_at > valid_until:
        results.append(check("snapshot.age", Status.OPEN, "snapshot validity window has elapsed"))
    else:
        results.append(check("snapshot.age", Status.PASS, "snapshot is within its validity window"))
    if reviewed_at_ns is not None and reviewed_at_ns > evaluated_at:
        results.append(check("attestation.time", Status.FAIL, "snapshot review is after evaluation time"))
    if environment == "production" and production_snapshot and reviewed_by is not None and reviewed_at_ns is not None:
        results.append(check("attestation.production", Status.PASS, "production snapshot is signed off"))
    else:
        results.append(check("attestation.production", Status.OPEN, "production snapshot is not signed off"))

    root_observation = storage["root"]
    artifacts = require_array(storage["artifacts"], "storage.artifacts")
    custom = require_object(storage["customInspection"], "storage.customInspection")
    require_exact_keys(
        custom,
        "storage.customInspection",
        {"complete", "credentialUrlsStored", "accessControlVerified"},
    )
    custom_complete = require_nullable_bool(custom["complete"], "storage.customInspection.complete")
    custom_credentials = require_nullable_bool(
        custom["credentialUrlsStored"], "storage.customInspection.credentialUrlsStored"
    )
    custom_access = require_nullable_bool(
        custom["accessControlVerified"], "storage.customInspection.accessControlVerified"
    )
    if storage_kind == "custom":
        if root_observation is not None or artifacts:
            raise EvidenceError("custom storage must not declare bundled filesystem artifacts")
        if permission_custom is None:
            results.append(
                check(
                    "custom.binding",
                    Status.OPEN,
                    "custom-store observations are not permission-bundle bound",
                )
            )
            effective_custom = custom
        else:
            effective_custom = permission_custom
            for name, value in (
                ("complete", custom_complete),
                ("credentialUrlsStored", custom_credentials),
                ("accessControlVerified", custom_access),
            ):
                if permission_custom[name] != value:
                    results.append(
                        check(
                            f"custom.binding.{name}",
                            Status.FAIL,
                            "custom-store observation is not bundle-bound",
                        )
                    )
        results.extend(
            (
                observation("custom.complete", effective_custom["complete"], safe_when=True, description="custom-store inspection"),
                observation("custom.credentials", effective_custom["credentialUrlsStored"], safe_when=False, description="custom-store credential URL persistence", incomplete_is_open=False),
                observation("custom.access", effective_custom["accessControlVerified"], safe_when=True, description="custom-store access control", incomplete_is_open=False),
            )
        )
    else:
        if any(
            value is not None
            for value in (custom_complete, custom_credentials, custom_access)
        ):
            raise EvidenceError(
                "bundled storage customInspection fields must all be null"
            )
        root_value = require_object(root_observation, "storage.root")
        require_exact_keys(
            root_value,
            "storage.root",
            {"path", "exists", "ownerTrusted", "aclComplete", "lessTrustedReadable", "lessTrustedWritable", "unexpectedLink"},
        )
        root_path = absolute_path(root_value["path"], "storage.root.path", platform)
        path_type = PureWindowsPath if platform == "windows" else PurePosixPath
        expected_root = str(path_type(install_dir) / ".autoupdater")
        if path_key(root_path, platform) != path_key(expected_root, platform):
            raise EvidenceError(
                "storage.root.path must be the bundled installDir/.autoupdater root"
            )
        root_exists, root_checks = bind_permission_entry(
            root_value,
            permission_entries.get("root"),
            role="root",
            expected_path_digest=path_digest(root_path, platform),
            context="storage.root",
        )
        results.extend(root_checks)
        results.append(
            observation(
                "storage.root.exists",
                root_exists,
                safe_when=True,
                description="state root existence",
            )
        )

        seen_roles: set[str] = set()
        seen_paths: set[str] = set()
        expected_names = {
            "primary": "state.json",
            "lastKnownGood": "state.json.lkg",
            "resume": "state.json.resume",
        }
        for index, raw_artifact in enumerate(artifacts):
            context = f"storage.artifacts[{index}]"
            artifact = require_object(raw_artifact, context)
            require_exact_keys(
                artifact,
                context,
                {"role", "path", "exists", "contentSha256", "ownerTrusted", "aclComplete", "lessTrustedReadable", "lessTrustedWritable", "unexpectedLink"},
            )
            role = require_enum(artifact["role"], f"{context}.role", STATE_ROLES)
            if role in seen_roles:
                raise EvidenceError("storage.artifacts contains a duplicate role")
            seen_roles.add(role)
            path_text = absolute_path(artifact["path"], f"{context}.path", platform)
            normalized_path = path_key(path_text, platform)
            if normalized_path in seen_paths:
                raise EvidenceError("storage.artifacts contains duplicate paths")
            seen_paths.add(normalized_path)
            expected_path = str(path_type(root_path) / expected_names[role])
            if normalized_path != path_key(expected_path, platform):
                raise EvidenceError(
                    f"{context}.path does not match the fixed state artifact contract"
                )
            exists, artifact_permission_checks = bind_permission_entry(
                artifact,
                permission_entries.get(role),
                role=role,
                expected_path_digest=path_digest(path_text, platform),
                context=context,
            )
            results.extend(artifact_permission_checks)
            permission_values = {
                name: require_nullable_bool(artifact[name], f"{context}.{name}")
                for name in (
                    "ownerTrusted",
                    "aclComplete",
                    "lessTrustedReadable",
                    "lessTrustedWritable",
                    "unexpectedLink",
                )
            }
            content_digest = artifact["contentSha256"]
            if content_digest is not None:
                require_sha256(content_digest, f"{context}.contentSha256")
            identifier = f"artifact.{role}"
            if exists is None:
                if content_digest is not None or any(
                    value is not None for value in permission_values.values()
                ):
                    raise EvidenceError(
                        f"{context} unknown existence requires null observations"
                    )
                results.append(check(f"{identifier}.exists", Status.OPEN, "artifact existence was not established"))
                continue
            if not exists:
                if content_digest is not None or any(
                    value is not None for value in permission_values.values()
                ):
                    raise EvidenceError(
                        f"{context} absent artifact requires null observations"
                    )
                results.append(check(f"{identifier}.exists", Status.PASS, "artifact is absent"))
                continue
            if content_digest is None:
                results.append(check(f"{identifier}.content", Status.OPEN, "artifact digest is absent"))
                continue
            if not platform_matches_host:
                results.append(
                    check(
                        f"{identifier}.content",
                        Status.OPEN,
                        "artifact bytes require inspection on the declared platform",
                    )
                )
                continue
            try:
                contents = read_limited_bytes(Path(path_text))
            except EvidenceError:
                results.append(check(f"{identifier}.content", Status.OPEN, "artifact is unreadable"))
                continue
            actual_digest = hashlib.sha256(contents).hexdigest()
            if actual_digest != content_digest:
                results.append(check(f"{identifier}.digest", Status.FAIL, "artifact digest does not match"))
            else:
                results.append(check(f"{identifier}.digest", Status.PASS, "artifact bytes match the snapshot"))
            try:
                state_document = parse_json_bytes(contents)
                _, credential_urls, unknown_query_urls = scan_json_strings(
                    state_document, credential_names
                )
            except EvidenceError:
                results.append(check(f"{identifier}.scan", Status.OPEN, "artifact JSON could not be fully inspected"))
                continue
            if credential_urls:
                results.append(check(f"{identifier}.urls", Status.FAIL, "credential-bearing URL is persisted"))
            elif unknown_query_urls:
                results.append(check(f"{identifier}.urls", Status.OPEN, "query-bearing URL needs credential classification"))
            else:
                results.append(check(f"{identifier}.urls", Status.PASS, "no credential-bearing URL was detected"))
        if seen_roles != STATE_ROLES:
            raise EvidenceError("storage.artifacts must cover primary, lastKnownGood, and resume")

    results.append(
        observation(
            "urlSampling.complete",
            sampling_complete,
            safe_when=True,
            description="production URL sample coverage",
        )
    )
    if url_sample_bytes is None:
        if (
            sampling_complete is True
            or sample_count != 0
            or sample_digest is not None
            or credential_mode == "short-lived-signed"
        ):
            results.append(check("urlSampling.bytes", Status.OPEN, "declared URL samples were not supplied on stdin"))
        sample_known_credentials = 0
        sample_userinfo = 0
        sample_unknown_queries = 0
        valid_url_count = 0
    else:
        if len(url_sample_bytes) > MAX_EVIDENCE_BYTES:
            raise EvidenceError("URL samples exceed the byte limit")
        lines = [line for line in url_sample_bytes.decode("utf-8").splitlines() if line]
        if len(lines) > MAX_URL_SAMPLES:
            raise EvidenceError("URL samples exceed the entry limit")
        if len(lines) != sample_count:
            results.append(check("urlSampling.count", Status.FAIL, "URL sample count does not match"))
        actual_digest = hashlib.sha256(url_sample_bytes).hexdigest()
        if sample_digest is None:
            results.append(check("urlSampling.digest", Status.OPEN, "URL sample digest is absent"))
        elif sample_digest != actual_digest:
            results.append(check("urlSampling.digest", Status.FAIL, "URL sample digest does not match"))
        else:
            results.append(check("urlSampling.digest", Status.PASS, "URL sample bytes match the snapshot"))
        sample_known_credentials = 0
        sample_userinfo = 0
        sample_unknown_queries = 0
        valid_url_count = 0
        malformed = 0
        for value in lines:
            is_url, userinfo, known_query, unclassified = url_characteristics(
                value, credential_names
            )
            if not is_url:
                malformed += 1
            else:
                valid_url_count += 1
            if userinfo:
                sample_userinfo += 1
            if known_query:
                sample_known_credentials += 1
            if unclassified:
                sample_unknown_queries += 1
        if malformed:
            results.append(check("urlSampling.format", Status.OPEN, "one or more URL samples are malformed"))

    if transport_mode == "unknown":
        results.append(check("urlSampling.transport", Status.OPEN, "update transport mode is unknown"))
    elif transport_mode == "network" and valid_url_count == 0:
        results.append(check("urlSampling.transport", Status.OPEN, "network deployment has no validated URL sample"))
    elif transport_mode == "local-only" and sample_count != 0:
        results.append(check("urlSampling.transport", Status.FAIL, "local-only deployment declares network URL samples"))
    else:
        results.append(check("urlSampling.transport", Status.PASS, "URL samples match the transport mode"))
    if sample_userinfo:
        results.append(check("urlSampling.userinfo", Status.FAIL, "URL userinfo credentials are forbidden"))

    if credential_mode == "unknown":
        results.append(check("urlSampling.mode", Status.OPEN, "URL credential mode is unknown"))
    elif credential_mode == "none":
        if sample_known_credentials:
            results.append(check("urlSampling.mode", Status.FAIL, "samples contradict the no-credential policy"))
        elif sample_unknown_queries:
            results.append(check("urlSampling.mode", Status.OPEN, "query samples need credential classification"))
        else:
            results.append(check("urlSampling.mode", Status.PASS, "no credential-bearing sample was detected"))
    else:
        if transport_mode != "network":
            results.append(check("urlSampling.mode", Status.FAIL, "signed URL credentials require network transport"))
        elif not credential_names or sample_known_credentials == 0:
            results.append(check("urlSampling.mode", Status.OPEN, "signed-URL credential samples were not classified"))
        else:
            results.append(check("urlSampling.mode", Status.PASS, "signed-URL credential keys were observed and classified"))
        results.append(
            observation(
                "urlSampling.lifetime",
                short_lived,
                safe_when=True,
                description="signed-URL lifetime and scope",
                incomplete_is_open=False,
            )
        )
    if sample_unknown_queries:
        results.append(check("urlSampling.classification", Status.OPEN, "one or more URL components need credential classification"))
    results.append(
        observation(
            "urlSampling.telemetry",
            telemetry,
            safe_when=True,
            description="surrounding telemetry redaction",
            incomplete_is_open=False,
        )
    )
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--permission-evidence", type=Path)
    parser.add_argument("--raw-permission-evidence", type=Path)
    parser.add_argument("--url-samples-stdin", action="store_true")
    parser.add_argument("--evaluated-at", required=True)
    args = parser.parse_args()
    try:
        sample_bytes = None
        if args.url_samples_stdin:
            sample_bytes = sys.stdin.buffer.read(MAX_EVIDENCE_BYTES + 1)
        checks = validate_document(
            load_json(args.evidence),
            permission_evidence=args.permission_evidence,
            raw_permission_evidence=args.raw_permission_evidence,
            url_sample_bytes=sample_bytes,
            evaluated_at=timestamp_nanoseconds(args.evaluated_at, "evaluated-at"),
        )
    except (EvidenceError, UnicodeError):
        print(json.dumps({"risk": RISK_ID, "status": "OPEN", "error": "evidence is invalid, incomplete, or unreadable"}, sort_keys=True))
        return 3
    status = combine_statuses(Status(item["status"]) for item in checks)
    print(json.dumps({"risk": RISK_ID, "status": status.value, "checks": checks}, indent=2, sort_keys=True))
    return exit_code(status)


if __name__ == "__main__":
    raise SystemExit(main())
