import collections
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import action_budget_eval as a


TEMPLATE = {
    "model": "test",
    "temperature": 0.2,
    "max_completion_tokens": 256,
    "messages": [
        {"role": "system", "content": "Use evidence."},
        {"role": "user", "content": "Original question."},
    ],
}
EVIDENCE = {
    "spells": [{"document": ["doc"], "name": "cost", "content": "Fixture spell cost."}],
    "rules": [{"document": ["doc"], "name": "limit", "content": "Fixture rule limit."}],
}
MODEL = {"data": [{"id": "test", "root": "pinned-test", "max_model_len": 8192}]}


class Client:
    def __init__(self, endpoint):
        self.calls = 0

    def request(self, path, body=None):
        if path == "/v1/models":
            return MODEL
        assert path == "/v1/chat/completions"
        self.calls += 1
        return {
            "choices": [
                {"finish_reason": "stop", "message": {"content": "Test answer."}}
            ]
        }


class ActionBudgetTests(unittest.TestCase):
    def test_counterbalance_and_rubric_isolation(self):
        matrix = list(a.matrix())
        self.assertEqual(len(matrix), 32)
        counts = collections.Counter(
            (case, coverage, policy) for _, case, _, coverage, policy in matrix
        )
        self.assertEqual(set(counts.values()), {2})
        for seed, case, question, coverage, policy in matrix:
            body = a.request(TEMPLATE, EVIDENCE, question, coverage, policy, seed)
            text = json.dumps(body)
            self.assertNotIn('"rubric"', text)
            for _, _, rubric in a.CASES:
                self.assertNotIn(rubric, text)
        self.assertEqual(TEMPLATE["messages"][1]["content"], "Original question.")

    def test_independent_interventions(self):
        base = a.request(TEMPLATE, EVIDENCE, "Question", "spells", "current", 0)
        rules = a.request(TEMPLATE, EVIDENCE, "Question", "rules", "current", 0)
        policy = a.request(TEMPLATE, EVIDENCE, "Question", "spells", "checklist", 0)
        self.assertEqual(base["messages"][0], rules["messages"][0])
        self.assertEqual(base["messages"][1], policy["messages"][1])
        self.assertNotIn("Fixture rule limit.", base["messages"][1]["content"])
        self.assertIn("Fixture rule limit.", rules["messages"][1]["content"])
        for other in (rules, policy):
            self.assertEqual(
                {k: v for k, v in base.items() if k != "messages"},
                {k: v for k, v in other.items() if k != "messages"},
            )

    def test_rehashed_tampering_rejects(self):
        for fault, expected in (
            ("request", "Request differs"),
            ("missing_trial", "Trial set differs"),
            ("model", "Model catalog changed"),
            ("source", "Source identity differs"),
            ("template", "Template identity differs"),
        ):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source = root / "source"
                source.mkdir()
                (source / "index.dndsidx").write_bytes(b"fixture")
                template = root / "template.json"
                template.write_text("fixture")
                output = root / "result"
                with (
                    patch.object(a, "prepare", return_value=(TEMPLATE, EVIDENCE)),
                    patch.object(a, "Client", Client),
                ):
                    a.run("http://127.0.0.1:1", source, template, output)
                    self.assertEqual(
                        a.verify(output, source, template)["verified_trials"], 32
                    )
                    name = "trials.jsonl"
                    if fault in ("request", "missing_trial"):
                        rows = [
                            json.loads(line)
                            for line in (output / name).read_text().splitlines()
                        ]
                        if fault == "request":
                            rows[0]["request"]["messages"][1]["content"] += (
                                " Hidden oracle."
                            )
                        else:
                            rows.pop()
                        (output / name).write_text(
                            "".join(json.dumps(r) + "\n" for r in rows)
                        )
                    elif fault == "model":
                        name = "models-after.json"
                        (output / name).write_text(
                            json.dumps({"data": [{"id": "changed"}]})
                        )
                    elif fault == "source":
                        (source / "index.dndsidx").write_bytes(b"changed")
                    else:
                        template.write_text("changed")
                    manifest = a.read(output / "manifest.json")
                    manifest["files"][name] = a.digest((output / name).read_bytes())
                    (output / "manifest.json").write_text(json.dumps(manifest))
                    with self.assertRaisesRegex(ValueError, expected):
                        a.verify(output, source, template)


if __name__ == "__main__":
    unittest.main()
