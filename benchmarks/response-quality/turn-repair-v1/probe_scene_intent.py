"""Compare the production C grammar with bounded model extraction, offline."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

from run_mlx_adapter import METADATA, MODEL, REVISION, WEIGHT, fetch, load_base_model
from semantic_eval import HERE, sha, write

ROOT = HERE.parents[2]
PACK = HERE / "scene-intent-challenge.json"
FIELDS = {"intent", "character_name", "proposed_spell"}
CONDITIONS = ("c_parser", "local_model")
SOURCES = [
    "benchmarks/response-quality/turn-repair-v1/scene_intent_probe.c",
    "voice/c-runtime/common/dnd_scene.c",
    "voice/c-runtime/common/utf8.c",
    "product/companions-frontend/c-companions/cmp_json.c",
]
HEADERS = [
    "voice/c-runtime/common/dnd_tools.h", "voice/c-runtime/common/utf8.h",
    "voice/c-runtime/common/dnd_retrieval_types.h", "voice/c-runtime/wire/pb_min.h",
    "contracts/handler-base/c-pb/pb_dnd_request.h",
    "contracts/handler-base/c-pb/pb_dnd_campaign.h",
    "contracts/handler-base/c-pb/pb_dnd_action.h",
    "product/companions-frontend/c-companions/cmp_json.h",
]
INCLUDES = ["voice/c-runtime/common", "voice/c-runtime/wire",
            "contracts/handler-base/c-pb", "product/companions-frontend/c-companions"]


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("Duplicate JSON field")
        value[key] = item
    return value


def assess(case, response, finish_reason="stop"):
    value = None
    failures = []
    try:
        value = json.loads(response, object_pairs_hook=unique_object)
        if not isinstance(value, dict) or set(value) != FIELDS:
            raise ValueError("Wrong output fields")
        if value["intent"] not in ("scene_presence_question", "other"):
            raise ValueError("Unknown intent")
        for field in ("character_name", "proposed_spell"):
            text = value[field]
            if not isinstance(text, str) or len(text.encode()) > 200:
                raise ValueError("Invalid extracted label")
            if text and (text not in case["utterance"] or any(ord(c) < 32 for c in text)):
                raise ValueError("Label is not a verbatim bounded span")
        if value["intent"] == "other" and (value["character_name"] or value["proposed_spell"]):
            raise ValueError("Other intent must clear extracted labels")
        if value["intent"] == "scene_presence_question" and not value["character_name"]:
            raise ValueError("Lookup needs an explicit character label")
        if finish_reason != "stop":
            raise ValueError("Generation did not stop")
    except (ValueError, TypeError, UnicodeError) as error:
        failures.append(str(error))
    valid = not failures
    raw_lookup = isinstance(value, dict) and value.get("intent") == "scene_presence_question"
    return {
        "valid": valid, "exact": valid and value == case["target"],
        "lookup_requested": raw_lookup,
        "unsupported_lookup_proposal": raw_lookup and case["target"]["intent"] == "other",
        "failures": failures,
        "prediction": value if valid else None,
    }


def load_cases(pack):
    cases = pack["cases"]
    if len(cases) != 24 or len({c["case_id"] for c in cases}) != 24:
        raise ValueError("Expected 24 distinct development cases")
    if len({c["family"] for c in cases}) != 6:
        raise ValueError("Expected six complete scenario families")
    if any(sum(c["family"] == f for c in cases) != 4 for f in {c["family"] for c in cases}):
        raise ValueError("Unbalanced scenario families")
    if sum(c["target"]["intent"] == "scene_presence_question" for c in cases) != 14:
        raise ValueError("Missing lookup or decline controls")
    for case in cases:
        if not assess(case, json.dumps(case["target"]))["exact"]:
            raise ValueError("Invalid authored target")
    return cases


def model_messages(pack, case):
    return [{"role": "system", "content": pack["system_prompt"]},
            {"role": "user", "content": json.dumps({"utterance": case["utterance"]}, ensure_ascii=False)}]


def summarize(cases, pairs):
    if [p["case_id"] for p in pairs] != [c["case_id"] for c in cases]:
        raise ValueError("Incomplete, duplicate, or reordered results")
    report = {}
    for condition in CONDITIONS:
        rows = [p[condition] for p in pairs]
        positive = [r for c, r in zip(cases, rows) if c["target"]["intent"] == "scene_presence_question"]
        negative = [r for c, r in zip(cases, rows) if c["target"]["intent"] == "other"]
        repairs = [r for c, r in zip(cases, rows) if c["target"]["proposed_spell"]]
        report[condition] = {
            "cases": len(rows), "valid": sum(r["score"]["valid"] for r in rows),
            "exact": sum(r["score"]["exact"] for r in rows),
            "lookup_controls": len(positive), "exact_lookups": sum(r["score"]["exact"] for r in positive),
            "decline_controls": len(negative), "exact_declines": sum(r["score"]["exact"] for r in negative),
            "repair_controls": len(repairs), "exact_repairs": sum(r["score"]["exact"] for r in repairs),
            "unsupported_lookup_proposals": sum(r["score"]["unsupported_lookup_proposal"] for r in rows),
            "median_complete_ms": statistics.median(r["milliseconds"] for r in rows),
        }
    return report


def build_parser(output):
    source = output / "source"
    for name in SOURCES + HEADERS:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((ROOT / name).read_bytes())
    binary = output / "scene-intent-probe"
    command = ["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", *["-I" + str(source / p) for p in INCLUDES],
               *[str(source / p) for p in SOURCES], "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, check=True, timeout=60)
    (output / "compile.log").write_bytes(built.stdout + built.stderr)
    write(output / "compiler.json", {"command": command,
          "version": subprocess.check_output(["clang", "--version"], text=True),
          "binary_sha256": sha(binary.read_bytes())})
    return binary


def run(output):
    import importlib.metadata
    import mlx.core as mx
    from mlx.utils import tree_flatten
    from mlx_lm import stream_generate
    from mlx_lm.sample_utils import make_sampler
    from mlx_lm.utils import load_tokenizer
    import numpy as np

    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT).strip():
        raise ValueError("Commit source before running this experiment")
    pack = json.loads(PACK.read_bytes())
    cases = load_cases(pack)
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    write(output / "cases.json", pack)
    for name in ("probe_scene_intent.py", "run_mlx_adapter.py", "semantic_eval.py"):
        (output / name).write_bytes((HERE / name).read_bytes())
    write(output / "recipe.json", {
        "schema": "waterdeep-scene-intent-study/v1",
        "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "case_pack_sha256": sha(PACK.read_bytes()), "model": MODEL, "model_revision": REVISION,
        "weight_sha256": WEIGHT[1], "weight_bytes": WEIGHT[0],
        "temperature": 0, "seed": 0, "maximum_new_tokens": 128, "maximum_prompt_tokens": 1024,
        "runtime_versions": {n: importlib.metadata.version(n) for n in ("mlx", "mlx-lm", "transformers", "numpy")},
        "training_performed": False, "production_gate": False,
        "design": "Known C grammar coverage controls plus authored paraphrases and scope exclusions; not a population sample.",
        "limits": ["No independent human labels.", "No lookup is executed and no scene fact is supplied.",
                   "Extracted text cannot confer action authority.", "C subprocess timing includes process launch and sanitizers; model timing is complete generation."],
    })
    started = time.monotonic()
    def check_time():
        if time.monotonic() - started > 1200:
            raise TimeoutError("Experiment deadline exceeded")
    binary = build_parser(output)
    pairs = []
    for case in cases:
        begin = time.monotonic()
        result = subprocess.run([str(binary), case["utterance"]], capture_output=True, text=True, timeout=5, check=True)
        if result.stderr:
            raise ValueError("C parser emitted sanitizer diagnostics")
        pairs.append({"case_id": case["case_id"], "c_parser": {
            "response": result.stdout, "response_sha256": sha(result.stdout.encode()),
            "milliseconds": (time.monotonic() - begin) * 1000,
            "score": assess(case, result.stdout),
        }})
    write(output / "c-baseline.json", pairs)
    metadata = output / "metadata"
    metadata.mkdir()
    for name, identity in METADATA.items():
        check_time()
        (metadata / name).write_bytes(fetch(name, identity, git_blob=True).read())
    tokenizer = load_tokenizer(metadata, tokenizer_config_extra={"trust_remote_code": False, "local_files_only": True})
    prompts = [tokenizer.apply_chat_template(model_messages(pack, c), tokenize=True, return_dict=False, add_generation_prompt=True) for c in cases]
    if any(len(p) > 1024 for p in prompts):
        raise ValueError("Prompt exceeds fixed token limit")
    write(output / "prompts.json", [{"case_id": c["case_id"], "messages": model_messages(pack, c), "tokens": len(p)} for c, p in zip(cases, prompts)])
    model = load_base_model(metadata, 0)
    model.eval()
    def hashes():
        return {n: sha(np.asarray(v.view(mx.uint8)).tobytes()) for n, v in tree_flatten(model.parameters())}
    before = hashes()
    write(output / "weights-before.json", before)
    warmup = tokenizer.apply_chat_template([{"role": "user", "content": "Say ready."}], tokenize=True, return_dict=False, add_generation_prompt=True)
    for _ in stream_generate(model, tokenizer, warmup, max_tokens=8, sampler=make_sampler(temp=0)):
        check_time()
    with (output / "pairs.jsonl").open("x") as journal:
        for case, prompt, pair in zip(cases, prompts, pairs):
            check_time()
            text, tokens, last = "", [], None
            begin = time.monotonic()
            for last in stream_generate(model, tokenizer, prompt, max_tokens=128, sampler=make_sampler(temp=0)):
                text += last.text
                tokens.append(int(last.token))
                check_time()
            if last is None:
                raise ValueError("Empty model stream")
            pair["local_model"] = {"response": text, "response_sha256": sha(text.encode()),
                "token_ids": tokens, "generation_tokens": last.generation_tokens, "finish_reason": last.finish_reason,
                "milliseconds": (time.monotonic() - begin) * 1000, "score": assess(case, text, last.finish_reason)}
            journal.write(json.dumps(pair, allow_nan=False) + "\n")
            journal.flush()
            print(json.dumps({"case_id": case["case_id"], "c_exact": pair["c_parser"]["score"]["exact"], "model_exact": pair["local_model"]["score"]["exact"]}), flush=True)
    after = hashes()
    write(output / "weights-after.json", after)
    if before != after:
        raise ValueError("Model weights changed during inference")
    report = {"schema": "waterdeep-scene-intent-study-result/v1", "conditions": summarize(cases, pairs),
              "weights_unchanged": True, "training_performed": False, "production_gate": False,
              "independent_human_review": False, "lookups_executed": 0, "game_mutations": 0,
              "elapsed_seconds": time.monotonic() - started, "mlx_peak_bytes": mx.get_peak_memory()}
    write(output / "report.json", report)
    write(output / "receipt.json", {"files": {str(p.relative_to(output)): sha(p.read_bytes()) for p in output.rglob("*") if p.is_file() and not p.name.startswith("._")}})
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    run(parser.parse_args().output)
