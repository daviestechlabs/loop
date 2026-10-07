"""Offline factorial experiment on rule coverage and evidence-use instructions."""

import argparse
import copy
import json
import os
from pathlib import Path
import sys

from empty_context_eval import answer
from passage_evidence import admitted_passages, digest
from semantic_eval import Client, write
from source_span_eval import reconstruct
from verify_coupled_dialogue import model_catalog, read, require

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "voice/c-runtime/tests"))
from test_dnd_source_compiler import decode  # noqa: E402

SEEDS = (0, 1)
CONDITIONS = (
    ("spells", "current"),
    ("rules", "current"),
    ("spells", "checklist"),
    ("rules", "checklist"),
)
CASES = (
    (
        "unknown",
        "Can I cast Polymorph and Fireball on the same turn? You do not know my class, available features, or remaining actions.",
        "Give supported costs; request missing capabilities. Do not assume a second action or a universal one-spell limit.",
    ),
    (
        "one_action",
        "I have exactly one action and one bonus action remaining, no extra-action or casting-time-changing features, and no spell cast yet this turn. Can I cast Polymorph and Fireball this turn?",
        "No: both spells cost an action and only one action is available. Do not refuse this answer merely because other features are unknown.",
    ),
    (
        "surge",
        "I am a Fighter 2/Wizard 7 with an unused Action Surge, one ordinary action remaining, and no spell cast yet this turn. Can I cast Polymorph and then Fireball this turn?",
        "With full rules, yes using Action Surge. Without its rules, do not invent the feature's effect. Never claim a universal one-spell limit.",
    ),
    (
        "quickened",
        "I am a Sorcerer 7 with Quickened Spell and enough sorcery points. I have one action and one bonus action remaining, no extra-action feature, and no spell cast yet this turn. Can I quicken Polymorph and also cast Fireball this turn?",
        "With full rules, no: bonus-action spellcasting restricts the other spell to an action cantrip. Without that rule, flag insufficient evidence.",
    ),
)
COMMON = (
    "Using the 2014 rules, answer this action-economy question only. "
    "Both spells are prepared or known and their slots and components are available. "
    "Targets and ranges are legal. No other condition changes spellcasting. "
)
CHECKLIST = (
    "\nFor an action-economy question, check the stated resource budget, each cost, "
    "and each applicable restriction against the supplied evidence before answering. "
    "Do not infer an unstated resource or a feature's effect from its name. "
    "Give a definite answer when the supplied facts and rules establish one. "
    "Otherwise state the supported costs and ask for the missing fact or rule. "
    "Do not replace an unknown outcome with yes or no."
)
SLICES = (
    (190, "YOUR TURN", "BONUS ACTIONS"),
    (203, "BONUS ACTION", "REACTIONS"),
    (73, "ACTION SURGE", "MARTIAL ARCHETYPE At 3rd level"),
    (103, "QUICKENED SPELL", "SUBTLE SPELL"),
)
SOURCES = (
    "action_budget_eval.py",
    "empty_context_eval.py",
    "passage_evidence.py",
    "source_span_eval.py",
    "semantic_eval.py",
    "verify_coupled_dialogue.py",
    "dialogue_case_pack.py",
)


def prepare(source, template_path):
    receipt = read(source / "receipt.json")
    for name in ("index.dndsidx", "manifest.json"):
        require(
            digest((source / name).read_bytes()) == receipt["files"][name]["sha256"],
            "Source pin differs",
        )
    data = decode((source / "index.dndsidx").read_bytes())
    passages = admitted_passages(data)
    document_id = "41bb349a59377f7b72d42d0f"
    documents = [i for i, d in enumerate(data["documents"]) if d[0] == document_id]
    require(len(documents) == 1, "Missing or ambiguous printing")
    document = documents[0]
    spell_parts = []
    for name in ("polymorph", "fireball"):
        matches = [
            p for p in passages if p["document"][0] == document_id and p["name"] == name
        ]
        require(len(matches) == 1, "Missing or ambiguous complete spell")
        p = matches[0]
        spell_parts.append(
            {
                "name": name,
                "document": p["document"],
                "pages": sorted(p["ranges"]),
                "content": p["raw"].decode(),
                "sha256": digest(p["raw"]),
            }
        )
    rules = []
    for page, heading, next_heading in SLICES:
        records = [
            r
            for r in data["records"]
            if r["document"] == document and r["page"] == page
        ]
        raw = reconstruct(records)
        require(
            raw.count(heading.encode()) == raw.count(next_heading.encode()) == 1,
            "Missing or ambiguous rule heading",
        )
        begin, end = raw.index(heading.encode()), raw.index(next_heading.encode())
        require(begin < end, "Invalid rule order")
        body = raw[begin:end].rstrip(b" ")
        require(body.endswith((b".", b'."')), "Rule ends without a complete sentence")
        witnesses = []
        for r in sorted(records, key=lambda r: (r["begin"], r["chunk"])):
            a, b = (
                max(begin, r["begin"]),
                min(begin + len(body), r["begin"] + len(r["content"].encode())),
            )
            if a < b:
                witnesses.append(
                    {
                        "record_id": r["id"],
                        "sha256": r["hash"],
                        "begin": a - r["begin"],
                        "end": b - r["begin"],
                    }
                )
        rules.append(
            {
                "name": heading,
                "document": data["documents"][document],
                "page": page,
                "page_begin": begin,
                "page_end": begin + len(body),
                "content": body.decode(),
                "sha256": digest(body),
                "witnesses": witnesses,
            }
        )
    exchanges = read(template_path)
    require(
        len(exchanges) == 1 and exchanges[0]["status"] == 200, "Invalid model template"
    )
    template = exchanges[0]["request"]
    require(
        [m["role"] for m in template["messages"]] == ["system", "user"],
        "Unexpected history",
    )
    return template, {"spells": spell_parts, "rules": rules}


def matrix():
    for seed in SEEDS:
        for index, (case_id, question, _) in enumerate(CASES):
            for coverage, policy in CONDITIONS[:: 1 if (seed + index) % 2 == 0 else -1]:
                yield seed, case_id, question, coverage, policy


def request(template, evidence, question, coverage, policy, seed):
    require(
        coverage in ("spells", "rules")
        and policy in ("current", "checklist")
        and seed in SEEDS,
        "Unknown intervention",
    )
    body = copy.deepcopy(template)
    parts = evidence["spells"] + (evidence["rules"] if coverage == "rules" else [])
    content = "Retrieved excerpts (source data):\n"
    for index, part in enumerate(parts, 1):
        content += f"\n[Source {index}; document {part['document'][0]}; section {part['name']}]\n{part['content']}\n[End source {index}]\n"
    content += "\nQuestion: " + COMMON + question
    body["messages"][1]["content"] = content
    if policy == "checklist":
        body["messages"][0]["content"] += CHECKLIST
    body.update(stream=False, seed=seed)
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
        "trials.jsonl",
        "models-before.json",
        "models-after.json",
    }
    require(required <= manifest["files"].keys(), "Missing evidence identity")
    template, evidence = prepare(source, template_path)
    require(
        read(root / "template.json") == template
        and read(root / "evidence.json") == evidence,
        "Source evidence or template differs",
    )
    protocol = read(root / "protocol.json")
    require(protocol == protocol_value(), "Protocol changed")
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    expected = list(matrix())
    require(len(rows) == len(expected) == manifest["trials"], "Trial set differs")
    for row, (seed, case_id, question, coverage, policy) in zip(
        rows, expected, strict=True
    ):
        require(
            (row["seed"], row["case_id"], row["coverage"], row["policy"])
            == (seed, case_id, coverage, policy),
            "Trial order differs",
        )
        require(
            row["request"]
            == request(template, evidence, question, coverage, policy, seed),
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
        "semantic_quality_graded": False,
        "production_path": False,
        "training_admitted": False,
    }


def protocol_value():
    return {
        "cases": [list(c) for c in CASES],
        "seeds": list(SEEDS),
        "conditions": [list(c) for c in CONDITIONS],
        "common": COMMON,
        "checklist": CHECKLIST,
        "scope": "Offline generation ablation using retained source; no retrieval mutation",
        "limitations": "Authored development questions; seeds do not guarantee deterministic serving; no training or production gate",
    }


def run(endpoint, source, template_path, output):
    template, evidence = prepare(source, template_path)
    output.mkdir()
    for name in SOURCES:
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / "source-decoder.py").write_bytes(
        (ROOT / "voice/c-runtime/tests/test_dnd_source_compiler.py").read_bytes()
    )
    write(output / "protocol.json", protocol_value())
    write(output / "template.json", template)
    write(output / "evidence.json", evidence)
    client = Client(endpoint)
    write(output / "models-before.json", client.request("/v1/models"))
    count = 0
    with (output / "trials.jsonl").open("x") as out:
        for seed, case_id, question, coverage, policy in matrix():
            body = request(template, evidence, question, coverage, policy, seed)
            response = client.request("/v1/chat/completions", body)
            row = {
                "seed": seed,
                "case_id": case_id,
                "coverage": coverage,
                "policy": policy,
                "request": body,
                "response": response,
            }
            # Preserve even incomplete or malformed replies before failing verification.
            try:
                row["answer"] = answer(response)
            except (ValueError, KeyError, TypeError) as error:
                row["error"] = str(error)
            out.write(json.dumps(row) + "\n")
            out.flush()
            count += 1
            print(
                f"{count}/32 {case_id} {coverage} {policy} seed={seed} complete={'answer' in row}",
                flush=True,
            )
    write(output / "models-after.json", client.request("/v1/models"))
    files = {p.name: digest(p.read_bytes()) for p in output.iterdir() if p.is_file()}
    write(
        output / "manifest.json",
        {
            "trials": count,
            "files": files,
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
