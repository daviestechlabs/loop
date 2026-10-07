"""Independent passage evidence faults, including rehashed boundary changes."""

import copy
import hashlib
import struct
import unittest

from passage_evidence import admitted_passages, passage_text


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def identity(c):
    raw = bytearray(b"dnd-complete-passage/v1\0" + c["content_hash"].encode())
    for w in c["witnesses"]:
        raw.extend(w["record_id"].encode())
        raw.extend(
            struct.pack("<4I", w["page"], w["chunk_index"], w["begin"], w["end"])
        )
    c["passage_id"] = sha(raw)


class PassageEvidence(unittest.TestCase):
    def setUp(self):
        page1, page2 = b"AEGIS A ward.", b"It lasts. BEACON A light."
        assembly = page1 + b"\nIt lasts."
        self.data = {
            "documents": [["doc", "players-handbook", "a" * 64]],
            "records": [],
            "passages": [
                {
                    "document": 0,
                    "first_page": 1,
                    "last_page": 2,
                    "heading_begin": 0,
                    "heading_end": 5,
                    "next_heading_begin": 10,
                    "next_heading_end": 16,
                    "hash": sha(assembly),
                }
            ],
        }
        self.c = {
            "kind": "complete_passage",
            "record_id": "",
            "chunk_index": 0,
            "score_metric": "none",
            "score": 0,
            "document_id": "doc",
            "book_slug": "players-handbook",
            "source_sha256": "a" * 64,
            "content_hash": sha(assembly),
            "section": "aegis",
            "page_start": 1,
            "page_end": 2,
            "corpus_version": "sha256:" + "b" * 64,
            "embedding_model": "fixture",
            "source": "book://fixture",
            "witnesses": [],
        }
        self.records = {}
        for i, (page, chunk, begin, raw, cut) in enumerate(
            [
                (1, 0, 0, page1[:9], (0, 9)),
                (1, 1, 7, page1[7:], (2, len(page1) - 7)),
                (2, 0, 0, page2, (0, 9)),
            ]
        ):
            rid = f"{i + 1:064x}"
            self.data["records"].append(
                {
                    "id": rid,
                    "hash": sha(raw),
                    "document": 0,
                    "page": page,
                    "chunk": chunk,
                    "begin": begin,
                    "content": raw.decode(),
                }
            )
            meta = {
                k: self.c[k]
                for k in (
                    "document_id",
                    "book_slug",
                    "source_sha256",
                    "corpus_version",
                    "embedding_model",
                )
            }
            meta.update(
                content_hash=sha(raw), page_start=page, page_end=page, chunk_index=chunk
            )
            self.records[rid] = (
                {"content": raw.decode(), "source": self.c["source"]},
                meta,
            )
            self.c["witnesses"].append(
                {
                    "record_id": rid,
                    "content_hash": sha(raw),
                    "page": page,
                    "chunk_index": chunk,
                    "begin": cut[0],
                    "end": cut[1],
                    "record_length": len(raw),
                }
            )
        identity(self.c)

    def check(self):
        return passage_text(
            self.c, self.records, self.data, admitted_passages(self.data)
        )

    def test_valid_overlap_and_page_separator(self):
        self.assertEqual(self.check(), "AEGIS A ward.\nIt lasts.")

    def test_missing_or_repeated_exact_witness(self):
        self.records.pop(self.c["witnesses"][1]["record_id"])
        with self.assertRaises(ValueError):
            self.check()
        self.setUp()
        self.c["witnesses"].insert(1, copy.deepcopy(self.c["witnesses"][0]))
        identity(self.c)
        with self.assertRaises(ValueError):
            self.check()

    def test_changed_content_even_when_backend_rehashes(self):
        record, meta = self.records[self.c["witnesses"][0]["record_id"]]
        record["content"] = "OTHER RAW"
        meta["content_hash"] = sha(record["content"].encode())
        self.c["witnesses"][0]["content_hash"] = meta["content_hash"]
        identity(self.c)
        with self.assertRaises(ValueError):
            self.check()

    def test_changed_scope_fields(self):
        for key in (
            "document_id",
            "book_slug",
            "source_sha256",
            "corpus_version",
            "embedding_model",
        ):
            with self.subTest(key=key):
                self.setUp()
                self.records[self.c["witnesses"][1]["record_id"]][1][key] = "changed"
                with self.assertRaises(ValueError):
                    self.check()

    def test_rehashed_offsets_cannot_move_boundary_or_create_gap(self):
        for at, key, value in (
            (0, "begin", 1),
            (1, "begin", 3),
            (2, "end", 10),
            (1, "chunk_index", 0),
            (2, "page", 3),
        ):
            with self.subTest(at=at, key=key):
                self.setUp()
                self.c["witnesses"][at][key] = value
                identity(self.c)
                with self.assertRaises(ValueError):
                    self.check()

    def test_truncation_and_unadmitted_passage(self):
        self.c["witnesses"].pop()
        identity(self.c)
        with self.assertRaises(ValueError):
            self.check()
        self.setUp()
        self.data["passages"] = []
        with self.assertRaises(ValueError):
            self.check()

    def test_identity_and_duplicate_definition(self):
        self.c["passage_id"] = "e" * 64
        with self.assertRaises(ValueError):
            self.check()
        identity(self.c)
        self.data["passages"] *= 2
        with self.assertRaises(ValueError):
            self.check()

    def test_pinned_assembly_and_page_overlap(self):
        self.data["passages"][0]["hash"] = "c" * 64
        with self.assertRaises(ValueError):
            self.check()
        self.setUp()
        r = self.data["records"][1]
        r["content"] = "wrong."
        r["hash"] = sha(r["content"].encode())
        with self.assertRaises(ValueError):
            self.check()


if __name__ == "__main__":
    unittest.main()
