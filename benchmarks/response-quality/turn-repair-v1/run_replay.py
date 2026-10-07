"""Offline C lifecycle evaluator and Anvil report export; no deployment client."""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import uuid

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    os.umask(0o077)
    out = args.output.resolve()
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT))
    sources = [
        HERE / "replay.c",
        HERE / "action_core.h",
        pathlib.Path(__file__),
        HERE / "cases.json",
        ROOT / "product/anvil/src/lib/anvil/learning-contract.ts",
    ]
    manifest = {str(p.relative_to(ROOT)): sha(p) for p in sources}
    write(
        out / "source-manifest.json",
        {"base_revision": revision, "dirty": dirty, "files": manifest},
    )
    results = []
    compiler = subprocess.check_output(["cc", "--version"], text=True)
    for name, define in [
        ("reference", None),
        ("eager", "REPLAY_MUTANT_EAGER"),
        ("stale", "REPLAY_MUTANT_STALE"),
        ("duplicate", "REPLAY_MUTANT_DUPLICATE"),
    ]:
        binary = out / name
        command = [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O1",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
        ]
        if define:
            command.append("-D" + define)
        command.extend([str(HERE / "replay.c"), "-o", str(binary)])
        subprocess.run(command, check=True, timeout=60, capture_output=True)
        run = subprocess.run(
            [str(binary)], capture_output=True, text=True, timeout=15, check=False
        )
        rows = [json.loads(line) for line in run.stdout.splitlines()]
        summary = rows[-1] if rows else {}
        valid = (
            summary.get("checks") == 15
            and len(rows) == 16
            and not run.stderr
            and all(type(r.get("passed")) is bool for r in rows[:-1])
        )
        failed = sum(not r["passed"] for r in rows[:-1]) if valid else -1
        valid = valid and summary.get("failures") == failed
        expected_failure = define is not None
        passed = valid and (
            (run.returncode == 1 and failed > 0)
            if expected_failure
            else (run.returncode == 0 and failed == 0)
        )
        results.append(
            {
                "variant": name,
                "expected_failure": expected_failure,
                "evaluation_passed": passed,
                "exit_code": run.returncode,
                "binary_sha256": sha(binary),
                "command": command,
                "observations": rows,
                "stderr": run.stderr,
            }
        )
    if manifest != {str(p.relative_to(ROOT)): sha(p) for p in sources}:
        raise RuntimeError("Source changed during evaluation")
    evidence = {
        "schema": "dnd-turn-repair-replay/v1",
        "source_revision": revision,
        "source_dirty": dirty,
        "compiler": compiler,
        "host": platform.platform(),
        "model_tested": False,
        "audio_tested": False,
        "production_gate": False,
        "variants": results,
    }
    write(out / "replay-evidence.json", evidence)
    good = all(r["evaluation_passed"] for r in results)
    report = {
        "schemaVersion": "anvil-learning-report/v1",
        "goalId": "cohesive-homelab-models-v1",
        "trackId": "cohesion-speed",
        "runId": str(uuid.uuid4()),
        "sequence": 1,
        "recordedAt": datetime.datetime.now(datetime.UTC)
        .isoformat()
        .replace("+00:00", "Z"),
        "title": "Waterdeep turn repair: C lifecycle development replay",
        "status": "held" if good else "failed",
        "scope": "development",
        "summary": "Mock C lifecycle only. Both modular and native duplex remain candidates. "
        "No model, speech, production executor, or WebTransport behavior was evaluated. "
        "Source hashes bind the working snapshot; sourceRevision identifies its base commit.",
        "sourceRevision": revision,
        "progress": {
            "completed": sum(r["evaluation_passed"] for r in results),
            "total": 4,
            "unit": "checks",
        },
        "metrics": [
            {
                "name": "Lifecycle assertions passed",
                "value": sum(r["passed"] for r in results[0]["observations"][:-1]),
                "unit": "count",
                "direction": "higher",
            },
            {
                "name": "Fault variants detected",
                "value": sum(r["evaluation_passed"] for r in results[1:]),
                "unit": "count",
                "direction": "higher",
            },
        ],
        "blockers": [
            "No admitted modular or native duplex model comparison has run.",
            "Human audio, scene grounding, and production executor integration remain unmeasured.",
            "Development scenarios need human review; this report does not authorize training or promotion.",
        ],
        "evidence": [
            {"name": p.name, "sha256": sha(p)}
            for p in [out / "source-manifest.json", out / "replay-evidence.json"]
        ],
    }
    write(out / "anvil-learning-report.json", report)
    write(
        out / "evidence-receipt.json",
        {
            "schema": "dnd-turn-repair-receipt/v1",
            "files": {
                p.name: sha(p)
                for p in [
                    out / "source-manifest.json",
                    out / "replay-evidence.json",
                    out / "anvil-learning-report.json",
                ]
            },
        },
    )
    print(
        json.dumps(
            {
                "checks_passed": good,
                "anvil_report": str(out / "anvil-learning-report.json"),
            }
        )
    )
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
