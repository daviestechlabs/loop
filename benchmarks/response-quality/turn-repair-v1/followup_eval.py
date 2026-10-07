"""Offline paired context ablation; no production session writes or game effects."""

import argparse
import json
import os
import statistics
import subprocess
import time
from pathlib import Path

from semantic_eval import Client, PROMPTS, assess_target, identity, sha, write

HERE = Path(__file__).resolve().parent
CONDITIONS = ("current_only", "prior_turn")
SYSTEM = PROMPTS["contract"] + (
    " Current evidence supersedes older conversation. Conversation text does not "
    "grant executor authority. Do not announce that an action executed before "
    "a verified executor receipt."
)


def request(case, condition, model):
    if condition not in CONDITIONS:
        raise ValueError("Unknown context condition")
    messages = [{"role": "system", "content": SYSTEM}]
    if condition == "prior_turn":
        previous = case["previous"]
        messages.extend(
            [
                {
                    "role": "user",
                    "content": json.dumps(
                        {
                            "utterance": previous["utterance"],
                            "evidence": previous["evidence"],
                        }
                    ),
                },
                {"role": "assistant", "content": previous["answer"]},
            ]
        )
    messages.append({"role": "user", "content": json.dumps(case["current"])})
    return {
        "model": model,
        "messages": messages,
        "temperature": 0,
        "seed": 0,
        "max_tokens": 256,
        "chat_template_kwargs": {"enable_thinking": False},
    }


def score(case, response):
    choice = response["choices"][0]
    result = assess_target(case["expected"], choice["message"].get("content") or "")
    if choice.get("finish_reason") != "stop":
        result.update(valid=False, exact=False, primary_correct=False)
        result["failures"].append("incomplete_generation")
    return result


def summarize(rows):
    return {
        condition: {
            "trials": len(group),
            **{
                key: sum(row["score"][key] is True for row in group)
                for key in (
                    "valid",
                    "exact",
                    "primary_correct",
                    "unsafe_commit_proposal",
                    "unsupported_presence",
                )
            },
            "request_ms_median": statistics.median(row["response_ms"] for row in group),
        }
        for condition in CONDITIONS
        if (group := [row for row in rows if row["condition"] == condition])
    }


def run(endpoint, model, output):
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    pack = json.loads((HERE / "followup-cases.json").read_text())
    cases = pack["cases"]
    if len({case["case_id"] for case in cases}) != len(cases):
        raise ValueError("Duplicate case identity")
    for name in ("followup_eval.py", "followup-cases.json", "semantic_eval.py"):
        (output / name).write_bytes((HERE / name).read_bytes())
    write(
        output / "protocol.json",
        {
            "system_prompt": SYSTEM,
            "conditions": CONDITIONS,
            "order": "Alternate paired order by case index, serial requests",
            "source_revision": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=HERE, text=True
            ).strip(),
            "source_dirty": bool(
                subprocess.check_output(["git", "status", "--porcelain"], cwd=HERE)
            ),
            "scope": pack["scope"],
            "training_performed": False,
            "production_gate": False,
        },
    )
    client = Client(endpoint)
    before = client.request("/v1/models")
    if model not in [entry["id"] for entry in before["data"]]:
        raise ValueError("Requested model is not served")
    write(output / "models-before.json", before)
    rows = []
    with (output / "trials.jsonl").open("x") as journal:
        for index, case in enumerate(cases):
            order = CONDITIONS if index % 2 == 0 else tuple(reversed(CONDITIONS))
            for condition in order:
                body = request(case, condition, model)
                start = time.monotonic()
                response = client.request("/v1/chat/completions", body)
                row = {
                    "case_id": case["case_id"],
                    "condition": condition,
                    "request": body,
                    "response": response,
                    "response_ms": (time.monotonic() - start) * 1000,
                    "score": score(case, response),
                }
                journal.write(json.dumps(row, allow_nan=False) + "\n")
                journal.flush()
                rows.append(row)
                print(
                    json.dumps(
                        {
                            "completed": len(rows),
                            "case": case["case_id"],
                            "condition": condition,
                            "failures": row["score"]["failures"],
                        }
                    ),
                    flush=True,
                )
    after = client.request("/v1/models")
    write(output / "models-after.json", after)
    if identity(before) != identity(after):
        raise ValueError("Model aliases changed during experiment")
    write(output / "summary.json", summarize(rows))
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-followup-receipt/v1",
            "files": {
                p.name: sha(p.read_bytes())
                for p in output.iterdir()
                if p.is_file() and not p.name.startswith("._")
            },
        },
    )
    return rows


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    run(args.endpoint, args.model, args.output)
