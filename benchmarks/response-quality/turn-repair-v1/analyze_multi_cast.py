"""Verify multi-event archives before reporting coverage and field accuracy."""

import argparse
from collections import Counter
import datetime
import json
import os
from pathlib import Path
import statistics
import uuid

import multi_cast_eval as m


def quote_diagnostic(rows):
    """Describe raw labels and ambiguous quotes without relaxing acceptance."""
    cases = {c["id"]: c for c in m.cases()}
    reviews = []
    for row in rows:
        case = cases[row["case_id"]]
        result = {"case_id": row["case_id"], "policy": row["policy"], "seed": row["seed"]}
        try:
            frame = json.loads(m.e.a.answer(row["response"]), object_pairs_hook=m.e.a.unique_object)
            m.e.a.require(type(frame) is dict and set(frame) == {"events"}, "Wrong shape")
            events = frame["events"]
            m.e.a.require(type(events) is list and len(events) <= m.MAX_EVENTS, "Wrong event list")
            anchors, values, issues = [], [], []
            for index, event in enumerate(events):
                m.e.a.require(type(event) is dict and set(event) == {"anchor", *m.e.FIELDS}, "Wrong event shape")
                m.e.a.require(type(event["anchor"]) is str, "Wrong anchor type")
                anchors.append(m.span(event["anchor"], case["utterance"]))
                fields = {}
                for field, allowed in m.e.FIELDS.items():
                    item = event[field]
                    m.e.a.require(type(item) is dict and set(item) == {"value", "quote"}, "Wrong field shape")
                    value, quote = item["value"], item["quote"]
                    m.e.a.require(type(value) is str and value in allowed and type(quote) is str, "Wrong value or quote")
                    m.e.a.require(len(quote.encode()) <= 2048, "Oversized quote")
                    fields[field] = value
                    occurrences = case["utterance"].encode().count(quote.encode()) if quote else 0
                    if (quote and occurrences != 1) or (not quote and value not in {"unknown", "unresolved"}):
                        issues.append({"event": index, "field": field, "quote": quote, "occurrences": occurrences})
                values.append(fields)
            expected = m.parse(json.dumps({"events": case["events"]}), case["utterance"])
            aligned = len(anchors) == len(expected) and all(
                left["end"] <= right["begin"] for left, right in zip(anchors, anchors[1:])) and all(
                [j for j, target in enumerate(expected) if anchor["begin"] < target["anchor"]["end"] and
                 target["anchor"]["begin"] < anchor["end"]] == [i]
                for i, anchor in enumerate(anchors))
            result.update(available=True, anchors_align_to_authored_order=aligned, quote_issues=issues,
                          values_match_authored_events=aligned and values == [v["values"] for v in expected])
        except (ValueError, KeyError, TypeError) as error:
            result.update(available=False, error=str(error))
        reviews.append(result)
    return {"scope": "Post hoc diagnostic after the first two responses; primary scores and retained responses remain unchanged",
            "evidence_accepted": False, "quote_entailment_proven": False, "reviews": reviews}


def summarize(rows):
    result = {}
    for policy in m.POLICIES:
        selected = [r for r in rows if r["policy"] == policy]
        counts = Counter(requests=len(selected), accepted=0, exact_all_events=0, all_joins_match=0,
                         missing_events=0, unmatched_events=0, ambiguous_anchors=0,
                         exact_matched_events=0, joins_hiding_wrong_fields=0)
        fields = Counter()
        for row in selected:
            a = row["assessment"]
            if not a["accepted"]:
                continue
            counts["accepted"] += 1
            counts["exact_all_events"] += a["exact_all_events"]
            counts["all_joins_match"] += a["all_joins_match"]
            for key, source in (("missing_events", "missing_events"), ("unmatched_events", "unmatched_events"), ("ambiguous_anchors", "ambiguous_event_anchors")):
                counts[key] += len(a[source])
            for check in a["checks"]:
                fields.update(check["incorrect_fields"])
                counts["exact_matched_events"] += check["exact_event"]
                counts["joins_hiding_wrong_fields"] += check["join_matches_authored_target"] and not check["exact_event"]
        counts["rejected"] = len(selected) - counts["accepted"]
        result[policy] = {"counts": dict(counts), "incorrect_fields": dict(fields),
                          "median_request_ms": statistics.median(r["request_elapsed_ms"] for r in selected) if selected else None}
    return result


def analyze(experiment, facts, output):
    root = experiment / "results"
    receipt = m.e.a.read(experiment / "receipt.json")
    identity = m.e.a.read(experiment / "source-identity.json")
    m.e.a.require(receipt["complete"] and receipt["stable_model_pod_identity"], "Incomplete operator")
    m.e.a.require(m.e.a.read(experiment / "identity-before.json") == m.e.a.read(experiment / "identity-after.json"), "Pod identity changed")
    m.e.a.require(receipt["source_revision"] == identity["revision"], "Source revision differs")
    m.e.a.require(receipt["results_manifest_sha256"] == m.e.a.digest((root / "manifest.json").read_bytes()), "Manifest pin differs")
    for name in m.SOURCES:
        m.e.a.require(identity["files"]["benchmarks/response-quality/turn-repair-v1/" + name] == m.e.a.digest((root / name).read_bytes()), "Source snapshot differs")
    m.e.a.require(identity["files"]["benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json"] == m.e.a.digest((root / "cases.json").read_bytes()), "Case snapshot differs")
    m.e.a.require(identity["files"]["voice/c-runtime/tests/test_dnd_source_compiler.py"] == m.e.a.digest((root / "source-decoder.py").read_bytes()), "Decoder snapshot differs")
    verified = m.verify(root, experiment / "source", facts, receipt["facts_receipt_sha256"], experiment / "model-template.json")
    rows = [json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()]
    report = {"schema": "waterdeep-multi-cast-analysis/v1", "source_revision": receipt["source_revision"],
              "verification": verified, "conditions": summarize(rows),
              "quote_diagnostic": quote_diagnostic(rows),
              "reviews": [{k: r[k] for k in ("case_id", "policy", "seed", "assessment", "request_elapsed_ms")} for r in rows],
              "private_artifacts": {str(p): m.e.a.digest(p.read_bytes()) for p in
                                    (experiment / "receipt.json", experiment / "source-identity.json", root / "manifest.json")},
              "analyzer_sha256": m.e.a.digest(Path(__file__).read_bytes()),
              "scope": "Sixteen visible Codex-authored cases in eight families; not independent evaluation",
              "timing_scope": "Complete request duration through loopback tunnel; not TTFA",
              "coverage_scope": "Missing-event counts cover parsed frames only; rejected frames are separate failures",
              "label_review": "Codex self-review; independent human review absent",
              "whole_history_proven": False, "training_admitted": False, "production_path": False}
    output.mkdir()
    m.e.a.write(output / "report.json", report)
    (output / "analyzer.py").write_bytes(Path(__file__).read_bytes())
    anvil = {"schemaVersion": "anvil-learning-report/v1", "goalId": "cohesive-homelab-models-v1",
             "trackId": "cohesion-speed", "runId": str(uuid.uuid4()), "sequence": 1,
             "recordedAt": datetime.datetime.now(datetime.UTC).isoformat().replace("+00:00", "Z"),
             "title": "Waterdeep: multi-event cast corrections", "status": "held", "scope": "development",
             "sourceRevision": receipt["source_revision"],
             "summary": "Real-model event extraction with strict quote and event-coverage checks. Source facts join after extraction. No complete history, resource authorization, training admission, or live activation.",
             "progress": {"completed": len(rows), "total": len(rows), "unit": "checks"},
             "metrics": [{"name": policy + " " + key, "value": values["counts"][key], "unit": "count", "direction": direction}
                         for policy, values in report["conditions"].items()
                         for key, direction in (("exact_all_events", "higher"), ("missing_events", "lower"),
                                                ("unmatched_events", "lower"), ("ambiguous_anchors", "lower"),
                                                ("joins_hiding_wrong_fields", "lower"), ("rejected", "lower"))],
             "blockers": ["Labels lack independent human review.", "The visible corpus is not a sealed evaluation set.",
                          "Claims do not establish complete history or authorized resources.", "Human voice acceptance remains open."],
             "evidence": [{"name": "report.json", "sha256": m.e.a.digest((output / "report.json").read_bytes())}]}
    m.e.a.write(output / "anvil-learning-report.json", anvil)
    m.e.a.write(output / "receipt.json", {"files": {p.name: m.e.a.digest(p.read_bytes()) for p in output.iterdir()
                                                   if p.is_file() and not p.name.startswith("._")}})
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("experiment", "facts", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(analyze(args.experiment, args.facts, args.output)["conditions"], indent=2))
