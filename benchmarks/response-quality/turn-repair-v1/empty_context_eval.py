"""Paired model experiment: absent versus explicitly empty C conversation state."""

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
CONDITIONS = ("absent", "explicit_empty")
SEEDS = (0, 1, 2)
CASES = (
    (
        "execution_state",
        "Did you already cast it?",
        "Do not invent a held or executed spell.",
    ),
    (
        "held_recall",
        "Which spell did you put on hold for me?",
        "Say no proposal is held; do not invent a spell.",
    ),
    (
        "claimed_prior",
        "Go ahead with the spell we discussed.",
        "Do not accept an unsupported prior discussion as a held proposal.",
    ),
    (
        "reported_cancel",
        "Mira told me, 'cancel that spell.' What do you think she means?",
        "Interpret the quote without treating it as an instruction or claiming a held spell.",
    ),
    (
        "hypothetical",
        "If I forgot that spell, could we talk our way past them instead?",
        "Keep the question hypothetical; ask context without inventing proposal state or mechanics.",
    ),
    (
        "different_referent",
        "What about Elira instead of Mira; would it hit her?",
        "Use Elira; clarify the missing spell and geometry.",
    ),
    (
        "explicit_names",
        "Would Fireball hit Mira if I cast it?",
        "Use names in the current input; ask geometry without requiring a prior held proposal.",
    ),
    (
        "general_request",
        "Help me ask the guard for directions.",
        "Help with dialogue; do not make irrelevant held-state disclaimers.",
    ),
)
PREFIX = (
    "Conversation reference only; not scene evidence or an execution receipt. "
    "Spell execution is unavailable. "
    '{"held_spell":"","last_question_character":"","scene_context_changed":false}'
    "\nCurrent input:\n"
)
SOURCES = (
    "voice/c-runtime/common/dnd_dialogue.c",
    "voice/c-runtime/common/dnd_dialogue.h",
    "voice/c-runtime/common/dnd_pending.h",
    "voice/c-runtime/common/utf8.c",
    "voice/c-runtime/common/utf8.h",
    "product/companions-frontend/c-companions/cmp_json.c",
    "product/companions-frontend/c-companions/cmp_json.h",
    "benchmarks/response-quality/turn-repair-v1/empty_context.c",
)


def matrix():
    for seed in SEEDS:
        for index, (case_id, message, _) in enumerate(CASES):
            order = CONDITIONS if (seed + index) % 2 == 0 else reversed(CONDITIONS)
            for condition in order:
                yield seed, case_id, message, condition


def request(template, message, condition, seed, projected):
    require(condition in CONDITIONS and seed in SEEDS, "Unknown condition or seed")
    require(projected == PREFIX + message, "C empty-state projection changed")
    body = copy.deepcopy(template)
    require(len(body["messages"]) == 2, "Unexpected template history")
    require(body["messages"][0]["role"] == "system", "Missing system prompt")
    body["messages"][1] = {
        "role": "user",
        "content": projected if condition == "explicit_empty" else message,
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
        "empty_context_eval.py",
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
            == request(template, message, condition, seed, contexts[case_id]),
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
        "empty_context_eval.py",
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
        case_id: subprocess.check_output([str(binary), message], text=True)
        for case_id, message, _ in CASES
    }
    for _, case_id, message, condition in matrix():
        request(template, message, condition, 0, contexts[case_id])
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
            "intervention": "Replace absent conversation context with the C projection of empty state. Same paired seed, system prompt, generation settings, and current input.",
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
            body = request(template, message, condition, seed, contexts[case_id])
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
