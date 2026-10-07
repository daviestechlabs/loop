"""Verify extraction evidence and separate fact, format, and budget failures."""

import argparse
from collections import Counter
import copy
import datetime
import json
import os
from pathlib import Path
import re
import uuid

import action_fact_eval as a


def summarize(rows):
    gold = {case["id"]: case for case in a.cases()}
    conditions = {}
    for policy in a.POLICIES:
        selected = [row for row in rows if row["policy"] == policy]
        counts = dict.fromkeys(("accepted_frames", "exact_frames", "budget_matches",
                               "matching_budget_with_wrong_facts", "invented_known_frames",
                               "invented_known_fields", "false_fitting_budgets"), 0)
        counts["trials"] = len(selected)
        fields, reviews = Counter(), []
        for row in selected:
            result = row["interpretation"]
            counts["accepted_frames"] += result["accepted"]
            if result["accepted"]:
                wrong, invented = result["incorrect_fields"], result["unsupported_known_fields"]
                matches = result["budget_matches_authored_expectation"]
                counts["exact_frames"] += not wrong
                counts["budget_matches"] += matches
                counts["matching_budget_with_wrong_facts"] += matches and bool(wrong)
                counts["invented_known_frames"] += bool(invented)
                counts["invented_known_fields"] += len(invented)
                counts["false_fitting_budgets"] += result["budget_result"][0] == 3 and gold[row["case_id"]]["budget"] != 3
                fields.update(wrong)
            reviews.append({"case_id": row["case_id"], "seed": row["seed"],
                            "response_sha256": a.digest(json.dumps(row["response"], sort_keys=True, separators=(",", ":")).encode()),
                            "assessment": result})
        conditions[policy] = {"counts": counts, "incorrect_fields": dict(fields), "reviews": reviews}
    return conditions


def fence_diagnostic(rows):
    # Post hoc analysis only. It does not rewrite retained responses or change
    # the experiment's parser, its primary scores, or any production admission.
    cases = {case["id"]: case for case in a.cases()}
    result = copy.deepcopy(rows)
    changed = []
    for row in result:
        if row["interpretation"]["accepted"]:
            continue
        try:
            text = a.answer(row["response"])
        except (ValueError, KeyError, TypeError):
            continue
        match = re.fullmatch(r"```json\r?\n(.*)\r?\n```", text, re.DOTALL)
        if match:
            row["response"]["choices"][0]["message"]["content"] = match.group(1)
            row["interpretation"] = a.interpretation(row, cases[row["case_id"]])
            changed.append({"case_id": row["case_id"], "seed": row["seed"], "policy": row["policy"],
                            "accepted_after_fence_removal": row["interpretation"]["accepted"]})
    return {"scope": "Post hoc exact outer-fence removal and oracle projection only; not primary scores or admission",
            "changed": changed, "conditions": summarize(result)}


def operator_identity(root):
    receipt = a.read(root.parent / "receipt.json")
    identity = a.read(root.parent / "source-identity.json")
    a.require(receipt["complete"] and receipt["stable_model_pod_identity"], "Operator run incomplete")
    a.require(receipt["results_manifest_sha256"] == a.digest((root / "manifest.json").read_bytes()), "Operator manifest differs")
    a.require(re.fullmatch(r"[0-9a-f]{40}", receipt["source_revision"]) is not None and identity["revision"] == receipt["source_revision"], "Source revision differs")
    for name in a.SOURCES:
        a.require(identity["files"]["benchmarks/response-quality/turn-repair-v1/" + name] == a.digest((root / name).read_bytes()), "Archived source identity differs")
    a.require(identity["files"]["voice/c-runtime/tests/test_dnd_source_compiler.py"] == a.digest((root / "source-decoder.py").read_bytes()), "Decoder identity differs")
    a.require(a.read(root.parent / "identity-before.json") == a.read(root.parent / "identity-after.json"), "Pod identity differs")
    return receipt["source_revision"]


def anvil_report(report, report_hash):
    conditions = report["conditions"]
    total = sum(c["counts"]["trials"] for c in conditions.values())
    return {
        "schemaVersion": "anvil-learning-report/v1", "goalId": "cohesive-homelab-models-v1",
        "trackId": "cohesion-speed", "runId": str(uuid.uuid4()), "sequence": 1,
        "recordedAt": datetime.datetime.now(datetime.UTC).isoformat().replace("+00:00", "Z"),
        "title": "Waterdeep: extracted action facts through C budget checks",
        "status": "held", "scope": "development", "sourceRevision": report["source_revision"],
        "summary": "Real-model extraction with exact utterance quotes and bounded C evaluation. Fact errors and budget errors are separate. Labels are authored development expectations. No training or game writes.",
        "progress": {"completed": total, "total": total, "unit": "cases"},
        "metrics": [{"name": policy + " " + metric, "value": result["counts"][metric], "unit": "count", "direction": direction}
                    for policy, result in conditions.items()
                    for metric, direction in (("exact_frames", "higher"), ("budget_matches", "higher"),
                                              ("matching_budget_with_wrong_facts", "lower"), ("false_fitting_budgets", "lower"))],
        "blockers": ["Independent human label review is absent.",
                     "Quote membership does not prove entailment or authorized campaign state.",
                     "Spell-plan resolution and rule translation are manual fixtures; production voice remains untested."],
        "evidence": [{"name": "report.json", "sha256": report_hash}],
    }


def analyze(root, source, template, output):
    verification = a.verify(root, source, template)
    revision = operator_identity(root)
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    report = {
        "schema": "waterdeep-action-fact-analysis/v1", "experiment_directory": str(root),
        "experiment_manifest_sha256": a.digest((root / "manifest.json").read_bytes()),
        "trials_sha256": a.digest((root / "trials.jsonl").read_bytes()),
        "analyzer_sha256": a.digest(Path(__file__).read_bytes()),
        "operator_receipt_sha256": a.digest((root.parent / "receipt.json").read_bytes()),
        "source_identity_sha256": a.digest((root.parent / "source-identity.json").read_bytes()),
        "source_revision": revision, "verification": verification,
        "conditions": summarize(rows), "fence_diagnostic": fence_diagnostic(rows),
        "label_origin": "Assistant-authored development expectations; independent human review absent",
        "quote_validation": "Input membership only; not semantic entailment or authorized campaign state",
        "scope": "Extracted resource/history fields and bounded C projection; fixed spell plan and manually translated rules",
        "training_admitted": False, "production_path": False, "human_voice_accepted": False,
    }
    output.mkdir()
    a.write(output / "report.json", report)
    lines = ["# Action fact extraction results", "", "Scores use authored development labels.",
             "A matching budget can hide incorrect facts.", "",
             "| Condition | Trials | Accepted | Exact facts | Matching budget | Matching budget with wrong facts | False fitting budget |",
             "|-----------|--------|----------|-------------|-----------------|----------------------------------|----------------------|"]
    for policy, result in report["conditions"].items():
        c = result["counts"]
        lines.append(f"| {policy} | {c['trials']} | {c['accepted_frames']} | {c['exact_frames']} | {c['budget_matches']} | {c['matching_budget_with_wrong_facts']} | {c['false_fitting_budgets']} |")
    lines += ["", "Quoted spans prove membership only.", "The model does not authorize an action.",
              "The separate fence diagnostic does not change these primary scores.",
              "Repeated authored cases do not establish unseen accuracy or live voice quality.", ""]
    (output / "report.md").write_text("\n".join(lines))
    a.write(output / "anvil-learning-report.json", anvil_report(report, a.digest((output / "report.json").read_bytes())))
    a.write(output / "receipt.json", {"files": {name: a.digest((output / name).read_bytes()) for name in ("report.json", "report.md", "anvil-learning-report.json")}})
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    result = analyze(args.root, args.source, args.template, args.output)
    print(json.dumps({p: v["counts"] for p, v in result["conditions"].items()}, indent=2))
