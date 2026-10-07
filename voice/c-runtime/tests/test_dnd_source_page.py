"""Compare C alignment with exhaustive candidate paths on small pages."""

import ctypes
import itertools
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

RUNTIME = Path(__file__).resolve().parents[1]


class Chunk(ctypes.Structure):
    _fields_ = [
        ("text", ctypes.c_char_p),
        ("length", ctypes.c_size_t),
        ("chunk", ctypes.c_uint32),
    ]


class Alignment(ctypes.Structure):
    _fields_ = [("begin", ctypes.c_uint32 * 128), ("count", ctypes.c_size_t)]


def exhaustive(page, chunks):
    positions = [
        [i for i in range(len(page)) if page[i : i + len(chunk)] == chunk]
        for chunk in chunks
    ]
    valid = []
    for starts in itertools.product(*positions):
        ends = [start + len(chunk) for start, chunk in zip(starts, chunks, strict=True)]
        if starts[0] != 0 or ends[-1] != len(page):
            continue
        if list(starts) != sorted(starts) or ends != sorted(ends):
            continue
        covered = set()
        for start, end in zip(starts, ends, strict=True):
            covered.update(range(start, end))
        if len(covered) == len(page):
            valid.append(starts)
    return valid


class NativeAlignment(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="dnd-source-page-")
        cls.addClassCleanup(cls.directory.cleanup)
        binary = Path(cls.directory.name) / "page.so"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wconversion",
                "-Wsign-conversion",
                "-fPIC",
                "-shared",
                "-I"
                + str(RUNTIME.parents[1] / "product/companions-frontend/c-companions"),
                str(RUNTIME / "common/dnd_source_page.c"),
                str(RUNTIME / "common/utf8.c"),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
        )
        cls.library = ctypes.CDLL(str(binary))
        cls.align = cls.library.dnd_source_page_align
        cls.align.argtypes = [
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(Chunk),
            ctypes.c_size_t,
            ctypes.POINTER(Alignment),
        ]
        cls.align.restype = ctypes.c_int

    def test_exhaustive_paths(self):
        randomizer = random.Random(79021)
        counts = [0, 0, 0]
        for _ in range(1500):
            page = bytes(
                randomizer.choice(b"abc") for _ in range(randomizer.randint(1, 8))
            )
            chunks = []
            for _ in range(randomizer.randint(1, 4)):
                start = randomizer.randrange(len(page))
                end = randomizer.randint(start + 1, len(page))
                chunks.append(page[start:end])
            expected = exhaustive(page, chunks)
            native = (Chunk * len(chunks))(
                *(Chunk(chunk, len(chunk), i) for i, chunk in enumerate(chunks))
            )
            out = Alignment()
            ctypes.memset(ctypes.byref(out), 0xA5, ctypes.sizeof(out))
            result = self.align(page, len(page), native, len(native), ctypes.byref(out))
            with self.subTest(page=page, chunks=chunks):
                if len(expected) == 1:
                    counts[0] += 1
                    self.assertEqual(result, 0)
                    self.assertEqual(out.count, len(chunks))
                    self.assertEqual(tuple(out.begin[: out.count]), expected[0])
                else:
                    status = 2 if expected else 1
                    counts[status] += 1
                    self.assertEqual(result, status)
                    self.assertEqual(bytes(out), bytes(ctypes.sizeof(out)))
        self.assertTrue(all(counts), counts)


if __name__ == "__main__":
    unittest.main()
