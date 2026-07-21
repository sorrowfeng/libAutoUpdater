#!/usr/bin/env python3
"""Validate an offline RISK-001 production privilege-boundary snapshot."""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any

from evidence_contract import (
    EvidenceError,
    Status,
    combine_statuses,
    exit_code,
    load_json,
    require_array,
    require_bool,
    require_enum,
    require_exact_keys,
    require_nullable_bool,
    require_object,
    require_sha256,
    require_text,
    require_timestamp,
    timestamp_nanoseconds,
)


RISK_ID = "RISK-001"
BASE_PATH_ROLES = {
    "installRoot",
    "tempRoot",
    "applyPlan",
    "journalRoot",
    "backupRoot",
    "helperExecutable",
}
CONDITIONAL_PATH_ROLES = {"stateFile", "restartExecutable"}
ALLOWED_PATH_ROLES = BASE_PATH_ROLES | CONDITIONAL_PATH_ROLES
BROKER_CONTROLS = (
    "authenticatedChannel",
    "trustedRootsBound",
    "ownerAclChecked",
    "oneTimeNonce",
    "intentAndDigestBound",
    "releaseAuthorizationVerified",
    "brokerOnlyPlanPublication",
)


def check(identifier: str, status: Status, reason: str) -> dict[str, str]:
    return {"id": identifier, "status": status.value, "reason": reason}


def boolean_observation(
    identifier: str, value: bool | None, *, safe_when: bool, description: str
) -> dict[str, str]:
    if value is None:
        return check(identifier, Status.OPEN, f"{description} was not established")
    if value == safe_when:
        return check(identifier, Status.PASS, f"{description} satisfies policy")
    return check(identifier, Status.FAIL, f"{description} violates policy")


def completeness_observation(
    identifier: str, value: bool | None, description: str
) -> dict[str, str]:
    if value is True:
        return check(identifier, Status.PASS, f"{description} is complete")
    return check(identifier, Status.OPEN, f"{description} is incomplete")


def validate_document(document: Any) -> list[dict[str, str]]:
    root = require_object(document, "evidence")
    require_exact_keys(
        root,
        "evidence",
        {
            "schemaVersion",
            "deployment",
            "principals",
            "paths",
            "launch",
            "restart",
            "stateStore",
            "attestation",
        },
    )
    if root["schemaVersion"] != 1 or isinstance(root["schemaVersion"], bool):
        raise EvidenceError("evidence.schemaVersion must be integer 1")

    deployment = require_object(root["deployment"], "deployment")
    require_exact_keys(
        deployment,
        "deployment",
        {"id", "environment", "platform", "capturedAt", "configurationSha256"},
    )
    require_text(deployment["id"], "deployment.id")
    environment = require_enum(
        deployment["environment"],
        "deployment.environment",
        {"production", "staging", "test", "development"},
    )
    require_enum(
        deployment["platform"], "deployment.platform", {"windows", "macos", "linux"}
    )
    captured_at = timestamp_nanoseconds(
        deployment["capturedAt"], "deployment.capturedAt"
    )
    require_sha256(deployment["configurationSha256"], "deployment.configurationSha256")

    principals = require_object(root["principals"], "principals")
    require_exact_keys(
        principals,
        "principals",
        {
            "application",
            "helper",
            "applicationPrivilege",
            "helperPrivilege",
            "sameCredential",
            "lessTrusted",
        },
    )
    require_text(principals["application"], "principals.application")
    require_text(principals["helper"], "principals.helper")
    application_privilege = require_enum(
        principals["applicationPrivilege"],
        "principals.applicationPrivilege",
        {"standard", "elevated", "system", "unknown"},
    )
    helper_privilege = require_enum(
        principals["helperPrivilege"],
        "principals.helperPrivilege",
        {"standard", "elevated", "system", "unknown"},
    )
    same_credential = require_nullable_bool(
        principals["sameCredential"], "principals.sameCredential"
    )
    less_trusted = require_array(principals["lessTrusted"], "principals.lessTrusted")
    if not less_trusted:
        raise EvidenceError("principals.lessTrusted must identify at least one trust class")
    normalized_principals = [
        require_text(item, f"principals.lessTrusted[{index}]")
        for index, item in enumerate(less_trusted)
    ]
    if len(set(normalized_principals)) != len(normalized_principals):
        raise EvidenceError("principals.lessTrusted contains duplicate identities")

    restart = require_object(root["restart"], "restart")
    require_exact_keys(restart, "restart", {"policy", "privilege"})
    restart_policy = require_enum(
        restart["policy"],
        "restart.policy",
        {"disabled", "trusted-generated", "allowlist", "caller-controlled", "unknown"},
    )
    restart_privilege = require_enum(
        restart["privilege"],
        "restart.privilege",
        {"same", "dropped", "retained", "unknown"},
    )

    state_store = require_object(root["stateStore"], "stateStore")
    require_exact_keys(
        state_store,
        "stateStore",
        {
            "kind",
            "accessControlVerified",
            "atomicCasVerified",
            "crashDurabilityVerified",
        },
    )
    store_kind = require_enum(
        state_store["kind"], "stateStore.kind", {"bundled-json", "custom"}
    )
    store_observations = {
        name: require_nullable_bool(state_store[name], f"stateStore.{name}")
        for name in (
            "accessControlVerified",
            "atomicCasVerified",
            "crashDurabilityVerified",
        )
    }

    required_path_roles = set(BASE_PATH_ROLES)
    if store_kind == "bundled-json":
        required_path_roles.add("stateFile")
    if restart_policy not in {"disabled", "unknown"}:
        required_path_roles.add("restartExecutable")

    results: list[dict[str, str]] = []
    paths = require_array(root["paths"], "paths")
    observed_roles: set[str] = set()
    for index, raw_path in enumerate(paths):
        context = f"paths[{index}]"
        path = require_object(raw_path, context)
        require_exact_keys(
            path,
            context,
            {
                "role",
                "path",
                "exists",
                "ownerTrusted",
                "aclComplete",
                "lessTrustedWritable",
                "lessTrustedReplaceable",
                "unexpectedLink",
            },
        )
        role = require_enum(path["role"], f"{context}.role", ALLOWED_PATH_ROLES)
        if role in observed_roles:
            raise EvidenceError(f"duplicate path role: {role}")
        observed_roles.add(role)
        observed_path = require_text(path["path"], f"{context}.path", maximum=4096)
        path_type = (
            PureWindowsPath
            if deployment["platform"] == "windows"
            else PurePosixPath
        )
        if not path_type(observed_path).is_absolute():
            raise EvidenceError(f"{context}.path must be absolute for the target platform")
        exists = require_nullable_bool(path["exists"], f"{context}.exists")
        owner_trusted = require_nullable_bool(
            path["ownerTrusted"], f"{context}.ownerTrusted"
        )
        acl_complete = require_nullable_bool(
            path["aclComplete"], f"{context}.aclComplete"
        )
        writable = require_nullable_bool(
            path["lessTrustedWritable"], f"{context}.lessTrustedWritable"
        )
        replaceable = require_nullable_bool(
            path["lessTrustedReplaceable"], f"{context}.lessTrustedReplaceable"
        )
        unexpected_link = require_nullable_bool(
            path["unexpectedLink"], f"{context}.unexpectedLink"
        )
        results.extend(
            (
                completeness_observation(
                    f"path.{role}.exists",
                    exists,
                    f"{role} existence evidence",
                ),
                boolean_observation(
                    f"path.{role}.owner",
                    owner_trusted,
                    safe_when=True,
                    description=f"{role} trusted ownership",
                ),
                completeness_observation(
                    f"path.{role}.acl",
                    acl_complete,
                    f"{role} ACL evidence",
                ),
                boolean_observation(
                    f"path.{role}.write",
                    writable,
                    safe_when=False,
                    description=f"{role} lower-trust write access",
                ),
                boolean_observation(
                    f"path.{role}.replace",
                    replaceable,
                    safe_when=False,
                    description=f"{role} lower-trust replace/delete access",
                ),
                boolean_observation(
                    f"path.{role}.link",
                    unexpected_link,
                    safe_when=False,
                    description=f"{role} unexpected link/reparse state",
                ),
            )
        )
    if not required_path_roles.issubset(observed_roles):
        missing = required_path_roles - observed_roles
        raise EvidenceError("paths is missing role(s): " + ", ".join(sorted(missing)))

    launch = require_object(root["launch"], "launch")
    require_exact_keys(launch, "launch", {"kind", *BROKER_CONTROLS})
    launch_kind = require_enum(
        launch["kind"], "launch.kind", {"stock", "authenticated-broker", "unknown"}
    )
    controls = {
        name: require_nullable_bool(launch[name], f"launch.{name}")
        for name in BROKER_CONTROLS
    }
    privilege_rank = {"standard": 0, "elevated": 1, "system": 2}
    if launch_kind == "unknown" or application_privilege == "unknown" or helper_privilege == "unknown":
        results.append(
            check(
                "launch.privilege",
                Status.OPEN,
                "launch or privilege relationship is unknown",
            )
        )
    elif launch_kind == "stock" and privilege_rank[helper_privilege] > privilege_rank[application_privilege]:
        results.append(
            check(
                "launch.privilege",
                Status.FAIL,
                "stock launcher evidence shows a privilege transition",
            )
        )
    elif launch_kind == "stock" and (
        same_credential is not True
        or principals["application"] != principals["helper"]
        or application_privilege != helper_privilege
    ):
        results.append(
            check(
                "launch.privilege",
                Status.OPEN,
                "stock launcher same-credential inheritance was not established",
            )
        )
    else:
        results.append(
            check(
                "launch.privilege",
                Status.PASS,
                "launch privilege relationship is explicit",
            )
        )
    if launch_kind == "authenticated-broker":
        for name, value in controls.items():
            results.append(
                boolean_observation(
                    f"launch.{name}",
                    value,
                    safe_when=True,
                    description=f"broker control {name}",
                )
            )

    if restart_policy == "unknown" or restart_privilege == "unknown":
        results.append(
            check(
                "restart.policy",
                Status.OPEN,
                "restart policy or privilege is unknown",
            )
        )
    elif restart_policy == "caller-controlled" and helper_privilege in {
        "elevated",
        "system",
    }:
        results.append(
            check(
                "restart.policy",
                Status.FAIL,
                "a higher-privilege helper accepts a caller-controlled restart command",
            )
        )
    else:
        results.append(
            check("restart.policy", Status.PASS, "restart command policy is bounded")
        )

    for name, value in store_observations.items():
        results.append(
            boolean_observation(
                f"stateStore.{name}",
                value,
                safe_when=True,
                description=f"{store_kind} state-store {name}",
            )
        )

    attestation = require_object(root["attestation"], "attestation")
    require_exact_keys(
        attestation,
        "attestation",
        {"productionSnapshot", "reviewedBy", "reviewedAt"},
    )
    production_snapshot = require_bool(
        attestation["productionSnapshot"], "attestation.productionSnapshot"
    )
    reviewed_by = attestation["reviewedBy"]
    reviewed_at = attestation["reviewedAt"]
    if reviewed_by is not None:
        require_text(reviewed_by, "attestation.reviewedBy")
    if reviewed_at is not None:
        reviewed_at_ns = timestamp_nanoseconds(
            reviewed_at, "attestation.reviewedAt"
        )
        if reviewed_at_ns < captured_at:
            raise EvidenceError(
                "attestation.reviewedAt must not precede deployment.capturedAt"
            )
    if environment != "production" or not production_snapshot:
        results.append(
            check(
                "attestation.production",
                Status.OPEN,
                "evidence is not attested as a production deployment snapshot",
            )
        )
    elif reviewed_by is None or reviewed_at is None:
        results.append(
            check(
                "attestation.review",
                Status.OPEN,
                "production evidence lacks reviewer sign-off",
            )
        )
    else:
        results.append(
            check(
                "attestation.review",
                Status.PASS,
                "production evidence is signed off",
            )
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    try:
        checks = validate_document(load_json(args.evidence))
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
    report = {
        "risk": RISK_ID,
        "status": status.value,
        "summary": (
            "deployment blocker found"
            if status is Status.FAIL
            else (
                "production evidence is complete"
                if status is Status.PASS
                else "production evidence remains incomplete"
            )
        ),
        "checks": checks,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return exit_code(status)


if __name__ == "__main__":
    raise SystemExit(main())
