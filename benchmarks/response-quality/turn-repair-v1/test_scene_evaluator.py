"""Exercise the real C parser and reject malformed evaluation inputs."""

import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

PACK = Path(__file__).resolve().parent
ROOT = PACK.parents[2]


class SceneEvaluatorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="waterdeep-scene-")
        cls.root = Path(cls.work.name)
        cls.binary = cls.root / "evaluator"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                *[
                    "-I" + str(ROOT / p)
                    for p in [
                        "voice/c-runtime/common",
                        "voice/c-runtime/wire",
                        "product/companions-frontend/c-companions",
                        "contracts/handler-base/c-pb",
                    ]
                ],
                str(PACK / "scene_evaluator.c"),
                *[
                    str(ROOT / p)
                    for p in [
                        "voice/c-runtime/common/dnd_scene.c",
                        "voice/c-runtime/common/utf8.c",
                        "product/companions-frontend/c-companions/cmp_json.c",
                    ]
                ],
                "-o",
                str(cls.binary),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, dataset, profile=None):
        (self.root / "dataset.json").write_text(
            dataset if isinstance(dataset, str) else json.dumps(dataset)
        )
        (self.root / "profile.json").write_text(
            json.dumps(profile)
            if profile
            else (PACK / "scene-profile.json").read_text()
        )
        return subprocess.run(
            [
                str(self.binary),
                "--dataset",
                str(self.root / "dataset.json"),
                "--profile",
                str(self.root / "profile.json"),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_production_parser_and_failure_observation(self):
        data = json.loads((PACK / "scene-cases.json").read_text())
        result = self.run_case(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        cases = json.loads(result.stdout)["cases"]
        self.assertEqual(len(cases), 10)
        self.assertTrue(all(c["passed"] for c in cases))
        # A wrong oracle must fail semantically, without crashing or hiding the observation.
        data["cases"][0]["name"] = "Eli"
        result = self.run_case(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        case = json.loads(result.stdout)["cases"][0]
        self.assertFalse(case["passed"])
        self.assertEqual(
            json.loads(case["output"]), {"recognized": True, "name": "Mira"}
        )

    def test_ambiguity_regressions(self):
        data = json.loads((PACK / "scene-ambiguity-cases.json").read_text())
        result = self.run_case(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        for case in json.loads(result.stdout)["cases"]:
            with self.subTest(case=case["id"]):
                self.assertTrue(case["passed"], case)

    def test_intent_profile_preserves_proposals_and_separates_rejected_grammar(self):
        data = json.loads((PACK / "scene-intent-cases.json").read_text())
        profile = json.loads((PACK / "scene-intent-profile.json").read_text())
        result = self.run_case(data, profile)
        self.assertEqual(result.returncode, 0, result.stderr)
        cases = json.loads(result.stdout)["cases"]
        self.assertEqual(len(cases), 20)
        self.assertTrue(all(c["passed"] for c in cases), cases)
        self.assertEqual(json.loads(cases[0]["output"])["proposed_spell"], "fireball")
        data["cases"][0]["proposed_spell"] = "Polymorph"
        changed = json.loads(self.run_case(data, profile).stdout)["cases"][0]
        self.assertFalse(changed["passed"])
        self.assertEqual(json.loads(changed["output"])["proposed_spell"], "fireball")

    def test_profile_versions_cannot_mix_or_omit_intent_expectations(self):
        data = json.loads((PACK / "scene-intent-cases.json").read_text())
        profile = json.loads((PACK / "scene-intent-profile.json").read_text())
        self.assertEqual(self.run_case(data).returncode, 2)
        old = json.loads((PACK / "scene-cases.json").read_text())
        self.assertEqual(self.run_case(old, profile).returncode, 2)
        for variant in ("missing", "extra", "rejected-proposal"):
            changed = copy.deepcopy(data)
            if variant == "missing":
                del changed["cases"][0]["proposed_spell"]
            elif variant == "extra":
                changed["cases"][0]["execute"] = True
            else:
                changed["cases"][-1]["proposed_spell"] = "Fireball"
            self.assertEqual(self.run_case(changed, profile).returncode, 2)

    def test_intent_profile_detects_compiled_proposal_loss(self):
        source = (ROOT / "voice/c-runtime/common/dnd_scene.c").read_text()
        needle = "memcpy(intent->proposed_spell, spell, strlen(spell) + 1u);"
        self.assertEqual(source.count(needle), 1)
        mutant = self.root / "dnd_scene_mutant.c"
        mutant.write_text(source.replace(needle, "intent->proposed_spell[0] = '\\0';"))
        binary = self.root / "mutant"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                *[
                    "-I" + str(ROOT / p)
                    for p in [
                        "voice/c-runtime/common",
                        "voice/c-runtime/wire",
                        "product/companions-frontend/c-companions",
                        "contracts/handler-base/c-pb",
                    ]
                ],
                str(PACK / "scene_evaluator.c"),
                str(mutant),
                str(ROOT / "voice/c-runtime/common/utf8.c"),
                str(ROOT / "product/companions-frontend/c-companions/cmp_json.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        old_binary = self.binary
        self.binary = binary
        try:
            old = self.run_case(json.loads((PACK / "scene-cases.json").read_text()))
            self.assertEqual(old.returncode, 0, old.stderr)
            self.assertTrue(all(c["passed"] for c in json.loads(old.stdout)["cases"]))
            result = self.run_case(
                json.loads((PACK / "scene-intent-cases.json").read_text()),
                json.loads((PACK / "scene-intent-profile.json").read_text()),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            failures = [
                c["id"] for c in json.loads(result.stdout)["cases"] if not c["passed"]
            ]
            self.assertEqual(
                failures,
                [
                    "fireball-repair",
                    "period-repair",
                    "repair-chain",
                    "multiword-spell",
                    "polymorph-repair",
                ],
            )
        finally:
            self.binary = old_binary

    def test_repair_chain_and_label_boundaries(self):
        rows = [
            (
                "chain",
                "I cast fireball, no wait is Mira here, no wait is Eli here?",
                True,
                "Eli",
            ),
            (
                "cast-label",
                "I cast magic missile, wait is Lady Mira here?",
                True,
                "Lady Mira",
            ),
            ("apostrophe", "Is Nar'l here?", True, "Nar'l"),
            ("hyphen", "Is Fel-rekt here?", True, "Fel-rekt"),
            ("operator-substring", "Is Nori here?", True, "Nori"),
            (
                "conditional-cast",
                "I cast fireball if needed, wait is Mira here?",
                False,
                "",
            ),
            ("reported-cast", "He says I cast fireball, wait is Mira here?", False, ""),
            ("quoted-cast", 'I cast "fireball, wait is Mira here?', False, ""),
            ("compound", "Is Mira or Eli here?", False, ""),
        ]
        data = {
            "schema": "waterdeep-scene-cases/v1",
            "cases": [
                dict(id=i, utterance=u, recognized=r, name=n) for i, u, r, n in rows
            ],
        }
        result = self.run_case(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        for case in json.loads(result.stdout)["cases"]:
            with self.subTest(case=case["id"]):
                self.assertTrue(case["passed"], case)

    def test_bad_inputs_produce_no_partial_result(self):
        original = json.loads((PACK / "scene-cases.json").read_text())
        variants = []
        for change in [
            lambda d: d["cases"].append(d["cases"][0]),
            lambda d: d.update(cases=[]),
            lambda d: d["cases"][1].update(command="shell"),
            lambda d: d["cases"][1].update(recognized="true"),
            lambda d: d["cases"][1].update(utterance="x" * 2048),
            lambda d: d["cases"][1].update(id='bad"id'),
        ]:
            data = copy.deepcopy(original)
            change(data)
            variants.append(json.dumps(data))
        variants += [
            '{"schema":"waterdeep-scene-cases/v1","schema":"other","cases":[]}',
            json.dumps(original) + " trailing",
            json.dumps(original) + "\x00",
            "x" * 1048576,
        ]
        for value in variants:
            with self.subTest(value=value[:80]):
                result = self.run_case(value)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertEqual(result.stderr, "")
        result = self.run_case(
            original, {"schema": "waterdeep-scene-profile/v1", "scope": "model-quality"}
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
