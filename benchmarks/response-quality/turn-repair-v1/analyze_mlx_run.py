"""Verify a local adapter comparison and replay proposals through the mock C boundary."""

import argparse
import json
import statistics
import subprocess
from pathlib import Path

from admit_model_outputs import admission_input, execute
from semantic_eval import HERE, assess_target, sha, write


def verified_files(root, schema):
    receipt_bytes = (root / "receipt.json").read_bytes()
    receipt = json.loads(receipt_bytes)
    if receipt.get("schema") != schema:
        raise ValueError("Unexpected local adapter receipt")
    files = {}
    for name, digest in receipt["files"].items():
        relative = Path(name)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or any(
                (root / Path(*relative.parts[:i])).is_symlink()
                for i in range(1, len(relative.parts) + 1)
            )
        ):
            raise ValueError("Invalid evidence path")
        path = root / relative
        if path.stat().st_size > 12 * 1024**2:
            raise ValueError("Evidence file exceeds bound")
        files[name] = path.read_bytes()
        if sha(files[name]) != digest:
            raise ValueError("Changed local adapter evidence")
    return files, sha(receipt_bytes)


def verify_response(case, result):
    if sha(result["response"].encode()) != result["response_sha256"]:
        raise ValueError("Changed model response")
    if "target" in case:
        score = assess_target(case["target"], result["response"])
        if result["finish_reason"] != "stop":
            score.update(valid=False, exact=False, primary_correct=False)
        if score != result["score"]:
            raise ValueError("Changed model score")


def load_comparison(root):
    files, receipt_hash = verified_files(root, "waterdeep-ram-qlora-receipt/v1")
    required = {"evaluation-cases.json", "pairs.jsonl", "recipe.json", "report.json"}
    if not required <= files.keys():
        raise ValueError("Required evidence is absent from the receipt")
    cases_list = json.loads(files["evaluation-cases.json"])
    cases = {c["case_id"]: c for c in cases_list}
    pairs = [json.loads(line) for line in files["pairs.jsonl"].splitlines()]
    if (
        len(cases) != 46
        or len(cases_list) != 46
        or len(pairs) != 46
        or {p["case_id"] for p in pairs} != set(cases)
    ):
        raise ValueError("Incomplete paired comparison")
    for pair in pairs:
        case = cases[pair["case_id"]]
        if pair["group"] != case["group"]:
            raise ValueError("Changed evaluation group")
        for mode in ("base", "adapter"):
            verify_response(case, pair[mode])
    return cases, pairs, receipt_hash


def load_ablation(root):
    from run_mlx_ablation import SEEDS, VARIANTS

    files, receipt_hash = verified_files(root, "waterdeep-mlx-ablation-receipt/v1")
    required = {"evaluation-cases.json", "trials.jsonl", "recipe.json", "report.json"}
    if not required <= files.keys():
        raise ValueError("Required evidence is absent from the receipt")
    conditions = ["base"] + [
        f"{variant}/seed-{seed}" for seed in SEEDS for variant in VARIANTS
    ]
    if json.loads(files["recipe.json"])["conditions"] != conditions:
        raise ValueError("Unexpected ablation conditions")
    cases_list = json.loads(files["evaluation-cases.json"])
    cases = {case["case_id"]: case for case in cases_list}
    rows = [json.loads(line) for line in files["trials.jsonl"].splitlines()]
    expected = {(case_id, condition) for case_id in cases for condition in conditions}
    identities = {(row["case_id"], row["condition"]) for row in rows}
    if (
        len(cases) != 46
        or len(cases_list) != 46
        or len(rows) != 460
        or identities != expected
    ):
        raise ValueError("Incomplete ablation matrix")
    pairs = {
        key: {"case_id": key, "group": case["group"]} for key, case in cases.items()
    }
    for row in rows:
        case = cases[row["case_id"]]
        if row["group"] != case["group"]:
            raise ValueError("Changed evaluation group")
        verify_response(case, row)
        pairs[row["case_id"]][row["condition"]] = row
    return cases, list(pairs.values()), receipt_hash


def analyze(root, output, *, ablation=False):
    cases, pairs, receipt_hash = (load_ablation if ablation else load_comparison)(root)
    modes = (
        [name for name in pairs[0] if name not in ("case_id", "group")]
        if ablation
        else ["base", "adapter"]
    )
    output.mkdir(parents=True, exist_ok=False)
    for name in (
        "action_core.h",
        "admission_main.c",
        "admit_model_outputs.py",
        "semantic_eval.py",
        "analyze_mlx_run.py",
        "run_mlx_ablation.py",
        "run_mlx_adapter.py",
    ):
        (output / name).write_bytes((HERE / name).read_bytes())
    binary = output / "admission"
    command = [
        "cc",
        "-std=c11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        str(output / "admission_main.c"),
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True, capture_output=True, timeout=60)
    results = []
    for pair in pairs:
        case = cases[pair["case_id"]]
        if "target" not in case:
            continue
        for mode in modes:
            response = pair[mode]
            wire = admission_input(json.loads(case["prompt"]), response["score"])
            observed = execute(binary, wire)
            # Inspect a literal flag even when the full contract failed.
            # This diagnostic never admits an invalid response to the C kernel.
            try:
                parsed = json.loads(response["response"])
            except ValueError:
                parsed = None
            flag = parsed.get("commit_requested") if isinstance(parsed, dict) else None
            if type(flag) is not bool:
                flag = None
            expected = int(case["target"]["commit_requested"])
            results.append(
                {
                    "case_id": case["case_id"],
                    "group": case["group"],
                    "mode": mode,
                    "model_contract_valid": response["score"]["valid"],
                    "literal_commit_flag": flag,
                    "wire": wire,
                    "observed": observed,
                    "expected_new_commits": expected,
                }
            )
    report = {
        "schema": "waterdeep-mlx-admission/v1",
        "source_receipt_sha256": receipt_hash,
        "binary_sha256": sha(binary.read_bytes()),
        "compiler_command": command,
        "scope": "Real local model responses and synthetic trusted evidence through a mock C boundary.",
        "live_executor": False,
        "production_gate": False,
        "outcomes": results,
        "modes": {},
    }
    for mode in modes:
        selected = [r for r in results if r["mode"] == mode]
        positive = [r for r in selected if r["expected_new_commits"] == 1]
        report["modes"][mode] = {
            "turn_cases": len(selected),
            "invalid_contracts": sum(not r["model_contract_valid"] for r in selected),
            "literal_flags_unknown": sum(
                r["literal_commit_flag"] is None for r in selected
            ),
            "unsafe_literal_commit_flags": sum(
                r["literal_commit_flag"] is True and r["expected_new_commits"] == 0
                for r in selected
            ),
            "unsafe_mock_commits": sum(
                r["observed"]["new_commits"] > r["expected_new_commits"]
                for r in selected
            ),
            "positive_cases": len(positive),
            "positive_commits": sum(
                r["observed"]["new_commits"] == 1 for r in positive
            ),
            "median_full_generation_ms": statistics.median(
                p[mode]["milliseconds"] for p in pairs
            ),
            "median_generation_tokens_per_second": statistics.median(
                p[mode]["generation_tokens_per_second"] for p in pairs
            ),
            "total_generated_tokens": sum(p[mode]["generation_tokens"] for p in pairs),
            "length_limited_responses": sum(
                p[mode]["finish_reason"] != "stop" for p in pairs
            ),
        }
    write(output / "report.json", report)
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-mlx-admission-receipt/v1",
            "files": {
                p.name: sha(p.read_bytes()) for p in output.iterdir() if p.is_file()
            },
        },
    )
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ablation", action="store_true")
    args = parser.parse_args()
    print(
        json.dumps(
            analyze(args.run, args.output, ablation=args.ablation)["modes"], indent=2
        )
    )
