"""Independent controls for offline capacity measurement; no model calls."""

import hashlib
import itertools
import random
import unittest

from rule_span_audit import (
    candidate_headings,
    candidate_ranges,
    minimum_cover,
    reconstruct,
)


def record(begin, text, name):
    return {
        "begin": begin,
        "content": text,
        "id": name,
        "hash": hashlib.sha256(text.encode()).hexdigest(),
    }


class AuditTests(unittest.TestCase):
    def test_source_shaped_heading(self):
        raw = b"A prior rule ends. S h i e l d 1st-level abjuration Casting Time: 1 reaction Range: Self."
        (row,) = candidate_headings(raw)
        self.assertEqual(row["name"], "shield")
        self.assertEqual(raw[row["begin"] : row["header_begin"]], b"S h i e l d ")

    def test_spaced_ordinal_cannot_hide_an_intervening_spell(self):
        rows = self.candidates(
            {
                1: "AEGIS 1st-level abjuration Casting Time: 1 action. It ends. 123 Unknown 8 th-level abjuration Casting Time: 1 action. It ends. LIGHT Evocation cantrip Casting Time: 1 action."
            }
        )
        self.assertFalse(rows[0]["complete_candidate"])
        self.assertEqual(rows[0]["reason"], "Unresolved intervening spell header")

    def test_slash_name_is_source_derived(self):
        (row,) = candidate_headings(
            b"WARD/LIGHT 8 th-level abjuration Casting Time: 1 action."
        )
        self.assertEqual(row["name"], "wardlight")

    def test_unicode_byte_offsets(self):
        raw = "Élan ends. AEGIS 1st-level abjuration Casting Time: 1 action.".encode()
        (row,) = candidate_headings(raw)
        self.assertEqual(raw[row["begin"] : row["header_begin"]], b"AEGIS ")

    def test_cantrip_and_ritual(self):
        self.assertEqual(
            [
                r["name"]
                for r in candidate_headings(
                    b"LIGHT Evocation cantrip Casting Time: 1 action. WARD 1st-level abjuration (ritual) Casting Time: 1 minute."
                )
            ],
            ["light", "ward"],
        )

    def test_reject_body_shape_and_non_header(self):
        for raw in (
            b"An ordinary narrative title 1st-level abjuration Casting Time: 1 action.",
            b"AEGIS 1st-level abjuration Missing Time: 1 action.",
            b"123 AEGIS 1st-level abjuration Casting Time: 1 action.",
        ):
            self.assertEqual(candidate_headings(raw), [])

    def test_full_interval_and_original_hashes(self):
        rows = [
            record(0, "abcdefgh", "a"),
            record(5, "fghijklm", "b"),
            record(10, "klmnop", "c"),
        ]
        spans = minimum_cover(rows, 2, 15)
        self.assertEqual(
            [(s["record_id"], s["begin"], s["end"]) for s in spans],
            [("a", 2, 8), ("b", 3, 8), ("c", 3, 5)],
        )
        self.assertEqual([s["content_hash"] for s in spans], [r["hash"] for r in rows])

    def test_gap_and_invalid_interval(self):
        for begin, end in ((0, 8), (2, 2), (-1, 2), (4, 3)):
            with self.assertRaises(ValueError):
                minimum_cover(
                    [record(0, "abc", "a"), record(5, "fgh", "b")], begin, end
                )

    def test_ties_and_input_order(self):
        rows = [record(0, "abcdef", "b"), record(0, "abcdef", "a")]
        self.assertEqual(minimum_cover(rows, 1, 5), minimum_cover(rows[::-1], 1, 5))
        self.assertEqual(minimum_cover(rows, 1, 5)[0]["record_id"], "a")

    def test_overlap_disagreement_and_holes(self):
        for rows in (
            [record(0, "abc", "a"), record(1, "XX", "b")],
            [record(0, "ab", "a"), record(3, "d", "b")],
        ):
            with self.assertRaises(ValueError):
                reconstruct(rows)

    def candidates(self, texts):
        raw = {p: text.encode() for p, text in texts.items()}
        pages = {p: [record(0, text, str(p))] for p, text in texts.items()}
        return candidate_ranges(raw, pages)

    def test_cross_page_continuation(self):
        rows = self.candidates(
            {
                1: "AEGIS 1st-level abjuration Casting Time: 1 action. The target",
                2: " keeps the ward. LIGHT Evocation cantrip Casting Time: 1 action. It shines.",
            }
        )
        self.assertTrue(rows[0]["complete_candidate"])
        self.assertTrue(rows[0]["crosses_page"])
        self.assertEqual(rows[0]["minimum_records"], 2)
        self.assertFalse(rows[0]["admitted"])

    def test_unknown_heading_blocks_merged_body(self):
        rows = self.candidates(
            {
                1: "AEGIS 1st-level abjuration Casting Time: 1 action. 123 unparsed name 2nd-level evocation Casting Time: 1 action. It burns. LIGHT Evocation cantrip Casting Time: 1 action."
            }
        )
        self.assertEqual(rows[0]["reason"], "Unresolved intervening spell header")
        self.assertFalse(rows[0]["complete_candidate"])

    def test_missing_page_rejects(self):
        rows = self.candidates(
            {
                1: "AEGIS 1st-level abjuration Casting Time: 1 action. A ward starts.",
                3: "LIGHT Evocation cantrip Casting Time: 1 action. It shines.",
            }
        )
        self.assertFalse(rows[0]["complete_candidate"])
        self.assertEqual(rows[0]["reason"], "Nonconsecutive or excessive page span")

    def test_page_start_does_not_complete_truncated_prior_body(self):
        rows = self.candidates(
            {
                1: "AEGIS 1st-level abjuration Casting Time: 1 action. An unfinished",
                2: "LIGHT Evocation cantrip Casting Time: 1 action. It shines.",
            }
        )
        self.assertFalse(rows[0]["complete_candidate"])

    def test_minimum_against_exhaustive_subsets(self):
        rng = random.Random(41)
        text = "abcdefghijklmnopqrst"
        for _ in range(100):
            intervals = {(rng.randrange(0, 19), rng.randrange(1, 21)) for _ in range(8)}
            rows = [
                record(a, text[a:b], str(i))
                for i, (a, b) in enumerate(sorted(intervals))
                if a < b
            ]
            minimum = None
            for n in range(1, len(rows) + 1):
                if any(
                    all(
                        any(
                            r["begin"] <= x < r["begin"] + len(r["content"])
                            for r in subset
                        )
                        for x in range(3, 17)
                    )
                    for subset in itertools.combinations(rows, n)
                ):
                    minimum = n
                    break
            if minimum is None:
                with self.assertRaises(ValueError):
                    minimum_cover(rows, 3, 17)
            else:
                self.assertEqual(len(minimum_cover(rows, 3, 17)), minimum)


if __name__ == "__main__":
    unittest.main()
