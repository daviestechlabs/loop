import copy
import json
from pathlib import Path
import unittest

from probe_output_contract import CONDITIONS, cases_from_pack, score_output, summarize
from semantic_eval import ACTION_STATES, ACTS, FORMAT, PROMPTS

ROOT = Path(__file__).resolve().parents[3]


class OutputContractTests(unittest.TestCase):
    def test_policy_comparison_uses_existing_prompts_and_identical_format(self):
        pack = self.pack()
        vocabulary = cases_from_pack(pack)
        policy = cases_from_pack(pack, "policy")
        for old, new in zip(vocabulary, policy):
            self.assertEqual(old["prompt"], new["prompt"])
            self.assertEqual(old["target"], new["target"])
            self.assertEqual(new["systems"], PROMPTS)
            for system in new["systems"].values():
                self.assertTrue(system.endswith(FORMAT))
        pairs = []
        for case in policy:
            response = json.dumps({**case["target"], "answer": "Fixture."})
            pair = {"case_id": case["case_id"]}
            for condition in ("minimal", "contract"):
                pair[condition] = {**score_output(case["target"], response, "stop"),
                                   "milliseconds": 1, "generation_tokens": 10}
            pairs.append(pair)
        measured = summarize(policy, pairs, ("minimal", "contract"))
        self.assertEqual(measured["contract"]["positive_control_exact"], 1)
        self.assertEqual(measured["minimal"]["exact_state_fields"], 8)
        with self.assertRaises(ValueError):
            summarize(policy, pairs, ("contract", "contract"))
        with self.assertRaises(ValueError):
            cases_from_pack(pack, "tuned-to-case")

    def pack(self):
        source = json.loads((ROOT / "workflows/qwen-qlora-runner/data/turn-repair-v1/examples.json").read_bytes())
        return {"system_prompt": "Interpret this supplied utterance.",
                "cases": [case for case in source["cases"] if case["split"] == "evaluation"]}

    def test_only_enumerations_change_and_oracles_stay_outside_prompts(self):
        pack = self.pack()
        cases = cases_from_pack(pack)
        for source, case in zip(pack["cases"], cases):
            self.assertEqual(json.loads(case["prompt"]), {"utterance": source["utterance"], "evidence": source["evidence"]})
            self.assertEqual(case["systems"]["legacy"], pack["system_prompt"])
            self.assertEqual(case["systems"]["enumerated"], pack["system_prompt"]
                             + " dialogue_act must be one of " + json.dumps(ACTS)
                             + ". action_state must be one of " + json.dumps(ACTION_STATES) + ".")
            self.assertNotIn("answer", case["target"])

    def test_training_rows_duplicates_and_missing_positive_control_reject(self):
        pack = self.pack()
        for broken in ("training", "duplicate", "positive", "missing"):
            candidate = copy.deepcopy(pack)
            if broken == "training":
                candidate["cases"][0]["split"] = "train"
            elif broken == "duplicate":
                candidate["cases"][1]["case_id"] = candidate["cases"][0]["case_id"]
            elif broken == "positive":
                for case in candidate["cases"]:
                    case["target"]["commit_requested"] = False
            else:
                candidate["cases"].pop()
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                cases_from_pack(candidate)

    def test_fenced_or_invented_output_is_not_repaired(self):
        case = cases_from_pack(self.pack())[0]
        correct = {**case["target"], "answer": "The flare stays on hold."}
        fenced = score_output(case["target"], "```json\n" + json.dumps(correct) + "\n```", "stop")
        self.assertFalse(fenced["score"]["valid"])
        self.assertIsNone(fenced["diagnostic"]["commit_requested"])
        invented = {**correct, "dialogue_act": "assert_presence", "commit_requested": True}
        assessed = score_output(case["target"], json.dumps(invented), "stop")
        self.assertFalse(assessed["score"]["valid"])
        self.assertEqual(assessed["diagnostic"]["unknown_labels"], ["dialogue_act"])
        self.assertIs(assessed["diagnostic"]["commit_requested"], True)

    def test_truncated_correct_json_does_not_pass(self):
        case = cases_from_pack(self.pack())[0]
        text = json.dumps({**case["target"], "answer": "The flare stays on hold."})
        self.assertTrue(score_output(case["target"], text, "stop")["score"]["exact"])
        truncated = score_output(case["target"], text, "length")
        self.assertFalse(truncated["score"]["exact"])
        self.assertIn("generation_not_stopped", truncated["score"]["failures"])

    def test_complete_pairs_and_positive_controls_are_required(self):
        cases = cases_from_pack(self.pack())
        pairs = []
        for case in cases:
            pair = {"case_id": case["case_id"]}
            for condition in CONDITIONS:
                response = {**case["target"], "answer": "Reviewed fixture."}
                if condition == "legacy":
                    response["commit_requested"] = False
                    if case["target"]["commit_requested"]:
                        response["action_state"] = "held"
                pair[condition] = {**score_output(case["target"], json.dumps(response), "stop"),
                                   "milliseconds": 1, "generation_tokens": 10}
            pairs.append(pair)
        summary = summarize(cases, pairs)
        self.assertEqual(summary["enumerated"]["exact_state_fields"], 8)
        self.assertEqual(summary["legacy"]["positive_control_exact"], 0)
        self.assertEqual(summary["enumerated"]["positive_control_exact"], 1)
        for broken in (pairs[:-1], list(reversed(pairs)), [pairs[0]] * 8):
            with self.assertRaises(ValueError):
                summarize(cases, broken)


if __name__ == "__main__":
    unittest.main()
