"""Matched prompt-only versus schema-constrained cast-event experiment."""

import json
from pathlib import Path
import time

import cast_event_eval as e
from cast_event_schema import constrained_request, response_format

SOURCES = (*e.SOURCES, "cast_event_schema.py", "cast_event_decoding_eval.py")
DECODINGS = ("prompt_only", "json_schema")


def matrix():
    for seed in (0, 1):
        for index, case in enumerate(e.cases()):
            for decoding in DECODINGS[::1 if (seed + index) % 2 == 0 else -1]:
                yield seed, case, decoding


def request(template, case, decoding, seed):
    e.a.require(decoding in DECODINGS, "Unknown decoding condition")
    baseline = e.request(template, case, "basic", seed)
    return constrained_request(baseline) if decoding == "json_schema" else baseline


def protocol():
    return {"decodings": list(DECODINGS), "seeds": [0, 1], "trials": 32,
            "fixed_prompt": "basic", "expected_join": e.EXPECTED_JOIN, "printing": e.PRINTING,
            "response_format": response_format(),
            "scope": "Paired output-format intervention; no training, complete history, budget, or product activation"}


def verify(root, source, facts_directory, facts_receipt_sha256, template_path):
    facts, template = e.inputs(source, facts_directory, facts_receipt_sha256, template_path)
    manifest = e.a.read(root / "manifest.json")
    required = {*SOURCES, "cases.json", "conditions.json", "facts.json", "template.json", "protocol.json",
                "trials.jsonl", "models-before.json", "models-after.json", "source-decoder.py"}
    e.a.require(required <= manifest["files"].keys(), "Missing experiment evidence")
    for name, sha in manifest["files"].items():
        rel = Path(name)
        e.a.require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe evidence path")
        path = root / rel
        e.a.require(not path.is_symlink() and e.a.digest(path.read_bytes()) == sha, "Experiment evidence changed")
    e.a.require(e.a.read(root / "facts.json") == facts and e.a.read(root / "template.json") == template, "Inputs differ")
    e.a.require(e.a.read(root / "protocol.json") == protocol(), "Protocol differs")
    for name in ("cases.json", "conditions.json"):
        e.a.require(e.a.read(root / name) == e.a.read(e.DATA / name), "Dataset differs")
    e.a.require(manifest["facts_receipt_sha256"] == facts_receipt_sha256 and
                manifest["template_sha256"] == e.a.digest(template_path.read_bytes()), "Input pins differ")
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    expected = list(matrix())
    e.a.require(len(rows) == len(expected) == manifest["trials"], "Trial count differs")
    for row, (seed, case, decoding) in zip(rows, expected, strict=True):
        e.a.require((row["seed"], row["case_id"], row["decoding"]) == (seed, case["id"], decoding), "Trial order differs")
        e.a.require(row["request"] == request(template, case, decoding, seed), "Request differs")
        e.a.require(row["assessment"] == e.assess(row["response"], case, facts), "Assessment differs")
        e.a.require(type(row["request_elapsed_ms"]) is int and row["request_elapsed_ms"] >= 0, "Invalid request duration")
    e.a.require(e.a.model_catalog(e.a.read(root / "models-before.json")) == e.a.model_catalog(e.a.read(root / "models-after.json")), "Model catalog changed")
    return {"verified_trials": len(rows), "accepted_frames": sum(r["assessment"]["accepted"] for r in rows),
            "training_admitted": False, "production_path": False}


def run(endpoint, source, facts_directory, facts_receipt_sha256, template_path, output):
    facts, template = e.inputs(source, facts_directory, facts_receipt_sha256, template_path)
    output.mkdir()
    for name in SOURCES:
        (output / name).write_bytes((e.HERE / name).read_bytes())
    (output / "source-decoder.py").write_bytes((e.s.ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes())
    for name in ("cases.json", "conditions.json"):
        (output / name).write_bytes((e.DATA / name).read_bytes())
    e.a.write(output / "facts.json", facts)
    e.a.write(output / "template.json", template)
    e.a.write(output / "protocol.json", protocol())
    client = e.a.Client(endpoint)
    before = client.request("/v1/models")
    e.a.require(template["model"] in [m["id"] for m in before["data"]], "Requested model is not served")
    e.a.write(output / "models-before.json", before)
    count = 0
    with (output / "trials.jsonl").open("x") as stream:
        for seed, case, decoding in matrix():
            body = request(template, case, decoding, seed)
            start = time.perf_counter_ns()
            response = client.request("/v1/chat/completions", body)
            elapsed = (time.perf_counter_ns() - start) // 1_000_000
            row = {"seed": seed, "case_id": case["id"], "decoding": decoding,
                   "request": body, "response": response, "request_elapsed_ms": elapsed,
                   "assessment": e.assess(response, case, facts)}
            stream.write(json.dumps(row) + "\n")
            stream.flush()
            count += 1
            print(f"{count}/32 {case['id']} {decoding} seed={seed} accepted={row['assessment']['accepted']}", flush=True)
    e.a.write(output / "models-after.json", client.request("/v1/models"))
    e.a.write(output / "manifest.json", {"trials": count, "facts_receipt_sha256": facts_receipt_sha256,
                                        "template_sha256": e.a.digest(template_path.read_bytes()),
                                        "files": {p.name: e.a.digest(p.read_bytes()) for p in output.iterdir() if p.is_file() and not p.name.startswith("._")}})
    e.a.write(output / "verification.json", verify(output, source, facts_directory, facts_receipt_sha256, template_path))
