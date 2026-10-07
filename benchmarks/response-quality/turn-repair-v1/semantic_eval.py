"""Measure text interpretation using an existing loopback model endpoint."""

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import time
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ACTION_STATES = (
    "held",
    "none",
    "canceled",
    "committed",
    "eligible_for_governed_commit",
)
ACTS = (
    "answer_scene_question",
    "ask_for_scene_fact",
    "clarify_entity",
    "discard_stale_result",
    "reject_foreign_confirmation",
    "explain_committed_action",
    "ask_for_target_geometry",
    "state_player_uncertainty",
    "clarify_rules_and_geometry",
    "confirm_prepared_action",
    "return_saved_receipt",
    "resolve_condition",
    "revalidate_prepared_action",
)
FORMAT = (
    "Return only a JSON object with exactly these keys: dialogue_act, action_state, "
    "presence, commit_requested, answer. dialogue_act must be one of "
    + json.dumps(ACTS)
    + ". action_state must be one of "
    + json.dumps(ACTION_STATES)
    + ". presence must be present, absent, or unknown; it describes what can be asserted "
    "to this player now, not a guess. commit_requested is a boolean. "
    "answer is a short spoken reply. State fields describe your proposed next step; "
    "you cannot execute anything."
)
PROMPTS = {
    "minimal": "Interpret the latest D&D player utterance using the supplied context. "
    + FORMAT,
    "contract": (
        "Interpret the latest D&D player utterance using the supplied context. "
        "A correction or intervening question holds a proposed action. "
        "Quoted actions are not proposals. Conditional plans need evidence before action. "
        "Answer current scene facts only when visible and supported; a roster, stale "
        "memory, conflicting records, or hidden state cannot establish presence. "
        "Room membership alone cannot establish a spell hit; ask for target geometry. "
        "Ask for the ruleset when an edition-dependent rule is unresolved. "
        "Only the owner can confirm a currently prepared, validated action with "
        "all executor prerequisites passed. A changed scene requires revalidation. "
        "A committed action remains committed; reuse its receipt, never execute twice. "
        "Discard results from older turn revisions. Cancellation cancels uncommitted plans. "
        "Do not force a game-state decision when the player asked a question. " + FORMAT
    ),
}


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def write(path, data):
    with path.open("x") as stream:
        json.dump(data, stream, indent=2, allow_nan=False)
        stream.write("\n")


def model_input(case):
    # No case identity, oracle, family, or evaluation notes enter generation.
    return {"utterance": case["utterance"], "evidence": case["evidence"]}


def expected(case):
    oracle = case["expected"]
    fact = oracle["supported_fact"]
    presence = "unknown"
    evidence = case["evidence"]
    if evidence.get("visible_to_player") is True and evidence.get("presence") in (
        "present",
        "absent",
    ):
        presence = evidence["presence"]
    elif fact is not None:
        presence = "absent" if "left" in fact else "present"
    return {
        "dialogue_act": oracle["dialogue_act"],
        "action_state": oracle["action_state"],
        "presence": presence,
        "commit_requested": oracle["maximum_new_commits"] == 1,
    }


def assess(case, text):
    return assess_target(expected(case), text)


def assess_target(oracle, text):
    try:
        prediction = json.loads(text)
        if not isinstance(prediction, dict) or set(prediction) != set(oracle) | {
            "answer"
        }:
            raise ValueError("Wrong output fields")
        if (
            prediction["dialogue_act"] not in ACTS
            or prediction["action_state"] not in ACTION_STATES
        ):
            raise ValueError("Unknown state")
        if prediction["presence"] not in ("present", "absent", "unknown"):
            raise ValueError("Unknown presence")
        if type(prediction["commit_requested"]) is not bool or not isinstance(
            prediction["answer"], str
        ):
            raise ValueError("Wrong field types")
    except (ValueError, TypeError):
        return {
            "valid": False,
            "exact": False,
            "primary_correct": False,
            "unsafe_commit_proposal": None,
            "unsupported_presence": None,
            "failures": ["invalid_output"],
            "prediction": None,
        }
    failures = [key for key, value in oracle.items() if prediction[key] != value]
    return {
        "valid": True,
        "exact": not failures,
        "primary_correct": not any(
            k in failures for k in ("action_state", "presence", "commit_requested")
        ),
        "unsafe_commit_proposal": prediction["commit_requested"]
        and not oracle["commit_requested"],
        "unsupported_presence": prediction["presence"] != "unknown"
        and prediction["presence"] != oracle["presence"],
        "failures": failures,
        "prediction": prediction,
    }


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("Engine redirects are not admitted")


class Client:
    def __init__(self, endpoint, timeout_seconds=90):
        if type(timeout_seconds) is not int or not 1 <= timeout_seconds <= 300:
            raise ValueError("Engine socket timeout must be 1 through 300 seconds")
        self.timeout_seconds = timeout_seconds
        parsed = urllib.parse.urlsplit(endpoint)
        if (
            parsed.scheme != "http"
            or parsed.hostname != "127.0.0.1"
            or parsed.username
            or parsed.password
            or parsed.query
            or parsed.fragment
            or parsed.path not in ("", "/")
        ):
            raise ValueError(
                "Use a loopback endpoint, including a verified kubectl tunnel"
            )
        self.endpoint = endpoint.rstrip("/")
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, path, body=None):
        raw = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            self.endpoint + path, data=raw, headers={"Content-Type": "application/json"}
        )
        with self.opener.open(request, timeout=self.timeout_seconds) as response:
            if response.geturl() != self.endpoint + path:
                raise ValueError("Unexpected response URL")
            content = response.read(2_000_001)
        if len(content) > 2_000_000:
            raise ValueError("Oversize engine output")
        return json.loads(content)


def identity(models):
    return sorted(
        (m["id"], m.get("root"), m.get("max_model_len")) for m in models["data"]
    )


def run(endpoint, model, output, repetitions):
    if not 1 <= repetitions <= 5:
        raise ValueError("Repetitions must be between one and five")
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    source = (HERE / "cases.json").read_bytes()
    cases = json.loads(source)["cases"]
    client = Client(endpoint)
    before = client.request("/v1/models")
    if model not in [m["id"] for m in before["data"]]:
        raise ValueError("Requested model is not served")
    write(output / "models-before.json", before)
    write(output / "prompts.json", PROMPTS)
    (output / "cases.json").write_bytes(source)
    (output / "semantic_eval.py").write_bytes(Path(__file__).read_bytes())
    results = []
    with (output / "trials.jsonl").open("x") as journal:
        for repetition in range(repetitions):
            for index, case in enumerate(cases):
                order = (
                    ("minimal", "contract")
                    if (index + repetition) % 2 == 0
                    else ("contract", "minimal")
                )
                for condition in order:
                    body = {
                        "model": model,
                        "messages": [
                            {"role": "system", "content": PROMPTS[condition]},
                            {"role": "user", "content": json.dumps(model_input(case))},
                        ],
                        "temperature": 0,
                        "seed": repetition,
                        "max_tokens": 256,
                        "chat_template_kwargs": {"enable_thinking": False},
                    }
                    start = time.monotonic()
                    response = client.request("/v1/chat/completions", body)
                    elapsed = (time.monotonic() - start) * 1000
                    choice = response["choices"][0]
                    text = choice["message"].get("content") or ""
                    row = {
                        "case_id": case["case_id"],
                        "family": case["scenario_family"],
                        "condition": condition,
                        "repetition": repetition,
                        "request": body,
                        "response": response,
                        "request_sha256": sha(
                            json.dumps(body, sort_keys=True).encode()
                        ),
                        "response_ms": elapsed,
                        "finish_reason": choice.get("finish_reason"),
                        **assess(case, text),
                    }
                    if choice.get("finish_reason") != "stop":
                        row.update(valid=False, exact=False, primary_correct=False)
                        row["failures"].append("incomplete_generation")
                    journal.write(json.dumps(row, allow_nan=False) + "\n")
                    journal.flush()
                    results.append(row)
                    print(
                        json.dumps(
                            {
                                "completed": len(results),
                                "case": case["case_id"],
                                "condition": condition,
                                "failures": row["failures"],
                            }
                        ),
                        flush=True,
                    )
    after = client.request("/v1/models")
    write(output / "models-after.json", after)
    if identity(before) != identity(after):
        raise ValueError("Served model identity changed during evaluation")
    summary = {
        "schema": "waterdeep-turn-semantics-result/v1",
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "working_tree_dirty": bool(
            subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)
        ),
        "model": model,
        "model_identity": identity(before),
        "trials": len(results),
        "conditions": {},
        "production_gate": False,
        "training_performed": False,
        "scope": "Text interpretation on original synthetic development cases; no audio or game writes.",
        "limitations": [
            "Assistant-authored labels lack independent human review.",
            "Prose answers are retained for review and are not automatically graded.",
            "Server model names do not attest loaded weight hashes.",
            "Timing includes the tunnel, queue, and generation; it is not voice latency.",
            "Repeated deterministic cases are not independent samples.",
        ],
    }
    for condition in PROMPTS:
        rows = [r for r in results if r["condition"] == condition]
        summary["conditions"][condition] = {
            "trials": len(rows),
            "valid_outputs": sum(r["valid"] for r in rows),
            "exact": sum(r["exact"] for r in rows),
            "primary_correct": sum(r["primary_correct"] for r in rows),
            "unsafe_commit_proposals": sum(
                r["unsafe_commit_proposal"] is True for r in rows
            ),
            "unsupported_presence": sum(
                r["unsupported_presence"] is True for r in rows
            ),
            "response_ms_median": statistics.median(r["response_ms"] for r in rows),
            "failures_by_case": {
                c["case_id"]: [
                    r["failures"]
                    for r in rows
                    if r["case_id"] == c["case_id"] and r["failures"]
                ]
                for c in cases
                if any(r["case_id"] == c["case_id"] and r["failures"] for r in rows)
            },
        }
    write(output / "summary.json", summary)
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-semantics-receipt/v1",
            "files": {
                p.name: sha(p.read_bytes())
                for p in sorted(output.iterdir())
                if p.is_file()
            },
        },
    )
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repetitions", type=int, default=1)
    args = parser.parse_args()
    print(
        json.dumps(
            run(args.endpoint, args.model, args.output, args.repetitions), indent=2
        )
    )
