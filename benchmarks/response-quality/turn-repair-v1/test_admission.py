import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from admit_model_outputs import admission_input, execute
from semantic_eval import HERE


class AdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "admission"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                str(HERE / "admission_main.c"),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            timeout=60,
        )
        cls.positive = [1, 1, 1, 1, 1, 42, 42, 7, 7, 9, 1, 1, 1, 0]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_confirmed_owner_commits_exactly_once_despite_retry(self):
        self.assertEqual(execute(self.binary, self.positive)["new_commits"], 1)

    def test_every_missing_prerequisite_and_mismatched_identity_blocks(self):
        for index, value in [(i, 0) for i in range(13)] + [
            (4, 2),
            (6, 43),
            (8, 8),
            (13, 1),
        ]:
            values = list(self.positive)
            values[index] = value
            with self.subTest(index=index, value=value):
                self.assertEqual(execute(self.binary, values)["new_commits"], 0)

    def test_malformed_wire_rejects_without_output(self):
        for values in [
            [],
            [*self.positive, 1],
            [-1, *self.positive[1:]],
            [2, *self.positive[1:]],
            ["1x", *self.positive[1:]],
        ]:
            result = subprocess.run(
                [str(self.binary), *map(str, values)],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")

    def test_model_claim_cannot_replace_fixture_authority(self):
        cases = json.loads((HERE / "cases.json").read_text())["cases"]
        affirmative = {
            "valid": True,
            "prediction": {
                "commit_requested": True,
                "action_state": "eligible_for_governed_commit",
            },
        }
        for case in cases:
            result = execute(self.binary, admission_input(case, affirmative))
            self.assertEqual(
                result["new_commits"], case["expected"]["maximum_new_commits"]
            )
        positive = next(c for c in cases if c["case_id"] == "confirmed-action")
        self.assertEqual(
            execute(self.binary, admission_input(positive, {"valid": False}))[
                "new_commits"
            ],
            0,
        )


if __name__ == "__main__":
    unittest.main()
