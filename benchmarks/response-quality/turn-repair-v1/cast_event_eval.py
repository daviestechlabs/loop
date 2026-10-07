"""Offline cast-event extraction and source metadata joins; no game writes."""

import argparse
import copy
import json
import os
from pathlib import Path

import action_fact_eval as a
import spell_source_facts as s
from validate_cast_event_data import FIELDS, validate

HERE = Path(__file__).resolve().parent
DATA = HERE / "synthetic/cast-events-20260928"
POLICIES = ("basic", "revision_aware")
PRINTING = "41bb349a59377f7b72d42d0f"
INDEX_SHA = "76a7a11cd2d055b35e32a25f61af61d88e32f94f6da29a1b348aa74f5b426ae3"
MANIFEST_SHA = "904240bb055819608f7ce40a1cdc90bc179dc039408cfd81050d91ea49a3a8fc"
EXPECTED_JOIN = {
    "planned_cast_a": "levelled_action_claim", "planned_cast_b": "excluded_event",
    "corrected_actor_a": "levelled_action_claim", "corrected_actor_b": "excluded_event",
    "corrected_time_a": "levelled_action_claim", "corrected_time_b": "excluded_event",
    "unclear_spell_a": "levelled_action_claim", "unclear_spell_b": "unresolved_spell",
}
SOURCES = tuple(dict.fromkeys((*a.SOURCES, "cast_event_eval.py", "spell_source_facts.py",
                             "validate_cast_event_data.py")))
INSTRUCTION = (
    "Extract the final unretracted claim about the single focal spell cast in the player's utterance. "
    "Return JSON only with exactly actor, time, status, spell, modifier. "
    "Each field is an object with exactly value and quote. "
    "Allowed values: " + json.dumps({k: sorted(v) for k, v in FIELDS.items()}) + ". "
    "Actor is relative to the outer speaker. Time describes the focal cast, not the conversation. "
    "Occurred means claimed completed casting; proposed means intended or hypothetical casting; "
    "negated means denied occurrence or an explicitly canceled plan; uncertain means occurrence is unknown. "
    "Spell is the named identity, never its level or legality. Use unresolved when identity is not recoverable. "
    "Modifier none requires an explicit statement of no casting-time modifiers. Otherwise use unknown. "
    "For each known value supply a unique exact nonempty substring of the utterance as quote. "
    "Unknown or unresolved values may use an empty quote. Never quote this instruction. "
    "Record claims only; do not authorize casting, infer resources, or change game state."
)
REVISION = (
    " Resolve corrections before filling fields. Replace only the corrected field. "
    "A completed cast with corrected timing remains completed. A corrected caster does not alter the spell. "
    "Distinguish intentions from completed casts, the outer speaker from quoted characters, "
    "and current turns from previous turns. Preserve explicitly unresolved identity."
)


def cases():
    data = a.read(DATA / "cases.json")
    a.require(data["scope"] == "development" and not data["training_admitted"], "Wrong dataset scope")
    validate(data["cases"], a.read(DATA / "conditions.json"))
    a.require({c["id"] for c in data["cases"]} == set(EXPECTED_JOIN), "Case set changed")
    return data["cases"]


def parse(text, utterance):
    a.require(len(text.encode()) <= 16000, "Oversized event")
    frame = json.loads(text, object_pairs_hook=a.unique_object)
    a.require(type(frame) is dict and set(frame) == set(FIELDS), "Event fields differ")
    values, spans = {}, {}
    raw = utterance.encode()
    for field, allowed in FIELDS.items():
        item = frame[field]
        a.require(type(item) is dict and set(item) == {"value", "quote"}, "Event field shape differs")
        value, quote = item["value"], item["quote"]
        a.require(type(value) is str and value in allowed, "Invalid event value")
        a.require(type(quote) is str and len(quote.encode()) <= 2048, "Invalid event quote")
        a.require(bool(quote) or value in {"unknown", "unresolved"}, "Known event value lacks quote")
        if quote:
            q = quote.encode()
            a.require(raw.count(q) == 1, "Quote absent or ambiguous")
            begin = raw.index(q)
            spans[field] = {"begin": begin, "end": begin + len(q), "sha256": a.digest(q)}
        else:
            spans[field] = None
        values[field] = value
    return values, spans


def join(values, facts):
    # A single excluded event cannot prove that the complete turn has no casts.
    if values["actor"] == "other" or values["time"] in {"previous_turn", "next_turn"} or values["status"] in {"proposed", "negated"}:
        return {"kind": "excluded_event"}
    if values["actor"] != "speaker" or values["time"] != "this_turn" or values["status"] != "occurred":
        return {"kind": "unresolved_event"}
    matches = [f for f in facts if f["name"] == values["spell"].replace("_", "") and f["document"][0] == PRINTING]
    if values["spell"] == "unresolved" or len(matches) != 1 or not matches[0]["valid_header"]:
        return {"kind": "unresolved_spell"}
    fact = matches[0]
    bound = {"source_document": fact["document"], "assembly_sha256": fact["assembly_sha256"],
             "level": fact["level"], "standard_cost": fact["standard_cost"],
             "classification": fact["classification"], "casting_time": fact["casting_time"]}
    if values["modifier"] != "none":
        return {"kind": "modified_cast_unresolved", **bound}
    if fact["standard_cost"] == 0:
        return {"kind": "unsupported_casting_cost", **bound}
    if fact["standard_cost"] != 1:
        return {"kind": "other_standard_cost", **bound}
    return {"kind": "action_cantrip_claim" if fact["level"] == 0 else "levelled_action_claim", **bound}


def matrix():
    for seed in (0, 1):
        for index, case in enumerate(cases()):
            for policy in POLICIES[::1 if (seed + index) % 2 == 0 else -1]:
                yield seed, case, policy


def request(template, case, policy, seed):
    body = copy.deepcopy(template)
    body["messages"] = [{"role": "system", "content": INSTRUCTION + (REVISION if policy == "revision_aware" else "")},
                        {"role": "user", "content": case["utterance"]}]
    body.update(stream=False, seed=seed, max_completion_tokens=1024)
    # Keep decoding identical between prompts. No gold labels or source facts
    # enter the model input; joins happen only after extraction.
    return body


def assess(response, case, facts):
    try:
        values, spans = parse(a.answer(response), case["utterance"])
    except (ValueError, KeyError, TypeError) as error:
        return {"accepted": False, "error": str(error)}
    joined = join(values, facts)
    wrong = [k for k in FIELDS if values[k] != case["expected"][k]]
    return {"accepted": True, "values": values, "quote_spans": spans, "incorrect_fields": wrong,
            "exact_event": not wrong, "join": joined,
            "join_matches_authored_target": joined["kind"] == EXPECTED_JOIN[case["id"]]}


def inputs(source, facts_directory, facts_receipt_sha256, template_path):
    a.require(a.digest((facts_directory / "receipt.json").read_bytes()) == facts_receipt_sha256, "Fact receipt pin differs")
    facts = s.verify(facts_directory, source, INDEX_SHA, MANIFEST_SHA)
    template, _ = a.ab.prepare(source, template_path)
    return facts, template


def protocol():
    return {"policies": list(POLICIES), "seeds": [0, 1], "trials": 32,
            "expected_join": EXPECTED_JOIN, "printing": PRINTING,
            "scope": "Single focal event; source join is not full history, budget, or casting permission"}


def verify(root, source, facts_directory, facts_receipt_sha256, template_path):
    facts, template = inputs(source, facts_directory, facts_receipt_sha256, template_path)
    manifest = a.read(root / "manifest.json")
    required = {*SOURCES, "cases.json", "conditions.json", "facts.json", "template.json", "protocol.json",
                "trials.jsonl", "models-before.json", "models-after.json", "source-decoder.py"}
    a.require(required <= manifest["files"].keys(), "Missing experiment evidence")
    for name, sha in manifest["files"].items():
        rel = Path(name)
        a.require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe evidence path")
        path = root / rel
        a.require(not path.is_symlink() and a.digest(path.read_bytes()) == sha, "Experiment evidence changed")
    a.require(a.read(root / "facts.json") == facts and a.read(root / "template.json") == template, "Experiment inputs differ")
    a.require(a.read(root / "protocol.json") == protocol(), "Protocol differs")
    a.require(a.read(root / "cases.json") == a.read(DATA / "cases.json") and
              a.read(root / "conditions.json") == a.read(DATA / "conditions.json"), "Dataset differs")
    a.require(manifest["facts_receipt_sha256"] == facts_receipt_sha256 and
              manifest["template_sha256"] == a.digest(template_path.read_bytes()), "Input pins differ")
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    expected = list(matrix())
    a.require(len(rows) == len(expected) == manifest["trials"], "Trial count differs")
    for row, (seed, case, policy) in zip(rows, expected, strict=True):
        a.require((row["seed"], row["case_id"], row["policy"]) == (seed, case["id"], policy), "Trial order differs")
        a.require(row["request"] == request(template, case, policy, seed), "Request differs")
        a.require(row["assessment"] == assess(row["response"], case, facts), "Assessment differs")
    a.require(a.model_catalog(a.read(root / "models-before.json")) == a.model_catalog(a.read(root / "models-after.json")), "Model catalog changed")
    return {"verified_trials": len(rows), "accepted_frames": sum(r["assessment"]["accepted"] for r in rows),
            "training_admitted": False, "production_path": False}


def run(endpoint, source, facts_directory, facts_receipt_sha256, template_path, output):
    facts, template = inputs(source, facts_directory, facts_receipt_sha256, template_path)
    output.mkdir()
    for name in SOURCES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "source-decoder.py").write_bytes((s.ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes())
    for name in ("cases.json", "conditions.json"):
        (output / name).write_bytes((DATA / name).read_bytes())
    a.write(output / "facts.json", facts)
    a.write(output / "template.json", template)
    a.write(output / "protocol.json", protocol())
    client = a.Client(endpoint)
    a.write(output / "models-before.json", client.request("/v1/models"))
    count = 0
    with (output / "trials.jsonl").open("x") as stream:
        for seed, case, policy in matrix():
            body = request(template, case, policy, seed)
            response = client.request("/v1/chat/completions", body)
            row = {"seed": seed, "case_id": case["id"], "policy": policy, "request": body, "response": response,
                   "assessment": assess(response, case, facts)}
            stream.write(json.dumps(row) + "\n")
            stream.flush()
            count += 1
            print(f"{count}/32 {case['id']} {policy} seed={seed} accepted={row['assessment']['accepted']}", flush=True)
    a.write(output / "models-after.json", client.request("/v1/models"))
    a.write(output / "manifest.json", {"trials": count, "facts_receipt_sha256": facts_receipt_sha256,
                                      "template_sha256": a.digest(template_path.read_bytes()),
                                      "files": {p.name: a.digest(p.read_bytes()) for p in output.iterdir() if p.is_file() and not p.name.startswith("._")}})
    a.write(output / "verification.json", verify(output, source, facts_directory, facts_receipt_sha256, template_path))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("source", "facts", "template", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--facts-receipt-sha256", required=True)
    parser.add_argument("--endpoint")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    if args.verify:
        print(json.dumps(verify(args.output, args.source, args.facts, args.facts_receipt_sha256, args.template)))
    else:
        a.require(args.endpoint, "Endpoint required")
        run(args.endpoint, args.source, args.facts, args.facts_receipt_sha256, args.template, args.output)
