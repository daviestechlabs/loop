"""Offline adapter and independent reference for explicit quote contexts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE_FILES = ("voice/c-runtime/common/dnd_claim_evidence.h", "voice/c-runtime/common/utf8.c",
                "voice/c-runtime/common/utf8.h", "benchmarks/response-quality/turn-repair-v1/claim_evidence_probe.c",
                "benchmarks/response-quality/turn-repair-v1/claim_evidence.py")


def oracle(text, context, quote):
    empty = (0, 0, 0, 0)
    if not 0 < len(text) <= 16384 or len(context) > 4096 or not 0 < len(quote) <= 2048:
        return (0, *empty)
    try:
        for value in (text, context, quote):
            value.decode("utf-8", errors="strict")
            if b"\0" in value:
                return (0, *empty)
    except UnicodeDecodeError:
        return (0, *empty)
    occurrences = lambda raw, part: [m.start() for m in re.finditer(b"(?=" + re.escape(part) + b")", raw)]
    begin, end = 0, len(text)
    if context:
        matches = occurrences(text, context)
        if len(matches) != 1:
            return (-1 if not matches else -2, *empty)
        begin, end = matches[0], matches[0] + len(context)
    matches = occurrences(text[begin:end], quote)
    if len(matches) != 1:
        return (-3 if not matches else -4, *empty)
    return (1, begin, end, begin + matches[0], begin + matches[0] + len(quote))


def compile_probe(output, include_root=None):
    binary = output / "claim-evidence-probe"
    includes = [] if include_root is None else ["-I" + str(include_root)]
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wconversion", "-Wsign-conversion", "-Wshadow", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", *includes, "-I" + str(ROOT / "voice/c-runtime"),
               str(HERE / "claim_evidence_probe.c"), str(ROOT / "voice/c-runtime/common/utf8.c"),
               "-o", str(binary)]
    subprocess.run(command, check=True, capture_output=True)
    return binary, command


def sanitizer_env():
    # The kernel allocates nothing. LeakSanitizer cannot trace this restricted
    # CI process; address and undefined-behavior checks remain fail-fast.
    return dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")


def execute(binary, rows):
    rows = list(rows)
    for text, context, quote in rows:
        if len(text) > 16385 or len(context) > 4097 or len(quote) > 2049:
            raise ValueError("Probe input exceeds the explicit one-byte overflow controls")
    raw = b"".join(struct.pack(">III", len(text), len(context), len(quote)) + text + context + quote
                   for text, context, quote in rows)
    result = subprocess.run([str(binary)], input=raw, capture_output=True, check=True, timeout=30, env=sanitizer_env())
    values = [tuple(map(int, line.split())) for line in result.stdout.decode().splitlines()]
    if len(values) != len(rows) or any(len(v) != 5 for v in values):
        raise ValueError("Probe response shape differs")
    return values


def collect(output):
    names = (*SOURCE_FILES, "benchmarks/response-quality/turn-repair-v1/test_claim_evidence.py",
             "benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    saved = {name: (ROOT / name).read_bytes() for name in names}
    for name, raw in saved.items():
        if raw != subprocess.check_output(["git", "show", revision + ":" + name], cwd=ROOT):
            raise ValueError("Uncommitted evidence source: " + name)
    output.mkdir()
    for name, raw in saved.items():
        path = output / "source" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    command = [sys.executable, "-m", "unittest", "discover", "-s", str(HERE), "-p", "test_claim_evidence.py", "-v"]
    test = subprocess.run(command, capture_output=True, timeout=120)
    (output / "tests.stdout").write_bytes(test.stdout)
    (output / "tests.stderr").write_bytes(test.stderr)
    match = re.search(rb"Ran (\d+) tests? in ", test.stderr)
    tests_run = int(match[1]) if match else 0
    compiler = subprocess.check_output(["cc", "--version"], text=True)
    report = {"source_revision": revision, "command": command, "exit_code": test.returncode, "tests_run": tests_run,
              "compiler": compiler, "python": sys.version, "scope": "C byte-membership kernel; no model intervention",
              "live_activation": False, "training_admitted": False, "entailment_proven": False}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, raw in saved.items():
        if raw != (ROOT / name).read_bytes():
            raise ValueError("Evidence source changed during execution: " + name)
    files = {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in output.rglob("*") if p.is_file() and not p.name.startswith("._")}
    (output / "receipt.json").write_text(json.dumps({"complete": test.returncode == 0 and tests_run > 0, "files": files}, indent=2) + "\n")
    if test.returncode or not tests_run:
        raise RuntimeError("Claim evidence checks failed; retained output is not a passing proof")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(collect(args.output), indent=2))
