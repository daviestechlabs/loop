"""Extract source-bound spell header facts with C; no model inference or admission."""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess

import action_budget_eval as ab
from passage_evidence import admitted_passages, digest
from semantic_eval import write
from verify_coupled_dialogue import read, require

HERE = Path(__file__).resolve().parent
ROOT = ab.ROOT
SOURCE_FILES = (
    "voice/c-runtime/common/dnd_source_spell.h",
    "voice/c-runtime/common/dnd_source_index.h",
    "voice/c-runtime/common/dnd_retrieval.h",
    "voice/c-runtime/common/dnd_retrieval_types.h",
    "product/companions-frontend/c-companions/cmp_json.h",
    "benchmarks/response-quality/turn-repair-v1/spell_source_probe.c",
    "voice/c-runtime/tests/test_dnd_source_compiler.py",
    *("benchmarks/response-quality/turn-repair-v1/" + name for name in ab.SOURCES),
)


def compile_probe(output):
    binary = output / "spell-source-probe"
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wconversion", "-Wsign-conversion", "-Wshadow",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(ROOT / "voice/c-runtime"),
               "-I" + str(ROOT / "product/companions-frontend/c-companions"),
               str(HERE / "spell_source_probe.c"), "-o", str(binary)]
    subprocess.run(command, check=True, capture_output=True)
    return binary, command


def extract(binary, rows):
    require(all(len(raw) <= 65536 for raw in rows), "Source span too large")
    payload = b"".join(struct.pack(">I", len(raw)) + raw for raw in rows)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    process = subprocess.run([str(binary)], input=payload, capture_output=True, env=env, check=True)
    result = [list(map(int, line.split())) for line in process.stdout.splitlines()]
    require(len(result) == len(rows) and all(len(row) == 9 for row in result), "Incomplete C results")
    for raw, row in zip(rows, result, strict=True):
        if row[0]:
            require(row[0] == 1 and 0 <= row[1] <= 9 and 0 <= row[2] <= 3, "Invalid fact value")
            require(0 <= row[5] < row[6] <= row[7] < row[8] <= len(raw), "Invalid fact range")
        else:
            require(row == [0, 255, 0, 0, 0, 0, 0, 0, 0], "Invalid header retained facts")
    return result


def source_passages(source, index_sha256, manifest_sha256):
    require(digest((source / "index.dndsidx").read_bytes()) == index_sha256, "Index pin differs")
    require(digest((source / "manifest.json").read_bytes()) == manifest_sha256, "Manifest pin differs")
    receipt = read(source / "receipt.json")
    require(receipt["files"]["index.dndsidx"]["sha256"] == index_sha256 and
            receipt["files"]["manifest.json"]["sha256"] == manifest_sha256, "Source receipt differs")
    passages = admitted_passages(ab.decode((source / "index.dndsidx").read_bytes()))
    rows = []
    for passage in passages:
        definition = passage["definition"]
        offset = definition["heading_end"] - definition["heading_begin"]
        raw = passage["raw"]
        require(0 < offset < len(raw), "Header offset differs")
        while offset < len(raw) and raw[offset] == 32:
            offset += 1
        require(offset < len(raw), "Missing header")
        rows.append({"name": passage["name"], "document": passage["document"],
                     "assembly_sha256": digest(raw), "header_offset": offset,
                     "definition": definition, "raw": raw})
    return rows


def collect(source, index_sha256, manifest_sha256, output):
    rows = source_passages(source, index_sha256, manifest_sha256)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    names = (*SOURCE_FILES, str(Path(__file__).relative_to(ROOT)))
    for name in names:
        committed = subprocess.check_output(["git", "show", revision + ":" + name], cwd=ROOT)
        require(committed == (ROOT / name).read_bytes(), "Uncommitted experiment source: " + name)
    output.mkdir()
    sources = output / "source"
    sources.mkdir()
    for name in SOURCE_FILES:
        target = sources / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())
    (output / "collector.py").write_bytes(Path(__file__).read_bytes())
    binary, command = compile_probe(output)
    results = extract(binary, [row["raw"][row["header_offset"]:] for row in rows])
    facts = []
    for row, result in zip(rows, results, strict=True):
        entry = {k: v for k, v in row.items() if k != "raw"}
        entry["valid_header"] = bool(result[0])
        if result[0]:
            entry.update(level=result[1], standard_cost=result[2], ritual_tag=bool(result[3]),
                         conditional_reaction=bool(result[4]))
            for name, begin, end in (("classification", result[5], result[6]), ("casting_time", result[7], result[8])):
                begin += row["header_offset"]
                end += row["header_offset"]
                entry[name] = {"begin": begin, "end": end, "sha256": digest(row["raw"][begin:end])}
        facts.append(entry)
    write(output / "facts.json", facts)
    write(output / "build.json", {"command": command, "binary_sha256": digest(binary.read_bytes()),
                                  "compiler": subprocess.check_output(["cc", "--version"], text=True).splitlines()[0]})
    write(output / "receipt.json", {"source_revision": revision, "index_sha256": index_sha256, "manifest_sha256": manifest_sha256,
                                    "passages": len(rows), "valid_headers": sum(row["valid_header"] for row in facts),
                                    "files": {str(p.relative_to(output)): digest(p.read_bytes()) for p in output.rglob("*") if p.is_file() and not p.name.startswith("._")},
                                    "source_admission_changed": False, "training_admitted": False,
                                    "scope": "Header facts only; does not establish actor, turn, occurrence, resources, modifiers, or casting permission"})
    return facts


def verify(output, source, index_sha256, manifest_sha256):
    """Reconstruct source spans and independently read the accepted header fields."""
    receipt = read(output / "receipt.json")
    require(receipt["index_sha256"] == index_sha256 and
            receipt["manifest_sha256"] == manifest_sha256, "Fact source pins differ")
    for name, sha in receipt["files"].items():
        rel = Path(name)
        require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe receipt path")
        path = output / rel
        require(not path.is_symlink() and digest(path.read_bytes()) == sha, "Fact evidence changed")
    rows = source_passages(source, index_sha256, manifest_sha256)
    facts = read(output / "facts.json")
    require(len(rows) == len(facts) == receipt["passages"], "Fact count differs")
    for row, fact in zip(rows, facts, strict=True):
        require(all(fact[k] == v for k, v in row.items() if k != "raw"), "Passage identity differs")
        # This verifier covers accepted facts only. Rejected headers remain
        # absent evidence; it cannot certify completeness of the C recognizer.
        if not fact["valid_header"]:
            continue
        raw = row["raw"]
        head = raw[row["header_offset"]:]
        level = re.match(rb"([1-9]) ?(?:st|nd|rd|th)-level ", head, re.I)
        cantrip = re.match(rb"[A-Za-z]+ (cantrip) ", head, re.I)
        require(level is not None or cantrip is not None, "Classification unreadable")
        expected_level = int(level[1]) if level else 0
        cast = re.search(rb"Casting Time: (.+?) +Range: ", head, re.I)
        require(cast is not None, "Casting time unreadable")
        cost = cast[1].lower()
        conditional = cost.startswith(b"1 reaction, ") and bool(cost[len(b"1 reaction, "):].strip())
        expected_cost = {b"1 action": 1, b"1 bonus action": 2, b"1 reaction": 3}.get(cost, 3 if conditional else 0)
        require((fact["level"], fact["standard_cost"], fact["conditional_reaction"], fact["ritual_tag"]) ==
                (expected_level, expected_cost, conditional, b"(ritual) " in head[:cast.start()].lower()), "Source facts disagree")
        classification = (0, level.end() - 1) if level else cantrip.span(1)
        for name, (begin, end) in (("classification", classification), ("casting_time", cast.span(1))):
            begin += row["header_offset"]
            end += row["header_offset"]
            require(fact[name] == {"begin": begin, "end": end, "sha256": digest(raw[begin:end])}, "Fact source range differs")
    require(sum(f["valid_header"] for f in facts) == receipt["valid_headers"], "Valid count differs")
    return facts


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--index-sha256", required=True)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    rows = collect(args.source, args.index_sha256, args.manifest_sha256, args.output)
    print(json.dumps({"passages": len(rows), "valid_headers": sum(r["valid_header"] for r in rows)}))
