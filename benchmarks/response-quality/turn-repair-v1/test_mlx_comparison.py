import copy
import json
import tempfile
import unittest
from pathlib import Path

from analyze_mlx_run import load_ablation, load_comparison
from semantic_eval import assess_target, sha


class ComparisonEvidenceTests(unittest.TestCase):
    def fixture(self, root):
        target = {
            "dialogue_act": "answer_scene_question",
            "action_state": "held",
            "presence": "present",
            "commit_requested": False,
        }
        response = json.dumps({**target, "answer": "The action stays on hold."})
        result = {
            "response": response,
            "response_sha256": sha(response.encode()),
            "finish_reason": "stop",
            "score": assess_target(target, response),
        }
        cases = [
            {"case_id": str(i), "group": "fixture", "target": target} for i in range(46)
        ]
        pairs = [
            {
                "case_id": str(i),
                "group": "fixture",
                "base": copy.deepcopy(result),
                "adapter": copy.deepcopy(result),
            }
            for i in range(46)
        ]
        (root / "evaluation-cases.json").write_text(json.dumps(cases))
        (root / "pairs.jsonl").write_text("\n".join(json.dumps(p) for p in pairs))
        (root / "recipe.json").write_text("{}")
        (root / "report.json").write_text("{}")
        self.seal(root)
        return pairs

    def seal(self, root):
        (root / "receipt.json").write_text(
            json.dumps(
                {
                    "schema": "waterdeep-ram-qlora-receipt/v1",
                    "files": {
                        p.name: sha(p.read_bytes())
                        for p in root.iterdir()
                        if p.name != "receipt.json"
                    },
                }
            )
        )

    def test_changed_response_bytes_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            self.assertEqual(len(load_comparison(root)[1]), 46)
            with (root / "pairs.jsonl").open("a") as stream:
                stream.write("changed")
            with self.assertRaisesRegex(ValueError, "Changed local adapter evidence"):
                load_comparison(root)

    def test_missing_or_duplicate_pairs_are_rejected_after_resealing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pairs = self.fixture(root)
            for changed in (pairs[:-1], pairs[:-1] + pairs[:1]):
                (root / "pairs.jsonl").write_text(
                    "\n".join(json.dumps(p) for p in changed)
                )
                self.seal(root)
                with self.assertRaisesRegex(ValueError, "Incomplete paired comparison"):
                    load_comparison(root)

    def test_stale_score_cannot_survive_rescoring(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pairs = self.fixture(root)
            pairs[0]["adapter"]["score"]["exact"] = False
            (root / "pairs.jsonl").write_text("\n".join(json.dumps(p) for p in pairs))
            self.seal(root)
            with self.assertRaisesRegex(ValueError, "Changed model score"):
                load_comparison(root)

    def test_ablation_requires_each_condition_once_per_case(self):
        from run_mlx_ablation import SEEDS, VARIANTS

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pairs = self.fixture(root)
            conditions = ["base"] + [
                f"{variant}/seed-{seed}" for seed in SEEDS for variant in VARIANTS
            ]
            rows = [
                {
                    "case_id": pair["case_id"],
                    "group": pair["group"],
                    "condition": condition,
                    **copy.deepcopy(pair["base"]),
                }
                for pair in pairs
                for condition in conditions
            ]
            (root / "recipe.json").write_text(json.dumps({"conditions": conditions}))

            def seal_rows(values):
                (root / "trials.jsonl").write_text(
                    "\n".join(json.dumps(row) for row in values)
                )
                self.seal(root)
                receipt = json.loads((root / "receipt.json").read_bytes())
                receipt["schema"] = "waterdeep-mlx-ablation-receipt/v1"
                (root / "receipt.json").write_text(json.dumps(receipt))

            seal_rows(rows)
            self.assertEqual(len(load_ablation(root)[1]), 46)
            for changed in (rows[:-1], rows[:-1] + rows[:1]):
                seal_rows(changed)
                with self.assertRaisesRegex(ValueError, "Incomplete ablation matrix"):
                    load_ablation(root)
            seal_rows(rows)
            (root / "recipe.json").write_text(
                json.dumps({"conditions": conditions[:-1]})
            )
            seal_rows(rows)
            with self.assertRaisesRegex(ValueError, "Unexpected ablation conditions"):
                load_ablation(root)


if __name__ == "__main__":
    unittest.main()
