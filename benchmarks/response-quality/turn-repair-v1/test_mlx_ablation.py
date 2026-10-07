import copy
import json
import unittest
from collections import Counter

from run_mlx_ablation import SEEDS, VARIANTS, epoch_indices, prepare_ablation
from run_mlx_adapter import ROOT, supervised_tokens
from semantic_eval import PROMPTS


class AblationDesignTests(unittest.TestCase):
    def inputs(self):
        rows = {}
        for split, counts in (("train", (12, 40)), ("validation", (4, 8))):
            rows[split] = [
                {
                    "example_id": f"{prefix}-{split}-{i}",
                    "messages": [
                        {"role": "system", "content": "Original instruction."},
                        {"role": "user", "content": f"{prefix}-{split}-{i}"},
                        {"role": "assistant", "content": "Original answer."},
                    ],
                }
                for prefix, count in zip(("turn", "cohesion"), counts, strict=True)
                for i in range(count)
            ]
        turn_pack = json.loads(
            (
                ROOT / "workflows/qwen-qlora-runner/data/turn-repair-v1/examples.json"
            ).read_bytes()
        )
        files = {
            "evaluation-only/turn-cases.json": json.dumps(
                {
                    "system_prompt": "Original instruction.",
                    "cases": [
                        c for c in turn_pack["cases"] if c["split"] == "evaluation"
                    ],
                }
            ).encode(),
            "evaluation-only/cohesion-cases.json": (
                ROOT
                / "workflows/qwen-qlora-runner/data/cohesion-v1/evaluation-cases.json"
            ).read_bytes(),
        }
        return rows, files

    def test_contract_derivation_preserves_targets_and_prose(self):
        rows, files = self.inputs()
        before = copy.deepcopy(rows)
        derived, cases = prepare_ablation(rows, files)
        self.assertEqual(rows, before)
        for split in rows:
            for original, changed in zip(rows[split], derived[split], strict=True):
                self.assertEqual(original["messages"][1:], changed["messages"][1:])
                expected = (
                    PROMPTS["contract"]
                    if changed["ablation_source"] == "turn"
                    else original["messages"][0]["content"]
                )
                self.assertEqual(changed["messages"][0]["content"], expected)
        self.assertEqual(sum("target" in c for c in cases), 26)
        self.assertEqual(
            {c["system"] for c in cases if "target" in c}, {PROMPTS["contract"]}
        )

    def test_equal_source_exposure_and_filtered_order(self):
        rows, files = self.inputs()
        derived, _ = prepare_ablation(rows, files)
        rows = derived["train"]
        for seed in SEEDS:
            mixed = epoch_indices(rows, "mixed", seed)
            self.assertEqual(Counter(mixed), Counter({i: 2 for i in range(52)}))
            for variant, size in zip(VARIANTS, (24, 80, 104), strict=True):
                observed = epoch_indices(rows, variant, seed)
                wanted = [
                    i
                    for i in mixed
                    if variant == "mixed"
                    or rows[i]["ablation_source"] == variant.split("-")[0]
                ]
                self.assertEqual(observed, wanted)
                self.assertEqual(len(observed), size)
        with self.assertRaises(ValueError):
            epoch_indices(rows, "unreviewed", 0)

    def test_unknown_training_source_is_rejected(self):
        rows, files = self.inputs()
        rows["train"][0]["example_id"] = "unknown-0"
        with self.assertRaisesRegex(ValueError, "Unknown supervised source"):
            prepare_ablation(rows, files)

    def test_longer_contract_requires_explicit_larger_bound(self):
        class Tokenizer:
            def apply_chat_template(self, messages, **kwargs):
                return list(range(513)) if len(messages) == 3 else list(range(450))

        row = {"messages": [{"role": r} for r in ("system", "user", "assistant")]}
        with self.assertRaises(ValueError):
            supervised_tokens(row, Tokenizer())
        self.assertEqual(len(supervised_tokens(row, Tokenizer(), maximum=768)[0]), 513)
        for limit in (0, 1025, True):
            with self.assertRaises(ValueError):
                supervised_tokens(row, Tokenizer(), maximum=limit)


if __name__ == "__main__":
    unittest.main()
