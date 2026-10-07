import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import action_fact_eval as a
from analyze_action_facts import analyze, fence_diagnostic, summarize
from test_action_budget_eval import EVIDENCE, TEMPLATE
from test_action_fact_eval import FixtureClient, frame


class AnalysisTests(unittest.TestCase):
    def test_matching_budget_does_not_hide_invented_fact(self):
        row = {"case_id": "unknown", "seed": 0, "policy": "basic", "response": {},
               "interpretation": {"accepted": True, "incorrect_fields": ["surge_planned"],
                                  "unsupported_known_fields": ["surge_planned"], "budget_result": [2, 42, 12],
                                  "budget_matches_authored_expectation": True}}
        rejected = {**row, "interpretation": {"accepted": False, "error": "invalid JSON"}}
        counts = summarize([row, rejected])["basic"]["counts"]
        self.assertEqual(counts["trials"], 2)
        self.assertEqual(counts["exact_frames"], 0)
        self.assertEqual(counts["budget_matches"], 1)
        self.assertEqual(counts["matching_budget_with_wrong_facts"], 1)
        self.assertEqual(counts["invented_known_fields"], 1)

    def test_fence_diagnostic_is_separate_and_does_not_accept_arbitrary_prose(self):
        case = a.cases()[1]
        rows = []
        for content in ("```json\n" + json.dumps(frame(case)) + "\n```", "Here is JSON:\n" + json.dumps(frame(case))):
            row = {"case_id": case["id"], "seed": 0, "policy": "basic",
                   "response": {"choices": [{"finish_reason": "stop", "message": {"content": content}}]}}
            row["interpretation"] = a.interpretation(row, case)
            self.assertFalse(row["interpretation"]["accepted"])
            rows.append(row)
        original = copy.deepcopy(rows)
        result = fence_diagnostic(rows)
        self.assertEqual(rows, original)
        self.assertEqual(len(result["changed"]), 1)
        self.assertEqual(result["conditions"]["basic"]["counts"]["accepted_frames"], 1)

    def test_verified_fixture_export_is_held_and_operator_tamper_rejects(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            (source / "index.dndsidx").write_bytes(b"fixture")
            template = root / "template.json"
            template.write_text("fixture")
            results = root / "results"
            with patch.object(a.ab, "prepare", return_value=(TEMPLATE, EVIDENCE)), patch.object(a, "Client", FixtureClient), patch("builtins.print"):
                a.run("http://127.0.0.1:1", source, template, results)
                receipt = {"complete": True, "stable_model_pod_identity": True, "source_revision": "f" * 40,
                           "results_manifest_sha256": a.digest((results / "manifest.json").read_bytes())}
                a.write(root / "receipt.json", receipt)
                files = {"benchmarks/response-quality/turn-repair-v1/" + name: a.digest((results / name).read_bytes()) for name in a.SOURCES}
                files["voice/c-runtime/tests/test_dnd_source_compiler.py"] = a.digest((results / "source-decoder.py").read_bytes())
                a.write(root / "source-identity.json", {"revision": "f" * 40, "files": files})
                for name in ("identity-before.json", "identity-after.json"):
                    a.write(root / name, {"uid": "fixture"})
                report = analyze(results, source, template, root / "analysis")
                self.assertEqual(report["conditions"]["basic"]["counts"]["exact_frames"], 24)
                export = a.read(root / "analysis/anvil-learning-report.json")
                self.assertEqual((export["scope"], export["status"]), ("development", "held"))
                self.assertTrue(export["blockers"])
                receipt["results_manifest_sha256"] = "0" * 64
                (root / "receipt.json").write_text(json.dumps(receipt))
                with self.assertRaisesRegex(ValueError, "Operator manifest differs"):
                    analyze(results, source, template, root / "bad-analysis")


if __name__ == "__main__":
    unittest.main()
