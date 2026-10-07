"""Independent source fixtures and portable-wire checks for the offline C producer."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def decode(raw):
    """Read the documented wire format without calling the C encoder or decoder."""
    at = 72
    assert raw[:8] in (b"DNDSIDX1", b"DNDSIDX2")
    version = raw[7] - ord("0")

    def number():
        nonlocal at
        value = struct.unpack_from("<I", raw, at)[0]
        at += 4
        return value

    def string():
        nonlocal at
        length = number()
        value = raw[at : at + length].decode()
        at += length
        return value

    def group():
        result = dict(zip(("begin", "end", "count"), (number() for _ in range(3))))
        result["spans"] = [
            dict(zip(("record", "begin", "end"), (number() for _ in range(3))))
            for _ in range(2)
        ]
        return result

    scope = [string() for _ in range(3)]
    nd, nr, ne = [number() for _ in range(3)]
    np = number() if version == 2 else 0
    docs = [[string() for _ in range(3)] for _ in range(nd)]
    records = []
    for _ in range(nr):
        r = {"id": string(), "hash": string()}
        r.update(
            zip(("document", "page", "chunk", "begin"), (number() for _ in range(4)))
        )
        r["content"] = string()
        records.append(r)
    entries = []
    for _ in range(ne):
        e = {"name": string(), "alias": string()}
        e.update(
            zip(("document", "page", "kind", "flags"), (number() for _ in range(4)))
        )
        for key in ("heading", "body", "opening"):
            e[key] = group()
        e["fields"] = [group() for _ in range(5)]
        entries.append(e)
    passages = []
    for _ in range(np):
        definition = dict(
            zip(
                (
                    "document",
                    "first_page",
                    "last_page",
                    "heading_begin",
                    "heading_end",
                    "next_heading_begin",
                    "next_heading_end",
                ),
                (number() for _ in range(7)),
            )
        )
        definition["hash"] = string()
        passages.append(definition)
    assert at == len(raw)
    result = {"scope": scope, "documents": docs, "records": records, "entries": entries}
    if version == 2:
        result["passages"] = passages
    return result


def page(text, headings, kind=1, cuts=None):
    raw = text.encode()
    cuts = cuts or [(0, len(raw))]
    return {
        "page": 1,
        "kind": kind,
        "text": text,
        "text_sha256": sha(raw),
        "headings": headings,
        "records": [
            {
                "record_id": sha(f"{text}:{i}".encode()),
                "content_hash": sha(raw[a:b]),
                "chunk": i,
                "content": raw[a:b].decode(),
            }
            for i, (a, b) in enumerate(cuts)
        ],
    }


def fixture():
    corpus = "sha256:" + "1" * 64
    sources = [
        {
            "enabled": True,
            "book_slug": book,
            "source_sha256": sha(book.encode()),
            "etag": book,
            "source_key": book + ".pdf",
        }
        for book in ("players-handbook", "monster-manual")
    ]
    manifest = {
        "schema_version": "dnd-corpus-manifest/v1",
        "corpus_id": corpus,
        "enabled_sources": 2,
        "sources": sources,
    }
    text = (
        "WARDING STEP Once per turn, you can move. You must stop. "
        "REACTIONS A reaction can occur on your turn or someone else's turn."
    )
    # The opening crosses a record edge. Both original records are needed.
    pages = [
        page(text, ["warding step", "reactions"], cuts=[(0, 32), (20, len(text))]),
        page(
            "OWL Tiny beast Armor Class 11 Hit Points 1 Speed 5 ft. "
            "Challenge 0 Damage Immunities fire Senses darkvision.",
            ["owl"],
            kind=2,
        ),
    ]
    documents = [
        {
            "document_id": sha((corpus + ":" + s["etag"]).encode())[:24],
            "book_slug": s["book_slug"],
            "source_sha256": s["source_sha256"],
            "pages": [p],
        }
        for s, p in zip(sources, pages)
    ]
    return {
        "schema": "dnd-source-compiler-input/v1",
        "collection": "dnd_compiler_fixture",
        "corpus_version": corpus,
        "ruleset": "dnd-5e-2014",
        "documents": documents,
    }, manifest


class CompilerTests(unittest.TestCase):
    binary = None

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.data, self.manifest = fixture()
        self.output = self.root / "index"

    def run_compiler(self, success=True, raw=None, input_pin=None, manifest_pin=None):
        raw = raw if raw is not None else json.dumps(self.data).encode()
        manifest = json.dumps(self.manifest).encode()
        (self.root / "input").write_bytes(raw)
        (self.root / "manifest").write_bytes(manifest)
        process = subprocess.run(
            [
                str(self.binary),
                str(self.root / "input"),
                input_pin or sha(raw),
                str(self.root / "manifest"),
                manifest_pin or sha(manifest),
                "3" * 64,
                str(self.output),
            ],
            capture_output=True,
            timeout=20,
        )
        self.assertNotIn(b"Sanitizer", process.stderr)
        self.assertNotIn(b"runtime error:", process.stderr)
        if not success:
            self.assertEqual(process.returncode, 1, process.stderr.decode())
            self.assertEqual(process.stdout, b"")
            return None
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        report = json.loads(process.stdout)
        wire = self.output.read_bytes()
        self.assertEqual(sha(wire), report["artifact_sha256"])
        self.assertEqual(len(wire), report["bytes"])
        self.assertEqual(wire[8:40], bytes.fromhex(sha(manifest)))
        self.assertEqual(wire[40:72], bytes.fromhex("3" * 64))
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(self.root.glob("*.tmp.*")), [])
        return report, decode(wire)

    def rule_page(self):
        return self.data["documents"][0]["pages"][0]

    def test_declarative_spell_keeps_header_and_complete_effect(self):
        text = (
            "AEGIS 1st-level abjuration Casting Time: 1 reaction Range: Self "
            "Components: V, S Duration: 1 round A shell surrounds you. "
            "It adds three points of protection. "
            "KIND WARD 2nd-level enchantment (ritual) Casting Time: 1 action "
            "Range: Touch Components: V Duration: 1 minute A glow surrounds the target. "
            "LAMPLIGHT Evocation cantrip Casting Time: 1 action Range: Self "
            "Components: V Duration: 1 minute A light appears. "
            "REACTIONS You can react."
        )
        self.data["documents"][0]["pages"] = [
            page(text, ["aegis", "kind ward", "lamplight", "reactions"])
        ]
        _, decoded = self.run_compiler()
        spells = [e for e in decoded["entries"] if e["flags"] & 4]
        self.assertEqual(
            [e["name"] for e in spells], ["aegis", "kindward", "lamplight"]
        )
        for entry in spells:
            body = text[entry["body"]["begin"] : entry["body"]["end"]]
            self.assertIn("Casting Time:", body)
            self.assertIn("Range:", body)
            self.assertTrue(body.endswith("."))
        self.assertTrue(
            text[spells[0]["body"]["begin"] : spells[0]["body"]["end"]].endswith(
                "It adds three points of protection."
            )
        )

    def test_spaced_spell_ordinal_preserves_original_bytes(self):
        for level in (
            "1 st",
            "2 nd",
            "3 rd",
            "4 th",
            "5 th",
            "6 th",
            "7 th",
            "8 th",
            "9 th",
            "8  th",
        ):
            with self.subTest(level=level):
                self.output.unlink(missing_ok=True)
                text = (
                    "AEGIS "
                    + level
                    + "-level abjuration Casting Time: 1 action Range: Self "
                    "Components: V Duration: 1 round A shell surrounds you. "
                    "REACTIONS You can react."
                )
                self.data["documents"][0]["pages"] = [
                    page(text, ["aegis", "reactions"])
                ]
                _, decoded = self.run_compiler()
                spells = [e for e in decoded["entries"] if e["name"] == "aegis"]
                self.assertEqual(len(spells), 1)
                self.assertEqual(spells[0]["flags"], 4)
                self.assertGreater(spells[0]["body"]["count"], 0)
                self.assertEqual(
                    decoded["records"][spells[0]["heading"]["spans"][0]["record"]][
                        "content"
                    ],
                    text,
                )
                self.output.unlink()

    def test_spell_page_edge_and_unfinished_body_remain_incomplete(self):
        header = "AEGIS 1st-level abjuration Casting Time: 1 reaction Range: Self Components: V Duration: 1 round "
        for tail in (
            "A shell surrounds you.",
            "A shell surrounds you. Until next REACTIONS You can react.",
        ):
            with self.subTest(tail=tail):
                self.data["documents"][0]["pages"] = [
                    page(header + tail, ["aegis", "reactions"])
                ]
                _, decoded = self.run_compiler()
                entry = next(e for e in decoded["entries"] if e["name"] == "aegis")
                self.assertEqual(entry["flags"], 4)
                self.assertEqual(entry["body"]["count"], 0)
                self.output.unlink()

    def test_ordinary_heading_and_malformed_spell_fields_do_not_gain_spell_marker(self):
        header = "1st-level abjuration Casting Time: 1 reaction Range: Self Components: V Duration: 1 round "
        invalid = (
            "A metal shield surrounds you. It adds protection. ",
            header.replace("Range: Self ", ""),
            header.replace("Components: V", "Components: "),
            header.replace("1st-level", "10th-level"),
            header.replace("1st-level", "1 th-level"),
            header.replace("1st-level", "8 st-level"),
            header.replace("1st-level", "0 th-level"),
            header.replace("1st-level", "10 th-level"),
            header.replace("abjuration", "equipment"),
            header.replace("Duration:", "Duration-ish:"),
        )
        for prefix in invalid:
            with self.subTest(prefix=prefix):
                text = (
                    "AEGIS "
                    + prefix
                    + "A shell surrounds you. REACTIONS You can react."
                )
                self.data["documents"][0]["pages"] = [
                    page(text, ["aegis", "reactions"])
                ]
                _, decoded = self.run_compiler()
                self.assertFalse(any(e["flags"] & 4 for e in decoded["entries"]))
                self.assertFalse(any(e["name"] == "aegis" for e in decoded["entries"]))
                self.output.unlink()

    def section_fixture(self):
        self.data, self.manifest = fixture()
        self.data["schema"] = "dnd-source-compiler-input/v2"
        text = (
            "CONCENTRATION You can hold a spell. "
            "SCHOOLS OF MAGIC You can classify spells. "
            "Taking damage. A pulse requires a check. Its threshold is 13. "
            "Being dazed. You must stop."
        )
        child = text.index("Taking damage.")
        end = text.index("Being dazed.") - 1
        p = page(
            text,
            ["concentration", "schools of magic"],
            cuts=[(0, child - 5), (child - 15, child + 37), (child + 25, len(text))],
        )
        p["sections"] = [
            dict(
                parent_begin=0,
                parent_end=13,
                heading_begin=child,
                heading_end=child + 14,
                body_end=end,
            )
        ]
        self.data["documents"][0]["pages"] = [p]
        self.data["documents"][1]["pages"][0]["sections"] = []
        return p

    def group_text(self, wire, entry, group):
        raw = self.data["documents"][entry["document"]]["pages"][entry["page"] - 1][
            "text"
        ].encode()
        pieces = []
        cursor = group["begin"]
        for i, span in enumerate(group["spans"]):
            if i >= group["count"]:
                self.assertEqual(span, {"record": 0, "begin": 0, "end": 0})
                continue
            r = wire["records"][span["record"]]
            self.assertEqual(span["begin"], cursor)
            self.assertEqual(r["document"], entry["document"])
            self.assertEqual(r["page"], entry["page"])
            piece = r["content"].encode()[
                span["begin"] - r["begin"] : span["end"] - r["begin"]
            ]
            self.assertEqual(piece, raw[span["begin"] : span["end"]])
            pieces.append(piece)
            cursor = span["end"]
        self.assertEqual(cursor, group["end"])
        return b"".join(pieces).decode()

    def test_source_bytes_fields_and_remapped_records(self):
        report, wire = self.run_compiler()
        self.assertEqual((report["entries"], report["records"]), (3, 3))
        self.assertEqual(
            [r["id"] for r in wire["records"]], sorted(r["id"] for r in wire["records"])
        )
        for r in wire["records"]:
            self.assertEqual(sha(r["content"].encode()), r["hash"])
        step, reaction, owl = wire["entries"]
        self.assertEqual(step["name"], "wardingstep")
        self.assertEqual(step["flags"], 1)
        self.assertEqual(step["opening"]["count"], 2)
        self.assertEqual(
            self.group_text(wire, step, step["opening"]), "Once per turn, you can move."
        )
        self.assertEqual(
            self.group_text(wire, step, step["body"]),
            "Once per turn, you can move. You must stop.",
        )
        self.assertEqual(reaction["flags"], 2)
        self.assertEqual(owl["name"], "owl")
        self.assertEqual(
            [self.group_text(wire, owl, g) for g in owl["fields"]],
            [
                "Armor Class 11",
                "Hit Points 1",
                "Speed 5",
                "Challenge 0",
                "Damage Immunities fire",
            ],
        )

    def test_reviewed_subsection_parent_survives_sidebar(self):
        p = self.section_fixture()
        _, wire = self.run_compiler()
        e = next(e for e in wire["entries"] if e["kind"] == 3)
        self.assertEqual((e["name"], e["alias"]), ("takingdamage", "concentration"))
        self.assertEqual(self.group_text(wire, e, e["fields"][0]), "CONCENTRATION")
        self.assertEqual(self.group_text(wire, e, e["heading"]), "Taking damage.")
        self.assertEqual(
            self.group_text(wire, e, e["body"]),
            "A pulse requires a check. Its threshold is 13.",
        )
        self.assertEqual(
            self.group_text(wire, e, e["opening"]), "A pulse requires a check."
        )
        self.assertEqual(e["body"]["count"], 2)
        self.assertEqual(e["body"]["end"], p["sections"][0]["body_end"])
        for g in e["fields"][1:]:
            self.assertEqual(self.group_text(wire, e, g), "")

    def test_v2_without_sections_preserves_v1_bytes(self):
        self.run_compiler()
        old = self.output.read_bytes()
        self.output = self.root / "v2-index"
        self.data["schema"] = "dnd-source-compiler-input/v2"
        for d in self.data["documents"]:
            for p in d["pages"]:
                p["sections"] = []
        self.run_compiler()
        self.assertEqual(self.output.read_bytes(), old)

    def test_requested_subsection_cannot_hide_in_an_omitted_page(self):
        p = self.section_fixture()
        p["records"][1]["chunk"] = 2
        self.run_compiler(success=False)
        self.assertFalse(self.output.exists())

    def test_malformed_subsection_descriptors_reject_publication(self):
        for mutation in [
            dict(parent_begin=-1),
            dict(parent_end=0),
            dict(heading_begin=2**31),
            dict(body_end=2**32),
            dict(parent_begin=True),
            dict(parent_end="13"),
            dict(body_end=35),
            dict(heading_end=30),
            dict(extra="answer text"),
        ]:
            with self.subTest(mutation=mutation):
                p = self.section_fixture()
                p["sections"][0].update(mutation)
                self.run_compiler(success=False)
                self.assertFalse(self.output.exists())
        for variant in ["missing", "duplicate", "creature", "v1", "too-many"]:
            with self.subTest(variant=variant):
                p = self.section_fixture()
                if variant == "missing":
                    del p["sections"][0]["parent_end"]
                if variant == "duplicate":
                    p["sections"] *= 2
                if variant == "creature":
                    p["kind"] = 2
                if variant == "v1":
                    self.data["schema"] = "dnd-source-compiler-input/v1"
                if variant == "too-many":
                    p["sections"] *= 129
                self.run_compiler(success=False)
                self.assertFalse(self.output.exists())

    def test_deterministic_bytes(self):
        self.run_compiler()
        first = self.output.read_bytes()
        self.output = self.root / "second"
        self.run_compiler()
        self.assertEqual(first, self.output.read_bytes())

    def test_duplicate_hints_do_not_duplicate_entries(self):
        self.rule_page()["headings"] *= 2
        report, _ = self.run_compiler()
        self.assertEqual(report["entries"], 3)

    def test_hints_cannot_invent_a_rule(self):
        self.rule_page()["headings"] = ["invented rule"]
        report, _ = self.run_compiler()
        self.assertEqual(report["entries"], 1)

    def test_nested_heading_hints_keep_the_full_heading(self):
        self.data["documents"][0]["pages"] = [
            page(
                "HEAVY ARMOR BONUS You can block.",
                ["heavy armor bonus", "armor bonus", "bonus", "armor bonus"],
            )
        ]
        _, wire = self.run_compiler()
        self.assertEqual(
            [e["name"] for e in wire["entries"]], ["heavyarmorbonus", "owl"]
        )

    def test_gap_omits_whole_page(self):
        self.rule_page()["records"][1]["chunk"] = 2
        report, _ = self.run_compiler()
        self.assertEqual((report["incomplete_pages"], report["entries"]), (1, 1))

    def test_ambiguous_page_omitted(self):
        p = page("aaaaaaaa", [], cuts=[(0, 5), (1, 5), (5, 8)])
        self.data["documents"][0]["pages"] = [p]
        report, _ = self.run_compiler()
        self.assertEqual((report["ambiguous_pages"], report["entries"]), (1, 1))

    def test_repeated_text_uses_source_position(self):
        text = "WARDING STEP You can repeat repeat repeat words."
        second = text.index("repeat") + len("repeat ")
        p = page(
            text,
            ["warding step"],
            cuts=[(0, second + len("repeat ")), (second, len(text))],
        )
        self.data["documents"][0]["pages"] = [p]
        _, wire = self.run_compiler()
        r = next(r for r in wire["records"] if r["document"] == 0 and r["chunk"] == 1)
        self.assertEqual(r["begin"], second)
        entry = wire["entries"][0]
        self.assertEqual(
            self.group_text(wire, entry, entry["opening"]),
            "You can repeat repeat repeat words.",
        )

    def test_oversize_body_keeps_only_complete_opening(self):
        text = "WARDING STEP You can move. " + "x" * 1024 + "."
        self.data["documents"][0]["pages"] = [page(text, ["warding step"])]
        _, wire = self.run_compiler()
        e = wire["entries"][0]
        self.assertEqual(e["body"]["count"], 0)
        self.assertEqual(self.group_text(wire, e, e["opening"]), "You can move.")

    def test_unpunctuated_modal_is_not_an_opening(self):
        self.data["documents"][0]["pages"] = [
            page("WARDING STEP You can move", ["warding step"])
        ]
        report, _ = self.run_compiler()
        self.assertEqual(report["entries"], 1)

    def test_invalid_source_inputs(self):
        mutations = [
            lambda d: d.update(extra=1),
            lambda d: d.update(corpus_version="sha256:" + "f" * 64),
            lambda d: d["documents"][0].update(document_id="invented"),
            lambda d: d["documents"][0].update(source_sha256="f" * 64),
            lambda d: d["documents"][0].update(book_slug="dungeon-masters-guide"),
            lambda d: d["documents"][0]["pages"][0].update(text_sha256="f" * 64),
            lambda d: d["documents"][0]["pages"][0]["records"][0].update(
                content_hash="f" * 64
            ),
            lambda d: d["documents"][0]["pages"][0].update(page=0),
            lambda d: d["documents"][0]["pages"][0].update(kind=3),
            lambda d: d["documents"][0]["pages"][0].update(headings=["x" * 256]),
            lambda d: d["documents"][0]["pages"][0].update(headings=["é"]),
            lambda d: d["documents"][0]["pages"][0].update(headings=["word"] * 129),
            lambda d: d["documents"][0]["pages"].append(
                copy.deepcopy(d["documents"][0]["pages"][0])
            ),
        ]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                self.data, self.manifest = fixture()
                mutate(self.data)
                self.run_compiler(success=False)
                self.assertFalse(self.output.exists())

    def test_pins_and_malformed_json(self):
        for kwargs in (
            {"input_pin": "0" * 64},
            {"manifest_pin": "0" * 64},
            {"raw": b'{"schema":"x","schema":"y"}'},
            {"raw": json.dumps(self.data).encode() + b"\0"},
            {"raw": json.dumps(self.data).encode()[:-1]},
        ):
            with self.subTest(kwargs=kwargs.keys()):
                self.run_compiler(success=False, **kwargs)
                self.assertFalse(self.output.exists())

    def test_existing_output_preserved(self):
        self.output.write_bytes(b"other window output")
        self.run_compiler(success=False)
        self.assertEqual(self.output.read_bytes(), b"other window output")
        self.assertEqual(list(self.root.glob("*.tmp.*")), [])

    def test_output_symlink_preserved(self):
        target = self.root / "target"
        target.write_bytes(b"preserved")
        self.output.symlink_to(target)
        self.run_compiler(success=False)
        self.assertTrue(self.output.is_symlink())
        self.assertEqual(target.read_bytes(), b"preserved")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    CompilerTests.binary = args.binary.resolve()
    unittest.main(argv=[__file__], verbosity=2)
