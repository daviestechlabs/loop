import copy
import hashlib
import json
from pathlib import Path
import unittest

from validate_cast_event_data import validate


class DevelopmentDataTests(unittest.TestCase):
    def setUp(self):
        labels = dict(actor="speaker", time="this_turn", status="occurred", spell="fireball", modifier="none")
        self.conditions = []
        self.cases = []
        for ident, status, phrase in (("a", "occurred", "I completed"), ("b", "proposed", "I am planning")):
            expected = labels | {"status": status}
            text = phrase + " Fireball during this turn, with no casting modifiers. I mean my own character; can you record that claim for this example?"
            self.conditions.append(dict(id=ident, family="status", contrast_field="status", expected=expected))
            self.cases.append(dict(id=ident, family="status", utterance=text, expected=expected.copy(),
                                   quotes=dict(actor="my own character", time="this turn", status=phrase,
                                               spell="Fireball", modifier="no casting modifiers"), rationale="Authored fixture."))

    def test_valid_pair(self):
        result = validate(self.cases, self.conditions)
        self.assertEqual(result["families"], 1)
        self.assertFalse(result["training_admitted"])

    def test_committed_pack_and_generation_provenance(self):
        root = Path(__file__).parent / "synthetic/cast-events-20260928"
        data = json.loads((root / "cases.json").read_text())
        review = json.loads((root / "review.json").read_text())
        result = validate(data["cases"], json.loads((root / "conditions.json").read_text()))
        self.assertEqual(result["cases"], 8)
        self.assertEqual(data["scope"], "development")
        self.assertFalse(data["training_admitted"])
        self.assertEqual(data["author"], "Codex")
        self.assertEqual(data["grok_generated_cases"], review["grok_proposals_accepted"])
        self.assertEqual(review["grok_proposals_accepted"], 0)
        for name, expected in review["files"].items():
            self.assertEqual(hashlib.sha256((root / name).read_bytes()).hexdigest(), expected)

    def test_duplicate_missing_quote_and_wrong_target_reject(self):
        for change in (lambda c: c.append(copy.deepcopy(c[0])),
                       lambda c: c[0]["quotes"].update(spell="absent"),
                       lambda c: c[0]["quotes"].update(spell=""),
                       lambda c: c[0]["expected"].update(actor="other")):
            cases = copy.deepcopy(self.cases)
            change(cases)
            with self.assertRaises(ValueError):
                validate(cases, self.conditions)

    def test_relabeling_both_case_and_condition_cannot_hide_extra_contrast(self):
        self.cases[0]["expected"]["actor"] = "other"
        self.conditions[0]["expected"]["actor"] = "other"
        with self.assertRaisesRegex(ValueError, "unintended labels"):
            validate(self.cases, self.conditions)


if __name__ == "__main__":
    unittest.main()
