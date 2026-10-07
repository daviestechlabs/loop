"""Rescore immutable semantic runs with the current, hash-bound development oracle."""

import argparse
import datetime
import json
import os
import statistics
import uuid
from pathlib import Path

from semantic_eval import HERE, PROMPTS, assess, sha, write


def load_run(root):
    receipt = json.loads((root / "receipt.json").read_text())
    if receipt.get("schema") != "waterdeep-semantics-receipt/v1":
        raise ValueError("Unknown run receipt")
    files = receipt["files"]
    required = {
        "trials.jsonl",
        "cases.json",
        "summary.json",
        "semantic_eval.py",
        "prompts.json",
        "models-before.json",
        "models-after.json",
    }
    if not required <= set(files):
        raise ValueError("Incomplete receipt")
    for name, digest in files.items():
        if Path(name).name != name or name in (".", "..") or (root / name).is_symlink():
            raise ValueError("Invalid receipt path")
        if sha((root / name).read_bytes()) != digest:
            raise ValueError("Changed source evidence")
    if (root / "cases.json").read_bytes() != (HERE / "cases.json").read_bytes():
        raise ValueError("Case packs differ")
    cases = {
        c["case_id"]: c for c in json.loads((root / "cases.json").read_text())["cases"]
    }
    rows = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    seen = set()
    for row in rows:
        key = (row["case_id"], row["condition"], row["repetition"])
        if key in seen or key[0] not in cases or key[1] not in PROMPTS:
            raise ValueError("Unknown or duplicate trial")
        seen.add(key)
        choice = row["response"]["choices"][0]
        row["corrected_score"] = assess(
            cases[key[0]], choice["message"].get("content") or ""
        )
        if choice.get("finish_reason") != "stop":
            row["corrected_score"].update(
                valid=False, exact=False, primary_correct=False
            )
            row["corrected_score"]["failures"].append("incomplete_generation")
    repetitions = {row["repetition"] for row in rows}
    if not repetitions or seen != {
        (c, p, r) for c in cases for p in PROMPTS for r in repetitions
    }:
        raise ValueError("Incomplete paired matrix")
    return rows


def compare(roots, output):
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": "waterdeep-semantic-comparison/v1",
        "runs": [],
        "scorer_sha256": sha((HERE / "semantic_eval.py").read_bytes()),
        "comparison_sha256": sha(Path(__file__).read_bytes()),
        "cases_sha256": sha((HERE / "cases.json").read_bytes()),
        "production_gate": False,
        "raw_model_ranking_eligible": False,
        "endpoint_confound": "The fast-responder bridge extracts spoken text from JSON; its state-field scores are not raw model scores.",
        "training_performed": False,
        "scoring_correction": "Visible room presence remains known when spell geometry is unknown.",
        "scope": "Synthetic text development observations, not full-duplex or human audio evidence.",
    }
    for index, root in enumerate(roots):
        rows = load_run(root)
        source_summary = json.loads((root / "summary.json").read_text())
        entry = {
            "source_directory": str(root.resolve()),
            "source_receipt_sha256": sha((root / "receipt.json").read_bytes()),
            "model_identity": source_summary["model_identity"],
            "conditions": {},
        }
        for condition in PROMPTS:
            selected = [r for r in rows if r["condition"] == condition]
            scores = [r["corrected_score"] for r in selected]
            entry["conditions"][condition] = {
                "trials": len(selected),
                "valid_outputs": sum(s["valid"] for s in scores),
                "primary_correct": sum(s["primary_correct"] for s in scores),
                "exact": sum(s["exact"] for s in scores),
                "unsafe_commit_proposals": sum(
                    s["unsafe_commit_proposal"] is True for s in scores
                ),
                "unsupported_presence": sum(
                    s["unsupported_presence"] is True for s in scores
                ),
                "response_ms_median": statistics.median(
                    r["response_ms"] for r in selected
                ),
                "failures": [
                    {
                        "case_id": r["case_id"],
                        "repetition": r["repetition"],
                        **r["corrected_score"],
                    }
                    for r in selected
                    if not r["corrected_score"]["exact"]
                ],
            }
        write(output / f"rescored-{index}.json", rows)
        report["runs"].append(entry)
    write(output / "comparison.json", report)
    anvil = {
        "schemaVersion": "anvil-learning-report/v1",
        "goalId": "cohesive-homelab-models-v1",
        "trackId": "cohesion-speed",
        "runId": str(uuid.uuid4()),
        "sequence": 1,
        "recordedAt": datetime.datetime.now(datetime.UTC)
        .isoformat()
        .replace("+00:00", "Z"),
        "title": "Waterdeep: measured D&D turn semantics",
        "status": "held",
        "scope": "development",
        "sourceRevision": source_summary["source_revision"],
        "summary": "Two prompt conditions measured on existing inference endpoints. Raw answers and corrected state scores are retained. No training or game writes occurred.",
        "progress": {
            "completed": sum(
                v["trials"] for r in report["runs"] for v in r["conditions"].values()
            ),
            "total": sum(
                v["trials"] for r in report["runs"] for v in r["conditions"].values()
            ),
            "unit": "cases",
        },
        "metrics": [
            {
                "name": f"endpoint-{i}-{condition} primary state accuracy",
                "value": values["primary_correct"] / values["trials"],
                "unit": "fraction",
                "direction": "higher",
            }
            for i, r in enumerate(report["runs"])
            for condition, values in r["conditions"].items()
        ],
        "blockers": [
            "Unsafe commit proposals remain in the capacity model output.",
            "Fast-responder normalization prevents a raw model comparison.",
            "Audio, live action effects, and independently reviewed labels remain unmeasured.",
        ],
        "evidence": [
            {
                "name": "comparison.json",
                "sha256": sha((output / "comparison.json").read_bytes()),
            }
        ],
    }
    write(output / "anvil-learning-report.json", anvil)
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-semantic-comparison-receipt/v1",
            "files": {
                p.name: sha(p.read_bytes()) for p in output.iterdir() if p.is_file()
            },
        },
    )
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    compare(args.runs, args.output)
    print(args.output / "comparison.json")
