import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from analyze_cast_events import fence_diagnostic, summarize
from cast_event_schema import constrained_request, response_format
import cast_event_decoding_eval as d
import cast_event_eval as e
from test_cast_event_eval import fact
from validate_cast_event_data import FIELDS


class EventAnalysisTests(unittest.TestCase):
    def test_decoding_archive_verifies_and_rehashed_request_tampering_rejects(self):
        template = {"model": "fixture", "messages": [], "temperature": 0.2}
        by_text = {c["utterance"]: c for c in e.cases()}
        class Client:
            def __init__(self, endpoint):
                pass
            def request(self, path, body=None):
                if path == "/v1/models":
                    return {"data": [{"id": "fixture"}]}
                case = by_text[body["messages"][1]["content"]]
                frame = {k: {"value": v, "quote": case["quotes"][k]} for k, v in case["expected"].items()}
                text = json.dumps(frame)
                if "response_format" not in body:
                    text = "```json\n" + text + "\n```"
                return {"choices": [{"finish_reason": "stop", "message": {"content": text}}]}
        with tempfile.TemporaryDirectory() as tmp, patch.object(e, "inputs", return_value=([fact()], template)), patch.object(e.a, "Client", Client), patch("builtins.print"):
            root = Path(tmp)
            template_path = root / "template.json"
            template_path.write_text("fixture template")
            output = root / "result"
            d.run("unused", root, root, "pinned", template_path, output)
            verified = d.verify(output, root, root, "pinned", template_path)
            self.assertEqual(verified["verified_trials"], 32)
            self.assertEqual(verified["accepted_frames"], 16)
            rows = [json.loads(line) for line in (output / "trials.jsonl").read_text().splitlines()]
            rows[0]["request"]["seed"] = 99
            (output / "trials.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
            manifest = e.a.read(output / "manifest.json")
            manifest["files"]["trials.jsonl"] = e.a.digest((output / "trials.jsonl").read_bytes())
            (output / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "Request differs"):
                d.verify(output, root, root, "pinned", template_path)

    def test_decoding_comparison_has_matched_inputs_and_balanced_order(self):
        template = {"model": "fixture", "messages": [], "temperature": 0.2}
        rows = list(d.matrix())
        self.assertEqual(len(rows), 32)
        for seed, case, decoding in rows:
            request = d.request(template, case, decoding, seed)
            plain = d.request(template, case, "prompt_only", seed)
            self.assertEqual({k: v for k, v in request.items() if k != "response_format"}, plain)
        for case in e.cases():
            orders = [[decoding for seed, c, decoding in rows if seed == value and c["id"] == case["id"]] for value in (0, 1)]
            self.assertEqual(orders[0], orders[1][::-1])

    def test_fence_diagnostic_does_not_change_primary_or_accept_other_text(self):
        case = e.cases()[0]
        frame = {k: {"value": v, "quote": case["quotes"][k]} for k, v in case["expected"].items()}
        rows = []
        for text in ("```json\n" + json.dumps(frame) + "\n```", "Here you go: " + json.dumps(frame)):
            response = {"choices": [{"finish_reason": "stop", "message": {"content": text}}]}
            rows.append({"case_id": case["id"], "seed": 0, "policy": "basic", "response": response,
                         "assessment": e.assess(response, case, [fact()])})
        saved = copy.deepcopy(rows)
        diagnostic = fence_diagnostic(rows, [fact()])
        self.assertEqual(rows, saved)
        self.assertEqual(len(diagnostic["changed"]), 1)
        self.assertEqual(diagnostic["conditions"]["basic"]["counts"]["accepted"], 1)

    def test_schema_intervention_changes_only_response_format(self):
        baseline = {"messages": [{"role": "user", "content": "fixture"}], "seed": 0, "temperature": 0.2}
        constrained = constrained_request(baseline)
        self.assertNotIn("response_format", baseline)
        self.assertEqual({k: v for k, v in constrained.items() if k != "response_format"}, baseline)
        schema = response_format()["json_schema"]["schema"]
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(set(schema["required"]), set(FIELDS))
        for field, allowed in FIELDS.items():
            item = schema["properties"][field]
            self.assertFalse(item["additionalProperties"])
            self.assertEqual(set(item["required"]), {"value", "quote"})
            self.assertEqual(set(item["properties"]["value"]["enum"]), allowed)
        with self.assertRaises(ValueError):
            constrained_request(constrained)

    def test_format_errors_and_hidden_field_errors_remain_separate(self):
        def row(case, kind, exact, matches, wrong=()):
            return {"policy": "basic", "case_id": case, "assessment": {
                "accepted": True, "exact_event": exact, "join_matches_authored_target": matches,
                "join": {"kind": kind}, "incorrect_fields": list(wrong)}}
        rows = [row("planned_cast_b", "excluded_event", False, True, ["spell"]),
                row("planned_cast_b", "levelled_action_claim", False, False, ["status"]),
                row("planned_cast_a", "excluded_event", False, False, ["actor"]),
                {"policy": "basic", "case_id": "planned_cast_a", "assessment": {"accepted": False}}]
        report = summarize(rows)["basic"]
        self.assertEqual(report["counts"], {"trials": 4, "accepted": 3, "rejected": 1, "exact_events": 0,
                         "correct_joins": 1, "correct_joins_with_wrong_fields": 1, "false_current_spell_claims": 1,
                         "accepted_missed_current_spell_claims": 1})
        self.assertEqual(report["incorrect_fields"], {"spell": 1, "status": 1, "actor": 1})


if __name__ == "__main__":
    unittest.main()
