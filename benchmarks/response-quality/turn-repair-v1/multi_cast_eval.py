"""Bounded multi-event extraction study; claims never authorize game actions."""

from collections import Counter
import copy
import json
from pathlib import Path
import time

import cast_event_eval as e
from cast_event_schema import response_format as single_format

HERE = e.HERE
DATA = HERE / "synthetic/multi-cast-20260928.json"
SOURCES = (*e.SOURCES, "cast_event_schema.py", "multi_cast_eval.py")
MAX_EVENTS = 8
POLICIES = ("basic", "scoped_repair")
INSTRUCTION = (
    "Extract all distinct spell-cast events mentioned in the utterance, after corrections. "
    "Return JSON with exactly events, an array of at most eight objects. "
    "Each object has anchor plus actor, time, status, spell, modifier. "
    "The five fields each contain value and quote. Allowed values: "
    + json.dumps({k: sorted(v) for k, v in e.FIELDS.items()}) + ". "
    "Anchor is a unique exact quote covering that event's original mention before corrections. "
    "Keep separate events separate, including repeated uses of the same spell. "
    "A correction updates its existing event; it is not a new event. "
    "Order events by their original mention. Return an empty array only when no cast event is mentioned. "
    "Actor is relative to the outer speaker. Time refers to the cast. "
    "Occurred means claimed completed; proposed means intended or hypothetical; negated means denied "
    "occurrence or a canceled plan; uncertain means unknown occurrence. "
    "Spell is named identity, never level or permission. An unnamed cantrip has unresolved identity. "
    "Use modifier none only for an explicit claim of no casting-time modifiers; otherwise use unknown. "
    "Every known field needs a unique exact nonempty quote from the utterance. "
    "Unknown and unresolved fields may have empty quotes. "
    "Extract claims, not authorized game state. Do not infer missing events or resource balances."
)
SCOPED_REPAIR = (
    " First identify distinct events and bind each correction to its named event. "
    "Replace only the corrected fields; preserve all other events and fields. "
    "A quoted character's I is not the outer speaker. Uncertain completion is not negation. "
    "Keep completed casts separate from later intentions, canceled plans, and requests for scene facts."
)


def span(quote, utterance):
    raw, q = utterance.encode(), quote.encode()
    e.a.require(0 < len(q) <= 2048 and raw.count(q) == 1, "Anchor absent or ambiguous")
    begin = raw.index(q)
    return {"begin": begin, "end": begin + len(q), "sha256": e.a.digest(q)}


def parse(text, utterance):
    e.a.require(len(text.encode()) <= 131072, "Oversized multi-event frame")
    frame = json.loads(text, object_pairs_hook=e.a.unique_object)
    e.a.require(type(frame) is dict and set(frame) == {"events"}, "Frame fields differ")
    events = frame["events"]
    e.a.require(type(events) is list and len(events) <= MAX_EVENTS, "Event count invalid")
    result = []
    previous_end = 0
    for event in events:
        e.a.require(type(event) is dict and set(event) == {"anchor", *e.FIELDS}, "Event fields differ")
        e.a.require(type(event["anchor"]) is str, "Invalid event anchor")
        anchor = span(event["anchor"], utterance)
        e.a.require(anchor["begin"] >= previous_end, "Anchors overlap or are out of order")
        previous_end = anchor["end"]
        values, quotes = e.parse(json.dumps({k: event[k] for k in e.FIELDS}), utterance)
        result.append({"anchor": anchor, "values": values, "quote_spans": quotes})
    return result


def cases():
    data = e.a.read(DATA)
    e.a.require(data["schema"] == "waterdeep-multi-cast-development/v1" and data["scope"] == "development" and
                data["author"] == "Codex" and data["training_admitted"] is False and
                data["independent_evaluation"] is False, "Dataset scope differs")
    rows = data["cases"]
    e.a.require(len(rows) == 16 and len({c["id"] for c in rows}) == 16, "Case inventory differs")
    e.a.require(set(Counter(c["family"] for c in rows).values()) == {2}, "Families must remain paired")
    e.a.require(len({c["utterance"] for c in rows}) == len(rows), "Duplicate utterance")
    for case in rows:
        e.a.require(bool(case["rationale"]), "Missing rationale")
        parsed = parse(json.dumps({"events": case["events"]}), case["utterance"])
        e.a.require(len(parsed) == len(case["expected_joins"]), "Missing join labels")
    return rows


def response_format():
    event = single_format()["json_schema"]["schema"]
    event["properties"]["anchor"] = {"type": "string", "minLength": 1, "maxLength": 2048}
    event["required"].append("anchor")
    return {"type": "json_schema", "json_schema": {
        "name": "waterdeep_multi_cast_v1", "strict": True, "schema": {
            "type": "object", "additionalProperties": False, "required": ["events"],
            "properties": {"events": {"type": "array", "maxItems": MAX_EVENTS, "items": event}},
        }}}


def request(template, case, policy, seed):
    e.a.require(policy in POLICIES, "Unknown policy")
    body = copy.deepcopy(template)
    e.a.require("response_format" not in body and "structured_outputs" not in body, "Constrained template")
    body.update(messages=[{"role": "system", "content": INSTRUCTION + (SCOPED_REPAIR if policy == "scoped_repair" else "")},
                          {"role": "user", "content": case["utterance"]}],
                stream=False, seed=seed, max_completion_tokens=2048, response_format=response_format())
    return body


def assess(response, case, facts):
    try:
        extracted = parse(e.a.answer(response), case["utterance"])
    except (ValueError, KeyError, TypeError) as error:
        return {"accepted": False, "error": str(error)}
    expected = parse(json.dumps({"events": case["events"]}), case["utterance"])
    # Match anchors to authored original mentions, not to position in the output.
    # The span check proves membership only; independent entailment review remains absent.
    matches = {}
    unmatched = []
    ambiguous = []
    for index, event in enumerate(extracted):
        candidates = [j for j, target in enumerate(expected)
                      if event["anchor"]["begin"] < target["anchor"]["end"] and
                      target["anchor"]["begin"] < event["anchor"]["end"]]
        if len(candidates) != 1 or candidates[0] in matches:
            (ambiguous if candidates else unmatched).append(index)
        else:
            matches[candidates[0]] = index
    checks = []
    for target_index, index in sorted(matches.items()):
        values = extracted[index]["values"]
        wrong = [k for k in e.FIELDS if values[k] != expected[target_index]["values"][k]]
        joined = e.join(values, facts)
        checks.append({"target_event": target_index, "extracted_event": index,
                       "incorrect_fields": wrong, "exact_event": not wrong, "join": joined,
                       "join_matches_authored_target": joined["kind"] == case["expected_joins"][target_index]})
    missing = sorted(set(range(len(expected))) - matches.keys())
    exact = not (missing or unmatched or ambiguous) and all(c["exact_event"] for c in checks)
    return {"accepted": True, "extracted": extracted, "expected_event_count": len(expected),
            "missing_events": missing, "unmatched_events": unmatched, "ambiguous_event_anchors": ambiguous,
            "checks": checks, "exact_all_events": exact,
            "all_joins_match": not (missing or unmatched or ambiguous) and all(c["join_matches_authored_target"] for c in checks),
            "whole_history_proven": False, "resource_state_authorized": False}


def matrix():
    for index, case in enumerate(cases()):
        for policy in POLICIES[::1 if index % 2 == 0 else -1]:
            yield 0, case, policy


def protocol():
    return {"policies": list(POLICIES), "seeds": [0], "trials": 32, "schema": response_format(),
            "socket_timeout_seconds": 240,
            "primary": "All events exact, including event coverage and original-mention anchor alignment",
            "scope": "Sixteen visible authored cases; no independent evaluation, closed history, resources, or product activation"}


def verify(root, source, facts_directory, facts_receipt_sha256, template_path):
    facts, template = e.inputs(source, facts_directory, facts_receipt_sha256, template_path)
    manifest = e.a.read(root / "manifest.json")
    required = {*SOURCES, "cases.json", "facts.json", "template.json", "protocol.json", "trials.jsonl",
                "models-before.json", "models-after.json", "source-decoder.py"}
    e.a.require(required <= manifest["files"].keys(), "Missing experiment evidence")
    for name, sha in manifest["files"].items():
        e.a.require(name == Path(name).name, "Unsafe evidence path")
        path = root / name
        e.a.require(not path.is_symlink() and e.a.digest(path.read_bytes()) == sha, "Evidence changed")
    e.a.require(e.a.read(root / "facts.json") == facts and e.a.read(root / "template.json") == template, "Inputs differ")
    e.a.require(e.a.read(root / "protocol.json") == protocol() and e.a.read(root / "cases.json") == e.a.read(DATA), "Protocol or cases differ")
    e.a.require(manifest["facts_receipt_sha256"] == facts_receipt_sha256 and
                manifest["template_sha256"] == e.a.digest(template_path.read_bytes()), "Input pins differ")
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    expected = list(matrix())
    e.a.require(len(rows) == len(expected) == manifest["trials"], "Trial count differs")
    for row, (seed, case, policy) in zip(rows, expected, strict=True):
        e.a.require((row["seed"], row["case_id"], row["policy"]) == (seed, case["id"], policy), "Trial order differs")
        e.a.require(row["request"] == request(template, case, policy, seed), "Request differs")
        e.a.require(row["assessment"] == assess(row["response"], case, facts), "Assessment differs")
        e.a.require(type(row["request_elapsed_ms"]) is int and row["request_elapsed_ms"] >= 0, "Invalid duration")
    e.a.require(e.a.model_catalog(e.a.read(root / "models-before.json")) == e.a.model_catalog(e.a.read(root / "models-after.json")), "Model catalog changed")
    return {"verified_trials": len(rows), "accepted_frames": sum(r["assessment"]["accepted"] for r in rows),
            "exact_all_events": sum(r["assessment"].get("exact_all_events", False) for r in rows),
            "training_admitted": False, "production_path": False}


def run(endpoint, source, facts_directory, facts_receipt_sha256, template_path, output):
    facts, template = e.inputs(source, facts_directory, facts_receipt_sha256, template_path)
    output.mkdir()
    for name in SOURCES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "source-decoder.py").write_bytes((e.s.ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes())
    (output / "cases.json").write_bytes(DATA.read_bytes())
    for name, value in (("facts", facts), ("template", template), ("protocol", protocol())):
        e.a.write(output / (name + ".json"), value)
    client = e.a.Client(endpoint, timeout_seconds=240)
    before = client.request("/v1/models")
    e.a.require(template["model"] in [m["id"] for m in before["data"]], "Requested model absent")
    e.a.write(output / "models-before.json", before)
    count = 0
    with (output / "trials.jsonl").open("x") as stream:
        for seed, case, policy in matrix():
            body = request(template, case, policy, seed)
            start = time.perf_counter_ns()
            response = client.request("/v1/chat/completions", body)
            elapsed = (time.perf_counter_ns() - start) // 1_000_000
            assessment = assess(response, case, facts)
            row = {"seed": seed, "case_id": case["id"], "policy": policy, "request": body,
                   "response": response, "request_elapsed_ms": elapsed, "assessment": assessment}
            stream.write(json.dumps(row) + "\n")
            stream.flush()
            count += 1
            print(f"{count}/32 {case['id']} {policy} accepted={assessment['accepted']} exact={assessment.get('exact_all_events', False)}", flush=True)
    e.a.write(output / "models-after.json", client.request("/v1/models"))
    e.a.write(output / "manifest.json", {"trials": count, "facts_receipt_sha256": facts_receipt_sha256,
                                        "template_sha256": e.a.digest(template_path.read_bytes()),
                                        "files": {p.name: e.a.digest(p.read_bytes()) for p in output.iterdir()
                                                  if p.is_file() and not p.name.startswith("._")}})
    e.a.write(output / "verification.json", verify(output, source, facts_directory, facts_receipt_sha256, template_path))
