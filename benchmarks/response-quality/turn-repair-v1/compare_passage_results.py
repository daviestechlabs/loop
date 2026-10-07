"""Compare verified source-artifact trials without grading model prose."""

import argparse
import json
from pathlib import Path

from grounded_rules_eval import load_cases
from passage_evidence import digest
from verify_coupled_dialogue import read, require
from verify_grounded_rules import verify
from test_dnd_source_compiler import decode


def compare(root, baseline, candidate, repo, case_pack):
    configs = [
        read(source / "receipt.json")["config"] for source in (baseline, candidate)
    ]
    changed = {
        key
        for key in configs[0].keys() | configs[1].keys()
        if configs[0].get(key) != configs[1].get(key)
    }
    require(
        changed <= {"RAG_SOURCE_INDEX_SHA256", "RAG_SOURCE_COMPILER_SHA256"},
        "Source conditions changed unrelated configuration",
    )
    proof = {
        name: verify(root / name, source, repo, case_pack)
        for name, source in [("baseline", baseline), ("candidate", candidate)]
    }
    data = [
        decode((source / "index.dndsidx").read_bytes())
        for source in (baseline, candidate)
    ]
    for key in ("scope", "documents", "records", "entries"):
        require(data[0][key] == data[1][key], "Legacy source table changed: " + key)
    require(
        not data[0].get("passages") and data[1].get("passages"),
        "Expected a passage-only source extension",
    )
    records = {
        name: {
            (r["case_id"], r["condition"]): r
            for r in read(root / name / "results.json")
        }
        for name in proof
    }
    first_params = None
    rows = []
    for case_id, question, rubric in load_cases(case_pack):
        row = {"case_id": case_id, "question": question, "rubric": rubric}
        for name in proof:
            for condition in ("direct", "live_retrieval"):
                trial = records[name][case_id, condition]
                model = read(
                    root / name / f"{case_id}-{condition}" / "model-exchanges.json"
                )[0]["request"]
                params = {k: v for k, v in model.items() if k != "messages"}
                if first_params is None:
                    first_params = params
                require(
                    params == first_params, "Cross-condition model settings changed"
                )
                if condition == "direct":
                    continue
                final = next(
                    e for e in trial["events"] if e["type"] == "text_completed"
                )
                citations = json.loads(final["metadata"]["cascade_retrieval_citations"])
                row[name] = {
                    "answer": trial["answer"],
                    "answer_sha256": digest(trial["answer"].encode()),
                    "logical_sources": len(citations),
                    "complete_passages": sum(
                        c.get("kind") == "complete_passage" for c in citations
                    ),
                    "original_records": sum(
                        len(c["witnesses"])
                        if c.get("kind") == "complete_passage"
                        else 1
                        for c in citations
                    ),
                    "source_names": [c["section"] for c in citations],
                    "retrieval_calls": trial["retrieval_calls"],
                }
        row["direct_control_equal"] = (
            records["baseline"][case_id, "direct"]["answer"]
            == records["candidate"][case_id, "direct"]["answer"]
        )
        row["retrieval_answer_equal"] = (
            row["baseline"]["answer"] == row["candidate"]["answer"]
        )
        rows.append(row)
    return {
        "schema": "waterdeep-passage-comparison/v1",
        "integrity": proof,
        "legacy_tables_identical": True,
        "model_parameters_identical": True,
        "case_pack_sha256": digest(case_pack.read_bytes()),
        "direct_controls_equal": sum(r["direct_control_equal"] for r in rows),
        "rows": rows,
        "semantic_quality_graded": False,
        "training_admitted": False,
        "live_activation": False,
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("root", type=Path)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--candidate", type=Path, required=True)
    p.add_argument("--repo", type=Path, required=True)
    p.add_argument("--case-pack", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    value = compare(a.root, a.baseline, a.candidate, a.repo, a.case_pack)
    with a.output.open("x") as out:
        out.write(json.dumps(value, indent=2) + "\n")
    print(json.dumps({k: v for k, v in value.items() if k != "rows"}, indent=2))
