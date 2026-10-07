"""Freeze the paired input intervention and test its C admission boundary."""

import os
import subprocess
import tempfile
from pathlib import Path
import unittest

import remainder_context_eval as experiment


class RemainderTests(unittest.TestCase):
    def test_matrix_preserves_both_conditions_and_seeds(self):
        rows = list(experiment.matrix())
        self.assertEqual(len(rows), 42)
        self.assertEqual(len(set(rows)), 42)
        for seed in experiment.SEEDS:
            for case_id, *_ in experiment.CASES:
                self.assertEqual(
                    {r[3] for r in rows if r[:2] == (seed, case_id)},
                    set(experiment.CONDITIONS),
                )

    def test_only_current_input_changes_and_controls_are_identical(self):
        template = {
            "model": "default",
            "temperature": 0.2,
            "stream": True,
            "messages": [
                {"role": "system", "content": "unchanged"},
                {"role": "user", "content": "old"},
            ],
        }
        for case_id, _message, _held, remainder, _review in experiment.CASES:
            contexts = {
                c: experiment.expected_context(case_id, c)
                for c in experiment.CONDITIONS
            }
            a, b = (
                experiment.request(template, case_id, c, 1, contexts)
                for c in experiment.CONDITIONS
            )
            ca, cb = (x["messages"][1]["content"] for x in (a, b))
            self.assertEqual(
                ca.split("\nCurrent input:\n")[0], cb.split("\nCurrent input:\n")[0]
            )
            if remainder is None:
                self.assertEqual(a, b)
            else:
                self.assertNotEqual(ca, cb)
                self.assertTrue(cb.endswith("\nCurrent input:\n" + remainder))
            b["messages"][1] = a["messages"][1]
            self.assertEqual(a, b)
            with self.assertRaisesRegex(ValueError, "projection"):
                experiment.request(
                    template,
                    case_id,
                    "full_input",
                    1,
                    {"full_input": "invented context"},
                )
        self.assertEqual(template["messages"][1]["content"], "old")
        self.assertTrue(template["stream"])

    def test_actual_c_projection_preserves_control_and_question_bytes(self):
        common = experiment.ROOT / "voice/c-runtime/common"
        product = experiment.ROOT / "product/companions-frontend/c-companions"
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "projection"
            environment = dict(
                os.environ,
                ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                UBSAN_OPTIONS="halt_on_error=1",
            )
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I" + str(common),
                    "-I" + str(product),
                    str(experiment.HERE / "remainder_context.c"),
                    str(common / "dnd_dialogue.c"),
                    str(common / "utf8.c"),
                    str(product / "cmp_json.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
            )

            def project(message, held, condition):
                return subprocess.check_output(
                    [str(binary), str(held), condition, message],
                    text=True,
                    env=environment,
                )

            for case_id, message, held, _remainder, _review in experiment.CASES:
                for condition in experiment.CONDITIONS:
                    self.assertEqual(
                        project(message, held, condition),
                        experiment.expected_context(case_id, condition),
                    )
            for message in (
                "Drop it. What next?",
                "Cancel that spell and cast another.",
                "Forget the spellbook. What next?",
                "'Forget the spell.' What does that mean?",
                "Don't forget the spell. Could we talk?",
                "If Mira returns, forget the spell. What next?",
            ):
                self.assertEqual(
                    project(message, 0, "full_input"),
                    project(message, 0, "remaining_question"),
                )
            message = "  PLEASE\tforget the spell;\n  Could Élara — or Mira — talk? Then ask the guard.\n"
            result = project(message, 1, "remaining_question")
            self.assertTrue(
                result.endswith(
                    "\nCurrent input:\nCould Élara — or Mira — talk? Then ask the guard.\n"
                )
            )
            result = subprocess.run(
                [str(binary), "1", "remaining_question", "x" * 5000],
                capture_output=True,
                env=environment,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
