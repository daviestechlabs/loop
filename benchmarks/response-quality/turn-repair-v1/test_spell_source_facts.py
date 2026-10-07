import itertools
import random
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import spell_source_facts as s


def header(classification, cost, ritual=False):
    return (classification + (" (ritual)" if ritual else "") + " Casting Time: " + cost +
            " Range: 30 feet Components: V Duration: Instantaneous Test description.").encode()


class SourceFactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary, _ = s.compile_probe(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_all_levels_schools_supported_costs_and_source_ranges(self):
        rows, expected = [], []
        schools = ("abjuration", "conjuration", "divination", "enchantment", "evocation", "illusion", "necromancy", "transmutation")
        for level, school, cost, ritual, spaced in itertools.product(range(10), schools, ("1 action", "1 bonus action", "1 reaction", "1 reaction, when a fixture condition applies", "1 minute", "1 action or 1 hour"), (False, True), (False, True)):
            ordinal = {1: "st", 2: "nd", 3: "rd"}.get(level, "th")
            classification = f"{level}{' ' if spaced else ''}{ordinal}-level {school}" if level else school + " cantrip"
            raw = header(classification, cost, ritual)
            rows.append(raw)
            expected.append((level, {"1 action": 1, "1 bonus action": 2, "1 reaction": 3, "1 reaction, when a fixture condition applies": 3}.get(cost, 0), ritual, cost.startswith("1 reaction,"), cost.encode()))
        for raw, value, want in zip(rows, s.extract(self.binary, rows), expected, strict=True):
            self.assertEqual(value[:5], [1, want[0], want[1], int(want[2]), int(want[3])])
            self.assertEqual(raw[value[7]:value[8]], want[4])
            self.assertIn(b"cantrip" if not want[0] else b"-level", raw[value[5]:value[6]])

    def test_invalid_headers_reset_facts_and_unknown_cost_never_becomes_action(self):
        good = header("3rd-level evocation", "1 action")
        bad = [b"", b"garbage", good.replace(b"3rd", b"3th"), good.replace(b"3rd", b"0th"),
               good.replace(b"evocation", b"unknown"), good.replace(b"Casting Time:", b"Time:"),
               good.replace(b"Duration:", b"Delay:"), good + b"\0", good[:good.index(b"Instantaneous")]]
        self.assertTrue(all(v == [0, 255, 0, 0, 0, 0, 0, 0, 0] for v in s.extract(self.binary, bad)))
        for cost in ("2 actions", "1 minute", "1 reactionary action", "1 action plus special conditions", "1 reaction, "):
            value = s.extract(self.binary, [header("1st-level abjuration", cost)])[0]
            self.assertEqual(value[2], 0)

    def test_bounded_mutations_and_truncations_under_sanitizers(self):
        raw = header("3 rd-level evocation", "1 action")
        rows = [raw[:i] for i in range(len(raw) + 1)]
        rng = random.Random(20260928)
        for _ in range(2048):
            changed = bytearray(raw)
            changed[rng.randrange(len(raw))] = rng.randrange(256)
            rows.append(bytes(changed))
        # extract independently checks all successful ranges and erased failures.
        self.assertEqual(len(s.extract(self.binary, rows)), len(rows))

    def test_external_source_pins_reject_changed_source_before_decode(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)
            (source / "index.dndsidx").write_bytes(b"changed source")
            (source / "manifest.json").write_bytes(b"changed manifest")
            with patch.object(s.ab, "decode") as decoder:
                with self.assertRaisesRegex(ValueError, "Index pin differs"):
                    s.source_passages(source, "0" * 64, "0" * 64)
                with self.assertRaisesRegex(ValueError, "Manifest pin differs"):
                    s.source_passages(source, s.digest(b"changed source"), "0" * 64)
                decoder.assert_not_called()

    def test_independent_verifier_rejects_rehashed_wrong_level_and_span(self):
        raw = b"FIREBALL " + header("3rd-level evocation", "1 action")
        row = {"name": "fireball", "document": ["fixture"], "assembly_sha256": s.digest(raw),
               "header_offset": 9, "definition": {}, "raw": raw}
        fact = {k: v for k, v in row.items() if k != "raw"}
        fact.update(valid_header=True, level=3, standard_cost=1, ritual_tag=False, conditional_reaction=False)
        for name, text in (("classification", b"3rd-level"), ("casting_time", b"1 action")):
            begin = raw.index(text)
            fact[name] = {"begin": begin, "end": begin + len(text), "sha256": s.digest(text)}
        with tempfile.TemporaryDirectory() as tmp, patch.object(s, "source_passages", return_value=[row]):
            root = Path(tmp)
            def save():
                (root / "facts.json").unlink(missing_ok=True)
                (root / "receipt.json").unlink(missing_ok=True)
                s.write(root / "facts.json", [fact])
                s.write(root / "receipt.json", {"index_sha256": "index", "manifest_sha256": "manifest",
                        "passages": 1, "valid_headers": 1,
                        "files": {"facts.json": s.digest((root / "facts.json").read_bytes())}})
            save()
            self.assertEqual(s.verify(root, root, "index", "manifest"), [fact])
            fact["level"] = 0
            save()
            with self.assertRaisesRegex(ValueError, "Source facts disagree"):
                s.verify(root, root, "index", "manifest")
            fact["level"] = 3
            fact["classification"]["begin"] += 1
            save()
            with self.assertRaisesRegex(ValueError, "Fact source range differs"):
                s.verify(root, root, "index", "manifest")


if __name__ == "__main__":
    unittest.main()
