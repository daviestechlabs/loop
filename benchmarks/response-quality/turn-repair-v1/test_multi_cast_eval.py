import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import multi_cast_eval as m
from analyze_multi_cast import analyze, quote_diagnostic, summarize
from semantic_eval import Client
from test_cast_event_eval import fact


def reply(events):
    return {"choices": [{"finish_reason": "stop", "message": {"content": json.dumps({"events": events})}}]}


FACTS = [fact(), fact("polymorph", level=4)]


class MultiCastTests(unittest.TestCase):
    def test_quote_diagnostic_preserves_primary_failure_and_semantic_errors(self):
        case = m.cases()[0]
        events = copy.deepcopy(case["events"])
        events[0]["status"]["quote"] = "completed"
        response = reply(events)
        assessment = m.assess(response, case, FACTS)
        self.assertFalse(assessment["accepted"])
        row = {"case_id": case["id"], "policy": "basic", "seed": 0, "response": response, "assessment": assessment}
        original = copy.deepcopy(row)
        diagnostic = quote_diagnostic([row])
        self.assertTrue(diagnostic["reviews"][0]["values_match_authored_events"])
        self.assertEqual(diagnostic["reviews"][0]["quote_issues"], [{"event": 0, "field": "status", "quote": "completed", "occurrences": 2}])
        self.assertEqual(row, original)
        self.assertFalse(diagnostic["evidence_accepted"])
        events[0]["time"]["value"] = "this_turn"
        row["response"] = reply(events)
        self.assertFalse(quote_diagnostic([row])["reviews"][0]["values_match_authored_events"])

    def test_authored_controls_preserve_unknowns_and_independent_join_targets(self):
        for case in m.cases():
            result = m.assess(reply(case["events"]), case, FACTS)
            self.assertTrue(result["exact_all_events"], case["id"])
            self.assertTrue(result["all_joins_match"], case["id"])
            self.assertFalse(result["whole_history_proven"])
            self.assertFalse(result["resource_state_authorized"])

    def test_omission_extra_event_and_wrong_correction_cannot_pass_by_valid_json(self):
        case = m.cases()[0]
        omitted = m.assess(reply(case["events"][1:]), case, FACTS)
        self.assertTrue(omitted["accepted"])
        self.assertEqual(omitted["missing_events"], [0])
        self.assertFalse(omitted["all_joins_match"])
        # Reusing unrelated but literal text as an event anchor is not evidence of an extra cast.
        extra = copy.deepcopy(case["events"])
        ghost = copy.deepcopy(extra[-1])
        ghost["anchor"] = "Both casts used no casting-time modifiers."
        extra.append(ghost)
        assessed = m.assess(reply(extra), case, FACTS)
        self.assertTrue(assessed["accepted"])
        self.assertEqual(assessed["unmatched_events"], [2])
        self.assertFalse(assessed["exact_all_events"])
        # A wrong field can be hidden by an unchanged excluded-event join.
        wrong = copy.deepcopy(case["events"])
        wrong[0]["actor"]["value"] = "other"
        assessed = m.assess(reply(wrong), case, FACTS)
        self.assertTrue(assessed["all_joins_match"])
        self.assertFalse(assessed["exact_all_events"])
        self.assertEqual(assessed["checks"][0]["incorrect_fields"], ["actor"])
        summary = summarize([{"policy": "basic", "assessment": assessed, "request_elapsed_ms": 5},
                             {"policy": "basic", "assessment": omitted, "request_elapsed_ms": 7},
                             {"policy": "basic", "assessment": {"accepted": False}, "request_elapsed_ms": 9}])["basic"]
        self.assertEqual(summary["counts"]["joins_hiding_wrong_fields"], 1)
        self.assertEqual(summary["counts"]["missing_events"], 1)
        self.assertEqual(summary["counts"]["rejected"], 1)
        self.assertEqual(summary["counts"]["exact_all_events"], 0)

    def test_anchor_merging_duplicate_keys_and_bounds_reject(self):
        case = m.cases()[0]
        for events in (case["events"][::-1], [case["events"][0]] * 2, case["events"] * 5):
            self.assertFalse(m.assess(reply(events), case, FACTS)["accepted"])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            m.parse('{"events":[],"events":[]}', case["utterance"])
        for text in ('{"events":null}', '{"events":[],"permission":true}', '```json\n{"events":[]}\n```'):
            with self.assertRaises(ValueError):
                m.parse(text, case["utterance"])
        merged = copy.deepcopy(case["events"][:1])
        merged[0]["anchor"] = case["utterance"]
        result = m.assess(reply(merged), case, FACTS)
        self.assertTrue(result["accepted"])
        self.assertEqual(result["ambiguous_event_anchors"], [0])
        self.assertFalse(result["exact_all_events"])

    def test_repeated_spell_mentions_are_distinct_and_empty_extraction_is_not_history(self):
        case = next(c for c in m.cases() if c["id"] == "quoted_speaker_b")
        self.assertEqual([v["spell"]["value"] for v in case["events"]], ["fireball", "fireball"])
        result = m.assess(reply(case["events"][:1]), case, FACTS)
        self.assertEqual(result["missing_events"], [1])
        scene = next(c for c in m.cases() if c["id"] == "scene_only_a")
        result = m.assess(reply([]), scene, FACTS)
        self.assertTrue(result["exact_all_events"])
        self.assertFalse(result["whole_history_proven"])
        uncertain = next(c for c in m.cases() if c["id"] == "uncertainty_cancel_a")
        result = m.assess(reply(uncertain["events"]), uncertain, FACTS)
        self.assertEqual([c["join"]["kind"] for c in result["checks"]], ["unresolved_event", "excluded_event"])

    def test_prompt_comparison_hides_labels_and_changes_only_repair_instruction(self):
        case = m.cases()[0]
        template = {"model": "fixture", "temperature": 0.2}
        poisoned = dict(case, id="secret-id", family="secret-family", events=[], expected_joins=[], rationale="secret-rationale")
        baseline = m.request(template, case, "basic", 0)
        self.assertEqual(baseline, m.request(template, poisoned, "basic", 0))
        changed = m.request(template, case, "scoped_repair", 0)
        self.assertEqual(changed["messages"][0]["content"], baseline["messages"][0]["content"] + m.SCOPED_REPAIR)
        changed["messages"] = baseline["messages"]
        self.assertEqual(changed, baseline)
        rows = list(m.matrix())
        self.assertEqual(len(rows), 32)
        self.assertEqual([r[2] for r in rows[:4]], ["basic", "scoped_repair", "scoped_repair", "basic"])
        self.assertEqual(template, {"model": "fixture", "temperature": 0.2})

    def test_socket_timeout_is_bounded_and_does_not_expand_endpoint_access(self):
        self.assertEqual(Client("http://127.0.0.1:8080").timeout_seconds, 90)
        self.assertEqual(Client("http://127.0.0.1:8080", 240).timeout_seconds, 240)
        for invalid in (0, 301, True, 1.5):
            with self.assertRaises(ValueError):
                Client("http://127.0.0.1:8080", invalid)
        for endpoint in ("https://127.0.0.1", "http://example.com", "http://user:secret@127.0.0.1"):
            with self.assertRaises(ValueError):
                Client(endpoint, 240)
        response = unittest.mock.MagicMock()
        response.__enter__.return_value = response
        response.geturl.return_value = "http://127.0.0.1:8080/v1/models"
        response.read.return_value = b'{"data":[]}'
        client = Client("http://127.0.0.1:8080", 240)
        with patch.object(client.opener, "open", return_value=response) as opened:
            client.request("/v1/models")
            self.assertEqual(opened.call_args.kwargs["timeout"], 240)
            response.read.assert_called_once_with(2_000_001)

    def test_archive_reconstruction_rejects_rehashed_missing_event_assessment(self):
        template = {"model": "fixture", "temperature": 0.2}
        by_text = {c["utterance"]: c for c in m.cases()}
        class FakeClient:
            def __init__(self, endpoint, timeout_seconds):
                if timeout_seconds != 240:
                    raise ValueError("Wrong timeout")
            def request(self, path, body=None):
                if path == "/v1/models":
                    return {"data": [{"id": "fixture"}]}
                return reply(by_text[body["messages"][1]["content"]]["events"])
        with tempfile.TemporaryDirectory() as tmp, patch.object(m.e, "inputs", return_value=(FACTS, template)), patch.object(m.e.a, "Client", FakeClient), patch("builtins.print"):
            root = Path(tmp)
            template_path = root / "model-template.json"
            template_path.write_text(json.dumps(template))
            output = root / "results"
            m.run("unused", root, root, "pinned", template_path, output)
            self.assertEqual(m.verify(output, root, root, "pinned", template_path)["exact_all_events"], 32)
            files = {"benchmarks/response-quality/turn-repair-v1/" + name: m.e.a.digest((output / name).read_bytes()) for name in m.SOURCES}
            files["benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json"] = m.e.a.digest((output / "cases.json").read_bytes())
            files["voice/c-runtime/tests/test_dnd_source_compiler.py"] = m.e.a.digest((output / "source-decoder.py").read_bytes())
            m.e.a.write(root / "source-identity.json", {"revision": "a" * 40, "files": files})
            for phase in ("before", "after"):
                m.e.a.write(root / ("identity-" + phase + ".json"), {"uid": "fixture"})
            receipt = {"complete": True, "stable_model_pod_identity": True, "source_revision": "a" * 40,
                       "facts_receipt_sha256": "pinned", "results_manifest_sha256": m.e.a.digest((output / "manifest.json").read_bytes())}
            m.e.a.write(root / "receipt.json", receipt)
            report = analyze(root, root, root / "analysis")
            self.assertEqual(report["conditions"]["basic"]["counts"]["exact_all_events"], 16)
            self.assertEqual(report["conditions"]["scoped_repair"]["counts"]["exact_matched_events"], 29)
            exported = m.e.a.read(root / "analysis/anvil-learning-report.json")
            self.assertEqual((exported["status"], exported["scope"], len(exported["metrics"])), ("held", "development", 12))
            self.assertIn(exported["progress"]["unit"], {"steps", "cases", "checks"})
            (root / "receipt.json").write_text(json.dumps(dict(receipt, complete=False)))
            with self.assertRaisesRegex(ValueError, "Incomplete operator"):
                analyze(root, root, root / "incomplete-analysis")
            rows = [json.loads(line) for line in (output / "trials.jsonl").read_text().splitlines()]
            rows[0]["response"] = reply(m.cases()[0]["events"][1:])
            (output / "trials.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
            manifest = m.e.a.read(output / "manifest.json")
            manifest["files"]["trials.jsonl"] = m.e.a.digest((output / "trials.jsonl").read_bytes())
            (output / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "Assessment differs"):
                m.verify(output, root, root, "pinned", template_path)


if __name__ == "__main__":
    unittest.main()
