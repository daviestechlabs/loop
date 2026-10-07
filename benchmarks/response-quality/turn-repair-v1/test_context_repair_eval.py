import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import context_repair_eval as r
from test_context_cast_eval import events
from test_multi_cast_eval import FACTS, reply


def fixtures():
    cases = {case["id"]: case for case in r.x.m.cases()}
    return [
        dict(
            case_id=name,
            request=r.x.request(
                {"model": "fixture", "temperature": 0.2},
                cases[name],
                "explicit_context",
                0,
            ),
            response={
                "choices": [
                    {"finish_reason": "stop", "message": {"content": '{"events":null}'}}
                ]
            },
        )
        for name in r.ELIGIBLE
    ]


class Client:
    failure = None
    calls = 0

    def __init__(self, endpoint, timeout_seconds):
        if timeout_seconds != 240:
            raise ValueError("Wrong timeout")

    def request(self, path, body=None):
        if path == "/v1/models":
            return {"data": [{"id": "fixture"}]}
        type(self).calls += 1
        if self.failure:
            raise self.failure
        case = next(
            c for c in r.x.m.cases() if c["utterance"] == body["messages"][1]["content"]
        )
        return reply(events(case))


class RepairEvaluationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary, _ = r.x.c.compile_probe(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        Client.failure, Client.calls = None, 0

    def prepare_parent(self, path):
        path.mkdir()
        (path / "results").mkdir()
        r.x.snapshots(path / "results")
        r.x.e.a.write(path / "results/facts.json", FACTS)

    def test_matrix_balances_order_and_preserves_original_prefix_and_sampling(self):
        rows = fixtures()
        planned = r.matrix(rows, self.binary)
        self.assertEqual(len(planned), 18)
        for index, parent in enumerate(rows):
            group = planned[index * 3 : index * 3 + 3]
            self.assertEqual({p["arm"] for p in group}, set(r.f.ARMS))
            self.assertEqual(group[0]["arm"], r.f.ARMS[index % 3])
            for item in group:
                self.assertEqual(
                    item["request"]["messages"][:2], parent["request"]["messages"]
                )
                self.assertEqual(item["request"]["seed"], 0)
                self.assertEqual(item["request"]["temperature"], 0.2)
                self.assertEqual(item["request"]["max_completion_tokens"], 3072)
        for arm in r.f.ARMS:
            self.assertEqual(sum(planned[i]["arm"] == arm for i in range(0, 18, 3)), 2)
        poisoned = copy.deepcopy(rows)
        for row in poisoned:
            row.update(assessment={"expected": "hidden"}, rationale="hidden")
        self.assertEqual(r.matrix(poisoned, self.binary), planned)

    def test_matrix_rejects_ineligible_and_passing_parent_candidates(self):
        with self.assertRaisesRegex(ValueError, "Eligible"):
            r.matrix(fixtures()[1:], self.binary)
        rows = fixtures()
        rows[0]["response"] = {
            "choices": [
                {"finish_reason": "stop", "message": {"content": '{"events":[]}'}}
            ]
        }
        with self.assertRaisesRegex(ValueError, "no repair opportunity"):
            r.matrix(rows, self.binary)

    def test_parent_pin_rejects_before_analyzer_or_model_use(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp)
            (parent / "results").mkdir()
            (parent / "results/manifest.json").write_text("{}")
            with patch.object(r.parent_analysis, "analyze") as analyzer:
                with self.assertRaisesRegex(ValueError, "Parent manifest"):
                    r.parent_rows(parent, parent)
                analyzer.assert_not_called()

    def test_successful_archive_rejects_rehashed_request_and_assessment_changes(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            patch.object(r, "parent_rows", return_value=fixtures()),
            patch.object(r.x.e.a, "Client", Client),
            patch("builtins.print"),
        ):
            root = Path(tmp)
            parent, output = root / "parent", root / "output"
            self.prepare_parent(parent)
            result = r.run("unused", parent, root, output)
            self.assertEqual(Client.calls, 18)
            self.assertEqual(result["verified_trials"], 18)
            self.assertEqual(
                [
                    v["counts"]["exact_all_events"]
                    for v in result["conditions"].values()
                ],
                [6, 6, 6],
            )
            original = (output / "trials.jsonl").read_bytes()
            for mutation in ("request", "assessment", "order"):
                rows = [json.loads(line) for line in original.splitlines()]
                if mutation == "request":
                    rows[0]["request"]["messages"][-1]["content"] += " changed"
                elif mutation == "assessment":
                    rows[0]["assessment"]["exact_all_events"] = False
                else:
                    rows[0], rows[1] = rows[1], rows[0]
                (output / "trials.jsonl").write_text(
                    "".join(json.dumps(row) + "\n" for row in rows)
                )
                manifest = r.x.e.a.read(output / "manifest.json")
                manifest["files"]["trials.jsonl"] = r.x.e.a.digest(
                    (output / "trials.jsonl").read_bytes()
                )
                (output / "manifest.json").write_text(json.dumps(manifest))
                with self.assertRaisesRegex(
                    ValueError, "Request or trial order differs|Assessment differs"
                ):
                    r.verify(output, parent, root)

    def test_service_failure_is_retained_once_and_aborts_without_retry(self):
        Client.failure = TimeoutError("fixture timeout")
        self.check_failure("service_error", TimeoutError, "fixture timeout")

    def test_c_disagreement_retains_response_and_aborts_without_scoring_rejection(self):
        with patch.object(r.x, "assess", side_effect=RuntimeError("C disagreement")):
            self.check_failure("evaluation_error", RuntimeError, "C disagreement")

    def check_failure(self, field, exception, message):
        with (
            tempfile.TemporaryDirectory() as tmp,
            patch.object(r, "parent_rows", return_value=fixtures()),
            patch.object(r.x.e.a, "Client", Client),
        ):
            root = Path(tmp)
            parent, output = root / "parent", root / "output"
            self.prepare_parent(parent)
            with self.assertRaisesRegex(exception, message):
                r.run("unused", parent, root, output)
            self.assertEqual(Client.calls, 1)
            rows = [
                json.loads(line)
                for line in (output / "trials.jsonl").read_text().splitlines()
            ]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0][field]["type"], exception.__name__)
            self.assertNotIn("assessment", rows[0])
            self.assertEqual("response" in rows[0], field == "evaluation_error")
            self.assertFalse(r.x.e.a.read(output / "manifest.json")["complete"])
            with self.assertRaisesRegex(ValueError, "Incomplete repair archive"):
                r.verify(output, parent, root)

    def test_summary_does_not_hide_omitted_events_or_wrong_fields(self):
        case = r.x.m.cases()[0]
        omitted = r.x.assess(
            reply(events(case)[1:]), case, FACTS, "explicit_context", self.binary
        )
        wrong = events(case)
        wrong[0]["actor"]["value"] = "other"
        wrong = r.x.assess(reply(wrong), case, FACTS, "explicit_context", self.binary)
        rejected = r.x.assess(
            {"choices": [{"finish_reason": "stop", "message": {"content": "{}"}}]},
            case,
            FACTS,
            "explicit_context",
            self.binary,
        )
        rows = [
            dict(arm="raw", assessment=a, request_elapsed_ms=1)
            for a in (omitted, wrong, rejected)
        ]
        report = r.summarize(rows)["raw"]
        self.assertEqual(report["counts"]["missing_events"], 1)
        self.assertEqual(report["counts"]["joins_hiding_wrong_fields"], 1)
        self.assertEqual(report["counts"]["rejected"], 1)
        self.assertEqual(report["counts"]["exact_all_events"], 0)
        self.assertEqual(report["incorrect_fields"], {"actor": 1})


if __name__ == "__main__":
    unittest.main()
