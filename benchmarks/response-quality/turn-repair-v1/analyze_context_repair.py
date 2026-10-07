"""Verify completed conditional repairs and export held Anvil review evidence."""

import argparse
from collections import Counter
import datetime
import json
import os
from pathlib import Path
import re
import uuid

import context_repair_eval as r
import review_context_evidence as review


def verify_operator(experiment: Path, parent: Path, receipt_sha256: str) -> dict:
    a = r.x.e.a
    a.require(
        a.digest((experiment / "receipt.json").read_bytes()) == receipt_sha256,
        "Operator receipt pin differs",
    )
    receipt = a.read(experiment / "receipt.json")
    a.require(
        receipt["complete"] and receipt["stable_model_pod_identity"],
        "Incomplete operator",
    )
    a.require(
        receipt["parent_manifest_sha256"] == r.PARENT_MANIFEST,
        "Parent manifest differs",
    )
    a.require(
        receipt["parent_receipt_sha256"]
        == a.digest((parent / "receipt.json").read_bytes()),
        "Parent receipt differs",
    )
    before = a.read(experiment / "identity-before.json")
    a.require(
        before
        == a.read(experiment / "identity-after.json")
        == a.read(parent / "identity-after.json"),
        "Model Pod identity changed",
    )
    root = experiment / "results"
    a.require(
        receipt["results_manifest_sha256"]
        == a.digest((root / "manifest.json").read_bytes()),
        "Results manifest differs",
    )
    identity = a.read(experiment / "source-identity.json")
    a.require(
        re.fullmatch("[a-f0-9]{40}", receipt["source_revision"]) is not None
        and receipt["source_revision"] == identity["revision"],
        "Source revision differs",
    )
    a.require(
        identity["operator_sha256"]
        == a.digest((experiment / "operator.py").read_bytes()),
        "Operator source differs",
    )
    names = {
        "benchmarks/response-quality/turn-repair-v1/" + name: name
        for name in (*r.x.SOURCES, *r.SOURCES)
    }
    names.update(
        {
            "voice/c-runtime/common/" + name: "kernel/common/" + name
            for name in r.x.KERNEL_FILES
        }
    )
    names.update(
        {
            "voice/c-runtime/tests/test_dnd_source_compiler.py": "source-decoder.py",
            "benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json": "cases.json",
        }
    )
    a.require(set(identity["files"]) == set(names), "Source inventory differs")
    for source, snapshot in names.items():
        a.require(
            identity["files"][source] == a.digest((root / snapshot).read_bytes()),
            "Source snapshot differs",
        )
    return receipt


def ledger(rows: list[dict]) -> dict:
    # Reuse the reviewed byte-range diagnostic; arm names replace display policies only.
    report = review.ledger(
        [dict(row, policy=row["arm"], seed=row["request"]["seed"]) for row in rows]
    )
    report["counts"] = {
        arm: dict(
            Counter(
                e["location_relation"] for e in report["fields"] if e["policy"] == arm
            )
        )
        for arm in r.f.ARMS
    }
    report["scope"] = (
        "Diagnostic review of conditional repairs; no primary score changes or semantic approval"
    )
    return report


def analyze(
    experiment: Path, parent: Path, facts: Path, receipt_sha256: str, output: Path
) -> dict:
    receipt = verify_operator(experiment, parent, receipt_sha256)
    result = r.verify(experiment / "results", parent, facts)
    rows = [
        json.loads(line)
        for line in (experiment / "results/trials.jsonl").read_text().splitlines()
    ]
    report = dict(
        result,
        schema="waterdeep-context-repair-analysis/v1",
        source_revision=receipt["source_revision"],
        operator_receipt_sha256=receipt_sha256,
        parent_manifest_sha256=r.PARENT_MANIFEST,
        scope="Six visible failed development cases; one repair per arm; conditional results, not independent accuracy",
        timing_scope="Complete second-request duration including queue and tunnel; excludes parent request; not TTFA",
        label_review="Codex-authored labels; independent human semantic review absent",
        source_joins_scope="Spell metadata joins follow extraction; claims do not authorize resource or game state",
        private_artifacts={
            str(p): r.x.e.a.digest(p.read_bytes())
            for p in (
                experiment / "source-identity.json",
                experiment / "results/manifest.json",
                parent / "receipt.json",
            )
        },
    )
    output.mkdir()
    r.x.e.a.write(output / "report.json", report)
    evidence = ledger(rows)
    r.x.e.a.write(output / "ledger.json", evidence)
    (output / "ledger.md").write_text(review.markdown(evidence))
    for source in (Path(__file__), Path(review.__file__)):
        (output / source.name).write_bytes(source.read_bytes())
    anvil = {
        "schemaVersion": "anvil-learning-report/v1",
        "goalId": "cohesive-homelab-models-v1",
        "trackId": "cohesion-speed",
        "runId": str(uuid.uuid4()),
        "sequence": 1,
        "recordedAt": datetime.datetime.now(datetime.UTC)
        .isoformat()
        .replace("+00:00", "Z"),
        "title": "Waterdeep: conditional cast-evidence repair",
        "status": "held",
        "scope": "development",
        "sourceRevision": receipt["source_revision"],
        "summary": "One repair per feedback arm for six rejected development responses. Source and model identity, requests, assessments, and byte evidence verify. Conditional label agreement does not establish semantic support, general accuracy, or human voice quality.",
        "progress": {"completed": len(rows), "total": 18, "unit": "checks"},
        "metrics": [
            {
                "name": arm + " " + key,
                "value": values["counts"][key],
                "unit": "count",
                "direction": direction,
            }
            for arm, values in report["conditions"].items()
            for key, direction in (
                ("accepted", "higher"),
                ("exact_all_events", "higher"),
                ("rejected", "lower"),
                ("missing_events", "lower"),
                ("unmatched_events", "lower"),
                ("joins_hiding_wrong_fields", "lower"),
            )
        ],
        "blockers": [
            "Independent human semantic review remains absent.",
            "Byte membership does not prove quote relevance.",
            "Conditional visible cases do not establish independent accuracy.",
            "No resource authority or human voice gate is closed.",
        ],
        "evidence": [
            {"name": name, "sha256": r.x.e.a.digest((output / name).read_bytes())}
            for name in ("report.json", "ledger.json")
        ],
    }
    r.x.e.a.write(output / "anvil-learning-report.json", anvil)
    r.x.e.a.write(
        output / "receipt.json",
        {
            "files": {
                p.name: r.x.e.a.digest(p.read_bytes())
                for p in output.iterdir()
                if p.is_file() and not p.name.startswith("._")
            }
        },
    )
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("experiment", "parent", "facts", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--receipt-sha256", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(
        json.dumps(
            analyze(
                args.experiment,
                args.parent,
                args.facts,
                args.receipt_sha256,
                args.output,
            )["conditions"],
            indent=2,
        )
    )
