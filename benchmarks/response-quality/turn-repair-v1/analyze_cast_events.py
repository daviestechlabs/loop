"""Verify and summarize real-model event extraction without hiding join errors."""

import argparse
from collections import Counter
import copy
import datetime
import json
import os
from pathlib import Path
import re
import statistics
import uuid

import cast_event_eval as e


def summarize(rows, field="policy", groups=e.POLICIES):
    result = {}
    for policy in groups:
        selected = [r for r in rows if r[field] == policy]
        counts = Counter(trials=len(selected), accepted=0, rejected=0, exact_events=0, correct_joins=0,
                         correct_joins_with_wrong_fields=0, false_current_spell_claims=0,
                         accepted_missed_current_spell_claims=0)
        fields = Counter()
        for row in selected:
            assessment = row["assessment"]
            if not assessment["accepted"]:
                counts["rejected"] += 1
                continue
            counts["accepted"] += 1
            counts["exact_events"] += assessment["exact_event"]
            counts["correct_joins"] += assessment["join_matches_authored_target"]
            counts["correct_joins_with_wrong_fields"] += assessment["join_matches_authored_target"] and not assessment["exact_event"]
            predicted = assessment["join"]["kind"]
            expected = e.EXPECTED_JOIN[row["case_id"]]
            counts["false_current_spell_claims"] += predicted == "levelled_action_claim" and expected != predicted
            counts["accepted_missed_current_spell_claims"] += expected == "levelled_action_claim" and predicted != expected
            fields.update(assessment["incorrect_fields"])
        result[policy] = {"counts": dict(counts), "incorrect_fields": dict(fields)}
    return result


def fence_diagnostic(rows, facts, field="policy", groups=e.POLICIES):
    copied = copy.deepcopy(rows)
    cases = {c["id"]: c for c in e.cases()}
    changed = []
    for row in copied:
        if row["assessment"]["accepted"]:
            continue
        try:
            text = e.a.answer(row["response"])
        except (ValueError, KeyError, TypeError):
            continue
        match = re.fullmatch(r"```json\r?\n(.*)\r?\n```", text, re.DOTALL)
        if match:
            row["response"]["choices"][0]["message"]["content"] = match.group(1)
            row["assessment"] = e.assess(row["response"], cases[row["case_id"]], facts)
            changed.append({"case_id": row["case_id"], "seed": row["seed"], field: row[field],
                            "accepted_after_removal": row["assessment"]["accepted"]})
    return {"scope": "Post hoc exact outer-fence removal only; primary responses and scores remain unchanged",
            "changed": changed, "conditions": summarize(copied, field, groups)}


def analyze(experiment, facts, output, decoding=False):
    if decoding:
        import cast_event_decoding_eval as study
        field, groups = "decoding", study.DECODINGS
    else:
        study = e
        field, groups = "policy", e.POLICIES
    root = experiment / "results"
    receipt = e.a.read(experiment / "receipt.json")
    identity = e.a.read(experiment / "source-identity.json")
    e.a.require(receipt["complete"] and receipt["stable_model_pod_identity"], "Operator incomplete")
    e.a.require(e.a.read(experiment / "identity-before.json") == e.a.read(experiment / "identity-after.json"), "Pod changed")
    e.a.require(receipt["results_manifest_sha256"] == e.a.digest((root / "manifest.json").read_bytes()), "Operator manifest pin differs")
    e.a.require(identity["revision"] == receipt["source_revision"], "Operator revision differs")
    for name in study.SOURCES:
        e.a.require(identity["files"]["benchmarks/response-quality/turn-repair-v1/" + name] == e.a.digest((root / name).read_bytes()), "Source snapshot differs")
    for name in ("cases.json", "conditions.json"):
        e.a.require(identity["files"]["benchmarks/response-quality/turn-repair-v1/synthetic/cast-events-20260928/" + name] == e.a.digest((root / name).read_bytes()), "Dataset snapshot differs")
    e.a.require(identity["files"]["voice/c-runtime/tests/test_dnd_source_compiler.py"] == e.a.digest((root / "source-decoder.py").read_bytes()), "Decoder snapshot differs")
    verification = study.verify(root, experiment / "source", facts, receipt["facts_receipt_sha256"], experiment / "model-template.json")
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    report = {"schema": "waterdeep-cast-events-analysis/v1", "source_revision": receipt["source_revision"],
              "experiment_manifest_sha256": e.a.digest((root / "manifest.json").read_bytes()),
              "operator_receipt_sha256": e.a.digest((experiment / "receipt.json").read_bytes()),
              "source_identity_sha256": e.a.digest((experiment / "source-identity.json").read_bytes()),
              "analyzer_sha256": e.a.digest(Path(__file__).read_bytes()), "verification": verification,
              "condition_field": field, "conditions": summarize(rows, field, groups),
              "fence_diagnostic": fence_diagnostic(rows, e.a.read(root / "facts.json"), field, groups),
              "reviews": [{"case_id": r["case_id"], "seed": r["seed"], field: r[field],
                           "response_sha256": e.a.digest(json.dumps(r["response"], sort_keys=True, separators=(",", ":")).encode()),
                           "assessment": r["assessment"]} for r in rows],
              "scope": "Eight visible synthetic cases in four pairs; two seeds are repeated trials, not independent scenarios",
              "label_review": "Codex self-review only; independent human review absent",
              "quote_validation": "Exact membership, not entailment",
              "whole_turn_history_proven": False, "budget_evaluated": False,
              "production_path": False, "training_admitted": False}
    if decoding:
        report["request_timing"] = {
            "scope": "Complete request wall time through loopback tunnel; includes overhead and shared contention; not product TTFA",
            "conditions": {group: {"median_ms": statistics.median(r["request_elapsed_ms"] for r in rows if r[field] == group),
                                    "max_ms": max(r["request_elapsed_ms"] for r in rows if r[field] == group)} for group in groups},
        }
    output.mkdir()
    e.a.write(output / "report.json", report)
    (output / "analyzer.py").write_bytes(Path(__file__).read_bytes())
    report_hash = e.a.digest((output / "report.json").read_bytes())
    anvil = {"schemaVersion": "anvil-learning-report/v1", "goalId": "cohesive-homelab-models-v1",
             "trackId": "cohesion-speed", "runId": str(uuid.uuid4()), "sequence": 1,
             "recordedAt": datetime.datetime.now(datetime.UTC).isoformat().replace("+00:00", "Z"),
             "title": "Waterdeep: cast-event " + ("decoding comparison" if decoding else "extraction and source joins"), "status": "held", "scope": "development",
             "sourceRevision": receipt["source_revision"],
             "summary": "Real-model extraction over eight synthetic cases with verified spell metadata joined after inference. Event accuracy and join accuracy are separate. No whole-turn history, budget, or product activation claim.",
             "progress": {"completed": len(rows), "total": len(rows), "unit": "cases"},
             "metrics": [{"name": policy + " " + key, "value": result["counts"][key], "unit": "count", "direction": direction}
                         for policy, result in report["conditions"].items()
                         for key, direction in (("exact_events", "higher"), ("correct_joins", "higher"),
                                                ("correct_joins_with_wrong_fields", "lower"), ("false_current_spell_claims", "lower"))],
             "blockers": ["Independent human label review is absent.", "Only one focal event per utterance is tested.",
                          "No real-model cantrip, modifier, multi-event, resource, or human voice coverage in this batch."],
             "evidence": [{"name": "report.json", "sha256": report_hash}]}
    e.a.write(output / "anvil-learning-report.json", anvil)
    e.a.write(output / "receipt.json", {"files": {p.name: e.a.digest(p.read_bytes()) for p in output.iterdir() if p.is_file()}})
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("experiment", "facts", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--decoding", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(analyze(args.experiment, args.facts, args.output, args.decoding)["conditions"], indent=2))
