"""Post hoc source-fact substitution for one explicitly linked prior-cast case.

Actor, turn, spell identity, and lack of modifiers are manual fixture inputs.
This is not a natural-language event extractor or a new model trial.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess

import action_fact_eval as a
from analyze_action_facts import operator_identity
import spell_source_facts as s


def replay(experiment, facts_directory, output):
    source = experiment / "source"
    results = experiment / "results"
    verification = a.verify(results, source, experiment / "model-template.json")
    original_revision = operator_identity(results)
    manifest = a.read(results / "manifest.json")
    pins = a.read(source / "receipt.json")["files"]
    facts = s.verify(facts_directory, source, manifest["source_index_sha256"], pins["manifest.json"]["sha256"])
    matches = [f for f in facts if f["name"] == "fireball" and
               f["document"][0] == "41bb349a59377f7b72d42d0f"]
    a.require(len(matches) == 1 and matches[0]["valid_header"], "Missing or ambiguous fixture spell")
    fact = matches[0]
    a.require(fact["standard_cost"] == 1 and 0 <= fact["level"] <= 9, "Unsupported fixture casting cost")
    history = "action_cantrips" if fact["level"] == 0 else "incompatible"
    case = next(c for c in a.cases() if c["id"] == "earlier_levelled_spell")
    rows = [json.loads(line) for line in (results / "trials.jsonl").read_text().splitlines()]
    selected = [r for r in rows if r["case_id"] == case["id"]]
    a.require(len(selected) == 4 and all(r["interpretation"]["accepted"] for r in selected), "Trial subset differs")
    output.mkdir()
    binary, command = a.compile_probe(output, sanitize=True)
    assessments = []
    for row in selected:
        before = row["interpretation"]
        after_values = dict(before["values"], earlier_spell=history)
        encoded = a.project(after_values, case["plan"])
        result = a.execute(binary, [encoded])[0]
        a.require(tuple(result[:3]) == a.oracle(encoded), "C/oracle disagreement")
        assessments.append({"seed": row["seed"], "policy": row["policy"],
                            "before_values": before["values"], "after_values": after_values,
                            "before_budget": before["budget_result"], "after_budget": list(result[:3]),
                            "authored_expected_budget": case["budget"],
                            "after_matches": result[0] == case["budget"]})
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=s.ROOT, text=True).strip()
    snapshot_names = (*a.SOURCES, "replay_source_history.py", "spell_source_facts.py", "analyze_action_facts.py")
    snapshots = output / "source"
    snapshots.mkdir()
    for name in set(snapshot_names):
        raw = (s.HERE / name).read_bytes()
        committed = subprocess.check_output(["git", "show", revision + ":benchmarks/response-quality/turn-repair-v1/" + name], cwd=s.ROOT)
        a.require(raw == committed, "Uncommitted replay source")
        (snapshots / name).write_bytes(raw)
    report = {
        "schema": "waterdeep-source-history-replay/v1", "source_revision": revision,
        "original_model_source_revision": original_revision,
        "experiment_verification": verification,
        "original_manifest_sha256": a.digest((results / "manifest.json").read_bytes()),
        "facts_receipt_sha256": a.digest((facts_directory / "receipt.json").read_bytes()),
        "spell_fact": fact, "case_id": case["id"], "utterance_sha256": a.digest(case["utterance"].encode()),
        "manual_fixture": {"actor": "speaker", "time": "this_turn", "cast_occurred": True,
                           "spell": "fireball", "casting_modifiers": "none",
                           "source_printing": fact["document"][0]},
        "changed_field": "earlier_spell", "assessments": assessments,
        "before_false_fits": sum(r["before_budget"][0] == 3 for r in assessments),
        "after_false_fits": sum(r["after_budget"][0] == 3 for r in assessments),
        "new_model_requests": 0, "production_path": False, "training_admitted": False,
        "scope": "Post hoc intervention in four retained frames; manual cast linkage; other fact errors remain unchanged",
        "build": {"command": command, "binary_sha256": a.digest(binary.read_bytes())},
    }
    a.write(output / "report.json", report)
    a.write(output / "receipt.json", {"files": {str(p.relative_to(output)): a.digest(p.read_bytes())
                                               for p in output.rglob("*") if p.is_file() and not p.name.startswith("._")}})
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiment", required=True, type=Path)
    parser.add_argument("--facts", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    os.umask(0o077)
    result = replay(args.experiment, args.facts, args.output)
    print(json.dumps({key: result[key] for key in ("before_false_fits", "after_false_fits", "new_model_requests")}))
