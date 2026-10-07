"""Authored experiment packs stay bounded and cannot redirect evidence paths."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

from dialogue_case_pack import load_pack

HERE = Path(__file__).resolve().parent


class CasePackTests(unittest.TestCase):
    def setUp(self):
        self.pack = json.loads((HERE / "dialogue-challenge-v1.json").read_text())

    def check_pack(self, value):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pack.json"
            path.write_text(json.dumps(value))
            return load_pack(path)

    def test_authored_pack_keeps_twelve_unique_cases(self):
        value = self.check_pack(self.pack)
        self.assertEqual(len(value["cases"]), 12)
        self.assertEqual(
            sum(c["expected_proposal"] == "cleared" for c in value["cases"]), 2
        )

    def test_reject_unsafe_duplicate_and_missing_cases(self):
        for name in (
            "../outside",
            "/absolute",
            "x/y",
            "_",
            "x" * 65,
            "different_referent",
        ):
            bad = copy.deepcopy(self.pack)
            bad["cases"][0]["case_id"] = name
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.check_pack(bad)
        for cases in ([], self.pack["cases"] * 2):
            bad = {**self.pack, "cases": cases}
            with self.assertRaises(ValueError):
                self.check_pack(bad)

    def test_reject_changed_schema_scope_and_labels(self):
        mutations = (
            ("scene", "private-room"),
            ("expected_proposal", "executed"),
            ("message", "\x00"),
            ("message", ""),
            ("message", "é" * 1025),
            ("review_criteria", ""),
        )
        for key, value in mutations:
            bad = copy.deepcopy(self.pack)
            bad["cases"][0][key] = value
            with self.subTest(key=key, value=value[:20]), self.assertRaises(ValueError):
                self.check_pack(bad)
        bad = copy.deepcopy(self.pack)
        bad["cases"][0]["hidden_expected_answer"] = "leaked answer"
        with self.assertRaises(ValueError):
            self.check_pack(bad)
        with self.assertRaises(ValueError):
            self.check_pack({**self.pack, "schema": "unknown"})


if __name__ == "__main__":
    unittest.main()
