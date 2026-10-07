"""Offline trusted-fixture summaries; no live scene authority or tool execution."""
from admit_model_outputs import admission_input, execute


def summarize(evidence, binary):
    """Derive checks from fixture evidence, never labels or model predictions."""
    current = evidence.get("current_turn_revision", 1)
    result = evidence.get("result_turn_revision", current)
    if any(type(v) is not int or not 0 < v <= 1_000_000 for v in (current, result)):
        raise ValueError("Invalid turn revision")
    stale = current != result
    candidates = evidence.get("candidates", [])
    if not isinstance(candidates, list) or any(not isinstance(v, str) for v in candidates):
        raise ValueError("Invalid candidate list")
    ambiguous = len(set(candidates)) > 1
    scene = evidence.get("scene_revision")
    current_scene = evidence.get("current_scene_revision", scene)
    scene_current = (
        type(scene) is int and type(current_scene) is int
        and 0 < scene <= 1_000_000 and scene == current_scene
    )
    presence = "unknown"
    if scene_current and not stale and not ambiguous and evidence.get("visible_to_player") is True:
        if evidence.get("presence") in ("present", "absent"):
            presence = evidence["presence"]
    probe = {"valid": True, "prediction": {
        "commit_requested": True, "action_state": "eligible_for_governed_commit"
    }}
    outcome = execute(binary, admission_input({"evidence": evidence}, probe))
    return {
        "assertable_presence": presence,
        "result_revision_is_current": not stale,
        "multiple_named_candidates": ambiguous,
        "existing_action_receipt": bool(evidence.get("existing_receipt") or evidence.get("receipt")),
        "mock_executor_would_allow_new_commit": outcome["new_commits"] == 1,
        "scope": "Offline fixture checks. No actual game operation was executed.",
    }
