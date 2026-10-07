"""Adversarial source-boundary and intervention checks without model calls."""

import copy
import unittest

import source_span_eval as study


def record(text, begin=0):
    return {"content": text, "begin": begin, "hash": study.sha(text.encode())}


class SourceBoundaries(unittest.TestCase):
    def test_exact_overlap_and_unicode(self):
        self.assertEqual(
            study.reconstruct([record("A café "), record("café ends.", 2)]),
            "A café ends.".encode(),
        )

    def test_overlap_disagreement(self):
        with self.assertRaisesRegex(ValueError, "disagree"):
            study.reconstruct([record("abc"), record("de", 2)])

    def test_gap(self):
        with self.assertRaisesRegex(ValueError, "uncovered"):
            study.reconstruct([record("abc"), record("def", 4)])

    def test_hash(self):
        value = record("abc")
        value["content"] = "xyz"
        with self.assertRaisesRegex(ValueError, "hash"):
            study.reconstruct([value])

    def test_empty_page(self):
        with self.assertRaisesRegex(ValueError, "Missing"):
            study.reconstruct([])

    def test_page_bound(self):
        with self.assertRaisesRegex(ValueError, "bound"):
            study.reconstruct([record("x", 65536)])

    def test_heading_keeps_byte_offset(self):
        raw = (
            "Café. K i n d W a r d 1st-level abjuration Casting Time: 1 action".encode()
        )
        self.assertEqual(
            study.heading(raw, "Kind Ward", "1st-level abjuration"),
            len("Café. ".encode()),
        )

    def test_ambiguous_heading(self):
        raw = b"Kind Ward 1st-level abjuration. Kind Ward 1st-level abjuration."
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            study.heading(raw, "Kind Ward", "1st-level abjuration")

    def test_wrong_header(self):
        with self.assertRaisesRegex(ValueError, "Missing"):
            study.heading(b"Kind Ward equipment", "Kind Ward", "1st-level abjuration")

    def test_heading_substring(self):
        with self.assertRaisesRegex(ValueError, "Missing"):
            study.heading(
                b"Unkind Ward 1st-level abjuration", "Kind Ward", "1st-level abjuration"
            )

    def test_clip_intersection_uses_bytes(self):
        self.assertEqual(
            study.clipped(record("Café ends.", 20), 23, 25),
            {"begin": 3, "end": 5, "content": "é"},
        )

    def test_clip_empty(self):
        with self.assertRaisesRegex(ValueError, "Empty"):
            study.clipped(record("abc", 20), 24, 28)

    def test_clip_rejects_partial_codepoint(self):
        with self.assertRaises(UnicodeDecodeError):
            study.clipped(record("é", 20), 21, 22)

    def test_clip_does_not_rewrite_spacing(self):
        self.assertEqual(
            study.clipped(record("A s p a c e d rule."), 2, 14)["content"],
            "s p a c e d ",
        )


class Intervention(unittest.TestCase):
    def test_balanced_order(self):
        rows = list(study.matrix())
        self.assertEqual(len(rows), 36)
        for case, _, _ in study.CASES:
            orders = [
                tuple(r[3] for r in rows if r[0] == seed and r[1] == case)
                for seed in study.SEEDS
            ]
            for position in range(3):
                self.assertEqual(
                    {order[position] for order in orders}, set(study.CONDITIONS)
                )

    def test_only_evidence_seed_and_stream_change(self):
        citation = {
            "book_slug": "fixture",
            "record_id": "record",
            "page_start": 2,
            "page_end": 2,
            "section": "",
            "chunk_index": 0,
        }
        template = {
            "model": "fixture-model",
            "temperature": 0.2,
            "max_completion_tokens": 256,
            "stream": True,
            "messages": [
                {"role": "system", "content": "same instructions"},
                {"role": "user", "content": "old"},
            ],
        }
        prepared = {
            "template": template,
            "conditions": {
                c: [{"citation": citation, "content": c}] for c in study.CONDITIONS
            },
        }
        before = copy.deepcopy(prepared)
        for condition in study.CONDITIONS:
            result = study.request(prepared, "question", condition, 1)
            self.assertEqual(result["messages"][0], template["messages"][0])
            self.assertEqual(result["temperature"], 0.2)
            self.assertEqual(result["max_completion_tokens"], 256)
            self.assertFalse(result["stream"])
            self.assertEqual(result["seed"], 1)
            self.assertIn("\n" + condition + "\n", result["messages"][1]["content"])
            self.assertTrue(
                result["messages"][1]["content"].endswith("Question: question")
            )
        self.assertEqual(prepared, before)


if __name__ == "__main__":
    unittest.main()
