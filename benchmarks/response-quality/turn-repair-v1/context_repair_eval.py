"""One bounded repair attempt per feedback arm on verified parent rejections."""

from collections import Counter
import json
from pathlib import Path
import statistics
import tempfile
import time

import analyze_context_cast as parent_analysis
import context_repair_feedback as f

x = f.x
PARENT_MANIFEST = "206abebc8256797a105bd48b5553d25bda1093ef61739736ffca35b81785fe7a"
ELIGIBLE = (
    "plan_completion_a",
    "plan_completion_b",
    "actor_scope_b",
    "uncertainty_cancel_a",
    "unnamed_cantrip_a",
    "unnamed_cantrip_b",
)
SOURCES = ("context_repair_feedback.py", "context_repair_eval.py")


def parent_rows(parent: Path, facts: Path) -> list[dict]:
    """Verify the complete frozen study before selecting any repair opportunities."""
    x.e.a.require(
        x.e.a.digest((parent / "results/manifest.json").read_bytes())
        == PARENT_MANIFEST,
        "Parent manifest differs from frozen study",
    )
    with tempfile.TemporaryDirectory() as tmp:
        parent_analysis.analyze(parent, facts, Path(tmp) / "verified")
    rows = [
        json.loads(line)
        for line in (parent / "results/trials.jsonl").read_text().splitlines()
    ]
    selected = [
        r
        for r in rows
        if r["policy"] == "explicit_context" and not r["assessment"]["accepted"]
    ]
    x.e.a.require(
        tuple(r["case_id"] for r in selected) == ELIGIBLE, "Eligible cases differ"
    )
    return selected


def protocol() -> dict:
    return {
        "schema": "waterdeep-context-repair-protocol/v1",
        "parent_manifest_sha256": PARENT_MANIFEST,
        "eligible_cases": list(ELIGIBLE),
        "arms": list(f.ARMS),
        "trials": 18,
        "order": "Case order from parent; arm order rotates left by case index modulo three",
        "attempts_per_case_arm": 1,
        "socket_timeout_seconds": 240,
        "generation_inputs": "Original request, rejected candidate, and validator feedback only",
        "intervention": "Raw diagnostic versus field location versus location plus source occurrences",
        "primary": "Evidence acceptance and exact authored events, with coverage and source joins separate",
        "failure_policy": "Retain request and response or error, abort on service or evaluator failure; no retries",
        "sampling": "Preserve parent seed, temperature, schema, and 3072-token completion limit",
        "scope": "Conditional repair of six visible failed development cases; not independent evaluation",
        "semantic_support_proven": False,
        "training_admitted": False,
        "production_path": False,
    }


def matrix(rows: list[dict], binary: Path) -> list[dict]:
    """Construct requests without giving the feedback constructor authored labels."""
    x.e.a.require(
        tuple(r["case_id"] for r in rows) == ELIGIBLE, "Eligible cases differ"
    )
    result = []
    for index, row in enumerate(rows):
        text = row["request"]["messages"][1]["content"]
        utterance = x.Utterance(text, x.e.a.digest(text.encode()))
        candidate = x.e.a.answer(row["response"])
        offset = index % len(f.ARMS)
        for arm in f.ARMS[offset:] + f.ARMS[:offset]:
            result.append(
                {
                    "case_id": row["case_id"],
                    "arm": arm,
                    "parent_response_sha256": x.e.a.digest(
                        json.dumps(row["response"], sort_keys=True).encode()
                    ),
                    "request": f.request(
                        row["request"], utterance, candidate, binary, arm
                    ),
                }
            )
    return result


def summarize(rows: list[dict]) -> dict:
    result = {}
    for arm in f.ARMS:
        selected = [r for r in rows if r["arm"] == arm]
        counts = Counter(
            requests=len(selected),
            accepted=0,
            exact_all_events=0,
            missing_events=0,
            unmatched_events=0,
            ambiguous_anchors=0,
            exact_matched_events=0,
            joins_hiding_wrong_fields=0,
        )
        fields = Counter()
        for row in selected:
            a = row["assessment"]
            if not a["accepted"]:
                continue
            counts["accepted"] += 1
            counts["exact_all_events"] += a["exact_all_events"]
            for key, source in (
                ("missing_events", "missing_events"),
                ("unmatched_events", "unmatched_events"),
                ("ambiguous_anchors", "ambiguous_event_anchors"),
            ):
                counts[key] += len(a[source])
            for check in a["checks"]:
                fields.update(check["incorrect_fields"])
                counts["exact_matched_events"] += check["exact_event"]
                counts["joins_hiding_wrong_fields"] += (
                    check["join_matches_authored_target"] and not check["exact_event"]
                )
        counts["rejected"] = len(selected) - counts["accepted"]
        result[arm] = {
            "counts": dict(counts),
            "incorrect_fields": dict(fields),
            "median_request_ms": statistics.median(
                r["request_elapsed_ms"] for r in selected
            )
            if selected
            else None,
            "coverage_scope": "Accepted frames only; rejected frames remain separate failures",
        }
    return result


def verify(root: Path, parent: Path, facts: Path) -> dict:
    rows = parent_rows(parent, facts)
    manifest = x.e.a.read(root / "manifest.json")
    required = {
        *x.SOURCES,
        *SOURCES,
        *("kernel/common/" + n for n in x.KERNEL_FILES),
        "cases.json",
        "source-decoder.py",
        "protocol.json",
        "requests.json",
        "trials.jsonl",
        "models-before.json",
        "models-after.json",
        "build.json",
    }
    x.e.a.require(
        manifest["complete"] and required <= manifest["files"].keys(),
        "Incomplete repair archive",
    )
    for name, sha in manifest["files"].items():
        relative = Path(name)
        path = root / relative
        x.e.a.require(
            not relative.is_absolute()
            and ".." not in relative.parts
            and path.resolve().is_relative_to(root.resolve())
            and not path.is_symlink()
            and x.e.a.digest(path.read_bytes()) == sha,
            "Evidence changed",
        )
    for name in (*x.SOURCES, *SOURCES):
        x.e.a.require(
            (root / name).read_bytes() == (x.HERE / name).read_bytes(),
            "Evaluator source differs",
        )
    for name in x.KERNEL_FILES:
        x.e.a.require(
            (root / "kernel/common" / name).read_bytes()
            == (parent / "results/kernel/common" / name).read_bytes(),
            "Kernel source differs",
        )
    for name in ("cases.json", "source-decoder.py"):
        x.e.a.require(
            (root / name).read_bytes() == (parent / "results" / name).read_bytes(),
            "Parent snapshot differs",
        )
    x.e.a.require(x.e.a.read(root / "protocol.json") == protocol(), "Protocol differs")
    trials = [
        json.loads(line) for line in (root / "trials.jsonl").read_text().splitlines()
    ]
    case_map = {case["id"]: case for case in x.m.cases()}
    with tempfile.TemporaryDirectory() as tmp:
        binary, _ = x.compile_archive(root, Path(tmp))
        expected = matrix(rows, binary)
        x.e.a.require(
            x.e.a.read(root / "requests.json") == expected, "Prepared requests differ"
        )
        x.e.a.require(
            len(trials) == len(expected) == manifest["trials"] == 18,
            "Trial count differs",
        )
        for row, planned in zip(trials, expected, strict=True):
            x.e.a.require(
                all(row[key] == value for key, value in planned.items()),
                "Request or trial order differs",
            )
            x.e.a.require(
                row.get("service_error") is None
                and row.get("evaluation_error") is None,
                "Failed trial",
            )
            x.e.a.require(
                row["assessment"]
                == x.assess(
                    row["response"],
                    case_map[row["case_id"]],
                    x.e.a.read(parent / "results/facts.json"),
                    "explicit_context",
                    binary,
                ),
                "Assessment differs",
            )
            x.e.a.require(
                type(row["request_elapsed_ms"]) is int
                and row["request_elapsed_ms"] >= 0,
                "Invalid duration",
            )
    before = x.e.a.model_catalog(x.e.a.read(root / "models-before.json"))
    after = x.e.a.model_catalog(x.e.a.read(root / "models-after.json"))
    x.e.a.require(before == after, "Model catalog changed")
    return {
        "verified_trials": len(trials),
        "conditions": summarize(trials),
        "semantic_support_proven": False,
        "training_admitted": False,
        "production_path": False,
    }


def run(endpoint: str, parent: Path, facts: Path, output: Path) -> dict:
    rows = parent_rows(parent, facts)
    output.mkdir()
    x.snapshots(output)
    for name in SOURCES:
        (output / name).write_bytes((x.HERE / name).read_bytes())
    x.e.a.write(output / "protocol.json", protocol())
    cases = {case["id"]: case for case in x.m.cases()}
    source_facts = x.e.a.read(parent / "results/facts.json")
    complete, count = False, 0
    try:
        with tempfile.TemporaryDirectory() as tmp:
            binary, command = x.compile_archive(output, Path(tmp))
            x.e.a.write(
                output / "build.json",
                {
                    "command": command,
                    "binary_sha256": x.e.a.digest(binary.read_bytes()),
                },
            )
            requests = matrix(rows, binary)
            x.e.a.write(output / "requests.json", requests)
            client = x.e.a.Client(endpoint, timeout_seconds=240)
            before = client.request("/v1/models")
            x.e.a.require(
                requests[0]["request"]["model"] in [m["id"] for m in before["data"]],
                "Requested model absent",
            )
            x.e.a.write(output / "models-before.json", before)
            with (output / "trials.jsonl").open("x") as stream:
                for planned in requests:
                    row = dict(planned)
                    start = time.perf_counter_ns()
                    try:
                        try:
                            row["response"] = client.request(
                                "/v1/chat/completions", row["request"]
                            )
                        except Exception as error:
                            row["service_error"] = {
                                "type": type(error).__name__,
                                "message": str(error),
                            }
                            raise
                        finally:
                            row["request_elapsed_ms"] = (
                                time.perf_counter_ns() - start
                            ) // 1_000_000
                        try:
                            row["assessment"] = x.assess(
                                row["response"],
                                cases[row["case_id"]],
                                source_facts,
                                "explicit_context",
                                binary,
                            )
                        except Exception as error:
                            row["evaluation_error"] = {
                                "type": type(error).__name__,
                                "message": str(error),
                            }
                            raise
                    finally:
                        stream.write(json.dumps(row) + "\n")
                        stream.flush()
                        count += 1
                    print(
                        f"{count}/18 {row['case_id']} {row['arm']} accepted={row['assessment']['accepted']} exact={row['assessment'].get('exact_all_events', False)}",
                        flush=True,
                    )
            x.e.a.write(output / "models-after.json", client.request("/v1/models"))
            complete = x.e.a.model_catalog(before) == x.e.a.model_catalog(
                x.e.a.read(output / "models-after.json")
            )
            x.e.a.require(complete, "Model catalog changed")
    finally:
        x.e.a.write(
            output / "manifest.json",
            {
                "complete": complete,
                "trials": count,
                "files": {
                    str(p.relative_to(output)): x.e.a.digest(p.read_bytes())
                    for p in output.rglob("*")
                    if p.is_file() and not p.name.startswith("._")
                },
            },
        )
    result = verify(output, parent, facts)
    x.e.a.write(output / "verification.json", result)
    return result
