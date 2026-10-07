import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import budget_projection_eval as b
from test_action_budget_eval import Client, EVIDENCE, TEMPLATE


class ProjectionTests(unittest.TestCase):
    def test_capacity_keeps_feature_use_obligations(self):
        facts = {key: b.facts(raw, key) for key, raw in b.annotations().items()}
        surge = facts["surge"]
        self.assertEqual(surge["ordinary_actions_remaining"], 1)
        self.assertEqual(
            surge["unrestricted_spell_action_capacity_after_planned_features"], [2, 2]
        )
        self.assertEqual(
            surge["planned_feature_uses"],
            [
                {
                    "feature": "Action Surge",
                    "available": True,
                    "uses_to_expend": 1,
                    "additional_unrestricted_actions": 1,
                }
            ],
        )
        self.assertIsNone(facts["unknown"]["planned_feature_uses"])
        self.assertIsNone(facts["unknown"]["ordinary_actions_remaining"])
        self.assertEqual(facts["one_action"]["planned_feature_uses"], [])
        self.assertEqual(
            facts["quickened"]["planned_feature_uses"][0]["sorcery_points_to_expend"], 2
        )

    def test_only_c_result_changes_between_conditions(self):
        for seed, key, question, _ in b.matrix():
            raw = b.annotations()[key]
            result = b.oracle(raw)
            plain = b.request(
                TEMPLATE, EVIDENCE, raw, result, question, b.POLICIES[0], seed, key
            )
            derived = b.request(
                TEMPLATE, EVIDENCE, raw, result, question, b.POLICIES[1], seed, key
            )
            before, tail = derived["messages"][1]["content"].split(
                "\nC action-budget result (derived data):\n"
            )
            _, question_tail = tail.split("\nQuestion: ")
            derived["messages"][1]["content"] = before + "\nQuestion: " + question_tail
            self.assertEqual(derived, plain)
            for _, _, rubric in b.ab.CASES:
                self.assertNotIn(rubric, json.dumps(plain))

    def test_rehashed_annotation_or_verdict_change_rejects(self):
        for fault in ("annotation", "request"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source = root / "source"
                source.mkdir()
                (source / "index.dndsidx").write_bytes(b"fixture")
                template = root / "template.json"
                template.write_text("fixture")
                output = root / "results"
                with (
                    patch.object(b.ab, "prepare", return_value=(TEMPLATE, EVIDENCE)),
                    patch.object(b, "Client", Client),
                    patch("builtins.print"),
                ):
                    b.run("http://127.0.0.1:1", source, template, output)
                    self.assertEqual(
                        b.verify(output, source, template)["verified_trials"], 16
                    )
                    if fault == "annotation":
                        name, expected = "annotations.json", "Annotations changed"
                        value = b.read(output / name)
                        value["unknown"][3:5] = [2, 2]
                        (output / name).write_text(json.dumps(value))
                    else:
                        name, expected = "trials.jsonl", "Request differs"
                        rows = [
                            json.loads(line)
                            for line in (output / name).read_text().splitlines()
                        ]
                        rows[1]["request"]["messages"][1]["content"] = rows[1][
                            "request"
                        ]["messages"][1]["content"].replace(
                            '"decision": "unknown"', '"decision": "fits_action_budget"'
                        )
                        (output / name).write_text(
                            "".join(json.dumps(r) + "\n" for r in rows)
                        )
                    manifest = b.read(output / "manifest.json")
                    manifest["files"][name] = b.digest((output / name).read_bytes())
                    (output / "manifest.json").write_text(json.dumps(manifest))
                    with self.assertRaisesRegex(ValueError, expected):
                        b.verify(output, source, template)


if __name__ == "__main__":
    unittest.main()
