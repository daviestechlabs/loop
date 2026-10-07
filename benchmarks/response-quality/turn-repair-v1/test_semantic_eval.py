import json
import tempfile
import unittest
from pathlib import Path

from compare_semantics import load_run
from semantic_eval import (
    HERE,
    PROMPTS,
    Client,
    assess,
    expected,
    identity,
    model_input,
    sha,
)


class SemanticTests(unittest.TestCase):
    def setUp(self):
        self.cases = json.loads((HERE / "cases.json").read_text())["cases"]

    def test_oracle_is_excluded_from_every_request(self):
        for case in self.cases:
            self.assertEqual(set(model_input(case)), {"utterance", "evidence"})

    def test_correct_state_and_each_corrupted_dimension(self):
        for case in self.cases:
            prediction = {**expected(case), "answer": "Retained for human review."}
            self.assertTrue(assess(case, json.dumps(prediction))["exact"])
            for field in expected(case):
                changed = dict(prediction)
                if field == "commit_requested":
                    changed[field] = not prediction[field]
                elif field == "presence":
                    changed[field] = (
                        "absent" if prediction[field] != "absent" else "present"
                    )
                elif field == "action_state":
                    changed[field] = "held" if prediction[field] != "held" else "none"
                else:
                    changed[field] = (
                        "clarify_entity"
                        if prediction[field] != "clarify_entity"
                        else "resolve_condition"
                    )
                result = assess(case, json.dumps(changed))
                self.assertFalse(result["exact"])
                self.assertIn(field, result["failures"])

    def test_hypothetical_questions_do_not_create_pending_actions(self):
        by_id = {case["case_id"]: case for case in self.cases}
        for case_id in ("room-not-radius", "edition-unset"):
            case = by_id[case_id]
            self.assertNotIn("prepared_action", case["evidence"])
            self.assertEqual(expected(case)["action_state"], "none")
            self.assertFalse(expected(case)["commit_requested"])
        # Actual cast declarations and conditional plans still remain held.
        for case_id in ("repair-present", "conditional-action"):
            self.assertEqual(expected(by_id[case_id])["action_state"], "held")
            self.assertFalse(expected(by_id[case_id])["commit_requested"])
        self.assertEqual(
            expected(by_id["confirmed-action"])["action_state"],
            "eligible_for_governed_commit",
        )
        self.assertTrue(expected(by_id["confirmed-action"])["commit_requested"])

    def test_malformed_outputs_fail(self):
        for text in (
            "",
            "[]",
            "{}",
            "```json\n{}\n```",
            "null",
            '{"commit_requested": 1}',
        ):
            self.assertFalse(assess(self.cases[0], text)["valid"])

    def test_all_refusal_cannot_pass_positive_control(self):
        case = next(c for c in self.cases if c["case_id"] == "confirmed-action")
        prediction = {**expected(case), "commit_requested": False, "answer": "No."}
        self.assertFalse(assess(case, json.dumps(prediction))["primary_correct"])

    def test_receipt_tampering_and_missing_trials_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cases.json").write_bytes((HERE / "cases.json").read_bytes())
            for name in (
                "summary.json",
                "prompts.json",
                "models-before.json",
                "models-after.json",
            ):
                (root / name).write_text("{}")
            (root / "semantic_eval.py").write_text("# fixture source")
            rows = [
                {
                    "case_id": c["case_id"],
                    "condition": p,
                    "repetition": 0,
                    "response": {
                        "choices": [
                            {
                                "finish_reason": "stop",
                                "message": {
                                    "content": json.dumps(
                                        {**expected(c), "answer": "Review separately."}
                                    )
                                },
                            }
                        ]
                    },
                }
                for c in self.cases
                for p in PROMPTS
            ]

            def seal():
                (root / "receipt.json").write_text(
                    json.dumps(
                        {
                            "schema": "waterdeep-semantics-receipt/v1",
                            "files": {
                                p.name: sha(p.read_bytes())
                                for p in root.iterdir()
                                if p.name != "receipt.json"
                            },
                        }
                    )
                )

            (root / "trials.jsonl").write_text("\n".join(json.dumps(r) for r in rows))
            seal()
            self.assertEqual(len(load_run(root)), 36)
            (root / "summary.json").write_text("changed")
            with self.assertRaisesRegex(ValueError, "Changed source evidence"):
                load_run(root)
            (root / "trials.jsonl").write_text(
                "\n".join(json.dumps(r) for r in rows[:-1])
            )
            seal()
            with self.assertRaisesRegex(ValueError, "Incomplete paired matrix"):
                load_run(root)

    def test_presence_is_separate_from_spell_geometry(self):
        case = next(c for c in self.cases if c["case_id"] == "room-not-radius")
        self.assertEqual(expected(case)["presence"], "present")
        self.assertEqual(expected(case)["dialogue_act"], "ask_for_target_geometry")
        for case_id in ("stale-memory", "hidden-location", "late-research"):
            case = next(c for c in self.cases if c["case_id"] == case_id)
            self.assertEqual(expected(case)["presence"], "unknown")

    def test_endpoint_restriction(self):
        for url in (
            "https://127.0.0.1:8000",
            "http://example.org",
            "http://user@127.0.0.1",
            "http://127.0.0.1/path",
        ):
            with self.assertRaises(ValueError):
                Client(url)
        Client("http://127.0.0.1:8000")

    def test_model_creation_timestamp_is_not_weight_identity(self):
        a = {"data": [{"id": "model", "root": "base", "created": 1}]}
        b = {"data": [{"id": "model", "root": "base", "created": 2}]}
        self.assertEqual(identity(a), identity(b))
        b["data"][0]["root"] = "changed"
        self.assertNotEqual(identity(a), identity(b))


if __name__ == "__main__":
    unittest.main()
