"""Independent protoc oracle for exact-source citation ranges."""

from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
BIN = (
    Path(sys.argv.pop(1)).resolve()
    if len(sys.argv) > 1
    else ROOT / "voice/c-runtime/c-pb-interop"
)
PROTO = ROOT / "contracts/handler-base/proto"
BASE = "\n".join(
    [
        'source: "book://fixture"',
        'collection: "books"',
        'corpus_version: "sha256:' + "a" * 64 + '"',
        'embedding_model: "fixture"',
        'record_id: "' + "b" * 64 + '"',
        'document_id: "fixture"',
        'content_hash: "' + "c" * 64 + '"',
        'source_sha256: "' + "d" * 64 + '"',
    ]
)


def proto(operation, data):
    return subprocess.check_output(
        [
            "protoc",
            "--proto_path=" + str(PROTO),
            "--" + operation + "=messages.v1.RetrievalCitation",
            "messages/v1/messages.proto",
        ],
        input=data,
    )


def varint(value):
    data = bytearray()
    while value > 127:
        data.append((value & 127) | 128)
        value >>= 7
    data.append(value)
    return data


def raw_span(data):
    return bytes(varint((16 << 3) | 2) + varint(len(data)) + data)


class Spans(unittest.TestCase):
    def check(self, spans, accepted):
        text = BASE + "\n" + spans
        wire = proto("encode", text.encode())
        result = subprocess.run(
            [str(BIN), "roundtrip-citation"], input=wire, capture_output=True
        )
        self.assertEqual(result.returncode, 0 if accepted else 1)
        if accepted:
            self.assertEqual(proto("decode", wire), proto("decode", result.stdout))

    def test_legacy_and_zero_start(self):
        self.check("", True)
        self.check("excerpt_spans { end: 5 }", True)

    def test_disjoint_spans_and_bound(self):
        self.check("excerpt_spans { end: 5 } excerpt_spans { begin: 10 end: 20 }", True)
        self.check("excerpt_spans { begin: 8190 end: 8191 }", True)
        self.check(
            " ".join(
                f"excerpt_spans {{ begin: {i * 3} end: {i * 3 + 1} }}" for i in range(8)
            ),
            True,
        )

    def test_invalid_ranges(self):
        for value in [
            "excerpt_spans {}",
            "excerpt_spans { begin: 3 end: 3 }",
            "excerpt_spans { begin: 4 end: 3 }",
            "excerpt_spans { end: 8192 }",
            "excerpt_spans { end: 4294967295 }",
            "excerpt_spans { end: 5 } excerpt_spans { begin: 5 end: 9 }",
            "excerpt_spans { end: 5 } excerpt_spans { begin: 4 end: 9 }",
            "excerpt_spans { begin: 10 end: 20 } excerpt_spans { end: 5 }",
            " ".join(
                f"excerpt_spans {{ begin: {i * 3} end: {i * 3 + 1} }}" for i in range(9)
            ),
        ]:
            with self.subTest(value=value):
                self.check(value, False)

    def test_malformed_nested_fields(self):
        base = proto("encode", BASE.encode())
        for data in [
            b"\x08\x00\x08\x01\x10\x05",
            b"\x08\x00\x10\x05\x10\x06",
            b"\x18\x01\x10\x05",
            b"\x0a\x00\x10\x05",
            b"\x10\x80",
        ]:
            with self.subTest(data=data):
                result = subprocess.run(
                    [str(BIN), "roundtrip-citation"],
                    input=base + raw_span(data),
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 1)


class Passages(unittest.TestCase):
    def text(self, count=2):
        base = BASE.replace(
            'record_id: "' + "b" * 64 + '"', 'passage_id: "' + "b" * 64 + '"'
        )
        return (
            base
            + "\npage_start: 1 page_end: 1 score_metric: RETRIEVAL_SCORE_METRIC_NONE\n"
            + "\n".join(
                'witnesses { record_id: "'
                + f"{i + 1:064x}"
                + '" content_hash: "'
                + "e" * 64
                + f'" page: 1 chunk: {i} begin: 2 end: 6 record_length: 8 }}'
                for i in range(count)
            )
        )

    def check_text(self, text, accepted):
        wire = proto("encode", text.encode())
        result = subprocess.run(
            [str(BIN), "roundtrip-citation"], input=wire, capture_output=True
        )
        self.assertEqual(result.returncode, 0 if accepted else 1, result.stderr)
        if accepted:
            self.assertEqual(proto("decode", wire), proto("decode", result.stdout))

    def test_one_sixteen_and_overflow(self):
        for count in (1, 2, 16, 17):
            self.check_text(self.text(count), count <= 16)

    def test_passage_correlations(self):
        text = self.text()
        for bad in [
            text.replace("passage_id:", "record_id:"),
            text + ' record_id: "' + "c" * 64 + '"',
            text.replace("score_metric: RETRIEVAL_SCORE_METRIC_NONE", "score: 0.1"),
            text + " chunk_index: 1",
            text + " excerpt_spans { end: 2 }",
            text.replace("record_length: 8", "record_length: 8192"),
            text.replace("begin: 2 end: 6", "begin: 6 end: 6"),
            text.replace("begin: 2 end: 6", "begin: 2 end: 9"),
            text.replace("chunk: 1", "chunk: 0"),
            text.replace(f"{2:064x}", f"{1:064x}"),
            text.replace("page_end: 1", "page_end: 2"),
            text.replace("page: 1 chunk: 1", "page: 3 chunk: 1"),
            text.replace("page: 1", "page: 0"),
        ]:
            with self.subTest(bad=bad):
                self.check_text(bad, False)
        self.check_text(
            text.replace("page_end: 1", "page_end: 2").replace(
                "page: 1 chunk: 1", "page: 2 chunk: 0"
            ),
            True,
        )
        self.check_text(
            self.text(1).replace(
                "begin: 2 end: 6 record_length: 8",
                "begin: 8190 end: 8191 record_length: 8191",
            ),
            True,
        )

    def test_duplicate_passage_field_and_unknown_witness_field(self):
        wire = proto("encode", self.text(1).encode())
        for suffix in [
            bytes(varint((17 << 3) | 2) + varint(64)) + b"a" * 64,
            bytes(varint((18 << 3) | 2) + varint(2) + b"\x40\x01"),
        ]:
            result = subprocess.run(
                [str(BIN), "roundtrip-citation"],
                input=wire + suffix,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
