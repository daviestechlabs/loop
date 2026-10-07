import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from probe_scene_intent import PACK, assess, build_parser, load_cases, model_messages, summarize


class SceneIntentStudyTests(unittest.TestCase):
    def setUp(self):
        self.pack = json.loads(PACK.read_bytes())
        self.cases = load_cases(self.pack)

    def test_complete_families_controls_and_target_validity(self):
        self.assertEqual(len(self.cases), 24)
        for fault in ("missing", "duplicate", "family", "lookup", "invented"):
            pack = copy.deepcopy(self.pack)
            if fault == "missing":
                pack["cases"].pop()
            elif fault == "duplicate":
                pack["cases"][1]["case_id"] = pack["cases"][0]["case_id"]
            elif fault == "family":
                pack["cases"][0]["family"] = pack["cases"][-1]["family"]
            elif fault == "lookup":
                pack["cases"][0]["target"] = {"intent": "other", "character_name": "", "proposed_spell": ""}
            else:
                pack["cases"][0]["target"]["character_name"] = "Invented person"
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                load_cases(pack)

    def test_only_utterance_enters_user_prompt(self):
        for case in self.cases:
            messages = model_messages(self.pack, case)
            self.assertEqual(messages[0], {"role": "system", "content": self.pack["system_prompt"]})
            self.assertEqual(json.loads(messages[1]["content"]), {"utterance": case["utterance"]})

    def test_protocol_errors_are_not_repaired_or_counted_as_correct(self):
        case = self.cases[0]
        good = case["target"]
        responses = [
            "```json\n" + json.dumps(good) + "\n```",
            json.dumps({**good, "character_name": "Invented"}),
            json.dumps({**good, "character_name": None}),
            json.dumps({**good, "commit_requested": True}),
            json.dumps({**good, "character_name": "x" * 201}),
            '{"intent":"other","intent":"scene_presence_question","character_name":"Alia","proposed_spell":""}',
        ]
        for response in responses:
            with self.subTest(response=response):
                score = assess(case, response)
                self.assertFalse(score["valid"])
                self.assertFalse(score["exact"])
        self.assertFalse(assess(case, json.dumps(good), "length")["valid"])

    def test_unsupported_lookup_is_visible_even_with_invalid_extra_field(self):
        case = next(c for c in self.cases if c["case_id"] == "silent-garden-geometry")
        prediction = {"intent": "scene_presence_question", "character_name": "Dalen", "proposed_spell": ""}
        score = assess(case, json.dumps(prediction))
        self.assertTrue(score["valid"])
        self.assertFalse(score["exact"])
        self.assertTrue(score["unsupported_lookup_proposal"])
        prediction["execute"] = True
        score = assess(case, json.dumps(prediction))
        self.assertFalse(score["valid"])
        self.assertTrue(score["unsupported_lookup_proposal"])

    def test_universal_decline_and_erased_repairs_cannot_pass(self):
        pairs = []
        for case in self.cases:
            decline = {"intent": "other", "character_name": "", "proposed_spell": ""}
            damaged = {**case["target"], "proposed_spell": ""}
            pairs.append({"case_id": case["case_id"],
                          "c_parser": {"score": assess(case, json.dumps(decline)), "milliseconds": 1},
                          "local_model": {"score": assess(case, json.dumps(damaged)), "milliseconds": 1}})
        counts = summarize(self.cases, pairs)
        self.assertEqual(counts["c_parser"]["exact_declines"], 10)
        self.assertEqual(counts["c_parser"]["exact_lookups"], 0)
        self.assertEqual(counts["local_model"]["exact_repairs"], 0)
        self.assertEqual(counts["local_model"]["repair_controls"], 3)
        for invalid in (pairs[:-1], list(reversed(pairs)), [pairs[0]] * 24):
            with self.assertRaises(ValueError):
                summarize(self.cases, invalid)

    def test_native_probe_preserves_spell_and_rejects_reported_question(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = build_parser(Path(directory))
            for utterance, want in (
                ("I cast Moon Ward, no wait is Dalen here?", {"intent": "scene_presence_question", "character_name": "Dalen", "proposed_spell": "Moon Ward"}),
                ('Dalen asked, "Is Alia here?"', {"intent": "other", "character_name": "", "proposed_spell": ""}),
                ("Is Néria here?", {"intent": "scene_presence_question", "character_name": "Néria", "proposed_spell": ""}),
            ):
                result = subprocess.run([str(binary), utterance], capture_output=True, text=True, check=True)
                self.assertFalse(result.stderr)
                self.assertEqual(json.loads(result.stdout), want)


if __name__ == "__main__":
    unittest.main()
