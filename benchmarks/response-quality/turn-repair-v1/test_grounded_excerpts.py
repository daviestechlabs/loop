"""Verify source boundaries separately from model answer quality."""

import unittest
from pathlib import Path
import subprocess
import tempfile

from verify_grounded_rules import build_renderer, excerpt_text


class Excerpts(unittest.TestCase):
    def text(self, spans, allowed=((0, 3), (8, 11)), content="one GAP two"):
        return excerpt_text(
            content,
            {"record_id": "fixture", "excerpt_spans": spans},
            {"fixture": set(allowed)},
        )

    def test_legacy_full_record(self):
        self.assertEqual(excerpt_text("full", {"record_id": "fixture"}, {}), "full")

    def test_exact_disjoint_and_union(self):
        self.assertEqual(
            self.text([{"begin": 0, "end": 3}, {"begin": 8, "end": 11}]),
            "one\n[Omitted source text]\ntwo",
        )
        self.assertEqual(self.text([{"begin": 0, "end": 3}], ((0, 2), (2, 3))), "one")
        self.assertEqual(self.text([{"begin": 0, "end": 3}], ((0, 2), (1, 3))), "one")

    def test_invalid_or_unadmitted_ranges(self):
        for spans in [
            [],
            [{"begin": True, "end": 3}],
            [{"begin": 0, "end": 2}],
            [{"begin": 0, "end": 11}],
            [{"begin": 0, "end": 3, "extra": 1}],
            [{"begin": 8, "end": 11}, {"begin": 0, "end": 3}],
            [{"begin": 0, "end": 3}, {"begin": 3, "end": 11}],
            [{"begin": 0, "end": 99}],
            [{"begin": 0, "end": 0}],
            [{"begin": 0, "end": 3}] * 9,
        ]:
            with self.subTest(spans=spans), self.assertRaises(ValueError):
                self.text(spans)

    def test_utf8_offsets(self):
        self.assertEqual(self.text([{"begin": 0, "end": 2}], ((0, 2),), "é"), "é")
        with self.assertRaises(UnicodeDecodeError):
            self.text([{"begin": 1, "end": 2}], ((1, 2),), "é")


class Presentation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="waterdeep-display-test-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.renderer = Path(cls.directory.name) / "render"
        build_renderer(Path(__file__).resolve().parents[3], cls.renderer)

    def test_presentation_preserves_words_and_literals(self):
        for raw, expected in [
            ("No *magic missile*.", "No magic missile."),
            ("**Shield** adds +5; 2 * 3 stays 6.", "Shield adds +5; 2 * 3 stays 6."),
            ("  Élan <laugh> protects you.  ", "Élan protects you."),
            ("An unmatched *marker stays.", "An unmatched *marker stays."),
        ]:
            with self.subTest(raw=raw):
                self.assertEqual(
                    subprocess.check_output(
                        [str(self.renderer)], input=raw.encode()
                    ).decode(),
                    expected,
                )

    def test_invalid_input_rejects(self):
        for data in (b"", b"x" * 2048, b"a\0b", b"\xff"):
            with self.subTest(data=data[:20]):
                self.assertEqual(
                    subprocess.run(
                        [str(self.renderer)], input=data, capture_output=True
                    ).returncode,
                    1,
                )


if __name__ == "__main__":
    unittest.main()
