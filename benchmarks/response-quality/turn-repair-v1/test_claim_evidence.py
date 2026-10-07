import itertools
import json
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

import claim_evidence as c


class ClaimEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary, cls.command = c.compile_probe(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_rows(self, rows):
        result = c.execute(self.binary, rows)
        self.assertEqual(result, [c.oracle(*r) for r in rows])
        for (text, context, quote), value in zip(rows, result, strict=True):
            if value[0] == 1:
                self.assertEqual(text[value[1]:value[2]], context or text)
                self.assertEqual(text[value[3]:value[4]], quote)
                self.assertTrue(value[1] <= value[3] < value[4] <= value[2] <= len(text))
            else:
                self.assertEqual(value[1:], (0, 0, 0, 0))
        return result

    def test_explicit_context_resolves_repeated_words_without_choosing_an_occurrence(self):
        text = b"I completed Fireball this turn. Mira completed Polymorph this turn."
        rows = [(text, b"", b"completed"),
                (text, b"I completed Fireball this turn", b"completed"),
                (text, b"Mira completed Polymorph this turn", b"completed"),
                (text, b"completed", b"completed"),
                (text, b"I completed Fireball this turn", b"Polymorph"),
                (text, b"absent context", b"completed")]
        result = self.check_rows(rows)
        self.assertEqual([v[0] for v in result], [-4, 1, 1, -2, -3, -1])
        self.assertNotEqual(result[1][3], result[2][3])

    def test_overlap_is_ambiguous_and_repeated_context_is_not_a_tie_breaker(self):
        rows = [(b"banana", b"", b"ana"), (b"banana", b"ana", b"a"),
                (b"banana", b"bana", b"ana"), (b"aaaa", b"", b"aa"),
                (b"left end; right end", b"left end", b"end")]
        self.assertEqual([v[0] for v in self.check_rows(rows)], [-4, -2, 1, -4, 1])

    def test_exhaustive_small_strings_against_regex_reference(self):
        words = lambda sizes: [bytes(x) for n in sizes for x in itertools.product(b"ab", repeat=n)]
        rows = list(itertools.product(words(range(6)), [b"", *words(range(1, 3))], words(range(1, 4))))
        self.assertEqual(len(rows), 6174)
        self.check_rows(rows)

    def test_utf8_and_bounds_reject_without_partial_ranges(self):
        raw = "Míra dit feu. Lynn dit feu.".encode()
        rows = [(raw, "Míra dit feu".encode(), b"feu"), (raw, b"", b"feu"),
                (raw, "Míra dit feu".encode(), "í".encode()), (raw, b"", b"\xad"),
                (b"\xc0\x80", b"", b"x"), (b"\xed\xa0\x80", b"", b"x"),
                (b"x\0x", b"", b"x"), (b"x", b"x\0", b"x"), (b"x", b"", b"x\0"),
                (b"x", b"", b""), (b"", b"", b"x"),
                (b"x" * 16384, b"", b"x"), (b"x" * 16385, b"", b"x"),
                (b"x", b"x" * 4096, b"x"), (b"x", b"x" * 4097, b"x"),
                (b"x", b"", b"x" * 2048), (b"x", b"", b"x" * 2049)]
        result = self.check_rows(rows)
        self.assertEqual(result[0][0], 1)
        self.assertEqual(result[0][3], len("Míra dit ".encode()))

    def test_mutations_and_all_truncations_preserve_reference_results(self):
        rng = random.Random(20260928)
        original = [b"I completed Fireball. Mira completed Polymorph.", b"I completed Fireball", b"completed"]
        rows = [tuple(original[:part] + [original[part][:end]] + original[part+1:])
                for part in range(3) for end in range(len(original[part]) + 1)]
        for _ in range(2048):
            changed = original.copy()
            part = rng.randrange(3)
            value = bytearray(changed[part])
            value[rng.randrange(len(value))] = rng.randrange(256)
            changed[part] = bytes(value)
            rows.append(tuple(changed))
        self.check_rows(rows)

    def test_existing_authored_unique_quotes_remain_byte_exact(self):
        data = json.loads((c.HERE / "synthetic/multi-cast-20260928.json").read_text())
        rows = [(case["utterance"].encode(), b"", event[field]["quote"].encode())
                for case in data["cases"] for event in case["events"]
                for field in ("actor", "time", "status", "spell", "modifier") if event[field]["quote"]]
        self.assertTrue(all(v[0] == 1 for v in self.check_rows(rows)))

    def test_null_inputs_reject_before_dereference(self):
        root = Path(self.tmp.name)
        source = root / "null-probe.c"
        source.write_text('''#include "common/dnd_claim_evidence.h"
int main(void) {
    const unsigned char x[] = "x";
    dnd_claim_evidence_span out;
    if (dnd_claim_evidence_resolve(x, 1, 0, 0, x, 1, 0) != DND_CLAIM_INVALID) return 1;
    if (dnd_claim_evidence_resolve(0, 1, 0, 0, x, 1, &out) != DND_CLAIM_INVALID) return 2;
    if (dnd_claim_evidence_resolve(x, 1, 0, 1, x, 1, &out) != DND_CLAIM_INVALID) return 3;
    if (dnd_claim_evidence_resolve(x, 1, 0, 0, 0, 1, &out) != DND_CLAIM_INVALID) return 4;
    if (out.context_begin || out.context_end || out.quote_begin || out.quote_end) return 5;
    if (dnd_claim_evidence_resolve(x, 1, 0, 0, x, 1, &out) != DND_CLAIM_BOUND) return 6;
    return 0;
}
''')
        command = [str(source) if arg == str(c.HERE / "claim_evidence_probe.c") else
                   str(root / "null-probe") if arg == str(self.binary) else arg for arg in self.command]
        subprocess.run(command, check=True, capture_output=True)
        result = subprocess.run([str(root / "null-probe")], capture_output=True, env=c.sanitizer_env(), timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_first_match_and_wrong_scope_mutants_are_detected_by_reference(self):
        header = (c.ROOT / "voice/c-runtime/common/dnd_claim_evidence.h").read_text()
        mutants = {
            "first_match": header.replace("if (++count == 2u) return 2u;", "if (++count == 1u) return 1u;"),
            "global_scope": header.replace("text + context_begin, context_end - context_begin,", "text, length,"),
        }
        rows = [(b"I completed Fireball. Mira completed Polymorph.", b"", b"completed"),
                (b"I completed Fireball. Mira completed Polymorph.", b"Mira completed Polymorph", b"completed")]
        for name, changed in mutants.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "common").mkdir()
                (root / "common/dnd_claim_evidence.h").write_text(changed)
                (root / "common/utf8.h").write_bytes((c.ROOT / "voice/c-runtime/common/utf8.h").read_bytes())
                binary, _ = c.compile_probe(root, root)
                # A successful process with wrong values proves assertion sensitivity.
                self.assertNotEqual(c.execute(binary, rows), [c.oracle(*r) for r in rows])


if __name__ == "__main__":
    unittest.main()
