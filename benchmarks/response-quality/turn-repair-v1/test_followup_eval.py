import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from followup_eval import CONDITIONS, HERE, request, run, score
from semantic_eval import sha
from verify_followup import verify


class FollowupTest(unittest.TestCase):
    def setUp(self):
        self.cases = json.loads((HERE / "followup-cases.json").read_text())["cases"]

    def test_oracle_and_review_do_not_enter_request(self):
        for case in self.cases:
            changed = copy.deepcopy(case)
            changed.update(
                case_id="sentinel",
                expected={"sentinel": True},
                answer_review="sentinel",
            )
            for condition in CONDITIONS:
                self.assertEqual(
                    request(case, condition, "model"),
                    request(changed, condition, "model"),
                )

    def test_only_prior_turn_changes_between_conditions(self):
        for case in self.cases:
            empty = request(case, "current_only", "model")
            prior = request(case, "prior_turn", "model")
            self.assertEqual(len(empty["messages"]), 2)
            self.assertEqual(len(prior["messages"]), 4)
            self.assertEqual(
                empty["messages"], [prior["messages"][0], prior["messages"][-1]]
            )
            self.assertEqual(
                {k: v for k, v in empty.items() if k != "messages"},
                {k: v for k, v in prior.items() if k != "messages"},
            )

    def test_foreign_condition_rejects(self):
        with self.assertRaises(ValueError):
            request(self.cases[0], "unknown", "model")

    def test_controls_and_truncation(self):
        self.assertEqual(sum(c["expected"]["commit_requested"] for c in self.cases), 1)
        for case in self.cases:
            reply = {
                "choices": [
                    {
                        "finish_reason": "stop",
                        "message": {
                            "content": json.dumps(
                                {**case["expected"], "answer": "fixture"}
                            )
                        },
                    }
                ]
            }
            self.assertTrue(score(case, reply)["primary_correct"])
            reply["choices"][0]["finish_reason"] = "length"
            self.assertFalse(score(case, reply)["valid"])
            self.assertFalse(score(case, reply)["primary_correct"])

    def test_malformed_or_unsafe_output_cannot_pass(self):
        case = self.cases[0]
        reply = {"choices": [{"finish_reason": "stop", "message": {"content": "yes"}}]}
        self.assertFalse(score(case, reply)["valid"])
        reply["choices"][0]["message"]["content"] = json.dumps(
            {**case["expected"], "commit_requested": True, "answer": "Casting now."}
        )
        result = score(case, reply)
        self.assertTrue(result["unsafe_commit_proposal"])
        self.assertFalse(result["primary_correct"])

    def test_retained_bundle_rejects_changed_inputs_and_missing_pairs(self):
        cases = {c["current"]["utterance"]: c for c in self.cases}

        class FixtureClient:
            def __init__(self, endpoint):
                pass

            def request(self, path, body=None):
                if path == "/v1/models":
                    return {"data": [{"id": "fixture"}]}
                case = cases[json.loads(body["messages"][-1]["content"])["utterance"]]
                return {
                    "choices": [
                        {
                            "finish_reason": "stop",
                            "message": {
                                "content": json.dumps(
                                    {**case["expected"], "answer": "fixture"}
                                )
                            },
                        }
                    ]
                }

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "run"
            with patch("followup_eval.Client", FixtureClient), patch("builtins.print"):
                run("http://127.0.0.1:1", "fixture", root)
            self.assertEqual(verify(root)["verified_trials"], 14)
            original = (root / "trials.jsonl").read_bytes()
            for mutation in (
                "missing_pair",
                "wrong_input",
                "wrong_score",
                "wrong_order",
            ):
                rows = [json.loads(line) for line in original.splitlines()]
                if mutation == "missing_pair":
                    rows.pop()
                elif mutation == "wrong_input":
                    rows[0]["request"]["messages"][-1]["content"] = "oracle leaked"
                elif mutation == "wrong_score":
                    rows[0]["score"]["primary_correct"] = False
                else:
                    rows.reverse()
                (root / "trials.jsonl").write_text(
                    "".join(json.dumps(r) + "\n" for r in rows)
                )
                receipt = json.loads((root / "receipt.json").read_text())
                receipt["files"]["trials.jsonl"] = sha(
                    (root / "trials.jsonl").read_bytes()
                )
                (root / "receipt.json").write_text(json.dumps(receipt))
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    verify(root)


if __name__ == "__main__":
    unittest.main()
