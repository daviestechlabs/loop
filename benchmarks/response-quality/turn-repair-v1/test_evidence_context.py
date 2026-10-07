import copy
import tempfile
import subprocess
import unittest
from pathlib import Path
from evidence_context import summarize

class EvidenceContextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "admission"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(Path(__file__).with_name("admission_main.c")),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_presence_needs_visibility_and_current_revision(self):
        e = {"presence": "present", "visible_to_player": True, "scene_revision": 2}
        self.assertEqual(summarize(e, self.binary)["assertable_presence"], "present")
        for altered in ({**e, "visible_to_player": False},
                        {**e, "current_turn_revision": 3, "result_turn_revision": 2},
                        {**e, "candidates": ["one", "two"]}):
            self.assertEqual(summarize(altered, self.binary)["assertable_presence"], "unknown")

    def test_presence_cannot_outlive_its_scene_revision(self):
        evidence = {
            "presence": "present", "visible_to_player": True,
            "scene_revision": 2, "current_scene_revision": 3,
        }
        self.assertEqual(summarize(evidence, self.binary)["assertable_presence"], "unknown")
        evidence["scene_revision"] = 3
        self.assertEqual(summarize(evidence, self.binary)["assertable_presence"], "present")
        del evidence["scene_revision"]
        self.assertEqual(summarize(evidence, self.binary)["assertable_presence"], "unknown")

    def test_commit_requires_all_fixture_prerequisites(self):
        e = dict(action_owner="a", speaker="a", action_state="prepared",
                 prepared_action="spell", operation_id="op", scene_revision=2,
                 prepared_scene_revision=2, explicit_confirmation=True,
                 verified_target=True, executor_prerequisites_passed=True)
        before = copy.deepcopy(e)
        self.assertTrue(summarize(e, self.binary)["mock_executor_would_allow_new_commit"])
        self.assertEqual(e, before)
        for changed in ({"speaker": "b"}, {"scene_revision": 3},
                        {"existing_receipt": "done"}, {"verified_target": False},
                        {"current_turn_revision": 3, "result_turn_revision": 2}):
            self.assertFalse(summarize({**e, **changed}, self.binary)["mock_executor_would_allow_new_commit"])

    def test_malformed_revisions_fail(self):
        for value in (True, -1, "1", None):
            with self.assertRaises(ValueError):
                summarize({"current_turn_revision": value}, self.binary)

    def test_missing_evidence_grants_nothing(self):
        s = summarize({}, self.binary)
        self.assertEqual(s["assertable_presence"], "unknown")
        self.assertFalse(s["mock_executor_would_allow_new_commit"])
