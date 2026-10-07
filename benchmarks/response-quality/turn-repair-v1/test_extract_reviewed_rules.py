"""Exercise admission and exact text spans using independently serialized fixtures."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


class ExtractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="reviewed-rules-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.environment = dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1",
        )
        common = ROOT / "voice/c-runtime/common"
        product = ROOT / "product/companions-frontend/c-companions"
        ent = ROOT / "product/companions-frontend/c-entitlements"
        flags = [
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I" + str(common),
            "-I" + str(product),
            "-I" + str(ent),
        ]
        deps = [
            common / "dnd_source_artifact.c",
            common / "dnd_source_passage.c",
            common / "dnd_source_page.c",
            common / "dnd_source_index.c",
            common / "utf8.c",
            product / "cmp_json.c",
            ent / "ent_books.c",
        ]
        for name, source in (
            ("extract", HERE / "extract_reviewed_rules.c"),
            ("emit", ROOT / "voice/c-runtime/tests/dnd_source_artifact_emit.c"),
        ):
            command = [
                "cc",
                *flags,
                str(source),
                *map(str, deps),
                "-lcrypto",
                "-o",
                str(cls.directory / name),
            ]
            shell = 'source "$1"; shift; cc ${CFLAGS:-} "$@"'
            subprocess.run(
                [
                    "bash",
                    "-ec",
                    shell,
                    "build",
                    str(ROOT / "benchmarks/retrieval-quality/scripts/native-env.sh"),
                    *command[1:],
                ],
                check=True,
                capture_output=True,
                env=cls.environment,
            )
        subprocess.run(
            [str(cls.directory / "emit"), str(cls.directory)],
            check=True,
            env=cls.environment,
        )
        cls.records = json.loads((cls.directory / "records.json").read_text())
        cls.args = [
            str(cls.directory / "extract"),
            str(cls.directory / "index.dndsidx"),
            str(cls.directory / "manifest.json"),
            hashlib.sha256((cls.directory / "index.dndsidx").read_bytes()).hexdigest(),
            hashlib.sha256((cls.directory / "manifest.json").read_bytes()).hexdigest(),
            "d" * 64,
            "dnd_text_chunks_c_" + "a" * 64,
            json.loads((cls.directory / "manifest.json").read_text())["corpus_id"],
            "dnd-5e-2014",
            "--all-phb",
        ]

    def test_non_terminated_artifact_spans_preserve_exact_text_and_hash(self):
        result = subprocess.check_output(self.args, env=self.environment)
        exported = json.loads(result)
        expected = [r for r in self.records if r["book_slug"] == "players-handbook"]
        self.assertGreater(len(expected), 1)
        self.assertEqual(exported["entries"], [])
        self.assertEqual(exported["records"], expected)
        for row in exported["records"]:
            self.assertEqual(
                hashlib.sha256(row["content"].encode()).hexdigest(), row["content_hash"]
            )

    def test_wrong_pins_produce_no_records(self):
        for index in (3, 4, 5):
            with self.subTest(argument=index):
                args = list(self.args)
                args[index] = "0" * 64
                result = subprocess.run(args, capture_output=True, env=self.environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
