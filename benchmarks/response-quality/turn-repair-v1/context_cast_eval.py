"""Prospective multi-cast evidence contexts; offline claims, never game authority."""

import copy
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import tempfile
import time

import claim_evidence as c
import multi_cast_eval as m

e = m.e
HERE = m.HERE
POLICIES = ("global_quote", "explicit_context")
SOURCES = (*m.SOURCES, "claim_evidence.py", "claim_evidence_probe.c", "context_cast_eval.py", "analyze_context_cast.py")
KERNEL_FILES = ("dnd_claim_evidence.h", "utf8.h", "utf8.c")
INSTRUCTION = m.INSTRUCTION.replace(
    "The five fields each contain value and quote.",
    "The five fields each contain value, quote, and context.").replace(
    "Every known field needs a unique exact nonempty quote from the utterance. ",
    "Every known field needs exact nonempty evidence from the utterance. ").replace(
    "Unknown and unresolved fields may have empty quotes.",
    "Unknown and unresolved fields may have empty quotes only with empty contexts.")
EVIDENCE = {
    "global_quote": " Context must always be empty. Every nonempty quote must occur exactly once in the complete utterance. Expand repeated short quotes until the quote is globally unique.",
    "explicit_context": " Context may be empty when the quote occurs exactly once in the complete utterance. Otherwise supply an exact context that occurs exactly once in the utterance and contains exactly one occurrence of the quote. Choose context that supports this field for this event, including its corrections. Never choose an arbitrary occurrence or borrow another event's claim.",
}


@dataclass(frozen=True)
class Utterance:
    text: str
    sha256: str

    def __post_init__(self):
        e.a.require(type(self.text) is str, "Invalid utterance")
        raw = self.text.encode()
        e.a.require(0 < len(raw) <= 16384 and b"\0" not in raw, "Invalid utterance bytes")
        e.a.require(e.a.digest(raw) == self.sha256, "Utterance identity differs")


def binding(case):
    return Utterance(case["utterance"], e.a.digest(case["utterance"].encode()))


def parse(text, utterance, policy, binary):
    e.a.require(policy in POLICIES, "Unknown policy")
    e.a.require(len(text.encode()) <= 196608, "Oversized context frame")
    frame = json.loads(text, object_pairs_hook=e.a.unique_object)
    e.a.require(type(frame) is dict and set(frame) == {"events"}, "Frame fields differ")
    events = frame["events"]
    e.a.require(type(events) is list and len(events) <= m.MAX_EVENTS, "Event count invalid")
    raw = utterance.text.encode()
    rows, slots, result = [], [], []
    for index, event in enumerate(events):
        e.a.require(type(event) is dict and set(event) == {"anchor", *e.FIELDS}, "Event fields differ")
        e.a.require(type(event["anchor"]) is str and 0 < len(event["anchor"].encode()) <= 2048, "Invalid anchor")
        rows.append((raw, b"", event["anchor"].encode()))
        slots.append((index, "anchor"))
        entry = {"values": {}, "quote_spans": {}, "context_spans": {}}
        for field, allowed in e.FIELDS.items():
            item = event[field]
            e.a.require(type(item) is dict and set(item) == {"value", "quote", "context"}, "Field shape differs")
            value, quote, context = (item[k] for k in ("value", "quote", "context"))
            e.a.require(type(value) is str and value in allowed, "Invalid value")
            e.a.require(type(quote) is str and len(quote.encode()) <= 2048, "Invalid quote")
            e.a.require(type(context) is str and len(context.encode()) <= 4096, "Invalid context")
            e.a.require(policy != "global_quote" or not context, "Global condition supplied context")
            e.a.require(bool(quote) or (value in {"unknown", "unresolved"} and not context), "Missing field evidence")
            entry["values"][field] = value
            entry["quote_spans"][field] = None
            entry["context_spans"][field] = None
            if quote:
                rows.append((raw, context.encode(), quote.encode()))
                slots.append((index, field))
        result.append(entry)
    try:
        resolved = c.execute(binary, rows)
    except ValueError as error:
        raise RuntimeError("C evidence process returned invalid output") from error
    # Kernel/reference disagreement aborts the experiment, not an ordinary model rejection.
    if resolved != [c.oracle(*r) for r in rows]:
        raise RuntimeError("C evidence resolver disagrees with independent reference")
    for (index, field), spans in zip(slots, resolved, strict=True):
        status, cb, ce, qb, qe = spans
        e.a.require(status == 1, f"Evidence rejected: event={index} field={field} status={status}")
        def record(begin, end):
            return {"begin": begin, "end": end, "sha256": e.a.digest(raw[begin:end]),
                    "utterance_sha256": utterance.sha256}
        if field == "anchor":
            result[index]["anchor"] = record(qb, qe)
        else:
            result[index]["quote_spans"][field] = record(qb, qe)
            result[index]["context_spans"][field] = record(cb, ce)
    e.a.require(all(a["anchor"]["end"] <= b["anchor"]["begin"] for a, b in zip(result, result[1:])), "Anchors overlap or are out of order")
    return result


def assess(response, case, facts, policy, binary):
    utterance = binding(case)
    try:
        extracted = parse(e.a.answer(response), utterance, policy, binary)
    except (ValueError, KeyError, TypeError) as error:
        return {"accepted": False, "error": str(error), "utterance_sha256": utterance.sha256}
    expected = m.parse(json.dumps({"events": case["events"]}), case["utterance"])
    matches, unmatched, ambiguous = {}, [], []
    for index, event in enumerate(extracted):
        targets = [j for j, target in enumerate(expected)
                   if event["anchor"]["begin"] < target["anchor"]["end"] and
                   target["anchor"]["begin"] < event["anchor"]["end"]]
        if len(targets) != 1 or targets[0] in matches:
            (ambiguous if targets else unmatched).append(index)
        else:
            matches[targets[0]] = index
    checks = []
    for target_index, index in sorted(matches.items()):
        values = extracted[index]["values"]
        wrong = [k for k in e.FIELDS if values[k] != expected[target_index]["values"][k]]
        joined = e.join(values, facts)
        checks.append({"target_event": target_index, "extracted_event": index, "incorrect_fields": wrong,
                       "exact_event": not wrong, "join": joined,
                       "join_matches_authored_target": joined["kind"] == case["expected_joins"][target_index]})
    missing = sorted(set(range(len(expected))) - matches.keys())
    covered = not (missing or unmatched or ambiguous)
    return {"accepted": True, "utterance_sha256": utterance.sha256, "extracted": extracted,
            "expected_event_count": len(expected), "missing_events": missing, "unmatched_events": unmatched,
            "ambiguous_event_anchors": ambiguous, "checks": checks,
            "exact_all_events": covered and all(x["exact_event"] for x in checks),
            "all_joins_match": covered and all(x["join_matches_authored_target"] for x in checks),
            "entailment_proven": False, "whole_history_proven": False, "resource_state_authorized": False}


def response_format():
    schema = m.response_format()
    schema["json_schema"]["name"] = "waterdeep_context_cast_v1"
    props = schema["json_schema"]["schema"]["properties"]["events"]["items"]["properties"]
    for field in e.FIELDS:
        props[field]["properties"]["context"] = {"type": "string", "maxLength": 4096}
        props[field]["required"].append("context")
    return schema


def request(template, case, policy, seed):
    e.a.require(policy in POLICIES, "Unknown policy")
    body = copy.deepcopy(template)
    e.a.require("response_format" not in body and "structured_outputs" not in body, "Constrained template")
    body.update(messages=[{"role": "system", "content": INSTRUCTION + EVIDENCE[policy]},
                          {"role": "user", "content": case["utterance"]}],
                stream=False, seed=seed, max_completion_tokens=3072, response_format=response_format())
    return body


def matrix():
    for index, case in enumerate(m.cases()):
        for policy in POLICIES[::1 if index % 2 == 0 else -1]:
            yield 0, case, policy


def protocol():
    return {"policies": list(POLICIES), "seeds": [0], "trials": 32, "schema": response_format(),
            "socket_timeout_seconds": 240, "max_completion_tokens": 3072,
            "intervention": "Evidence instruction and associated acceptance rule; identical schema, task instruction, and sampling",
            "primary": "Accepted evidence and exact authored events, including coverage; semantic entailment not independently graded",
            "scope": "Same sixteen visible development cases; new responses only; no history closure, resource authority, training, or activation"}


def compile_archive(root, build):
    binary = build / "context-claim-probe"
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wconversion",
               "-Wsign-conversion", "-Wshadow", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(root / "kernel"), str(root / "claim_evidence_probe.c"),
               str(root / "kernel/common/utf8.c"), "-o", str(binary)]
    subprocess.run(command, check=True, capture_output=True)
    return binary, command


def snapshots(output):
    for name in SOURCES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "kernel/common").mkdir(parents=True)
    for name in KERNEL_FILES:
        (output / "kernel/common" / name).write_bytes((c.ROOT / "voice/c-runtime/common" / name).read_bytes())
    (output / "source-decoder.py").write_bytes((c.ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes())
    (output / "cases.json").write_bytes(m.DATA.read_bytes())


def verify(root, source, facts_directory, facts_receipt_sha256, template_path):
    facts, template = e.inputs(source, facts_directory, facts_receipt_sha256, template_path)
    manifest = e.a.read(root / "manifest.json")
    required = {*SOURCES, *("kernel/common/" + n for n in KERNEL_FILES), "cases.json", "facts.json", "template.json",
                "protocol.json", "trials.jsonl", "models-before.json", "models-after.json", "source-decoder.py", "build.json"}
    e.a.require(required <= manifest["files"].keys(), "Missing experiment evidence")
    for name, sha in manifest["files"].items():
        path = Path(name)
        e.a.require(not path.is_absolute() and ".." not in path.parts, "Unsafe evidence path")
        path = root / path
        e.a.require(path.resolve().is_relative_to(root.resolve()) and not path.is_symlink() and e.a.digest(path.read_bytes()) == sha, "Evidence changed")
    # Use the pinned evaluator checkout. C is rebuilt from retained source, never a supplied binary.
    for name in SOURCES:
        e.a.require((root / name).read_bytes() == (HERE / name).read_bytes(), "Evaluator source differs")
    for name in KERNEL_FILES:
        e.a.require((root / "kernel/common" / name).read_bytes() == (c.ROOT / "voice/c-runtime/common" / name).read_bytes(), "Kernel source differs")
    e.a.require(e.a.read(root / "facts.json") == facts and e.a.read(root / "template.json") == template, "Inputs differ")
    e.a.require(e.a.read(root / "protocol.json") == protocol() and e.a.read(root / "cases.json") == e.a.read(m.DATA), "Protocol or cases differ")
    e.a.require(manifest["facts_receipt_sha256"] == facts_receipt_sha256 and manifest["template_sha256"] == e.a.digest(template_path.read_bytes()), "Input pins differ")
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    expected = list(matrix())
    e.a.require(len(rows) == len(expected) == manifest["trials"], "Trial count differs")
    with tempfile.TemporaryDirectory() as tmp:
        binary, _ = compile_archive(root, Path(tmp))
        for row, (seed, case, policy) in zip(rows, expected, strict=True):
            e.a.require((row["seed"], row["case_id"], row["policy"]) == (seed, case["id"], policy), "Trial order differs")
            e.a.require(row["request"] == request(template, case, policy, seed), "Request differs")
            e.a.require(row["assessment"] == assess(row["response"], case, facts, policy, binary), "Assessment differs")
            e.a.require(type(row["request_elapsed_ms"]) is int and row["request_elapsed_ms"] >= 0, "Invalid duration")
    e.a.require(e.a.model_catalog(e.a.read(root / "models-before.json")) == e.a.model_catalog(e.a.read(root / "models-after.json")), "Model catalog changed")
    return {"verified_trials": len(rows), "accepted_frames": sum(r["assessment"]["accepted"] for r in rows),
            "exact_all_events": sum(r["assessment"].get("exact_all_events", False) for r in rows),
            "training_admitted": False, "production_path": False}


def run(endpoint, source, facts_directory, facts_receipt_sha256, template_path, output):
    facts, template = e.inputs(source, facts_directory, facts_receipt_sha256, template_path)
    output.mkdir()
    snapshots(output)
    for name, value in (("facts", facts), ("template", template), ("protocol", protocol())):
        e.a.write(output / (name + ".json"), value)
    with tempfile.TemporaryDirectory() as tmp:
        binary, command = compile_archive(output, Path(tmp))
        e.a.write(output / "build.json", {"command": command, "compiler": subprocess.check_output(["cc", "--version"], text=True),
                                          "binary_sha256": e.a.digest(binary.read_bytes())})
        client = e.a.Client(endpoint, timeout_seconds=240)
        before = client.request("/v1/models")
        e.a.require(template["model"] in [model["id"] for model in before["data"]], "Requested model absent")
        e.a.write(output / "models-before.json", before)
        count = 0
        with (output / "trials.jsonl").open("x") as stream:
            for seed, case, policy in matrix():
                body = request(template, case, policy, seed)
                start = time.perf_counter_ns()
                response = client.request("/v1/chat/completions", body)
                elapsed = (time.perf_counter_ns() - start) // 1_000_000
                assessment = assess(response, case, facts, policy, binary)
                stream.write(json.dumps({"seed": seed, "case_id": case["id"], "policy": policy,
                                        "request": body, "response": response, "request_elapsed_ms": elapsed,
                                        "assessment": assessment}) + "\n")
                stream.flush()
                count += 1
                print(f"{count}/32 {case['id']} {policy} accepted={assessment['accepted']} exact={assessment.get('exact_all_events', False)}", flush=True)
        e.a.write(output / "models-after.json", client.request("/v1/models"))
    e.a.write(output / "manifest.json", {"trials": count, "facts_receipt_sha256": facts_receipt_sha256,
                                        "template_sha256": e.a.digest(template_path.read_bytes()),
                                        "files": {str(p.relative_to(output)): e.a.digest(p.read_bytes()) for p in output.rglob("*")
                                                  if p.is_file() and not p.name.startswith("._")}})
    e.a.write(output / "verification.json", verify(output, source, facts_directory, facts_receipt_sha256, template_path))
