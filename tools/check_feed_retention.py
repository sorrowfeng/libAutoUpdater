#!/usr/bin/env python3
"""Validate an offline RISK-002 release-expiry and retention snapshot."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from evidence_contract import (
    EvidenceError,
    MAX_EVIDENCE_BYTES,
    Status,
    combine_statuses,
    exit_code,
    load_json,
    require_array,
    require_bool,
    require_enum,
    require_exact_keys,
    require_integer,
    require_object,
    parse_json_bytes,
    require_sha256,
    require_text,
    timestamp_nanoseconds,
)


RISK_ID = "RISK-002"
TIMESTAMP_PROFILE = "libAutoUpdater-rfc3339-nanoseconds-v1"


def check(identifier: str, status: Status, reason: str) -> dict[str, str]:
    return {"id": identifier, "status": status.value, "reason": reason}


def nullable_text(value: Any, context: str, *, timestamp: bool = False) -> str | None:
    if value is None:
        return None
    if timestamp:
        timestamp_nanoseconds(value, context)
        return value
    return require_text(value, context)


def validate_policy(document: Any) -> dict[str, Any]:
    policy = require_object(document, "policy")
    require_exact_keys(
        policy,
        "policy",
        {
            "schemaVersion",
            "policyId",
            "canonicalTimestampProfile",
            "maxManifestLifetimeSeconds",
            "maxDownloadableAgeSeconds",
            "maxIndexAgeSeconds",
            "maxSnapshotAgeSeconds",
            "minimumDownloadableReleaseCount",
            "expiredMetadataMustBeUnavailable",
            "obsoleteIndexesMustBeUnavailable",
            "approvedBy",
            "approvedAt",
        },
    )
    if policy["schemaVersion"] != 1 or isinstance(policy["schemaVersion"], bool):
        raise EvidenceError("policy.schemaVersion must be integer 1")
    require_text(policy["policyId"], "policy.policyId")
    require_enum(
        policy["canonicalTimestampProfile"],
        "policy.canonicalTimestampProfile",
        {TIMESTAMP_PROFILE},
    )
    require_integer(
        policy["maxManifestLifetimeSeconds"],
        "policy.maxManifestLifetimeSeconds",
        minimum=1,
    )
    require_integer(
        policy["maxDownloadableAgeSeconds"],
        "policy.maxDownloadableAgeSeconds",
        minimum=1,
    )
    require_integer(
        policy["maxIndexAgeSeconds"],
        "policy.maxIndexAgeSeconds",
        minimum=1,
    )
    require_integer(
        policy["maxSnapshotAgeSeconds"],
        "policy.maxSnapshotAgeSeconds",
        minimum=1,
    )
    require_integer(
        policy["minimumDownloadableReleaseCount"],
        "policy.minimumDownloadableReleaseCount",
        minimum=1,
    )
    require_bool(
        policy["expiredMetadataMustBeUnavailable"],
        "policy.expiredMetadataMustBeUnavailable",
    )
    require_bool(
        policy["obsoleteIndexesMustBeUnavailable"],
        "policy.obsoleteIndexesMustBeUnavailable",
    )
    nullable_text(policy["approvedBy"], "policy.approvedBy")
    nullable_text(policy["approvedAt"], "policy.approvedAt", timestamp=True)
    return policy


def validate_snapshot(
    document: Any,
    policy: dict[str, Any],
    policy_digest: str,
    evaluated_at: int,
) -> list[dict[str, str]]:
    snapshot = require_object(document, "snapshot")
    require_exact_keys(
        snapshot,
        "snapshot",
        {
            "schemaVersion",
            "deployment",
            "capturedAt",
            "policyId",
            "policySha256",
            "inventoryComplete",
            "collector",
            "client",
            "releases",
            "indexes",
            "attestation",
        },
    )
    if snapshot["schemaVersion"] != 1 or isinstance(snapshot["schemaVersion"], bool):
        raise EvidenceError("snapshot.schemaVersion must be integer 1")
    require_text(snapshot["deployment"], "snapshot.deployment")
    captured_at = timestamp_nanoseconds(snapshot["capturedAt"], "snapshot.capturedAt")
    require_text(snapshot["policyId"], "snapshot.policyId")
    supplied_digest = snapshot["policySha256"]
    if supplied_digest is not None:
        require_sha256(supplied_digest, "snapshot.policySha256")
    inventory_complete = require_bool(
        snapshot["inventoryComplete"], "snapshot.inventoryComplete"
    )

    collector = require_object(snapshot["collector"], "snapshot.collector")
    require_exact_keys(collector, "snapshot.collector", {"name", "version"})
    require_text(collector["name"], "snapshot.collector.name")
    require_text(collector["version"], "snapshot.collector.version")

    client = require_object(snapshot["client"], "snapshot.client")
    require_exact_keys(
        client,
        "snapshot.client",
        {"configurationSha256", "rejectExpiredManifest", "routingMode"},
    )
    require_sha256(
        client["configurationSha256"], "snapshot.client.configurationSha256"
    )
    reject_expired = client["rejectExpiredManifest"]
    if reject_expired is not None and not isinstance(reject_expired, bool):
        raise EvidenceError(
            "snapshot.client.rejectExpiredManifest must be true, false, or null"
        )
    routing_mode = require_enum(
        client["routingMode"],
        "snapshot.client.routingMode",
        {"direct-manifest", "index", "unknown"},
    )

    attestation = require_object(snapshot["attestation"], "snapshot.attestation")
    require_exact_keys(
        attestation,
        "snapshot.attestation",
        {"productionSnapshot", "reviewedBy", "reviewedAt"},
    )
    production_snapshot = require_bool(
        attestation["productionSnapshot"],
        "snapshot.attestation.productionSnapshot",
    )
    reviewed_by = nullable_text(
        attestation["reviewedBy"], "snapshot.attestation.reviewedBy"
    )
    reviewed_at_text = nullable_text(
        attestation["reviewedAt"],
        "snapshot.attestation.reviewedAt",
        timestamp=True,
    )
    review_after_evaluation = False
    if reviewed_at_text is not None:
        reviewed_at = timestamp_nanoseconds(
            reviewed_at_text, "snapshot.attestation.reviewedAt"
        )
        if reviewed_at < captured_at:
            raise EvidenceError("snapshot review must not precede capture")
        if reviewed_at > evaluated_at:
            review_after_evaluation = True

    results: list[dict[str, str]] = []
    if snapshot["policyId"] != policy["policyId"]:
        results.append(check("policy.id", Status.FAIL, "snapshot policy ID does not match"))
    else:
        results.append(check("policy.id", Status.PASS, "snapshot policy ID matches"))
    if supplied_digest is None:
        results.append(check("policy.digest", Status.OPEN, "snapshot lacks a policy digest"))
    elif supplied_digest != policy_digest:
        results.append(check("policy.digest", Status.FAIL, "snapshot policy digest does not match"))
    else:
        results.append(check("policy.digest", Status.PASS, "snapshot is bound to the policy bytes"))
    if policy["expiredMetadataMustBeUnavailable"]:
        results.append(check("policy.expiry", Status.PASS, "policy removes expired metadata"))
    else:
        results.append(check("policy.expiry", Status.FAIL, "policy permits expired metadata to remain available"))
    if policy["obsoleteIndexesMustBeUnavailable"]:
        results.append(check("policy.index", Status.PASS, "policy removes obsolete indexes"))
    else:
        results.append(check("policy.index", Status.FAIL, "policy permits obsolete indexes to remain available"))
    if policy["approvedBy"] is None or policy["approvedAt"] is None:
        results.append(check("policy.approval", Status.OPEN, "retention policy lacks approval evidence"))
    else:
        approved_at = timestamp_nanoseconds(
            policy["approvedAt"], "policy.approvedAt"
        )
        if approved_at > evaluated_at:
            results.append(check("policy.approval", Status.FAIL, "retention policy approval is after evaluation time"))
        elif approved_at > captured_at:
            results.append(check("policy.approval", Status.OPEN, "retention policy was approved after snapshot capture"))
        else:
            results.append(check("policy.approval", Status.PASS, "retention policy was approved before capture"))
    if inventory_complete:
        results.append(check("inventory.complete", Status.PASS, "release inventory is declared complete"))
    else:
        results.append(check("inventory.complete", Status.OPEN, "release inventory is incomplete"))
    if production_snapshot and reviewed_by is not None and reviewed_at_text is not None:
        results.append(check("snapshot.attestation", Status.PASS, "production snapshot is signed off"))
    else:
        results.append(check("snapshot.attestation", Status.OPEN, "production snapshot is not signed off"))
    if review_after_evaluation:
        results.append(check("snapshot.reviewTime", Status.FAIL, "snapshot review is after evaluation time"))
    if captured_at > evaluated_at:
        results.append(check("snapshot.age", Status.FAIL, "snapshot capture is after evaluation time"))
    elif evaluated_at - captured_at > policy["maxSnapshotAgeSeconds"] * 1_000_000_000:
        results.append(check("snapshot.age", Status.OPEN, "snapshot is older than policy allows"))
    else:
        results.append(check("snapshot.age", Status.PASS, "snapshot age satisfies policy"))
    if reject_expired is True:
        results.append(check("client.expiry", Status.PASS, "client rejects expired release manifests"))
    elif reject_expired is False:
        results.append(check("client.expiry", Status.FAIL, "client permits expired release manifests"))
    else:
        results.append(check("client.expiry", Status.OPEN, "client expiry enforcement is unknown"))
    if routing_mode == "unknown":
        results.append(check("client.routing", Status.OPEN, "client routing mode is unknown"))
    else:
        results.append(check("client.routing", Status.PASS, "client routing mode is explicit"))

    releases = require_array(snapshot["releases"], "snapshot.releases")
    seen_resources: set[str] = set()
    definitely_available = 0
    possibly_available = 0
    for index, raw_release in enumerate(releases):
        context = f"snapshot.releases[{index}]"
        release = require_object(raw_release, context)
        require_exact_keys(
            release,
            context,
            {
                "resourceId",
                "version",
                "releaseId",
                "publishedAt",
                "expiresAt",
                "manifestStatus",
                "signatureStatus",
            },
        )
        resource_id = require_text(release["resourceId"], f"{context}.resourceId")
        if any(marker in resource_id for marker in ("://", "?", "#", "@")):
            raise EvidenceError(f"{context}.resourceId must be opaque and credential-free")
        require_text(release["releaseId"], f"{context}.releaseId")
        require_text(release["version"], f"{context}.version")
        if resource_id in seen_resources:
            raise EvidenceError("snapshot contains a duplicate resourceId")
        seen_resources.add(resource_id)

        published_at = timestamp_nanoseconds(
            release["publishedAt"], f"{context}.publishedAt"
        )
        expires_at = timestamp_nanoseconds(
            release["expiresAt"], f"{context}.expiresAt"
        )
        manifest_status = require_enum(
            release["manifestStatus"],
            f"{context}.manifestStatus",
            {"available", "unavailable", "unknown"},
        )
        signature_status = require_enum(
            release["signatureStatus"],
            f"{context}.signatureStatus",
            {"available", "unavailable", "unknown"},
        )
        prefix = f"release.{index}"
        if published_at > captured_at:
            results.append(check(f"{prefix}.publishedAt", Status.FAIL, "release publication is after snapshot capture"))
        if expires_at <= published_at:
            results.append(check(f"{prefix}.lifetime", Status.FAIL, "release expiry is not after publication"))
        elif expires_at - published_at > policy["maxManifestLifetimeSeconds"] * 1_000_000_000:
            results.append(check(f"{prefix}.lifetime", Status.FAIL, "manifest lifetime exceeds policy"))
        else:
            results.append(check(f"{prefix}.lifetime", Status.PASS, "manifest lifetime satisfies policy"))

        known_states = {manifest_status, signature_status} - {"unknown"}
        if manifest_status == "unknown" or signature_status == "unknown":
            results.append(check(f"{prefix}.availability", Status.OPEN, "metadata availability is incomplete"))
            if "unavailable" not in {manifest_status, signature_status}:
                possibly_available += 1
        elif len(known_states) != 1:
            results.append(check(f"{prefix}.availability", Status.FAIL, "manifest and signature availability differ"))
        elif manifest_status == "available":
            definitely_available += 1
            possibly_available += 1
            if expires_at <= captured_at:
                results.append(check(f"{prefix}.expiry", Status.FAIL, "expired signed metadata remains available"))
            elif expires_at <= evaluated_at:
                results.append(check(f"{prefix}.expiry", Status.OPEN, "manifest crossed its expiry boundary after capture"))
            elif captured_at - published_at > policy["maxDownloadableAgeSeconds"] * 1_000_000_000:
                results.append(check(f"{prefix}.age", Status.FAIL, "over-age signed metadata remains available"))
            elif evaluated_at - published_at > policy["maxDownloadableAgeSeconds"] * 1_000_000_000:
                results.append(check(f"{prefix}.age", Status.OPEN, "manifest crossed its maximum age after capture"))
            else:
                results.append(check(f"{prefix}.freshness", Status.PASS, "available metadata is within policy bounds"))
        else:
            results.append(check(f"{prefix}.availability", Status.PASS, "manifest and signature are both unavailable"))

    minimum = policy["minimumDownloadableReleaseCount"]
    if definitely_available >= minimum:
        results.append(check("retention.minimum", Status.PASS, "minimum release availability is satisfied"))
    elif not inventory_complete or possibly_available >= minimum:
        results.append(check("retention.minimum", Status.OPEN, "minimum release availability is not fully observed"))
    else:
        results.append(check("retention.minimum", Status.FAIL, "too few signed releases remain downloadable"))

    indexes = require_array(snapshot["indexes"], "snapshot.indexes")
    current_indexes = 0
    for index, raw_index in enumerate(indexes):
        context = f"snapshot.indexes[{index}]"
        index_entry = require_object(raw_index, context)
        require_exact_keys(
            index_entry,
            context,
            {
                "resourceId",
                "generatedAt",
                "current",
                "indexStatus",
                "signatureStatus",
            },
        )
        resource_id = require_text(index_entry["resourceId"], f"{context}.resourceId")
        if any(marker in resource_id for marker in ("://", "?", "#", "@")):
            raise EvidenceError(f"{context}.resourceId must be opaque and credential-free")
        if resource_id in seen_resources:
            raise EvidenceError("snapshot contains a duplicate resourceId")
        seen_resources.add(resource_id)
        generated_at = timestamp_nanoseconds(
            index_entry["generatedAt"], f"{context}.generatedAt"
        )
        current = require_bool(index_entry["current"], f"{context}.current")
        index_status = require_enum(
            index_entry["indexStatus"],
            f"{context}.indexStatus",
            {"available", "unavailable", "unknown"},
        )
        signature_status = require_enum(
            index_entry["signatureStatus"],
            f"{context}.signatureStatus",
            {"available", "unavailable", "unknown"},
        )
        prefix = f"index.{index}"
        if generated_at > captured_at:
            results.append(check(f"{prefix}.generatedAt", Status.FAIL, "index generation is after snapshot capture"))
        if current:
            current_indexes += 1
        if index_status == "unknown" or signature_status == "unknown":
            results.append(check(f"{prefix}.availability", Status.OPEN, "index availability is incomplete"))
        elif index_status != signature_status:
            results.append(check(f"{prefix}.availability", Status.FAIL, "index and signature availability differ"))
        elif current and index_status == "unavailable":
            results.append(check(f"{prefix}.availability", Status.FAIL, "current signed index is unavailable"))
        elif current:
            if captured_at - generated_at > policy["maxIndexAgeSeconds"] * 1_000_000_000:
                results.append(check(f"{prefix}.age", Status.FAIL, "current signed index exceeds the maximum age"))
            elif evaluated_at - generated_at > policy["maxIndexAgeSeconds"] * 1_000_000_000:
                results.append(check(f"{prefix}.age", Status.OPEN, "current index crossed its maximum age after capture"))
            else:
                results.append(check(f"{prefix}.age", Status.PASS, "current signed index is within the age policy"))
        elif index_status == "available":
            results.append(check(f"{prefix}.obsolete", Status.FAIL, "obsolete signed index remains available"))
        else:
            results.append(check(f"{prefix}.obsolete", Status.PASS, "obsolete signed index is unavailable"))

    if routing_mode == "index":
        if current_indexes == 1:
            results.append(check("index.current", Status.PASS, "exactly one current index is identified"))
        elif current_indexes == 0 and not inventory_complete:
            results.append(check("index.current", Status.OPEN, "current index is not present in an incomplete inventory"))
        else:
            results.append(check("index.current", Status.FAIL, "index routing does not identify exactly one current index"))
    elif routing_mode == "direct-manifest" and current_indexes != 0:
        results.append(check("index.current", Status.FAIL, "direct-manifest routing marks an index as current"))
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument(
        "--evaluated-at",
        required=True,
        help="Trusted current time using the updater's RFC 3339 profile",
    )
    args = parser.parse_args()
    try:
        with args.policy.open("rb") as handle:
            policy_bytes = handle.read(MAX_EVIDENCE_BYTES + 1)
        policy = validate_policy(parse_json_bytes(policy_bytes))
        evaluated_at = timestamp_nanoseconds(args.evaluated_at, "evaluated-at")
        checks = validate_snapshot(
            load_json(args.snapshot),
            policy,
            hashlib.sha256(policy_bytes).hexdigest(),
            evaluated_at,
        )
    except (OSError, EvidenceError):
        print(json.dumps({"risk": RISK_ID, "status": "OPEN", "error": "evidence is invalid, incomplete, or unreadable"}, sort_keys=True))
        return 3
    status = combine_statuses(Status(item["status"]) for item in checks)
    print(json.dumps({"risk": RISK_ID, "status": status.value, "checks": checks}, indent=2, sort_keys=True))
    return exit_code(status)


if __name__ == "__main__":
    raise SystemExit(main())
