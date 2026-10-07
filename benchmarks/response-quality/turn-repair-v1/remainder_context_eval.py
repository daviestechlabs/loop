"""Paired model experiment: full cancellation input versus its remaining question."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import time

from semantic_eval import Client, sha, write
from verify_coupled_dialogue import model_catalog, require

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CONDITIONS = ("full_input", "remaining_question")
SEEDS = (0, 1, 2)
CASES = (
    (
        "protect_held",
        "Please abandon my proposal. How else can I protect Mira?",
        1,
        "How else can I protect Mira?",
        "Answer protection question without repeating C status or inventing mechanics.",
    ),
    (
        "negotiate_held",
        "Forget the spell. Could we talk our way past them instead?",
        1,
        "Could we talk our way past them instead?",
        "Answer negotiation question without repeating C status or inventing NPC intent.",
    ),
    (
        "protect_fresh",
        "Please abandon my proposal. How else can I protect Mira?",
        0,
        "How else can I protect Mira?",
        "No held proposal; answer question without another cancellation disclaimer.",
    ),
    (
        "negotiate_fresh",
        "Forget the spell. Could we talk our way past them instead?",
        0,
        "Could we talk our way past them instead?",
        "No held proposal; answer negotiation without inventing state.",
    ),
    (
        "quoted",
        "Mira told me, 'cancel that spell.' What do you think she means?",
        1,
        None,
        "Interpret quoted speech without canceling or claiming cancellation.",
    ),
    (
        "conditional",
        "Cancel that spell only if Mira is still here.",
        1,
        None,
        "Do not satisfy an unverified condition or claim cancellation.",
    ),
    (
        "negated",
        "Don't abandon my proposal. How else can I protect Mira?",
        1,
        None,
        "Keep proposal and answer protection question without inventing mechanics.",
    ),
)


def expected_context(case_id, condition):
    require(condition in CONDITIONS, "Unknown condition")
    _, message, held, remainder, _ = next(c for c in CASES if c[0] == case_id)
    canceled = remainder is not None
    reference = {
        "held_spell": "fireball" if held and not canceled else "",
        "last_question_character": "Mira" if held else "",
        "scene_context_changed": False,
    }
    if canceled:
        reference["canceled_proposal"] = "fireball" if held else ""
    return (
        "Conversation reference only; not scene evidence or an execution receipt. "
        "Spell execution is unavailable. "
        + json.dumps(reference, separators=(",", ":"))
        + "\nCurrent input:\n"
        + (remainder if canceled and condition == "remaining_question" else message)
    )


SOURCES = (
    "voice/c-runtime/common/dnd_dialogue.c",
    "voice/c-runtime/common/dnd_dialogue.h",
    "voice/c-runtime/common/dnd_pending.h",
    "voice/c-runtime/common/utf8.c",
    "voice/c-runtime/common/utf8.h",
    "product/companions-frontend/c-companions/cmp_json.c",
    "product/companions-frontend/c-companions/cmp_json.h",
    "contracts/prompt-library/prompts/voice/dnd-dialogue.system.txt",
    "benchmarks/response-quality/turn-repair-v1/remainder_context.c",
)


def matrix():
    for seed in SEEDS:
        for index, (case_id, message, *_rest) in enumerate(CASES):
            order = CONDITIONS if (seed + index) % 2 == 0 else reversed(CONDITIONS)
            for condition in order:
                yield seed, case_id, message, condition


def request(template, case_id, condition, seed, projected):
    require(condition in CONDITIONS and seed in SEEDS, "Unknown condition or seed")
    require(
        projected[condition] == expected_context(case_id, condition),
        "C remainder projection changed",
    )
    body = copy.deepcopy(template)
    require(len(body["messages"]) == 2, "Unexpected template history")
    require(body["messages"][0]["role"] == "system", "Missing system prompt")
    body["messages"][1] = {
        "role": "user",
        "content": projected[condition],
    }
    body.update(stream=False, seed=seed)
    return body


def answer(response):
    choices = response["choices"]
    require(
        len(choices) == 1 and choices[0]["finish_reason"] == "stop",
        "Incomplete generation",
    )
    text = choices[0]["message"]["content"]
    require(isinstance(text, str) and text.strip(), "Empty answer")
    return text


def verify(root):
    manifest = json.loads((root / "manifest.json").read_text())
    for name, digest in manifest["files"].items():
        rel = Path(name)
        require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe evidence path")
        path = root / rel
        require(path.is_file() and not path.is_symlink(), "Missing or linked evidence")
        require(sha(path.read_bytes()) == digest, "Changed evidence: " + name)
    required = {
        "protocol.json",
        "template.json",
        "contexts.json",
        "trials.jsonl",
        "models-before.json",
        "models-after.json",
        "remainder_context_eval.py",
        "semantic_eval.py",
        "verify_coupled_dialogue.py",
        "dialogue_case_pack.py",
        "projection",
    }
    required.update("source/" + name for name in SOURCES)
    require(required <= manifest["files"].keys(), "Missing evidence identity")
    protocol = json.loads((root / "protocol.json").read_text())
    require(protocol["cases"] == [list(c) for c in CASES], "Case pack changed")
    require(
        protocol["seeds"] == list(SEEDS) and protocol["conditions"] == list(CONDITIONS),
        "Intervention changed",
    )
    template = json.loads((root / "template.json").read_text())[0]["request"]
    require(
        (root / "source" / SOURCES[-2]).read_text().strip()
        in template["messages"][0]["content"],
        "D&D prompt differs from retained source",
    )
    contexts = json.loads((root / "contexts.json").read_text())
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    expected = list(matrix())
    require(len(rows) == len(expected) == manifest["trials"], "Missing or extra trials")
    for row, (seed, case_id, message, condition) in zip(rows, expected, strict=True):
        require(
            (row["seed"], row["case_id"], row["condition"])
            == (seed, case_id, condition),
            "Wrong trial order",
        )
        require(
            row["request"]
            == request(template, case_id, condition, seed, contexts[case_id]),
            "Request differs from intervention",
        )
        require(
            row["answer"] == answer(row["response"]),
            "Answer differs from engine response",
        )
    require(
        model_catalog(json.loads((root / "models-before.json").read_text()))
        == model_catalog(json.loads((root / "models-after.json").read_text())),
        "Model catalog changed",
    )
    return {
        "verified_trials": len(rows),
        "quality_graded": False,
        "production_gate": False,
        "training_approved": False,
    }


def run(endpoint, template_path, output):
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    for name in (
        "remainder_context_eval.py",
        "semantic_eval.py",
        "verify_coupled_dialogue.py",
        "dialogue_case_pack.py",
    ):
        (output / name).write_bytes((HERE / name).read_bytes())
    for name in SOURCES:
        target = output / "source" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())
    (output / "template.json").write_bytes(template_path.read_bytes())
    template = json.loads(template_path.read_text())
    require(
        len(template) == 1 and template[0]["status"] == 200,
        "Invalid retained request template",
    )
    template = template[0]["request"]
    require(
        (ROOT / SOURCES[-2]).read_text().strip() in template["messages"][0]["content"],
        "Template does not use current D&D prompt",
    )
    source = output / "source"
    common = source / "voice/c-runtime/common"
    product = source / "product/companions-frontend/c-companions"
    binary = output / "projection"
    command = [
        "cc",
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(common),
        "-I" + str(product),
        str(source / SOURCES[-1]),
        str(common / "dnd_dialogue.c"),
        str(common / "utf8.c"),
        str(product / "cmp_json.c"),
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True, capture_output=True, timeout=60)
    contexts = {
        case_id: {
            condition: subprocess.check_output(
                [str(binary), str(held), condition, message], text=True
            )
            for condition in CONDITIONS
        }
        for case_id, message, held, _remainder, _review in CASES
    }
    for _, case_id, message, condition in matrix():
        request(template, case_id, condition, 0, contexts[case_id])
    write(output / "contexts.json", contexts)
    write(
        output / "protocol.json",
        {
            "cases": CASES,
            "seeds": SEEDS,
            "conditions": CONDITIONS,
            "source_revision": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
            ).strip(),
            "source_dirty": bool(
                subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)
            ),
            "compiler": subprocess.check_output(["cc", "--version"], text=True),
            "compile_command": command,
            "intervention": "Remove only the command already admitted by the bounded C cancellation grammar. Keep projected state, paired seed, system prompt and generation settings fixed. Controls preserve full input.",
            "scope": "Direct real-model text requests. Stream disabled for both conditions. No product session, audio, game effects, training, or promotion. Development cases; seeds do not guarantee reproducibility or independent samples.",
        },
    )
    client = Client(endpoint)
    before = client.request("/v1/models")
    require(
        template["model"] in [m["id"] for m in before["data"]],
        "Model alias unavailable",
    )
    write(output / "models-before.json", before)
    count = 0
    with (output / "trials.jsonl").open("x") as journal:
        for seed, case_id, message, condition in matrix():
            body = request(template, case_id, condition, seed, contexts[case_id])
            started = time.monotonic()
            response = client.request("/v1/chat/completions", body)
            row = {
                "seed": seed,
                "case_id": case_id,
                "condition": condition,
                "request": body,
                "response": response,
                "answer": answer(response),
                "response_ms": (time.monotonic() - started) * 1000,
            }
            journal.write(json.dumps(row, ensure_ascii=False) + "\n")
            journal.flush()
            count += 1
            print(
                json.dumps(
                    {
                        "trial": count,
                        "seed": seed,
                        "case": case_id,
                        "condition": condition,
                        "answer": row["answer"],
                    }
                ),
                flush=True,
            )
    write(output / "models-after.json", client.request("/v1/models"))
    hashes = {
        str(p.relative_to(output)): sha(p.read_bytes())
        for p in sorted(output.rglob("*"))
        if p.is_file() and not p.name.startswith("._")
    }
    write(output / "manifest.json", {"files": hashes, "trials": count})
    print(json.dumps(verify(output)), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint")
    parser.add_argument("--template", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if args.verify:
        print(json.dumps(verify(args.output), indent=2))
    else:
        if not args.endpoint or not args.template:
            parser.error("--endpoint and --template are required for inference")
        run(args.endpoint, args.template, args.output)
