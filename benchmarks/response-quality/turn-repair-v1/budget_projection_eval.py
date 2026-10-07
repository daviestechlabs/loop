"""Compare identical typed facts with and without a C-derived budget verdict."""

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
SOURCES = (
    *ab.SOURCES,
    "budget_projection_eval.py",
    "spell_budget_cases.py",
    "test_spell_budget.py",
    "spell_budget.c",
    "spell_budget.h",
    "spell_budget_probe.c",
)
POLICIES = ("typed_facts", "typed_facts_and_c")
INSTRUCTION = (
    "\nA typed scenario annotation, when supplied, separates current resources "
    "from capacity after the listed planned feature uses for this hypothetical question. "
    "Those uses are assumptions of the plan, not already executed actions. "
    "Its action count excludes restricted extra "
    "actions that cannot cast spells. An action-budget result, when supplied, "
    "is a calculation from those facts and the supplied rules. Preserve its "
    "conclusion when speaking. Unknown means the facts do not establish yes or no. "
    "This calculation does not execute a spell or establish any other prerequisite."
)


def annotations():
    return {
        "unknown": encoded(actions=(0, 2), bonus=(0, 1), reaction=(0, 1), history=4),
        "one_action": encoded(reaction=(0, 1)),
        "surge": encoded(actions=(2, 2), bonus=(0, 1), reaction=(0, 1)),
        "quickened": encoded(reaction=(0, 1), spells=((2, 0), (1, 0))),
    }


def facts(raw, case_id):
    require(
        case_id in annotations() and raw == annotations()[case_id],
        "Unknown typed scenario",
    )
    return {
        "scope": "Action economy only; manually annotated research scenario, not observed game state",
        "ruleset": "2014",
        "ordinary_actions_remaining": None if case_id == "unknown" else 1,
        "planned_feature_uses": (
            [
                {
                    "feature": "Action Surge",
                    "available": True,
                    "uses_to_expend": 1,
                    "additional_unrestricted_actions": 1,
                }
            ]
            if case_id == "surge"
            else [
                {
                    "feature": "Quickened Spell",
                    "available": True,
                    "sorcery_points_to_expend": 2,
                    "spell": "Polymorph",
                }
            ]
            if case_id == "quickened"
            else None
            if case_id == "unknown"
            else []
        ),
        "unrestricted_spell_action_capacity_after_planned_features": list(raw[3:5]),
        "capacity_condition": "Capacity includes the listed feature uses. Do not treat their effects as available without spending them.",
        "bonus_actions_remaining": list(raw[5:7]),
        "reactions_remaining": list(raw[7:9]),
        "capacity_encoding": "inclusive minimum and maximum; action capacity capped at two for this two-spell question",
        "earlier_spell": "unknown" if raw[9] == 4 else "none",
        "planned_spells": [
            {
                "cost": {1: "action", 2: "bonus_action"}[raw[i]],
                "cantrip": bool(raw[i + 1]),
            }
            for i in (10, 12)
        ],
    }


def matrix():
    for seed in ab.SEEDS:
        for index, (case_id, question, _) in enumerate(ab.CASES):
            for policy in POLICIES[:: 1 if (seed + index) % 2 == 0 else -1]:
                yield seed, case_id, question, policy


def request(template, evidence, annotation, result, question, policy, seed, case_id):
    require(policy in POLICIES, "Unknown projection condition")
    body = ab.request(template, evidence, question, "rules", "current", seed)
    body["messages"][0]["content"] += INSTRUCTION
    extra = "\nTyped scenario annotation (derived data):\n" + json.dumps(
        facts(annotation, case_id), sort_keys=True
    )
    if policy == "typed_facts_and_c":
        extra += "\nC action-budget result (derived data):\n" + json.dumps(
            {
                "decision": {
                    2: "unknown",
                    3: "fits_action_budget",
                    4: "exceeds_action_budget",
                }[result[0]],
                "scope": "Only the supplied action costs and bonus-action spell restriction; not execution authorization",
            },
            sort_keys=True,
        )
    content = body["messages"][1]["content"]
    require(content.count("\nQuestion: ") == 1, "Ambiguous question boundary")
    body["messages"][1]["content"] = content.replace(
        "\nQuestion: ", extra + "\nQuestion: "
    )
    return body


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
    for name, sha in manifest["files"].items():
        rel = Path(name)
        require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe evidence path")
        p = root / rel
        require(
            p.is_file() and not p.is_symlink() and digest(p.read_bytes()) == sha,
            "Changed evidence: " + name,
        )
    required = set(SOURCES) | {
        "source-decoder.py",
        "protocol.json",
        "template.json",
        "evidence.json",
        "annotations.json",
        "kernel-results.json",
        "kernel-build.json",
        "probe",
        "trials.jsonl",
        "models-before.json",
        "models-after.json",
    }
    require(required <= manifest["files"].keys(), "Missing evidence identity")
    template, evidence = ab.prepare(source, template_path)
    require(
        read(root / "template.json") == template
        and read(root / "evidence.json") == evidence,
        "Source material differs",
    )
    typed = annotations()
    require(
        read(root / "annotations.json") == {k: list(v) for k, v in typed.items()},
        "Annotations changed",
    )
    results = read(root / "kernel-results.json")
    require(set(results) == set(typed), "Kernel case set differs")
    for key, raw in typed.items():
        require(
            tuple(results[key][:3]) == oracle(raw),
            "C verdict disagrees with matching oracle",
        )
    require(read(root / "protocol.json") == protocol(), "Protocol changed")
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    expected = list(matrix())
    require(len(rows) == len(expected) == manifest["trials"], "Trial set differs")
    for row, (seed, case_id, question, policy) in zip(rows, expected, strict=True):
        require(
            (row["seed"], row["case_id"], row["policy"]) == (seed, case_id, policy),
            "Trial order differs",
        )
        require(
            row["request"]
            == request(
                template,
                evidence,
                typed[case_id],
                results[case_id],
                question,
                policy,
                seed,
                case_id,
            ),
            "Request differs from intervention",
        )
        require(
            row["answer"] == answer(row["response"]),
            "Answer differs from engine response",
        )
    require(
        model_catalog(read(root / "models-before.json"))
        == model_catalog(read(root / "models-after.json")),
        "Model catalog changed",
    )
    return {
        "verified_trials": len(rows),
        "verified_typed_cases": len(typed),
        "semantic_quality_graded": False,
        "natural_language_extraction_tested": False,
        "production_path": False,
        "training_admitted": False,
    }


def protocol():
    return {
        "cases": [list(c) for c in ab.CASES],
        "seeds": list(ab.SEEDS),
        "conditions": list(POLICIES),
        "shared_instruction": INSTRUCTION,
        "scope": "Both conditions receive identical hand-annotated facts and rule excerpts",
        "limitation": "Known development cases; semantic extraction and rule translation are not evaluated; no training or deployment",
    }


def run(endpoint, source, template_path, output):
    template, evidence = ab.prepare(source, template_path)
    typed = annotations()
    output.mkdir()
    for name in SOURCES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "source-decoder.py").write_bytes(
        (ab.ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes()
    )
    binary, command = compile_probe(output)
    values = execute(binary, list(typed.values()))
    results = dict(zip(typed, values, strict=True))
    for key, raw in typed.items():
        require(results[key][:3] == oracle(raw), "Kernel disagrees before inference")
    write(output / "kernel-results.json", results)
    write(
        output / "kernel-build.json",
        {"command": command, "binary_sha256": digest(binary.read_bytes())},
    )
    write(output / "annotations.json", {k: list(v) for k, v in typed.items()})
    write(output / "protocol.json", protocol())
    write(output / "template.json", template)
    write(output / "evidence.json", evidence)
    client = Client(endpoint)
    write(output / "models-before.json", client.request("/v1/models"))
    count = 0
    with (output / "trials.jsonl").open("x") as out:
        for seed, case_id, question, policy in matrix():
            body = request(
                template,
                evidence,
                typed[case_id],
                results[case_id],
                question,
                policy,
                seed,
                case_id,
            )
            response = client.request("/v1/chat/completions", body)
            row = {
                "seed": seed,
                "case_id": case_id,
                "policy": policy,
                "request": body,
                "response": response,
            }
            try:
                row["answer"] = answer(response)
            except (ValueError, KeyError, TypeError) as error:
                row["error"] = str(error)
            out.write(json.dumps(row) + "\n")
            out.flush()
            count += 1
            print(
                f"{count}/16 {case_id} {policy} seed={seed} complete={'answer' in row}",
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
    p = argparse.ArgumentParser()
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--template", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--endpoint")
    p.add_argument("--verify", action="store_true")
    a = p.parse_args()
    os.umask(0o077)
    if a.verify:
        print(json.dumps(verify(a.output, a.source, a.template), indent=2))
    else:
        require(a.endpoint, "Endpoint required")
        run(a.endpoint, a.source, a.template, a.output)
