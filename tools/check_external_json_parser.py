#!/usr/bin/env python3
"""Validate offline RISK-004 signing/approval JSON-parser evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from evidence_contract import (
    EvidenceError,
    Status,
    combine_statuses,
    exit_code,
    load_json,
    parse_json_bytes,
    read_limited_bytes,
    require_array,
    require_bool,
    require_exact_keys,
    require_integer,
    require_nullable_bool,
    require_object,
    require_sha256,
    require_text,
    timestamp_nanoseconds,
)


RISK_ID = "RISK-004"
EXPECTED_CORPUS_SHA256 = "07aea8a32d5ac33aabc078259c295e83550a8d8c56e1ac567ee6971304710146"


def check(identifier: str, status: Status, reason: str) -> dict[str, str]:
    return {"id": identifier, "status": status.value, "reason": reason}


def nullable_text(value: Any, context: str, *, maximum: int = 512) -> str | None:
    if value is None:
        return None
    return require_text(value, context, maximum=maximum)


def validate_file_binding(
    path: Path | None,
    expected_digest: str,
    *,
    identifier: str,
    description: str,
) -> list[dict[str, str]]:
    if path is None:
        return [check(identifier, Status.OPEN, f"{description} was not supplied")]
    try:
        contents = read_limited_bytes(path)
    except EvidenceError:
        return [check(identifier, Status.OPEN, f"{description} is unreadable")]
    if hashlib.sha256(contents).hexdigest() != expected_digest:
        return [check(identifier, Status.FAIL, f"{description} digest does not match")]
    return [check(identifier, Status.PASS, f"{description} bytes match the snapshot")]


def parse_corpus(contents: bytes) -> dict[str, dict[str, Any]]:
    try:
        text = contents.decode("utf-8")
    except UnicodeError as error:
        raise EvidenceError("corpus must be UTF-8") from error
    cases: dict[str, dict[str, Any]] = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("|")
        if len(parts) != 3:
            raise EvidenceError(f"corpus line {line_number} has invalid fields")
        expected, name, encoded = parts
        if expected not in {"accept", "reject"}:
            raise EvidenceError(f"corpus line {line_number} has invalid expectation")
        require_text(name, f"corpus line {line_number} name")
        if name in cases:
            raise EvidenceError("corpus contains a duplicate case name")
        if not encoded or encoded != encoded.lower() or len(encoded) % 2 != 0:
            raise EvidenceError(f"corpus line {line_number} has invalid hexadecimal")
        try:
            raw = bytes.fromhex(encoded)
        except ValueError as error:
            raise EvidenceError(f"corpus line {line_number} has invalid hexadecimal") from error
        expected_string: str | None = None
        expected_integer: str | None = None
        if expected == "accept":
            try:
                parsed = require_object(
                    parse_json_bytes(raw),
                    f"accepted corpus case {name}",
                )
            except EvidenceError as error:
                raise EvidenceError("accepted corpus case is not valid reference JSON") from error
            if "v" in parsed:
                value = parsed["v"]
                if isinstance(value, str):
                    try:
                        expected_string = value.encode("utf-8").hex()
                    except UnicodeError as error:
                        raise EvidenceError(
                            "accepted corpus string is not Unicode scalar text"
                        ) from error
                elif isinstance(value, int) and not isinstance(value, bool):
                    expected_integer = str(value)
        cases[name] = {
            "accepted": expected == "accept",
            "decodedStringUtf8Hex": expected_string,
            "integerDecimal": expected_integer,
        }
    if not cases:
        raise EvidenceError("corpus contains no cases")
    return cases


def validate_parser_identity(value: Any, context: str) -> dict[str, Any]:
    parser = require_object(value, context)
    require_exact_keys(parser, context, {"name", "version", "configurationSha256"})
    require_text(parser["name"], f"{context}.name")
    require_text(parser["version"], f"{context}.version")
    require_sha256(parser["configurationSha256"], f"{context}.configurationSha256")
    return parser


def validate_results(
    document: Any,
    *,
    expected_digest: str | None,
    actual_digest: str,
    corpus_digest: str,
    corpus_cases: dict[str, dict[str, Any]],
    expected_parser: dict[str, Any],
    snapshot_capture: int,
    evaluated_at: int,
) -> list[dict[str, str]]:
    results = require_object(document, "results")
    require_exact_keys(
        results,
        "results",
        {
            "schemaVersion",
            "corpusSha256",
            "capturedAt",
            "validUntil",
            "parser",
            "complete",
            "cases",
        },
    )
    if require_integer(results["schemaVersion"], "results.schemaVersion") != 1:
        raise EvidenceError("results.schemaVersion must be integer 1")
    require_sha256(results["corpusSha256"], "results.corpusSha256")
    results_capture = timestamp_nanoseconds(results["capturedAt"], "results.capturedAt")
    results_valid_until = timestamp_nanoseconds(
        results["validUntil"], "results.validUntil"
    )
    if results_valid_until <= results_capture:
        raise EvidenceError("results.validUntil must be after capturedAt")
    parser = validate_parser_identity(results["parser"], "results.parser")
    complete = require_bool(results["complete"], "results.complete")

    checks: list[dict[str, str]] = []
    if expected_digest is None:
        checks.append(check("results.digest", Status.OPEN, "workflow snapshot lacks a results digest"))
    elif expected_digest != actual_digest:
        checks.append(check("results.digest", Status.FAIL, "parser results digest does not match"))
    else:
        checks.append(check("results.digest", Status.PASS, "parser results bytes match the workflow snapshot"))
    if results["corpusSha256"] != corpus_digest:
        checks.append(check("results.corpus", Status.FAIL, "parser results target a different corpus"))
    else:
        checks.append(check("results.corpus", Status.PASS, "parser results target the current corpus"))
    if parser != expected_parser:
        checks.append(check("results.parser", Status.FAIL, "parser identity or configuration does not match"))
    else:
        checks.append(check("results.parser", Status.PASS, "parser identity and configuration match"))
    if results_capture > snapshot_capture:
        checks.append(check("results.capture", Status.FAIL, "parser results were captured after the workflow snapshot"))
    else:
        checks.append(check("results.capture", Status.PASS, "parser results predate the workflow snapshot"))
    if evaluated_at > results_valid_until:
        checks.append(check("results.age", Status.OPEN, "parser result validity has elapsed"))
    else:
        checks.append(check("results.age", Status.PASS, "parser results are current"))

    observations: dict[str, dict[str, Any]] = {}
    for index, raw_case in enumerate(require_array(results["cases"], "results.cases")):
        context = f"results.cases[{index}]"
        case = require_object(raw_case, context)
        require_exact_keys(
            case,
            context,
            {"name", "accepted", "decodedStringUtf8Hex", "integerDecimal"},
        )
        name = require_text(case["name"], f"{context}.name")
        if name in observations:
            raise EvidenceError("results contains a duplicate case name")
        accepted = require_bool(case["accepted"], f"{context}.accepted")
        decoded = nullable_text(
            case["decodedStringUtf8Hex"],
            f"{context}.decodedStringUtf8Hex",
            maximum=8192,
        )
        if decoded is not None:
            if decoded != decoded.lower() or len(decoded) % 2 != 0:
                raise EvidenceError(f"{context}.decodedStringUtf8Hex is invalid")
            try:
                bytes.fromhex(decoded)
            except ValueError as error:
                raise EvidenceError(f"{context}.decodedStringUtf8Hex is invalid") from error
        integer = nullable_text(
            case["integerDecimal"], f"{context}.integerDecimal", maximum=256
        )
        if integer is not None:
            try:
                if str(int(integer, 10)) != integer:
                    raise ValueError
            except ValueError as error:
                raise EvidenceError(f"{context}.integerDecimal is not canonical") from error
        observations[name] = {
            "accepted": accepted,
            "decodedStringUtf8Hex": decoded,
            "integerDecimal": integer,
        }

    unknown = observations.keys() - corpus_cases.keys()
    missing = corpus_cases.keys() - observations.keys()
    if unknown:
        checks.append(check("cases.unknown", Status.FAIL, "parser results contain unknown corpus cases"))
    if missing:
        checks.append(
            check(
                "cases.missing",
                Status.FAIL if complete else Status.OPEN,
                "parser results do not cover every corpus case",
            )
        )
    elif complete:
        checks.append(check("cases.complete", Status.PASS, "parser results cover every corpus case"))
    else:
        checks.append(check("cases.complete", Status.OPEN, "parser results are not declared complete"))

    for name in sorted(corpus_cases.keys() & observations.keys()):
        expected = corpus_cases[name]
        observed = observations[name]
        if observed["accepted"] != expected["accepted"]:
            checks.append(check(f"case.{name}", Status.FAIL, "parser accept/reject behavior differs"))
            continue
        if not expected["accepted"] and (
            observed["decodedStringUtf8Hex"] is not None
            or observed["integerDecimal"] is not None
        ):
            checks.append(check(f"case.{name}", Status.FAIL, "rejected case reports a semantic value"))
            continue
        if observed["decodedStringUtf8Hex"] != expected["decodedStringUtf8Hex"]:
            checks.append(check(f"case.{name}", Status.FAIL, "parser string decoding differs"))
            continue
        if observed["integerDecimal"] != expected["integerDecimal"]:
            checks.append(check(f"case.{name}", Status.FAIL, "parser integer semantics differ"))
            continue
        checks.append(check(f"case.{name}", Status.PASS, "parser behavior matches the corpus"))
    return checks


def validate_workflow(
    document: Any,
    *,
    corpus_digest: str,
    corpus_cases: dict[str, dict[str, Any]],
    workflow_path: Path | None,
    parser_configuration_path: Path | None,
    results_path: Path | None,
    evaluated_at: int,
) -> list[dict[str, str]]:
    root = require_object(document, "evidence")
    require_exact_keys(
        root,
        "evidence",
        {"schemaVersion", "deployment", "flow", "attestation"},
    )
    if require_integer(root["schemaVersion"], "evidence.schemaVersion") != 1:
        raise EvidenceError("evidence.schemaVersion must be integer 1")
    deployment = require_object(root["deployment"], "deployment")
    require_exact_keys(
        deployment,
        "deployment",
        {"id", "environment", "capturedAt", "validUntil", "workflowSha256"},
    )
    require_text(deployment["id"], "deployment.id")
    environment = require_text(deployment["environment"], "deployment.environment")
    if environment not in {"production", "staging", "test", "development"}:
        raise EvidenceError("deployment.environment is invalid")
    captured_at = timestamp_nanoseconds(deployment["capturedAt"], "deployment.capturedAt")
    valid_until = timestamp_nanoseconds(deployment["validUntil"], "deployment.validUntil")
    if valid_until <= captured_at:
        raise EvidenceError("deployment.validUntil must be after capturedAt")
    workflow_digest = require_sha256(
        deployment["workflowSha256"], "deployment.workflowSha256"
    )

    flow = require_object(root["flow"], "flow")
    require_exact_keys(
        flow,
        "flow",
        {"parserUsed", "signsExactInputBytes", "rejectsOnParseFailure", "parser", "resultsSha256"},
    )
    parser_used = require_nullable_bool(flow["parserUsed"], "flow.parserUsed")
    exact_bytes = require_nullable_bool(
        flow["signsExactInputBytes"], "flow.signsExactInputBytes"
    )
    rejects_failure = require_nullable_bool(
        flow["rejectsOnParseFailure"], "flow.rejectsOnParseFailure"
    )
    expected_parser = None
    if flow["parser"] is not None:
        expected_parser = validate_parser_identity(flow["parser"], "flow.parser")
    results_digest = flow["resultsSha256"]
    if results_digest is not None:
        require_sha256(results_digest, "flow.resultsSha256")

    attestation = require_object(root["attestation"], "attestation")
    require_exact_keys(
        attestation, "attestation", {"productionSnapshot", "reviewedBy", "reviewedAt"}
    )
    production = require_bool(
        attestation["productionSnapshot"], "attestation.productionSnapshot"
    )
    reviewed_by = nullable_text(attestation["reviewedBy"], "attestation.reviewedBy")
    reviewed_at_text = nullable_text(attestation["reviewedAt"], "attestation.reviewedAt")
    reviewed_at = None
    if reviewed_at_text is not None:
        reviewed_at = timestamp_nanoseconds(reviewed_at_text, "attestation.reviewedAt")
        if reviewed_at < captured_at:
            raise EvidenceError("attestation.reviewedAt must not precede capture")

    checks: list[dict[str, str]] = []
    checks.extend(
        validate_file_binding(
            workflow_path,
            workflow_digest,
            identifier="workflow.binding",
            description="workflow capture",
        )
    )
    if captured_at > evaluated_at:
        checks.append(check("snapshot.age", Status.FAIL, "workflow capture is after evaluation time"))
    elif evaluated_at > valid_until:
        checks.append(check("snapshot.age", Status.OPEN, "workflow evidence validity has elapsed"))
    else:
        checks.append(check("snapshot.age", Status.PASS, "workflow evidence is current"))
    if reviewed_at is not None and reviewed_at > evaluated_at:
        checks.append(check("attestation.time", Status.FAIL, "workflow review is after evaluation time"))
    if environment == "production" and production and reviewed_by is not None and reviewed_at is not None:
        checks.append(check("attestation.production", Status.PASS, "production workflow is signed off"))
    else:
        checks.append(check("attestation.production", Status.OPEN, "production workflow is not signed off"))

    if exact_bytes is True:
        checks.append(check("flow.exactBytes", Status.PASS, "signer signs the exact supplied bytes"))
    elif exact_bytes is False:
        checks.append(check("flow.exactBytes", Status.FAIL, "signer does not sign the exact supplied bytes"))
    else:
        checks.append(check("flow.exactBytes", Status.OPEN, "exact-byte signing was not established"))

    if parser_used is None:
        checks.append(check("flow.parser", Status.OPEN, "whether the signing workflow parses JSON is unknown"))
        return checks

    if not parser_used:
        if (
            expected_parser is not None
            or results_digest is not None
            or parser_configuration_path is not None
            or results_path is not None
            or rejects_failure is not None
        ):
            checks.append(
                check(
                    "flow.noParser",
                    Status.FAIL,
                    "no-parser flow contains parser-specific evidence",
                )
            )
        else:
            checks.append(check("flow.noParser", Status.PASS, "approval and signing do not parse JSON"))
        return checks

    if rejects_failure is True:
        checks.append(check("flow.parseFailure", Status.PASS, "parse failures stop approval/signing"))
    elif rejects_failure is False:
        checks.append(check("flow.parseFailure", Status.FAIL, "parse failures do not stop approval/signing"))
    else:
        checks.append(check("flow.parseFailure", Status.OPEN, "parse-failure handling is unknown"))
    if expected_parser is None:
        checks.append(check("flow.identity", Status.OPEN, "external parser identity is absent"))
        return checks
    checks.extend(
        validate_file_binding(
            parser_configuration_path,
            expected_parser["configurationSha256"],
            identifier="flow.configuration",
            description="parser configuration capture",
        )
    )
    if results_path is None:
        checks.append(check("results.present", Status.OPEN, "external parser results were not supplied"))
        return checks
    try:
        results_bytes = read_limited_bytes(results_path)
    except EvidenceError:
        checks.append(check("results.present", Status.OPEN, "external parser results are unreadable"))
        return checks
    results_document = parse_json_bytes(results_bytes)
    checks.extend(
        validate_results(
            results_document,
            expected_digest=results_digest,
            actual_digest=hashlib.sha256(results_bytes).hexdigest(),
            corpus_digest=corpus_digest,
            corpus_cases=corpus_cases,
            expected_parser=expected_parser,
            snapshot_capture=captured_at,
            evaluated_at=evaluated_at,
        )
    )
    return checks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--workflow", type=Path)
    parser.add_argument("--parser-configuration", type=Path)
    parser.add_argument("--results", type=Path)
    parser.add_argument("--evaluated-at", required=True)
    args = parser.parse_args()
    try:
        corpus_bytes = read_limited_bytes(args.corpus)
        corpus_digest = hashlib.sha256(corpus_bytes).hexdigest()
        if corpus_digest != EXPECTED_CORPUS_SHA256:
            raise EvidenceError("corpus does not match the canonical repository corpus")
        corpus_cases = parse_corpus(corpus_bytes)
        checks = validate_workflow(
            load_json(args.evidence),
            corpus_digest=corpus_digest,
            corpus_cases=corpus_cases,
            workflow_path=args.workflow,
            parser_configuration_path=args.parser_configuration,
            results_path=args.results,
            evaluated_at=timestamp_nanoseconds(args.evaluated_at, "evaluated-at"),
        )
    except EvidenceError:
        print(json.dumps({"risk": RISK_ID, "status": "OPEN", "error": "evidence is invalid, incomplete, or unreadable"}, sort_keys=True))
        return 3
    status = combine_statuses(Status(item["status"]) for item in checks)
    print(json.dumps({"risk": RISK_ID, "status": status.value, "checks": checks}, indent=2, sort_keys=True))
    return exit_code(status)


if __name__ == "__main__":
    raise SystemExit(main())
