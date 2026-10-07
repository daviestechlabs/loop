import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import action_fact_eval as a
from test_action_budget_eval import EVIDENCE, MODEL, TEMPLATE


def frame(case):
    return {
        key: {
            "value": value,
            "quote": "" if value in (None, "unknown") else case["utterance"],
        }
        for key, value in zip(a.FIELDS, case["expected"], strict=True)
    }


class FixtureClient:
    def __init__(self, endpoint):
        pass

    def request(self, path, body=None):
        if path == "/v1/models":
            return MODEL
        text = body["messages"][1]["content"].split("\nPlayer utterance:\n")[1]
        case = next(c for c in a.cases() if c["utterance"] == text)
        return {
            "choices": [
                {
                    "finish_reason": "stop",
                    "message": {"content": json.dumps(frame(case))},
                }
            ]
        }


class FactTests(unittest.TestCase):
    def test_kernel_disagreement_is_not_an_extraction_failure(self):
        case = a.cases()[1]
        row = {
            "response": {
                "choices": [
                    {
                        "finish_reason": "stop",
                        "message": {"content": json.dumps(frame(case))},
                    }
                ]
            }
        }
        with patch.object(a, "execute", return_value=[(3, 1, 1)]):
            with self.assertRaisesRegex(ValueError, "C/oracle disagreement"):
                a.interpretation(row, case, Path("unused-test-binary"))

    def test_quote_validation_is_not_semantic_validation(self):
        case = a.cases()[1]
        payload = frame(case)
        payload["ordinary_action"]["value"] = 0
        values, spans = a.parse(json.dumps(payload), case["utterance"])
        self.assertEqual(values["ordinary_action"], 0)
        self.assertEqual(spans["ordinary_action"]["begin"], 0)
        row = {
            "response": {
                "choices": [
                    {
                        "finish_reason": "stop",
                        "message": {"content": json.dumps(payload)},
                    }
                ]
            }
        }
        result = a.interpretation(row, case)
        self.assertEqual(result["incorrect_fields"], ["ordinary_action"])

    def test_malformed_and_invented_quotes_reject(self):
        case = a.cases()[1]
        for field, value in (
            ("ordinary_action", True),
            ("bonus_action", 2),
            ("surge_available", 1),
            ("earlier_spell", "missing"),
        ):
            payload = frame(case)
            payload[field]["value"] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                a.parse(json.dumps(payload), case["utterance"])
        for quote in ("Not spoken by the player", "", "action"):
            payload = frame(case)
            payload["ordinary_action"]["quote"] = quote
            with self.subTest(quote=quote), self.assertRaises(ValueError):
                a.parse(json.dumps(payload), case["utterance"])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            a.parse('{"ordinary_action":0,"ordinary_action":1}', case["utterance"])

    def test_unknown_feature_alternatives_are_not_added(self):
        values = dict(zip(a.FIELDS, [0, 1, "none", None, None, None], strict=True))
        raw = a.project(values, "two_actions")
        self.assertEqual(raw[3:5], bytes([0, 1]))
        self.assertEqual(a.oracle(raw)[0], 4)
        for available, granted in (
            (False, False),
            (True, True),
            (None, False),
            (True, None),
        ):
            values.update(
                surge_available=available,
                surge_planned=True,
                surge_action_remaining=granted,
            )
            with (
                self.subTest(available=available, granted=granted),
                self.assertRaises(ValueError),
            ):
                a.project(values, "two_actions")

    def test_case_labels_do_not_enter_requests(self):
        self.assertEqual(len(list(a.matrix())), 48)
        for case in a.cases():
            changed = copy.deepcopy(case)
            changed.update(
                id="DO_NOT_SEND_ID", expected=["DO_NOT_SEND_LABEL"] * 6, budget=991
            )
            first = a.request(TEMPLATE, EVIDENCE, case, "basic", 0)
            self.assertEqual(first, a.request(TEMPLATE, EVIDENCE, changed, "basic", 0))
            second = a.request(TEMPLATE, EVIDENCE, case, "revision_aware", 0)
            second["messages"][0]["content"] = second["messages"][0][
                "content"
            ].removesuffix(a.REVISION_INSTRUCTION)
            self.assertEqual(first, second)

    def test_utf8_spans_use_bytes(self):
        case = a.cases()[1]
        text = "Míra: " + case["utterance"]
        values, spans = a.parse(json.dumps(frame(case)), text)
        self.assertEqual(spans["ordinary_action"]["begin"], len("Míra: ".encode()))
        self.assertEqual(values["ordinary_action"], 1)

    def test_full_fixture_run_and_rehashed_interpretation_fault(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            (source / "index.dndsidx").write_bytes(b"fixture")
            template = root / "template.json"
            template.write_text("fixture")
            output = root / "results"
            with (
                patch.object(a.ab, "prepare", return_value=(TEMPLATE, EVIDENCE)),
                patch.object(a, "Client", FixtureClient),
                patch("builtins.print"),
            ):
                a.run("http://127.0.0.1:1", source, template, output)
                self.assertEqual(
                    a.verify(output, source, template)["accepted_extractions"], 48
                )
                rows = [
                    json.loads(line)
                    for line in (output / "trials.jsonl").read_text().splitlines()
                ]
                self.assertTrue(
                    all(
                        not r["interpretation"]["incorrect_fields"]
                        and r["interpretation"]["budget_matches_authored_expectation"]
                        for r in rows
                    )
                )
                rows[0]["interpretation"]["budget_result"][0] = 3
                (output / "trials.jsonl").write_text(
                    "".join(json.dumps(r) + "\n" for r in rows)
                )
                manifest = a.read(output / "manifest.json")
                manifest["files"]["trials.jsonl"] = a.digest(
                    (output / "trials.jsonl").read_bytes()
                )
                (output / "manifest.json").write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, "Interpretation differs"):
                    a.verify(output, source, template)


if __name__ == "__main__":
    unittest.main()
