"""Future plans must not become interrupted action proposals."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from probe_scene_intent import build_parser


class BeforeCastTests(unittest.TestCase):
    def test_previous_pack_is_preserved_and_only_the_declared_label_changes(self):
        here = Path(__file__).resolve().parent
        raw = (here / "scene-before-cast-cases.json").read_bytes()
        previous = json.loads(raw)
        revised = json.loads((here / "scene-before-cast-stt-cases.json").read_bytes())
        self.assertEqual(revised["derived_from"]["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertFalse(revised["independent_human_review"])
        self.assertFalse(revised["training_admitted"])
        current = {c["case_id"]: c for c in revised["cases"]}
        for case in previous["cases"]:
            if case["case_id"] == "missing-boundary":
                self.assertEqual(case["target"]["intent"], "other")
                self.assertEqual(current["stt-omitted-comma"]["utterance"], case["utterance"])
            else:
                self.assertEqual(current[case["case_id"]], case)

    def test_current_presence_and_counterfactual_scope(self):
        here = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as directory:
            binary = build_parser(Path(directory))
            for pack in ("scene-before-cast-stt-cases.json", "scene-intent-challenge.json"):
                cases = json.loads((here / pack).read_bytes())["cases"]
                for case in cases:
                    with self.subTest(pack=pack, case=case["case_id"]):
                        result = subprocess.run(
                            [str(binary), case["utterance"]], check=True,
                            capture_output=True, text=True, timeout=5,
                        )
                        self.assertFalse(result.stderr)
                        self.assertEqual(json.loads(result.stdout), case["target"])


if __name__ == "__main__":
    unittest.main()
