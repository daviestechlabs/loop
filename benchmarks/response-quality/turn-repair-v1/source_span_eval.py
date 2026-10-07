"""Offline paired source-boundary and coverage experiment; no product promotion."""

import argparse
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from empty_context_eval import answer
from semantic_eval import Client, sha, write
from verify_coupled_dialogue import model_catalog, require

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "voice/c-runtime/tests"))
from test_dnd_source_compiler import decode  # noqa: E402

CONDITIONS = ("whole_chunks", "faith_spans", "both_rule_spans")
SEEDS = (0, 1, 2)
CASES = (
    (
        "unknown_capability",
        "Using the 2014 rules, should I protect Mira with Sanctuary or Shield of Faith? You do not know my class, prepared spells, slots, or the threat.",
        "Ask for missing capabilities. Give only supported tradeoffs. Do not assign Shield effects to Sanctuary.",
    ),
    (
        "effect_attribution",
        "Using the 2014 rules, does Sanctuary give Mira +5 AC and protection from magic missile, or am I mixing up spells?",
        "Do not assign Shield effects to Sanctuary. Distinguish absent evidence from a supported explanation.",
    ),
    (
        "sanctuary_area",
        "Using the 2014 rules, will Sanctuary on Mira protect her from a Fireball explosion?",
        "Only the complete condition supplies Sanctuary. It explicitly excludes area effects.",
    ),
    (
        "faith_timing",
        "Using the 2014 rules, does casting Shield of Faith use my reaction or my bonus action, and does it require concentration?",
        "All conditions support bonus action and concentration. Preserve this control answer.",
    ),
)
SOURCE_FILES = (
    "source_span_eval.py",
    "empty_context_eval.py",
    "semantic_eval.py",
    "verify_coupled_dialogue.py",
    "dialogue_case_pack.py",
)


def read(path):
    return json.loads(path.read_text())


def reconstruct(records):
    require(records, "Missing page records")
    size = max(r["begin"] + len(r["content"].encode()) for r in records)
    require(0 < size <= 65536, "Page size exceeds bound")
    data, seen = bytearray(size), bytearray(size)
    for record in records:
        raw = record["content"].encode()
        require(sha(raw) == record["hash"], "Record content hash differs")
        for at, value in enumerate(raw, record["begin"]):
            require(not seen[at] or data[at] == value, "Overlapping records disagree")
            data[at], seen[at] = value, 1
    require(all(seen), "Page has uncovered bytes")
    data.decode()  # Reject broken UTF-8 before identifying any heading.
    return bytes(data)


def heading(raw, name, following):
    # Retain the source bytes; spacing tolerance locates OCR headings only.
    pattern = r"(?<![A-Za-z])" + r"\s*".join(re.escape(c) for c in name if c != " ")
    pattern += r"\s+(?=" + re.escape(following) + r")"
    text = raw.decode()
    hits = list(re.finditer(pattern, text, flags=re.I))
    require(len(hits) == 1, "Missing or ambiguous source heading: " + name)
    return len(text[: hits[0].start()].encode())


def clipped(record, begin, end):
    raw = record["content"].encode()
    a, b = max(begin, record["begin"]), min(end, record["begin"] + len(raw))
    require(a < b, "Empty source intersection")
    local_begin, local_end = a - record["begin"], b - record["begin"]
    return {
        "begin": local_begin,
        "end": local_end,
        "content": raw[local_begin:local_end].decode(),
    }


def render(records, message):
    text = "Retrieved excerpts (source data):\n"
    for index, record in enumerate(records, 1):
        c = record["citation"]
        text += f"\n[Source {index}; name {c['book_slug']}; record {c['record_id']}; pages {c['page_start']}-{c['page_end']}; section {c['section']}; chunk {c['chunk_index']}]\n{record['content']}\n[End source {index}]\n"
    return text + "\nQuestion: " + message


def prepare(source, baseline):
    receipt = read(source / "receipt.json")
    for name in ("index.dndsidx", "manifest.json"):
        require(
            sha((source / name).read_bytes()) == receipt["files"][name]["sha256"],
            "Source pin differs",
        )
    data = decode((source / "index.dndsidx").read_bytes())
    trial = read(baseline / "trial.json")
    exchanges = read(baseline / "model-exchanges.json")
    require(
        len(exchanges) == 1 and exchanges[0]["status"] == 200, "Invalid model template"
    )
    template = exchanges[0]["request"]
    require(
        [m["role"] for m in template["messages"]] == ["system", "user"],
        "Unexpected model history",
    )
    final = next(e for e in trial["events"] if e["type"] == "text_completed")
    citations = json.loads(final["metadata"]["cascade_retrieval_citations"])
    require(len(citations) == 2, "Expected two baseline source chunks")
    indexed = {r["id"]: r for r in data["records"]}
    records = []
    for citation in citations:
        record = indexed[citation["record_id"]]
        doc = data["documents"][record["document"]]
        require(
            doc
            == [
                citation["document_id"],
                citation["book_slug"],
                citation["source_sha256"],
            ],
            "Source identity differs",
        )
        require(
            record["hash"]
            == citation["content_hash"]
            == sha(record["content"].encode()),
            "Citation hash differs",
        )
        require(
            record["page"] == citation["page_start"] == citation["page_end"],
            "Citation page differs",
        )
        require(record["chunk"] == citation["chunk_index"], "Citation chunk differs")
        records.append({"citation": citation, "content": record["content"]})
    require(
        render(records, CASES[0][1]) == template["messages"][1]["content"],
        "Baseline request differs",
    )
    documents = {indexed[c["record_id"]]["document"] for c in citations}
    require(len(documents) == 1, "Mixed baseline printings")
    document = documents.pop()
    entries = [
        e
        for e in data["entries"]
        if e["document"] == document and e["name"] == "shieldoffaith"
    ]
    require(
        len(entries) == 1 and entries[0]["body"]["count"] and entries[0]["flags"] & 4,
        "Missing complete spell entry",
    )
    entry = entries[0]
    expected_ids = {
        data["records"][s["record"]]["id"]
        for s in entry["body"]["spans"][: entry["body"]["count"]]
    }
    require(
        expected_ids == {c["record_id"] for c in citations},
        "Baseline is not the exact spell group",
    )
    spans, evidence = [], []
    for item in records:
        record = indexed[item["citation"]["record_id"]]
        cut = clipped(record, entry["heading"]["begin"], entry["body"]["end"])
        spans.append({"citation": item["citation"], "content": cut["content"]})
        evidence.append(
            {
                "rule": "shield_of_faith",
                "record_id": record["id"],
                "content_hash": record["hash"],
                **cut,
            }
        )
    # This research boundary is explicitly reviewed and is not a production
    # cross-page compiler rule. It cannot silently select another printing.
    doc_id = data["documents"][document][0]
    require(doc_id == "7443e4b409ddaba7b55c3018", "Unreviewed Sanctuary printing")
    pages = {
        page: sorted(
            [
                r
                for r in data["records"]
                if r["document"] == document and r["page"] == page
            ],
            key=lambda r: r["chunk"],
        )
        for page in (250, 251)
    }
    raw = {page: reconstruct(rows) for page, rows in pages.items()}
    start = heading(raw[250], "Sanctuary", "1st-level abjuration")
    end = heading(raw[251], "Scorching Ray", "2nd-level evocation")
    while end and raw[251][end - 1 : end] == b" ":
        end -= 1
    require(raw[251][end - 1 : end] == b".", "Sanctuary continuation is incomplete")
    extra = []
    for page, begin, finish in ((250, start, len(raw[250])), (251, 0, end)):
        for record in pages[page]:
            if (
                record["begin"] >= finish
                or record["begin"] + len(record["content"].encode()) <= begin
            ):
                continue
            cut = clipped(record, begin, finish)
            citation = dict(
                citations[0],
                record_id=record["id"],
                content_hash=record["hash"],
                page_start=page,
                page_end=page,
                chunk_index=record["chunk"],
                section="",
            )
            extra.append({"citation": citation, "content": cut["content"]})
            evidence.append(
                {
                    "rule": "sanctuary",
                    "record_id": record["id"],
                    "content_hash": record["hash"],
                    **cut,
                }
            )
    require(len(extra) == 3, "Sanctuary record coverage changed")
    return {
        "template": template,
        "conditions": {
            "whole_chunks": records,
            "faith_spans": spans,
            "both_rule_spans": spans + extra,
        },
        "spans": evidence,
        "source_index_sha256": receipt["files"]["index.dndsidx"]["sha256"],
        "source_manifest_sha256": receipt["files"]["manifest.json"]["sha256"],
        "baseline_request_sha256": sha(
            json.dumps(template, sort_keys=True, separators=(",", ":")).encode()
        ),
    }


def matrix():
    for seed in SEEDS:
        for index, (case_id, message, _) in enumerate(CASES):
            offset = (seed + index) % len(CONDITIONS)
            for condition in CONDITIONS[offset:] + CONDITIONS[:offset]:
                yield seed, case_id, message, condition


def request(prepared, message, condition, seed):
    require(condition in CONDITIONS and seed in SEEDS, "Unknown experiment condition")
    body = copy.deepcopy(prepared["template"])
    body["messages"][1]["content"] = render(prepared["conditions"][condition], message)
    body.update(stream=False, seed=seed)
    return body


def verify(root, source, baseline):
    manifest = read(root / "manifest.json")
    required = set(SOURCE_FILES) | {
        "source_decoder.py",
        "prepared.json",
        "protocol.json",
        "trials.jsonl",
        "models-before.json",
        "models-after.json",
    }
    require(set(manifest["files"]) == required, "Evidence file set changed")
    for name, pin in manifest["files"].items():
        path = Path(name)
        require(
            not path.is_absolute() and ".." not in path.parts, "Unsafe evidence path"
        )
        require(sha((root / path).read_bytes()) == pin, "Changed evidence: " + name)
    protocol = read(root / "protocol.json")
    require(protocol["source_dirty"] is False, "Uncommitted experiment source")
    require(
        protocol["cases"] == [list(c) for c in CASES]
        and protocol["seeds"] == list(SEEDS)
        and protocol["conditions"] == list(CONDITIONS),
        "Experiment design changed",
    )
    expected = prepare(source, baseline)
    require(
        read(root / "prepared.json") == expected,
        "Source intervention differs from pinned records",
    )
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    require(len(rows) == len(list(matrix())) == manifest["trials"], "Missing trials")
    for row, (seed, case_id, message, condition) in zip(rows, matrix(), strict=True):
        require(
            (row["seed"], row["case_id"], row["condition"])
            == (seed, case_id, condition),
            "Trial order differs",
        )
        require(
            row["request"] == request(expected, message, condition, seed),
            "Request differs from intervention",
        )
        require(
            row["answer"] == answer(row["response"]),
            "Answer differs from model response",
        )
    require(
        model_catalog(read(root / "models-before.json"))
        == model_catalog(read(root / "models-after.json")),
        "Model catalog changed",
    )
    return {
        "verified_trials": len(rows),
        "verified_spans": len(expected["spans"]),
        "quality_graded": False,
        "production_gate": False,
        "training_approved": False,
    }


def run(endpoint, source, baseline, output):
    os.umask(0o077)
    prepared = prepare(source, baseline)
    output.mkdir(parents=True, exist_ok=False)
    write(output / "prepared.json", prepared)
    for name in SOURCE_FILES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "source_decoder.py").write_bytes(
        (ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes()
    )
    write(
        output / "protocol.json",
        {
            "cases": CASES,
            "conditions": CONDITIONS,
            "seeds": SEEDS,
            "source_revision": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
            ).strip(),
            "source_dirty": bool(
                subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)
            ),
            "scope": "Direct real-model text experiment. Same system prompt and generation settings; paired seeds and rotated order. Exact span bytes retain original record IDs and hashes. The complete condition has five records, exceeding the current four-record product limit. No product, speech, latency, source publication or training claim.",
        },
    )
    client = Client(endpoint)
    before = client.request("/v1/models")
    require(
        prepared["template"]["model"] in [m["id"] for m in before["data"]],
        "Model alias unavailable",
    )
    write(output / "models-before.json", before)
    with (output / "trials.jsonl").open("x") as journal:
        for seed, case_id, message, condition in matrix():
            body = request(prepared, message, condition, seed)
            start = time.monotonic()
            response = client.request("/v1/chat/completions", body)
            row = {
                "seed": seed,
                "case_id": case_id,
                "condition": condition,
                "request": body,
                "response": response,
                "answer": answer(response),
                "response_ms": (time.monotonic() - start) * 1000,
            }
            journal.write(json.dumps(row, ensure_ascii=False) + "\n")
            journal.flush()
            print(
                json.dumps(
                    {k: row[k] for k in ("seed", "case_id", "condition", "answer")}
                ),
                flush=True,
            )
    write(output / "models-after.json", client.request("/v1/models"))
    pins = {
        p.name: sha(p.read_bytes())
        for p in output.iterdir()
        if p.is_file() and not p.name.startswith("._")
    }
    write(output / "manifest.json", {"files": pins, "trials": len(list(matrix()))})
    print(json.dumps(verify(output, source, baseline)), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if args.verify:
        print(json.dumps(verify(args.output, args.source, args.baseline), indent=2))
    else:
        run(args.endpoint, args.source, args.baseline, args.output)
