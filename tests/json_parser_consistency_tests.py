#!/usr/bin/env python3
"""Regression tests for the offline external JSON-parser evidence validator."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any, Callable


VALIDATOR = Path()
CORPUS = Path()
WORK_DIR = Path()


def corpus_expectations() -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    for line in CORPUS.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        expectation, name, encoded = line.split("|")
        accepted = expectation == "accept"
        decoded: str | None = None
        integer: str | None = None
        if accepted:
            value = json.loads(bytes.fromhex(encoded).decode("utf-8"))
            if isinstance(value, dict) and isinstance(value.get("v"), str):
                decoded = value["v"].encode("utf-8").hex()
            elif (
                isinstance(value, dict)
                and isinstance(value.get("v"), int)
                and not isinstance(value["v"], bool)
            ):
                integer = str(value["v"])
        cases.append(
            {
                "name": name,
                "accepted": accepted,
                "decodedStringUtf8Hex": decoded,
                "integerDecimal": integer,
            }
        )
    by_name = {case["name"]: case for case in cases}
    assert by_name["escaped_surrogate_pair"]["decodedStringUtf8Hex"] == "f09f9880"
    assert by_name["raw_utf8"]["decodedStringUtf8Hex"] == "f09f9880"
    assert by_name["signed_min"]["integerDecimal"] == "-9223372036854775808"
    assert by_name["unsigned_max"]["integerDecimal"] == "18446744073709551615"
    assert by_name["beyond_binary64_integer"]["integerDecimal"] == "9007199254740993"
    return cases


class JsonParserConsistencyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = WORK_DIR / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True)
        self.workflow_capture = self.root / "workflow.capture"
        self.workflow_capture.write_bytes(b"immutable production workflow capture\n")
        self.parser_configuration = self.root / "parser-configuration.capture"
        self.parser_configuration.write_bytes(
            b"production signing parser executable and configuration\n"
        )
        self.parser = {
            "name": "production-signing-json-parser",
            "version": "2026.07.21",
            "configurationSha256": hashlib.sha256(
                self.parser_configuration.read_bytes()
            ).hexdigest(),
        }

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def results(self) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "corpusSha256": hashlib.sha256(CORPUS.read_bytes()).hexdigest(),
            "capturedAt": "2026-07-21T09:00:00Z",
            "validUntil": "2026-07-22T09:00:00Z",
            "parser": dict(self.parser),
            "complete": True,
            "cases": corpus_expectations(),
        }

    def evidence(self) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "deployment": {
                "id": "official-production-signing",
                "environment": "production",
                "capturedAt": "2026-07-21T10:00:00Z",
                "validUntil": "2026-07-22T10:00:00Z",
                "workflowSha256": hashlib.sha256(
                    self.workflow_capture.read_bytes()
                ).hexdigest(),
            },
            "flow": {
                "parserUsed": True,
                "signsExactInputBytes": True,
                "rejectsOnParseFailure": True,
                "parser": dict(self.parser),
                "resultsSha256": None,
            },
            "attestation": {
                "productionSnapshot": True,
                "reviewedBy": "release-owner@example.test",
                "reviewedAt": "2026-07-21T10:10:00Z",
            },
        }

    def run_validator(
        self,
        *,
        mutate_evidence: Callable[[dict[str, Any]], None] | None = None,
        mutate_results: Callable[[dict[str, Any]], None] | None = None,
        include_results: bool = True,
        include_workflow: bool = True,
        include_parser_configuration: bool = True,
        parser_configuration_path: Path | None = None,
        raw_results: bytes | None = None,
        evaluated_at: str = "2026-07-21T12:00:00Z",
    ) -> subprocess.CompletedProcess[str]:
        evidence = self.evidence()
        if mutate_evidence is not None:
            mutate_evidence(evidence)
        command = [
            sys.executable,
            str(VALIDATOR),
            str(self.root / "evidence.json"),
            "--corpus",
            str(CORPUS),
            "--evaluated-at",
            evaluated_at,
        ]
        if include_workflow:
            command.extend(["--workflow", str(self.workflow_capture)])
        if (
            include_parser_configuration
            and evidence["flow"]["parser"] is not None
        ):
            command.extend(
                [
                    "--parser-configuration",
                    str(parser_configuration_path or self.parser_configuration),
                ]
            )
        if include_results:
            if raw_results is None:
                results = self.results()
                if mutate_results is not None:
                    mutate_results(results)
                results_bytes = json.dumps(
                    results, separators=(",", ":"), sort_keys=True
                ).encode("utf-8")
            else:
                results_bytes = raw_results
            results_path = self.root / "results.json"
            results_path.write_bytes(results_bytes)
            evidence["flow"]["resultsSha256"] = hashlib.sha256(
                results_bytes
            ).hexdigest()
            command.extend(["--results", str(results_path)])
        (self.root / "evidence.json").write_text(
            json.dumps(evidence), encoding="utf-8"
        )
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def test_complete_matching_parser_snapshot_passes(self) -> None:
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_exact_byte_workflow_without_a_parser_passes(self) -> None:
        def no_parser(evidence: dict[str, Any]) -> None:
            evidence["flow"] = {
                "parserUsed": False,
                "signsExactInputBytes": True,
                "rejectsOnParseFailure": None,
                "parser": None,
                "resultsSha256": None,
            }

        result = self.run_validator(
            mutate_evidence=no_parser,
            include_results=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unknown_or_missing_production_evidence_remains_open(self) -> None:
        result = self.run_validator(
            mutate_evidence=lambda value: value["flow"].update(
                {
                    "parserUsed": None,
                    "signsExactInputBytes": None,
                    "rejectsOnParseFailure": None,
                    "parser": None,
                    "resultsSha256": None,
                }
            ),
            include_results=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("whether the signing workflow parses JSON is unknown", result.stdout)

        result = self.run_validator(include_results=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("external parser results were not supplied", result.stdout)

        result = self.run_validator(
            mutate_evidence=lambda value: value["attestation"].update(
                {"productionSnapshot": False, "reviewedBy": None, "reviewedAt": None}
            )
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("production workflow is not signed off", result.stdout)

    def test_duplicate_key_and_unicode_differences_fail(self) -> None:
        result = self.run_validator(
            mutate_results=lambda value: next(
                case for case in value["cases"] if case["name"] == "duplicate_key"
            ).update({"accepted": True})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser accept/reject behavior differs", result.stdout)

        result = self.run_validator(
            mutate_results=lambda value: next(
                case
                for case in value["cases"]
                if case["name"] == "escaped_surrogate_pair"
            ).update({"decodedStringUtf8Hex": "efbfbd"})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser string decoding differs", result.stdout)

    def test_large_integer_difference_fails(self) -> None:
        result = self.run_validator(
            mutate_results=lambda value: next(
                case
                for case in value["cases"]
                if case["name"] == "beyond_binary64_integer"
            ).update({"integerDecimal": "9007199254740992"})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser integer semantics differ", result.stdout)

    def test_digest_corpus_and_parser_bindings_fail_closed(self) -> None:
        evidence = self.evidence()
        results_bytes = json.dumps(
            self.results(), separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
        results_path = self.root / "tampered-results.json"
        results_path.write_bytes(results_bytes + b" ")
        evidence["flow"]["resultsSha256"] = hashlib.sha256(results_bytes).hexdigest()
        evidence_path = self.root / "tampered-evidence.json"
        evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
        command = [
            sys.executable,
            str(VALIDATOR),
            str(evidence_path),
            "--corpus",
            str(CORPUS),
            "--workflow",
            str(self.workflow_capture),
            "--parser-configuration",
            str(self.parser_configuration),
            "--results",
            str(results_path),
            "--evaluated-at",
            "2026-07-21T12:00:00Z",
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser results digest does not match", result.stdout)

        result = self.run_validator(
            mutate_results=lambda value: value.update({"corpusSha256": "d" * 64})
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser results target a different corpus", result.stdout)

        result = self.run_validator(
            mutate_results=lambda value: value["parser"].update(
                {"configurationSha256": "e" * 64}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser identity or configuration does not match", result.stdout)

        result = self.run_validator(
            mutate_evidence=lambda value: value["deployment"].update(
                {"workflowSha256": "f" * 64}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("workflow capture digest does not match", result.stdout)

        altered_configuration = self.root / "altered-parser-configuration.capture"
        altered_configuration.write_bytes(b"different parser configuration\n")
        result = self.run_validator(
            parser_configuration_path=altered_configuration,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parser configuration capture digest does not match", result.stdout)

    def test_missing_workflow_or_parser_configuration_remains_open(self) -> None:
        result = self.run_validator(include_workflow=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("workflow capture was not supplied", result.stdout)

        result = self.run_validator(include_parser_configuration=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("parser configuration capture was not supplied", result.stdout)

    def test_incomplete_results_open_but_false_completeness_claim_fails(self) -> None:
        def remove_case(results: dict[str, Any]) -> None:
            results["complete"] = False
            results["cases"].pop()

        result = self.run_validator(mutate_results=remove_case)
        self.assertEqual(result.returncode, 2)
        self.assertIn("do not cover every corpus case", result.stdout)

        result = self.run_validator(
            mutate_results=lambda value: value["cases"].pop()
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("do not cover every corpus case", result.stdout)

    def test_stale_results_open_and_future_results_fail(self) -> None:
        result = self.run_validator(evaluated_at="2026-07-22T09:00:00.000000001Z")
        self.assertEqual(result.returncode, 2)
        self.assertIn("parser result validity has elapsed", result.stdout)

        result = self.run_validator(
            mutate_results=lambda value: value.update(
                {
                    "capturedAt": "2026-07-21T10:00:00.000000001Z",
                    "validUntil": "2026-07-22T10:00:00Z",
                }
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("captured after the workflow snapshot", result.stdout)

    def test_unsafe_workflow_flags_fail(self) -> None:
        result = self.run_validator(
            mutate_evidence=lambda value: value["flow"].update(
                {"signsExactInputBytes": False}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not sign the exact supplied bytes", result.stdout)

        result = self.run_validator(
            mutate_evidence=lambda value: value["flow"].update(
                {"rejectsOnParseFailure": False}
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("parse failures do not stop", result.stdout)

        result = self.run_validator(
            mutate_evidence=lambda value: value["flow"].update(
                {
                    "parserUsed": None,
                    "signsExactInputBytes": False,
                    "rejectsOnParseFailure": None,
                    "parser": None,
                    "resultsSha256": None,
                }
            ),
            include_results=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not sign the exact supplied bytes", result.stdout)

    def test_no_parser_contradictions_produce_one_failure(self) -> None:
        def contradictory(evidence: dict[str, Any]) -> None:
            evidence["flow"].update(
                {
                    "parserUsed": False,
                    "rejectsOnParseFailure": True,
                }
            )

        result = self.run_validator(mutate_evidence=contradictory)
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        no_parser_checks = [
            check for check in report["checks"] if check["id"] == "flow.noParser"
        ]
        self.assertEqual(len(no_parser_checks), 1)
        self.assertEqual(no_parser_checks[0]["status"], "FAIL")

    def test_strict_json_and_invalid_result_schema_do_not_pass(self) -> None:
        result = self.run_validator(
            raw_results=b'{"schemaVersion":1,"schemaVersion":1}'
        )
        self.assertEqual(result.returncode, 3)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

        evidence_path = self.root / "duplicate-evidence.json"
        evidence_path.write_bytes(b'{"schemaVersion":1,"schemaVersion":1}')
        result = subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                str(evidence_path),
                "--corpus",
                str(CORPUS),
                "--workflow",
                str(self.workflow_capture),
                "--evaluated-at",
                "2026-07-21T12:00:00Z",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 3)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")

        result = self.run_validator(
            mutate_evidence=lambda value: value.update({"schemaVersion": 1.0})
        )
        self.assertEqual(result.returncode, 3)

        result = self.run_validator(
            mutate_results=lambda value: value.update({"schemaVersion": 1.0})
        )
        self.assertEqual(result.returncode, 3)

        oversized_integer = b'{"schemaVersion":' + b"1" * 5000 + b"}"
        result = self.run_validator(raw_results=oversized_integer)
        self.assertEqual(result.returncode, 3)
        self.assertNotIn("Traceback", result.stderr)
        self.assertNotIn("1111111111", result.stdout)

        oversized_evidence_path = self.root / "oversized-integer-evidence.json"
        oversized_evidence_path.write_bytes(oversized_integer)
        result = subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                str(oversized_evidence_path),
                "--corpus",
                str(CORPUS),
                "--evaluated-at",
                "2026-07-21T12:00:00Z",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 3)
        self.assertNotIn("Traceback", result.stderr)
        self.assertNotIn("1111111111", result.stdout)

    def test_noncanonical_corpus_cannot_pass(self) -> None:
        altered_corpus = self.root / "altered-corpus.txt"
        altered_corpus.write_text(
            "accept|empty_object|7b7d\n",
            encoding="utf-8",
        )
        evidence_path = self.root / "altered-corpus-evidence.json"
        evidence_path.write_text(json.dumps(self.evidence()), encoding="utf-8")
        result = subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                str(evidence_path),
                "--corpus",
                str(altered_corpus),
                "--workflow",
                str(self.workflow_capture),
                "--evaluated-at",
                "2026-07-21T12:00:00Z",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 3)
        self.assertEqual(json.loads(result.stdout)["status"], "OPEN")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    global VALIDATOR, CORPUS, WORK_DIR
    VALIDATOR = args.validator.resolve()
    CORPUS = args.corpus.resolve()
    WORK_DIR = args.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(JsonParserConsistencyTests)
    )
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
