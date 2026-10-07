"""Rescore retained text responses on identical-utterance context contrasts."""

import argparse
import itertools
import json
import os
from collections import defaultdict
from pathlib import Path

from semantic_eval import HERE, assess_target, sha, write

TURN_GROUPS = {"new-turn-evaluation", "original-turn-development"}
PRIMARY = ("action_state", "presence", "commit_requested")
TARGET = set(PRIMARY) | {"dialogue_act"}


def analyze(review):
    if review.get("schema") != "waterdeep-mlx-ablation-response-review/v1":
        raise ValueError("Unknown response archive")
    cases = {}
    groups = defaultdict(list)
    for case in review["cases"]:
        identity = case["case_id"]
        if identity in cases:
            raise ValueError("Duplicate case")
        cases[identity] = case
        if case["group"] not in TURN_GROUPS:
            continue
        if set(case["target"]) != TARGET:
            raise ValueError("Unexpected turn target")
        # Validate labels with the same contract used for generated responses.
        target_score = assess_target(case["target"], json.dumps({**case["target"], "answer": "label"}))
        if not target_score["valid"]:
            raise ValueError("Invalid turn target")
        prompt = json.loads(case["prompt"])
        if set(prompt) != {"utterance", "evidence"} or not isinstance(prompt["utterance"], str):
            raise ValueError("Unexpected turn input")
        if not isinstance(prompt["evidence"], dict):
            raise ValueError("Invalid context evidence")
        # Never pair different system prompts or different evaluation partitions.
        groups[(case["group"], case["system"], prompt["utterance"])].append(identity)
    if not cases:
        raise ValueError("Empty response archive")
    trials = {}
    conditions = set()
    for trial in review["trials"]:
        key = (trial["condition"], trial["case_id"])
        if key in trials or key[1] not in cases:
            raise ValueError("Unknown or duplicate trial")
        if trial["group"] != cases[key[1]]["group"]:
            raise ValueError("Trial partition differs from case")
        if sha(trial["response"].encode()) != trial["response_sha256"]:
            raise ValueError("Changed response bytes")
        conditions.add(key[0])
        trials[key] = trial
    if not conditions or set(trials) != set(itertools.product(conditions, cases)):
        raise ValueError("Incomplete condition matrix")
    contrasts = []
    for (partition, system, utterance), identities in sorted(groups.items()):
        pairs = []
        for left, right in itertools.combinations(sorted(identities), 2):
            a, b = cases[left], cases[right]
            changed = sorted(k for k in TARGET if a["target"][k] != b["target"][k])
            if not changed:
                continue
            if json.loads(a["prompt"])["evidence"] == json.loads(b["prompt"])["evidence"]:
                raise ValueError("Different labels have identical context")
            pairs.append({"cases": [left, right], "changed_fields": changed,
                          "primary_contrast": any(k in PRIMARY for k in changed)})
        if pairs:
            contrasts.append({"partition": partition, "utterance": utterance,
                              "system_sha256": sha(system.encode()), "pairs": pairs,
                              "cases": sorted({i for p in pairs for i in p["cases"]})})
    if not contrasts:
        raise ValueError("No matched context contrasts")
    results = {}
    members = sorted({i for group in contrasts for i in group["cases"]})
    for condition in sorted(conditions):
        scores = {}
        for identity in members:
            trial = trials[(condition, identity)]
            score = assess_target(cases[identity]["target"], trial["response"])
            if trial.get("finish_reason") != "stop":
                score.update(valid=False, primary_correct=False, exact=False)
                score["failures"].append("incomplete_generation")
            scores[identity] = score
        outcomes = []
        for group in contrasts:
            pairs = [{**p, "primary_correct": (all(scores[i]["primary_correct"] for i in p["cases"])
                                           if p["primary_contrast"] else None),
                      "exact": all(scores[i]["exact"] for i in p["cases"])}
                     for p in group["pairs"]]
            outcomes.append({"cases": group["cases"], "pairs": pairs,
                             "primary_correct": all(scores[i]["primary_correct"] for i in group["cases"]),
                             "exact": all(scores[i]["exact"] for i in group["cases"])})
        pairs = [p for g in outcomes for p in g["pairs"]]
        results[condition] = {
            "member_cases": len(members),
            "valid_members": sum(s["valid"] for s in scores.values()),
            "primary_members_correct": sum(s["primary_correct"] for s in scores.values()),
            "exact_members": sum(s["exact"] for s in scores.values()),
            "primary_pairs": sum(p["primary_contrast"] for p in pairs),
            "primary_pairs_correct": sum(p["primary_correct"] is True for p in pairs),
            "exact_pairs": len(pairs), "exact_pairs_correct": sum(p["exact"] for p in pairs),
            "context_groups": len(outcomes),
            "primary_groups_correct": sum(g["primary_correct"] for g in outcomes),
            "exact_groups_correct": sum(g["exact"] for g in outcomes),
            "groups": outcomes, "scores": scores,
        }
    return {"contrasts": contrasts, "conditions": results,
            "archive_counts": {"cases": len(cases), "conditions": len(conditions), "trials": len(trials)}}


def audit(review_path, evidence_path):
    raw, metadata_raw = review_path.read_bytes(), evidence_path.read_bytes()
    metadata, review = json.loads(metadata_raw), json.loads(raw)
    if metadata.get("schema") != "waterdeep-mlx-ablation-evidence/v1":
        raise ValueError("Unknown evidence record")
    if sha(raw) != metadata["response_review_sha256"]:
        raise ValueError("Response archive differs from committed evidence")
    if review["source_receipt_sha256"] != metadata["run"]["receipt_sha256"]:
        raise ValueError("Archive source receipt mismatch")
    report = analyze(review)
    counts = report["archive_counts"]
    if any(counts[k] != metadata["result"][v] for k, v in
           (("cases", "cases"), ("conditions", "conditions"), ("trials", "generations"))):
        raise ValueError("Archive counts differ from evidence")
    return {"schema": "waterdeep-context-pair-audit/v1", **report,
            "source": {"response_archive_sha256": sha(raw), "evidence_record_sha256": sha(metadata_raw),
                       "source_receipt_sha256": review["source_receipt_sha256"],
                       "model": metadata["recipe"]["model"], "model_revision": metadata["recipe"]["revision"],
                       "audit_source_sha256": sha(Path(__file__).read_bytes()),
                       "scorer_source_sha256": sha((HERE / "semantic_eval.py").read_bytes())},
            "new_generations": 0, "new_training": False, "production_gate": False,
            "limitations": ["Retrospective text analysis of committed responses; no new audio or model run.",
                            "Original full run directories are not reverified by this archive check.",
                            "Labels lack independent human review; this is development evidence.",
                            "Pairs share cases; seeds repeat scenarios and are not independent observations.",
                            "This does not evaluate floor yielding, interruption timing, or live game effects."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review", type=Path, default=HERE / "evidence/2026-09-12-mlx-ablation-responses.json")
    parser.add_argument("--evidence", type=Path, default=HERE / "evidence/2026-09-12-mlx-ablation.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.review, args.evidence)
    os.umask(0o077)
    write(args.output, result)
    for condition, scores in result["conditions"].items():
        print(condition, json.dumps({k: v for k, v in scores.items() if k not in ("groups", "scores")}))
