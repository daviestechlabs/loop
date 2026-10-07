"""Paired RAM-only inference: isolate output vocabulary from turn reasoning."""

import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import resource
import statistics
import subprocess
import time

from run_mlx_adapter import METADATA, MODEL, REVISION, WEIGHT, fetch, load_base_model, verified_data
from semantic_eval import ACTION_STATES, ACTS, HERE, PROMPTS, assess_target, model_input, sha, write

CONDITIONS = ("legacy", "enumerated")
COMPARISONS = {"vocabulary": CONDITIONS, "policy": ("minimal", "contract")}
MAX_TOKENS = 160


def cases_from_pack(pack, comparison="vocabulary"):
    if comparison not in COMPARISONS:
        raise ValueError("Unknown fixed prompt comparison")
    cases = pack["cases"]
    if len(cases) != 8 or len({case["case_id"] for case in cases}) != 8:
        raise ValueError("Expected eight distinct evaluation cases")
    if any(case["split"] != "evaluation" for case in cases):
        raise ValueError("Training or validation case entered evaluation")
    if sum(case["target"]["commit_requested"] is True for case in cases) != 1:
        raise ValueError("Expected one valid-action positive control")
    systems = {
        "legacy": pack["system_prompt"],
        "enumerated": pack["system_prompt"]
        + " dialogue_act must be one of " + json.dumps(ACTS)
        + ". action_state must be one of " + json.dumps(ACTION_STATES) + ".",
    }
    if comparison == "policy":
        systems = {condition: PROMPTS[condition] for condition in COMPARISONS[comparison]}
    return [
        {
            "case_id": case["case_id"],
            "family": case["scenario_family"],
            "prompt": json.dumps(model_input(case), sort_keys=True, ensure_ascii=False),
            "systems": systems.copy(),
            "target": {key: value for key, value in case["target"].items() if key != "answer"},
            "reference_answer": case["target"]["answer"],
        }
        for case in cases
    ]


def score_output(target, text, finish_reason):
    score = assess_target(target, text)
    if finish_reason != "stop":
        score.update(valid=False, exact=False, primary_correct=False)
        score["failures"] = [*score["failures"], "generation_not_stopped"]
    # Retain malformed output. Do not strip fences or coerce invented labels.
    diagnostic = {"json_object": False, "unknown_labels": [], "commit_requested": None}
    try:
        value = json.loads(text)
    except (ValueError, TypeError):
        return {"score": score, "diagnostic": diagnostic}
    if isinstance(value, dict):
        diagnostic["json_object"] = True
        for field, choices in (("dialogue_act", ACTS), ("action_state", ACTION_STATES)):
            if value.get(field) not in choices:
                diagnostic["unknown_labels"].append(field)
        if type(value.get("commit_requested")) is bool:
            diagnostic["commit_requested"] = value["commit_requested"]
    return {"score": score, "diagnostic": diagnostic}


def summarize(cases, pairs, conditions=CONDITIONS):
    if conditions not in COMPARISONS.values():
        raise ValueError("Unknown condition pair")
    if len(pairs) != len(cases) or [p["case_id"] for p in pairs] != [c["case_id"] for c in cases]:
        raise ValueError("Incomplete, reordered, or duplicate paired results")
    output = {}
    for condition in conditions:
        rows = [pair[condition] for pair in pairs]
        positives = [row for case, row in zip(cases, rows) if case["target"]["commit_requested"]]
        output[condition] = {
            "cases": len(rows),
            "valid": sum(row["score"]["valid"] for row in rows),
            "exact_state_fields": sum(row["score"]["exact"] for row in rows),
            "primary_correct": sum(row["score"]["primary_correct"] for row in rows),
            "unknown_label_outputs": sum(bool(row["diagnostic"]["unknown_labels"]) for row in rows),
            "known_true_commit_proposals": sum(row["diagnostic"]["commit_requested"] is True for row in rows),
            "commit_proposal_unknown": sum(row["diagnostic"]["commit_requested"] is None for row in rows),
            "positive_control_exact": sum(row["score"]["exact"] for row in positives),
            "positive_controls": len(positives),
            "median_complete_generation_ms": statistics.median(row["milliseconds"] for row in rows),
            "generated_tokens": sum(row["generation_tokens"] for row in rows),
        }
    return output


def run(data, output, comparison="vocabulary"):
    import mlx.core as mx
    from mlx.utils import tree_flatten
    from mlx_lm import stream_generate
    from mlx_lm.sample_utils import make_sampler
    from mlx_lm.utils import load_tokenizer
    import numpy as np

    started = time.monotonic()
    deadline = started + 1200

    def check_time():
        if time.monotonic() > deadline:
            raise TimeoutError("Bounded contract experiment deadline exceeded")

    os.umask(0o077)
    _, receipt, files = verified_data(data)
    cases = cases_from_pack(json.loads(files["evaluation-only/turn-cases.json"]), comparison)
    conditions = COMPARISONS[comparison]
    root = HERE.parents[2]
    if subprocess.check_output(["git", "-C", str(root), "status", "--porcelain"]).strip():
        raise ValueError("Commit the experiment source before inference")
    revision = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    recipe = {
        "schema": "waterdeep-output-contract-recipe/v1",
        "source_revision": revision, "model": MODEL, "model_revision": REVISION,
        "weight_sha256": WEIGHT[1], "weight_bytes": WEIGHT[0],
        "comparison": comparison, "conditions": list(conditions), "seed": 0, "temperature": 0,
        "maximum_new_tokens": MAX_TOKENS, "maximum_prompt_tokens": 1024,
        "case_count": len(cases), "families": sorted({c["family"] for c in cases}),
        "order": "alternate condition order per case; one observation per condition",
        "training_performed": False, "production_gate": False,
        "data_receipt_sha256": sha((data / "receipt.json").read_bytes()),
        "evaluation_pack_sha256": sha(files["evaluation-only/turn-cases.json"]),
        "runtime_versions": {name: importlib.metadata.version(name) for name in ("mlx", "mlx-lm", "transformers", "numpy")},
    }
    output.mkdir(parents=True, exist_ok=False)
    write(output / "recipe.json", recipe)
    write(output / "cases.json", cases)
    write(output / "data-receipt.json", receipt)
    for name in ("probe_output_contract.py", "run_mlx_adapter.py", "semantic_eval.py"):
        (output / name).write_bytes((HERE / name).read_bytes())
    metadata = output / "metadata"
    metadata.mkdir()
    for name, identity in METADATA.items():
        check_time()
        (metadata / name).write_bytes(fetch(name, identity, git_blob=True).read())
    tokenizer = load_tokenizer(metadata, tokenizer_config_extra={"trust_remote_code": False, "local_files_only": True})
    # Preflight every prompt before loading the weights; targets never enter messages.
    prompts = {}
    for case in cases:
        for condition in conditions:
            tokens = tokenizer.apply_chat_template([
                {"role": "system", "content": case["systems"][condition]},
                {"role": "user", "content": case["prompt"]},
            ], tokenize=True, return_dict=False, add_generation_prompt=True)
            if len(tokens) > 1024:
                raise ValueError("Prompt exceeds the fixed inference bound")
            prompts[(case["case_id"], condition)] = tokens
    write(output / "prompt-lengths.json", [{"case_id": key[0], "condition": key[1], "tokens": len(value)} for key, value in prompts.items()])
    model = load_base_model(metadata, 0)
    model.eval()
    check_time()

    def hashes():
        return {name: sha(np.asarray(value.view(mx.uint8)).tobytes()) for name, value in tree_flatten(model.parameters())}

    before = hashes()
    write(output / "weights-before.json", before)
    # Both conditions share this one warmup and the same unchanged model instance.
    warmup = tokenizer.apply_chat_template([{"role": "user", "content": "Say ready."}], tokenize=True, return_dict=False, add_generation_prompt=True)
    for _ in stream_generate(model, tokenizer, warmup, max_tokens=8, sampler=make_sampler(temp=0)):
        check_time()
    pairs = []
    with (output / "pairs.jsonl").open("x") as journal:
        for index, case in enumerate(cases):
            pair = {"case_id": case["case_id"]}
            order = conditions if index % 2 == 0 else tuple(reversed(conditions))
            for condition in order:
                check_time()
                begin = time.monotonic()
                text, identities, last = "", [], None
                for last in stream_generate(model, tokenizer, prompts[(case["case_id"], condition)], max_tokens=MAX_TOKENS, sampler=make_sampler(temp=0)):
                    text += last.text
                    identities.append(int(last.token))
                    check_time()
                if last is None:
                    raise ValueError("Empty generation stream")
                pair[condition] = {
                    "response": text, "response_sha256": sha(text.encode()), "token_ids": identities,
                    "finish_reason": last.finish_reason, "prompt_tokens": last.prompt_tokens,
                    "generation_tokens": last.generation_tokens, "milliseconds": (time.monotonic() - begin) * 1000,
                    **score_output(case["target"], text, last.finish_reason),
                }
            journal.write(json.dumps(pair, allow_nan=False) + "\n")
            journal.flush()
            pairs.append(pair)
            print(json.dumps({"completed": len(pairs), "total": len(cases), "case_id": case["case_id"]}), flush=True)
    after = hashes()
    write(output / "weights-after.json", after)
    if before != after:
        raise ValueError("Inference changed model parameters")
    report = {
        "schema": "waterdeep-output-contract-result/v1", "training_performed": False,
        "weights_unchanged": True, "weights_written_to_disk": False,
        "production_gate": False, "independent_human_review": False,
        "spoken_answers_reviewed": False, "human_audio": False,
        "conditions": summarize(cases, pairs, conditions), "elapsed_seconds": time.monotonic() - started,
        "mlx_peak_bytes": mx.get_peak_memory(),
        "process_peak_rss_platform_units": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        "host": platform.platform(),
    }
    write(output / "report.json", report)
    write(output / "receipt.json", {"schema": "waterdeep-output-contract-receipt/v1", "files": {
        str(p.relative_to(output)): sha(p.read_bytes()) for p in output.rglob("*") if p.is_file()
    }})
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--comparison", choices=tuple(COMPARISONS), default="vocabulary")
    args = parser.parse_args()
    run(args.data, args.output, args.comparison)
