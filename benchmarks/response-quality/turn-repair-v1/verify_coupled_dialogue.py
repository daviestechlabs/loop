"""Verify retained experiment integrity; this does not grade model answer quality."""

import argparse
import hashlib
import json
from pathlib import Path
from dialogue_case_pack import load_pack

CASES = (
    ("referents", "Would it hit her?", "hall"),
    ("recall", "Remind me which spell I was considering.", "hall"),
    ("safety", "Is that safe for her?", "hall"),
    ("execution", "Then go ahead and cast it.", "hall"),
    ("cancel_paraphrase", "Actually, drop the idea. What should I do instead?", "hall"),
    ("changed_scene", "Would it hit her here?", "courtyard"),
)
CONDITIONS = ("retained_conversation", "fresh_conversation")


def require(value, message):
    if not value:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text())


def model_catalog(value):
    # Retained responses differ in generated timestamps and permission IDs.
    # Compare every other field; pod identity is recorded by the operator.
    value = json.loads(json.dumps(value))
    for model in value["data"]:
        model.pop("created", None)
        for permission in model.get("permission", []):
            permission.pop("created", None)
            permission.pop("id", None)
    return value


def verify(root, case_pack=None):
    manifest = read(root / "manifest.json")
    protocol = read(root / "protocol.json")
    contract = protocol.get("context_contract", "held-reference-v1")
    output_contract = protocol.get("output_contract")
    require(
        output_contract in (None, "c-state-receipt-prefix-v1"),
        "Unknown output contract",
    )
    cases = CASES
    expectations = None
    if protocol.get("case_pack_sha256"):
        require(case_pack is not None, "An explicit authored case pack is required")
        require(
            hashlib.sha256(case_pack.read_bytes()).hexdigest()
            == protocol["case_pack_sha256"],
            "Case pack identity differs",
        )
        require(
            (root / "case-pack.json").read_bytes() == case_pack.read_bytes(),
            "Retained case pack differs",
        )
        pack = load_pack(case_pack)
        cases = tuple((c["case_id"], c["message"], c["scene"]) for c in pack["cases"])
        expectations = {c["case_id"]: c["expected_proposal"] for c in pack["cases"]}
    else:
        require(case_pack is None, "Unexpected external case pack")
    require(
        protocol["cases"] == [list(case) for case in cases], "Protocol cases differ"
    )
    require(
        contract in ("held-reference-v1", "cancellation-receipt-v1"),
        "Unknown context contract",
    )
    for name, digest in manifest["files"].items():
        relative = Path(name)
        require(
            not relative.is_absolute() and ".." not in relative.parts,
            "Unsafe manifest path",
        )
        path = root / relative
        require(
            path.is_file() and not path.is_symlink(),
            "Missing or linked artifact: " + name,
        )
        require(
            hashlib.sha256(path.read_bytes()).hexdigest() == digest,
            "Changed artifact: " + name,
        )
    required = {
        "protocol.json",
        "results.json",
        "models-before.json",
        "models-after.json",
        "coupled_dialogue_eval.py",
    }
    if expectations is not None:
        required.update(("case-pack.json", "dialogue_case_pack.py"))
    for case_id, _, _ in cases:
        for condition in CONDITIONS:
            required.update(
                f"{case_id}-{condition}/{name}"
                for name in (
                    "trial.json",
                    "prefix.json",
                    "followup.json",
                    "recall.json",
                    "model-exchanges.json",
                )
            )
    require(required <= manifest["files"].keys(), "Incomplete evidence manifest")
    require(
        model_catalog(read(root / "models-before.json"))
        == model_catalog(read(root / "models-after.json")),
        "Stable model catalog fields changed",
    )
    rows = read(root / "results.json")
    expected = [
        (c, m, s, condition)
        for index, (c, m, s) in enumerate(cases)
        for condition in (CONDITIONS if index % 2 == 0 else reversed(CONDITIONS))
    ]
    require(len(rows) == len(expected) == manifest["trials"], "Missing or extra trials")
    generation = None
    system_prompt = None
    state_failures = []
    for row, (case_id, message, scene, condition) in zip(rows, expected, strict=True):
        require(
            (row["case_id"], row["message"], row["scene"], row["condition"])
            == (case_id, message, scene, condition),
            "Trial identity or order changed",
        )
        directory = root / f"{case_id}-{condition}"
        if contract == "cancellation-receipt-v1":
            for name in (
                "prefix-receipt.json",
                "campaign-before.json",
                "campaign-after.json",
            ):
                require(
                    f"{case_id}-{condition}/{name}" in manifest["files"],
                    "Missing state evidence",
                )
            before = (directory / "campaign-before.json").read_bytes()
            require(
                before == (directory / "campaign-after.json").read_bytes(),
                "Campaign mutated",
            )
            require(
                hashlib.sha256(before).hexdigest() == row["campaign_sha256"],
                "Campaign hash differs",
            )
            call = read(directory / "prefix-receipt.json")["call"]
            require(
                hashlib.sha256(call["output_json"].encode()).hexdigest()
                == call["output_sha256"],
                "Scene output hash differs",
            )
        require(read(directory / "trial.json") == row, "Trial summary changed")
        require(
            row["completed"] and row["campaign_unchanged"] and row["cleanup_ok"],
            "Incomplete C path",
        )
        followup = read(directory / "followup.json")
        request = followup["request"]
        require(
            request["message"] == message and request["metadata"]["scene_id"] == scene,
            "Wrong follow-up input",
        )
        require(not request["enable_tts"], "Unexpected audio scope")
        require(
            request["conversation_id"]
            == ("scene-room" if condition == CONDITIONS[0] else "fresh-room"),
            "Wrong context intervention",
        )
        events = followup["events"]
        require(
            events and events[-1]["type"] == "completed" and followup["status"] == 200,
            "Turn did not complete",
        )
        require(
            all(e["request_id"] == request["request_id"] for e in events),
            "Cross-turn events",
        )
        finals = [e for e in events if e["type"] == "text_completed"]
        require(
            len(finals) == 1 and finals[0]["text"] == row["answer"],
            "Answer differs from C event",
        )
        exchanges = read(directory / "model-exchanges.json")
        require(
            len(exchanges) == row["model_calls"] == 1, "Unexpected model call count"
        )
        exchange = exchanges[0]
        require(
            exchange["status"] == 200 and "error" not in exchange,
            "Model transport failed",
        )
        chunks = [
            json.loads(line[6:])
            for line in exchange["response_sse"].splitlines()
            if line.startswith("data: ") and line != "data: [DONE]"
        ]
        finishes = [
            choice["finish_reason"]
            for chunk in chunks
            for choice in chunk.get("choices", [])
            if choice.get("finish_reason")
        ]
        require(finishes == ["stop"], "Incomplete model generation")
        model_request = exchange["request"]
        settings = {k: v for k, v in model_request.items() if k != "messages"}
        if generation is None:
            generation = settings
        require(settings == generation, "Generation parameters changed")
        messages = model_request["messages"]
        require(
            len(messages) == 2 and messages[-1]["role"] == "user",
            "Unexpected model history",
        )
        if system_prompt is None:
            system_prompt = messages[0]
        require(
            messages[0] == system_prompt and system_prompt["role"] == "system",
            "System prompt changed between trials",
        )
        content = messages[-1]["content"]
        if expectations is not None:
            # Verify observed C reference/receipt against the later C readback.
            # User-intent expectations are graded separately, not required to
            # pass in order to retain a complete failed experiment.
            canceled = '"canceled_proposal":' in content.split("\nCurrent input:", 1)[0]
        else:
            canceled = (
                contract == "cancellation-receipt-v1" and case_id == "cancel_paraphrase"
            )
        retained = condition == CONDITIONS[0]
        if retained or canceled:
            require(
                content.endswith("Current input:\n" + message), "Current input changed"
            )
            reference = json.loads(
                content[content.index("{") : content.index("\nCurrent input:")]
            )
            expected_reference = {
                "held_spell": "fireball" if retained and not canceled else "",
                "last_question_character": "Mira" if retained else "",
                "scene_context_changed": scene != "hall" and retained,
            }
            if canceled:
                expected_reference["canceled_proposal"] = "fireball" if retained else ""
            require(reference == expected_reference, "Wrong C-held context")
        else:
            require(content == message, "Fresh conversation leaked context")
        if output_contract == "c-state-receipt-prefix-v1":
            # This text experiment requires unmodified model prose. Reject
            # markup transformations rather than guessing a second renderer.
            model_text = "".join(
                choice.get("delta", {}).get("content", "") or ""
                for chunk in chunks
                for choice in chunk.get("choices", [])
            ).strip()
            require(bool(model_text), "Missing model answer after receipt")
            prefix = ""
            if canceled:
                prefix = (
                    "Canceled your fireball proposal. "
                    if retained
                    else "No spell proposal is on hold in this session. "
                )
            require(
                row["answer"] == prefix + model_text,
                "C receipt or model answer differs",
            )
        recall = read(directory / "recall.json")
        texts = [e["text"] for e in recall["events"] if e["type"] == "text_completed"]
        require(texts == [row["recalled_proposal"]], "Recall differs from C event")
        expected_recall = (
            "Your fireball proposal is on hold."
            if retained and not canceled
            else "No spell proposal is on hold in this session."
        )
        require(
            texts == [expected_recall], "Proposal state does not match admitted effects"
        )
        if expectations is not None and retained:
            observed = "cleared" if canceled else "held"
            if observed != expectations[case_id]:
                state_failures.append(
                    {
                        "case_id": case_id,
                        "condition": condition,
                        "expected": expectations[case_id],
                        "observed": observed,
                    }
                )
    return {
        "verified_trials": len(rows),
        "generation": generation,
        "quality_graded": False,
        "production_gate": False,
        "training_approved": False,
        "semantic_state_failures": state_failures,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--case-pack", type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.directory, args.case_pack), indent=2))
