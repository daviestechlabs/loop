"""Verify retained context-ablation bytes, paired inputs, completeness, and scores."""

import argparse
import json
import math
from pathlib import Path

from followup_eval import CONDITIONS, SYSTEM, request, score, summarize
from semantic_eval import identity, sha


def verify(root):
    receipt = json.loads((root / "receipt.json").read_text())
    required = {
        "followup_eval.py",
        "followup-cases.json",
        "semantic_eval.py",
        "protocol.json",
        "models-before.json",
        "models-after.json",
        "trials.jsonl",
        "summary.json",
    }
    if (
        receipt.get("schema") != "waterdeep-followup-receipt/v1"
        or set(receipt["files"]) != required
    ):
        raise ValueError("Unexpected receipt files")
    for name, digest in receipt["files"].items():
        path = root / name
        if path.is_symlink() or not path.is_file() or sha(path.read_bytes()) != digest:
            raise ValueError("Changed evidence: " + name)

    def read(name):
        return json.loads((root / name).read_text())

    protocol = read("protocol.json")
    if protocol["system_prompt"] != SYSTEM or protocol["conditions"] != list(
        CONDITIONS
    ):
        raise ValueError("Different experiment protocol")
    before, after = read("models-before.json"), read("models-after.json")
    if identity(before) != identity(after):
        raise ValueError("Changed model aliases")
    cases = read("followup-cases.json")["cases"]
    if len(cases) != len({c["case_id"] for c in cases}):
        raise ValueError("Duplicate cases")
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    planned = [
        (case, condition)
        for i, case in enumerate(cases)
        for condition in (CONDITIONS if i % 2 == 0 else reversed(CONDITIONS))
    ]
    if len(rows) != len(planned):
        raise ValueError("Incomplete paired trials")
    models = {entry["id"] for entry in before["data"]}
    if not rows or len({row["request"]["model"] for row in rows}) != 1:
        raise ValueError("Experiment requires one model alias")
    for row, (case, condition) in zip(rows, planned, strict=True):
        model = row["request"]["model"]
        if (row["case_id"], row["condition"]) != (case["case_id"], condition):
            raise ValueError("Changed trial order or identity")
        if model not in models or row["request"] != request(case, condition, model):
            raise ValueError("Changed model input")
        if row["score"] != score(case, row["response"]):
            raise ValueError("Changed trial score")
        if not math.isfinite(row["response_ms"]) or row["response_ms"] < 0:
            raise ValueError("Invalid duration")
    if summarize(rows) != read("summary.json"):
        raise ValueError("Changed summary")
    return {
        "verified_trials": len(rows),
        "summary": summarize(rows),
        "receipt_sha256": sha((root / "receipt.json").read_bytes()),
        "production_gate": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.directory), indent=2))
