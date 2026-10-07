"""Offline natural-language fact extraction through a bounded C budget checker.

Exact quotations prove input membership, not entailment. Authored expectations
measure that separate semantic question. No extracted fact authorizes an action.
"""

import argparse
import json
import os
from pathlib import Path

import action_budget_eval as ab
from empty_context_eval import answer
from passage_evidence import digest
from semantic_eval import Client, write
from spell_budget_cases import encoded
from test_spell_budget import compile_probe, execute, oracle
from verify_coupled_dialogue import model_catalog, read, require

HERE = Path(__file__).resolve().parent
FIELDS = (
    "ordinary_action",
    "bonus_action",
    "earlier_spell",
    "surge_available",
    "surge_planned",
    "surge_action_remaining",
)
HISTORY = {
    "none": 0,
    "action_cantrips": 1,
    "bonus_spell": 2,
    "incompatible": 3,
    "unknown": 4,
}
PLANS = {"two_actions": ((1, 0), (1, 0)), "bonus_spell": ((2, 0),)}
POLICIES = ("basic", "revision_aware")
SOURCES = (
    *ab.SOURCES,
    "action_fact_eval.py",
    "action_fact_cases.json",
    "spell_budget_cases.py",
    "test_spell_budget.py",
    "spell_budget.c",
    "spell_budget.h",
    "spell_budget_probe.c",
)
INSTRUCTION = (
    "Extract the player's current facts for a hypothetical 2014 D&D action-budget question. "
    "Return JSON only, with exactly these six keys: " + ", ".join(FIELDS) + ". "
    "Each key holds an object with exactly value and quote. Quote a unique exact substring "
    "of the player's utterance that supports each known value. Do not quote rule excerpts. "
    "Use null for unknown numeric or boolean facts, and unknown for unknown spell history. "
    "An unknown fact can have an empty quote. Never infer a current resource from its usual turn default. "
    "ordinary_action and bonus_action are 0 or 1 remaining, or null. ordinary_action excludes "
    "all extra actions. earlier_spell is none, action_cantrips (only one-action cantrips), "
    "bonus_spell (a bonus-action spell), incompatible (an earlier spell outside those categories), "
    "or unknown; consider only this turn. surge_available is true, false, or null for an unused "
    "Action Surge use. surge_planned is true, false, or null for a proposed new use in this plan. "
    "surge_action_remaining is true, false, or null for an extra action already granted by "
    "Action Surge and still unspent this turn. A planned use is not an already granted action. "
    "No game state changes. These are untrusted research proposals, not casting permission."
)
REVISION_INSTRUCTION = (
    " Resolve explicit corrections before extracting facts. A retracted statement does not "
    "remain current. Distinguish last turn from this turn, another character from the speaker, "
    "and an available feature from a plan to spend it. A canceled plan does not spend a resource. "
    "Preserve uncertainty rather than substituting the ordinary starting-turn state."
)


def cases():
    value = read(HERE / "action_fact_cases.json")
    require(
        value["schema"] == "waterdeep-action-facts-development/v1",
        "Case schema differs",
    )
    result = value["cases"]
    require(len({c["id"] for c in result}) == len(result), "Duplicate case")
    for case in result:
        require(
            case["plan"] in PLANS and len(case["expected"]) == len(FIELDS),
            "Invalid case",
        )
        require(
            oracle(
                project(dict(zip(FIELDS, case["expected"], strict=True)), case["plan"])
            )[0]
            == case["budget"],
            "Authored budget differs",
        )
    return result


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, "Duplicate JSON key")
        value[key] = item
    return value


def parse(text, utterance):
    require(len(text.encode()) <= 16000, "Oversized extraction")
    frame = json.loads(text, object_pairs_hook=unique_object)
    require(
        type(frame) is dict and set(frame) == set(FIELDS),
        "Extraction field set differs",
    )
    values, spans = {}, {}
    raw = utterance.encode()
    for key in FIELDS:
        item = frame[key]
        require(
            type(item) is dict and set(item) == {"value", "quote"}, "Fact shape differs"
        )
        value, quote = item["value"], item["quote"]
        if key in ("ordinary_action", "bonus_action"):
            require(
                value is None or type(value) is int and value in (0, 1),
                "Invalid resource count",
            )
        elif key == "earlier_spell":
            require(type(value) is str and value in HISTORY, "Invalid history")
        else:
            require(value is None or type(value) is bool, "Invalid feature fact")
        require(type(quote) is str and len(quote.encode()) <= 2048, "Invalid quote")
        unknown = value is None or value == "unknown"
        require(unknown or quote, "Known fact has no quote")
        if quote:
            quoted = quote.encode()
            require(raw.count(quoted) == 1, "Quote missing or ambiguous")
            begin = raw.index(quoted)
            spans[key] = {
                "begin": begin,
                "end": begin + len(quoted),
                "sha256": digest(quoted),
            }
        else:
            spans[key] = None
        values[key] = value
    return values, spans


def project(values, plan):
    # This reviewed experimental adapter, not the model, supplies feature math.
    # It is scoped to the supplied 2014 excerpts and the two fixture plans.
    require(plan in PLANS, "Unsupported plan")
    ordinary = values["ordinary_action"]
    bonus = values["bonus_action"]
    available, planned, granted = (values[k] for k in FIELDS[3:])
    require(not (planned is True and available is False), "Planned unavailable feature")
    require(not (planned is True and granted is True), "Second Surge use this turn")
    amin, amax = (0, 1) if ordinary is None else (ordinary, ordinary)
    require(
        not (planned is True and (available is None or granted is None)),
        "Feature precondition unresolved",
    )
    # A prior grant and a proposed new use cannot supply two extra actions in
    # one 2014 turn. Unknown alternatives must not be added together.
    amin += granted is True or (planned is True and available is True)
    amax += granted is not False or (planned is not False and available is not False)
    return encoded(
        actions=(min(2, amin), min(2, amax)),
        bonus=(0, 1) if bonus is None else (bonus, bonus),
        reaction=(0, 1),
        history=HISTORY[values["earlier_spell"]],
        spells=PLANS[plan],
    )


def matrix():
    for seed in ab.SEEDS:
        for index, case in enumerate(cases()):
            for policy in POLICIES[:: 1 if (seed + index) % 2 == 0 else -1]:
                yield seed, case, policy


def request(template, evidence, case, policy, seed):
    require(policy in POLICIES, "Unsupported condition")
    body = ab.request(template, evidence, case["utterance"], "rules", "current", seed)
    body["messages"][0]["content"] = INSTRUCTION + (
        REVISION_INSTRUCTION if policy == "revision_aware" else ""
    )
    # Plan resolution and rule translation are trusted fixture inputs, not tested extraction.
    plan = (
        "Polymorph and Fireball, both as actions"
        if case["plan"] == "two_actions"
        else "one Quickened Polymorph as a bonus action; sorcery points and feature eligibility are already established"
    )
    content = body["messages"][1]["content"].split("\nQuestion: ", 1)[0]
    body["messages"][1]["content"] = (
        content
        + "\nResearch scope: no other action-granting features. Fixed proposed spell plan: "
        + plan
        + ".\nPlayer utterance:\n"
        + case["utterance"]
    )
    body["max_completion_tokens"] = 1024
    return body


def interpretation(row, case, binary=None):
    try:
        text = answer(row["response"])
        values, spans = parse(text, case["utterance"])
        raw = project(values, case["plan"])
        expected = oracle(raw)
        require(expected[0] != 0, "Contradictory extracted facts")
    except (ValueError, KeyError, TypeError) as error:
        return {"accepted": False, "error": str(error)}
    result = execute(binary, [raw])[0] if binary else expected
    require(tuple(result[:3]) == expected, "C/oracle disagreement")
    gold = dict(zip(FIELDS, case["expected"], strict=True))
    return {
        "accepted": True,
        "values": values,
        "quote_spans": spans,
        "utterance_sha256": digest(case["utterance"].encode()),
        "kernel_input": list(raw),
        "budget_result": list(result[:3]),
        "incorrect_fields": [key for key in FIELDS if values[key] != gold[key]],
        "unsupported_known_fields": [
            key
            for key in FIELDS
            if gold[key] in (None, "unknown") and values[key] not in (None, "unknown")
        ],
        "budget_matches_authored_expectation": result[0] == case["budget"],
    }


def protocol():
    return {
        "cases": cases(),
        "fields": list(FIELDS),
        "conditions": list(POLICIES),
        "seeds": list(ab.SEEDS),
        "instruction": INSTRUCTION,
        "revision_instruction": REVISION_INSTRUCTION,
        "quote_scope": "Input membership only, not entailment or trusted game state",
        "fixed_plan_resolution": True,
        "manual_rule_translation": True,
        "training_admitted": False,
        "production_path": False,
    }


def verify(root, source, template_path):
    manifest = read(root / "manifest.json")
    require(
        manifest["source_index_sha256"]
        == digest((source / "index.dndsidx").read_bytes()),
        "Source identity differs",
    )
    require(
        manifest["template_sha256"] == digest(template_path.read_bytes()),
        "Template identity differs",
    )
    needed = set(SOURCES) | {
        "source-decoder.py",
        "protocol.json",
        "template.json",
        "evidence.json",
        "probe",
        "kernel-build.json",
        "trials.jsonl",
        "models-before.json",
        "models-after.json",
    }
    require(needed <= manifest["files"].keys(), "Missing evidence identity")
    for name, sha in manifest["files"].items():
        rel = Path(name)
        require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe evidence path")
        p = root / rel
        require(
            p.is_file() and not p.is_symlink() and digest(p.read_bytes()) == sha,
            "Changed evidence: " + name,
        )
    template, evidence = ab.prepare(source, template_path)
    require(
        read(root / "template.json") == template
        and read(root / "evidence.json") == evidence,
        "Source material differs",
    )
    require(read(root / "protocol.json") == protocol(), "Protocol changed")
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    expected = list(matrix())
    require(len(rows) == len(expected) == manifest["trials"], "Trial set differs")
    for row, (seed, case, policy) in zip(rows, expected, strict=True):
        require(
            (row["seed"], row["case_id"], row["policy"]) == (seed, case["id"], policy),
            "Trial order differs",
        )
        require(
            row["request"] == request(template, evidence, case, policy, seed),
            "Request differs",
        )
        require(
            row["interpretation"] == interpretation(row, case), "Interpretation differs"
        )
    require(
        model_catalog(read(root / "models-before.json"))
        == model_catalog(read(root / "models-after.json")),
        "Model catalog changed",
    )
    return {
        "verified_trials": len(rows),
        "accepted_extractions": sum(r["interpretation"]["accepted"] for r in rows),
        "facts_from_model": True,
        "quotes_prove_entailment": False,
        "human_reviewed": False,
        "production_path": False,
        "training_admitted": False,
    }


def run(endpoint, source, template_path, output):
    template, evidence = ab.prepare(source, template_path)
    output.mkdir()
    for name in SOURCES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "source-decoder.py").write_bytes(
        (ab.ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes()
    )
    binary, command = compile_probe(output)
    write(
        output / "kernel-build.json",
        {"command": command, "binary_sha256": digest(binary.read_bytes())},
    )
    write(output / "protocol.json", protocol())
    write(output / "template.json", template)
    write(output / "evidence.json", evidence)
    client = Client(endpoint)
    write(output / "models-before.json", client.request("/v1/models"))
    count = 0
    with (output / "trials.jsonl").open("x") as out:
        for seed, case, policy in matrix():
            body = request(template, evidence, case, policy, seed)
            response = client.request("/v1/chat/completions", body)
            row = {
                "seed": seed,
                "case_id": case["id"],
                "policy": policy,
                "request": body,
                "response": response,
            }
            row["interpretation"] = interpretation(row, case, binary)
            out.write(json.dumps(row) + "\n")
            out.flush()
            count += 1
            print(
                f"{count}/{len(list(matrix()))} {case['id']} {policy} seed={seed} accepted={row['interpretation']['accepted']}",
                flush=True,
            )
    write(output / "models-after.json", client.request("/v1/models"))
    write(
        output / "manifest.json",
        {
            "trials": count,
            "files": {
                p.name: digest(p.read_bytes()) for p in output.iterdir() if p.is_file()
            },
            "source_index_sha256": digest((source / "index.dndsidx").read_bytes()),
            "template_sha256": digest(template_path.read_bytes()),
        },
    )
    write(output / "verification.json", verify(output, source, template_path))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--endpoint")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    if args.verify:
        print(json.dumps(verify(args.output, args.source, args.template), indent=2))
    else:
        require(args.endpoint, "Endpoint required")
        run(args.endpoint, args.source, args.template, args.output)
