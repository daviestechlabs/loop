"""Check the paired intervention and the actual C empty-state projection."""

import subprocess
import tempfile
from pathlib import Path
import unittest

import empty_context_eval as experiment


class EmptyContextTests(unittest.TestCase):
    def test_each_case_has_both_conditions_at_each_seed(self):
        rows = list(experiment.matrix())
        self.assertEqual(len(rows), 48)
        self.assertEqual(len(set(rows)), 48)
        for seed in experiment.SEEDS:
            for case_id, _, _ in experiment.CASES:
                self.assertEqual(
                    {r[3] for r in rows if r[:2] == (seed, case_id)},
                    set(experiment.CONDITIONS),
                )

    def test_intervention_only_changes_user_context(self):
        template = {
            "model": "default",
            "temperature": 0.2,
            "stream": True,
            "messages": [
                {"role": "system", "content": "fixed guidance"},
                {"role": "user", "content": "prior captured question"},
            ],
        }
        message = "Did you cast it?"
        context = experiment.PREFIX + message
        a = experiment.request(template, message, "absent", 1, context)
        b = experiment.request(template, message, "explicit_empty", 1, context)
        self.assertEqual(a["messages"][1]["content"], message)
        self.assertEqual(b["messages"][1]["content"], context)
        b["messages"][1] = a["messages"][1]
        self.assertEqual(a, b)
        self.assertEqual(template["messages"][1]["content"], "prior captured question")
        self.assertTrue(template["stream"])
        with self.assertRaisesRegex(ValueError, "projection"):
            experiment.request(
                template,
                message,
                "explicit_empty",
                1,
                context.replace('"held_spell":""', '"held_spell":"fireball"'),
            )

    def test_projection_uses_empty_c_state_and_preserves_input(self):
        root = experiment.ROOT
        common = root / "voice/c-runtime/common"
        product = root / "product/companions-frontend/c-companions"
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "projection"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I" + str(common),
                    "-I" + str(product),
                    str(experiment.HERE / "empty_context.c"),
                    str(common / "dnd_dialogue.c"),
                    str(common / "utf8.c"),
                    str(product / "cmp_json.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
            )
            for message in (
                "What about Éowyn?",
                'Mira said "cancel it."',
                "Help the guard.",
            ):
                self.assertEqual(
                    subprocess.check_output([str(binary), message], text=True),
                    experiment.PREFIX + message,
                )
            result = subprocess.run([str(binary), "x" * 5000], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
