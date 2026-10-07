"""Interleave two source artifacts with direct controls through the same C stack."""

import argparse
import hashlib
from pathlib import Path

from coupled_dialogue_eval import model_identity, write
from grounded_rules_eval import CONDITIONS, load_cases, trial
from verify_coupled_dialogue import model_catalog, read, require
from verify_grounded_rules import verify


def run(endpoint, baseline, candidate, output, case_pack):
    cases = load_cases(case_pack)
    output.mkdir()
    sources = {
        "baseline": baseline.resolve(strict=True),
        "candidate": candidate.resolve(strict=True),
    }
    before = model_identity(endpoint)
    rows = {name: [] for name in sources}
    for name, source in sources.items():
        root = output / name
        root.mkdir()
        receipt = read(source / "receipt.json")
        for filename in ("index.dndsidx", "manifest.json"):
            require(
                hashlib.sha256((source / filename).read_bytes()).hexdigest()
                == receipt["files"][filename]["sha256"],
                "Source pin differs",
            )
        write(
            root / "design.json",
            {
                "conditions": CONDITIONS,
                "cases": cases,
                "case_pack_sha256": hashlib.sha256(case_pack.read_bytes()).hexdigest(),
                "repetitions": 1,
                "order": "Alternate source order and direct/retrieval order for successive cases",
                "source": receipt,
                "audio": False,
                "training": False,
                "scope": "Paired C product source-artifact pilot; real model and read-only Milvus; isolated auth and tools",
                "limitations": "Authored development questions, one response per condition, buffered relay, no held-out or latency claim",
            },
        )
        (root / "case-pack.json").write_bytes(case_pack.read_bytes())
        write(root / "model-before.json", before)
    for index, (case_id, message, _) in enumerate(cases):
        for name in tuple(sources)[:: 1 if index % 2 == 0 else -1]:
            for condition in CONDITIONS[:: 1 if index % 2 == 0 else -1]:
                row = trial(
                    endpoint,
                    sources[name],
                    output / name / f"{case_id}-{condition}",
                    case_id,
                    message,
                    condition,
                )
                rows[name].append(row)
                write(output / name / "results.json", rows[name])
                print(
                    f"{name} {case_id} {condition}: completed={row.get('completed')} model_calls={row.get('model_calls')} retrieval_calls={row.get('retrieval_calls')}",
                    flush=True,
                )
    after = model_identity(endpoint)
    require(model_catalog(before) == model_catalog(after), "Model catalog changed")
    results = {}
    for name, source in sources.items():
        write(output / name / "model-after.json", after)
        try:
            results[name] = verify(
                output / name, source, Path(__file__).resolve().parents[3], case_pack
            )
        except (ValueError, AssertionError, KeyError) as error:
            results[name] = {"verified": False, "error": str(error)}
    write(output / "integrity.json", results)
    require(
        all("error" not in r for r in results.values()),
        "One or more source conditions failed verification; retain all trials",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case-pack", type=Path, required=True)
    args = parser.parse_args()
    run(args.endpoint, args.baseline, args.candidate, args.output, args.case_pack)
