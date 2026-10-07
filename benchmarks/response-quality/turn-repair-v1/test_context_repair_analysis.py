import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import analyze_context_repair as a
from test_context_cast_eval import events
from test_context_repair_eval import Client, fixtures
from test_multi_cast_eval import FACTS, reply

r = a.r


class RepairAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.parent, self.experiment = self.root / "parent", self.root / "experiment"
        self.parent.mkdir()
        self.experiment.mkdir()
        (self.parent / "results").mkdir()
        r.x.snapshots(self.parent / "results")
        r.x.e.a.write(self.parent / "results/facts.json", FACTS)
        r.x.e.a.write(self.parent / "receipt.json", {"complete": True})
        r.x.e.a.write(self.parent / "identity-after.json", {"uid": "fixture"})
        Client.calls, Client.failure = 0, None
        self.mock_parent = patch.object(r, "parent_rows", return_value=fixtures())
        self.mock_parent.start()
        self.addCleanup(self.mock_parent.stop)
        self.addCleanup(self.tmp.cleanup)
        with patch.object(r.x.e.a, "Client", Client), patch("builtins.print"):
            r.run("unused", self.parent, self.root, self.experiment / "results")
        root = self.experiment / "results"
        paths = {
            "benchmarks/response-quality/turn-repair-v1/" + n: n
            for n in (*r.x.SOURCES, *r.SOURCES)
        }
        paths.update(
            {
                "voice/c-runtime/common/" + n: "kernel/common/" + n
                for n in r.x.KERNEL_FILES
            }
        )
        paths.update(
            {
                "voice/c-runtime/tests/test_dnd_source_compiler.py": "source-decoder.py",
                "benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json": "cases.json",
            }
        )
        (self.experiment / "operator.py").write_text("fixture operator")
        r.x.e.a.write(
            self.experiment / "source-identity.json",
            {
                "revision": "a" * 40,
                "operator_sha256": self.digest("operator.py"),
                "files": {
                    k: r.x.e.a.digest((root / v).read_bytes()) for k, v in paths.items()
                },
            },
        )
        for phase in ("before", "after"):
            r.x.e.a.write(
                self.experiment / ("identity-" + phase + ".json"), {"uid": "fixture"}
            )
        r.x.e.a.write(
            self.experiment / "receipt.json",
            {
                "complete": True,
                "stable_model_pod_identity": True,
                "source_revision": "a" * 40,
                "parent_manifest_sha256": r.PARENT_MANIFEST,
                "parent_receipt_sha256": r.x.e.a.digest(
                    (self.parent / "receipt.json").read_bytes()
                ),
                "results_manifest_sha256": self.digest("results/manifest.json"),
            },
        )

    def digest(self, name):
        return r.x.e.a.digest((self.experiment / name).read_bytes())

    def test_export_is_held_and_preserves_arm_counts_and_unreviewed_evidence(self):
        output = self.root / "analysis"
        report = a.analyze(
            self.experiment, self.parent, self.root, self.digest("receipt.json"), output
        )
        self.assertEqual(report["verified_trials"], 18)
        self.assertFalse(report["semantic_support_proven"])
        exported = r.x.e.a.read(output / "anvil-learning-report.json")
        self.assertEqual(
            (exported["status"], exported["scope"]), ("held", "development")
        )
        self.assertEqual(
            exported["progress"], {"completed": 18, "total": 18, "unit": "checks"}
        )
        self.assertEqual(len(exported["metrics"]), 18)
        ledger = r.x.e.a.read(output / "ledger.json")
        self.assertEqual(set(ledger["counts"]), set(r.f.ARMS))
        self.assertTrue(all(sum(v.values()) > 0 for v in ledger["counts"].values()))
        self.assertTrue(
            all(e["human_semantic_review"] == "unreviewed" for e in ledger["fields"])
        )
        for name, sha in r.x.e.a.read(output / "receipt.json")["files"].items():
            self.assertEqual(r.x.e.a.digest((output / name).read_bytes()), sha)

    def test_external_receipt_pin_and_incomplete_operator_fail_before_export(self):
        with self.assertRaisesRegex(ValueError, "Operator receipt pin"):
            a.verify_operator(self.experiment, self.parent, "0" * 64)
        receipt = r.x.e.a.read(self.experiment / "receipt.json")
        receipt["complete"] = False
        (self.experiment / "receipt.json").write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "Incomplete operator"):
            a.verify_operator(self.experiment, self.parent, self.digest("receipt.json"))

    def test_changed_pod_source_operator_and_manifest_reject(self):
        mutations = {
            "identity-after.json": {"uid": "different"},
            "source-identity.json": None,
            "results/manifest.json": {},
            "operator.py": "changed operator",
        }
        for name, replacement in mutations.items():
            path = self.experiment / name
            original = path.read_bytes()
            if replacement is None:
                replacement = json.loads(original)
                replacement["files"].pop(next(iter(replacement["files"])))
            path.write_text(
                replacement if type(replacement) is str else json.dumps(replacement)
            )
            try:
                with self.assertRaises(ValueError, msg=name):
                    a.verify_operator(
                        self.experiment, self.parent, self.digest("receipt.json")
                    )
            finally:
                path.write_bytes(original)

    def test_wrong_event_quote_remains_a_review_flag_despite_exact_labels(self):
        case = r.x.m.cases()[0]
        wrong = events(case)
        wrong[0]["status"].update(quote="completed", context=wrong[1]["anchor"])
        with tempfile.TemporaryDirectory() as tmp:
            binary, _ = r.x.c.compile_probe(Path(tmp))
            assessment = r.x.assess(
                reply(wrong), case, FACTS, "explicit_context", binary
            )
        self.assertTrue(assessment["exact_all_events"])
        row = dict(
            case_id=case["id"],
            arm="located",
            request={"seed": 0},
            assessment=assessment,
        )
        report = a.ledger([row])
        status = next(
            e
            for e in report["fields"]
            if e["target_event"] == 0 and e["field"] == "status"
        )
        self.assertEqual(status["location_relation"], "disjoint")
        self.assertFalse(status["semantic_support_proven"])
        self.assertEqual(report["counts"]["located"]["disjoint"], 1)
        self.assertFalse(report["human_review_complete"])

    def test_primary_rejections_stay_visible_in_review(self):
        case = r.x.m.cases()[0]
        assessment = {"accepted": False, "error": "fixture rejection"}
        row = dict(
            case_id=case["id"],
            arm="occurrences",
            request={"seed": 0},
            assessment=assessment,
        )
        report = a.ledger([copy.deepcopy(row)])
        self.assertEqual(report["reviews"][0]["rejection"], "fixture rejection")
        self.assertEqual(report["fields"], [])
        self.assertEqual(report["counts"]["occurrences"], {})


if __name__ == "__main__":
    unittest.main()
