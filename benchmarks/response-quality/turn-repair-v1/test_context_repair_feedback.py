import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import context_repair_feedback as f
from test_context_cast_eval import events


class RepairFeedbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary, _ = f.x.c.compile_probe(Path(cls.tmp.name))
        cls.case = f.x.m.cases()[0]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def candidate(self):
        rows = events(self.case)
        rows[0]["status"].update(quote="completed", context="")
        return json.dumps({"events": rows})

    def test_three_arms_preserve_prefix_schema_sampling_and_original(self):
        original = f.x.request(
            {"model": "fixture", "temperature": 0.2}, self.case, "explicit_context", 0
        )
        saved = copy.deepcopy(original)
        replies = [
            f.request(
                original, f.x.binding(self.case), self.candidate(), self.binary, arm
            )
            for arm in f.ARMS
        ]
        self.assertEqual(original, saved)
        for reply in replies:
            self.assertEqual(reply["messages"][:3], replies[0]["messages"][:3])
            without_messages = {
                key: value for key, value in reply.items() if key != "messages"
            }
            self.assertEqual(
                without_messages,
                {key: value for key, value in original.items() if key != "messages"},
            )
        self.assertEqual(
            len({reply["messages"][-1]["content"] for reply in replies}), 3
        )

    def test_hidden_label_changes_do_not_change_requests(self):
        poisoned = dict(
            self.case,
            events=[],
            expected_joins=["secret"],
            rationale="secret",
            id="secret",
        )
        for arm in f.ARMS:
            actual = []
            for case in (self.case, poisoned):
                original = f.x.request(
                    {"model": "fixture"}, case, "explicit_context", 0
                )
                actual.append(
                    f.request(
                        original, f.x.binding(case), self.candidate(), self.binary, arm
                    )
                )
            self.assertEqual(actual[0], actual[1])
            self.assertNotIn("secret", json.dumps(actual[0]))

    def test_ambiguous_evidence_has_every_occurrence_and_exact_source_windows(self):
        utterance = f.x.binding(self.case)
        got = f.feedback(utterance, self.candidate(), self.binary, "occurrences")
        self.assertEqual(got["location"], "events[0].status")
        self.assertEqual(got["status"], -4)
        self.assertEqual(got["occurrences"]["total_matches"], 2)
        self.assertFalse(got["semantic_support_proven"])
        raw = utterance.text.encode()
        for match in got["occurrences"]["matches"]:
            self.assertEqual(raw[match["begin"] : match["end"]], b"completed")
            self.assertEqual(
                raw[match["window_begin"] : match["window_end"]].decode(),
                match["window"],
            )

    def test_overlapping_substring_and_unicode_occurrences_are_not_word_filtered(self):
        text = "É" * 40 + "banana me name " + "é" * 40
        utterance = f.x.Utterance(text, f.x.e.a.digest(text.encode()))
        got = f.occurrences(utterance, "ana")
        self.assertEqual([r["begin"] for r in got["matches"]], [81, 83])
        self.assertEqual(f.occurrences(utterance, "me")["total_matches"], 2)
        for quote in ("ana", "me", "é"):
            for row in f.occurrences(utterance, quote)["matches"]:
                raw = text.encode()
                self.assertEqual(
                    raw[row["window_begin"] : row["window_end"]].decode(), row["window"]
                )
                self.assertEqual(raw[row["begin"] : row["end"]].decode(), quote)

    def test_truncation_missing_quotes_and_bounds_remain_explicit(self):
        text = "a" * 16384
        utterance = f.x.Utterance(text, f.x.e.a.digest(text.encode()))
        got = f.occurrences(utterance, "aa")
        self.assertEqual(got["total_matches"], 16383)
        self.assertEqual(len(got["matches"]), f.MAX_OCCURRENCES)
        self.assertTrue(got["truncated"])
        missing = f.occurrences(utterance, "absent")
        self.assertEqual(missing["matches"], [])
        self.assertEqual(missing["total_matches"], 0)
        for quote in ("", "a\0", "a" * 2049):
            self.assertFalse(f.occurrences(utterance, quote)["available"])
        with self.assertRaisesRegex(ValueError, "bounded"):
            f.feedback(utterance, " " * (f.MAX_CANDIDATE_BYTES + 1), self.binary, "raw")

    def test_malformed_frames_retain_diagnostic_without_inventing_location(self):
        for candidate in (
            '{"events":[],"events":[]}',
            "```json\n{}\n```",
            '{"events":null}',
        ):
            for arm in f.ARMS:
                got = f.feedback(f.x.binding(self.case), candidate, self.binary, arm)
                self.assertTrue(got["diagnostic"])
                self.assertNotIn("location", got)
                self.assertFalse(got["semantic_support_proven"])
                if arm == "occurrences":
                    self.assertFalse(got["occurrences"]["available"])

    def test_passing_but_wrong_event_evidence_is_not_a_repair_opportunity(self):
        rows = events(self.case)
        rows[0]["status"].update(quote="completed", context=rows[1]["anchor"])
        for arm in f.ARMS:
            with self.assertRaisesRegex(ValueError, "no repair opportunity"):
                f.feedback(
                    f.x.binding(self.case),
                    json.dumps({"events": rows}),
                    self.binary,
                    arm,
                )

    def test_c_disagreement_and_process_errors_abort(self):
        for error in (RuntimeError("C disagreement"), OSError("probe unavailable")):
            with (
                patch.object(f.x, "parse", side_effect=error),
                self.assertRaises(type(error)),
            ):
                f.feedback(f.x.binding(self.case), self.candidate(), self.binary, "raw")

    def test_rejects_changed_original_protocol_and_unknown_arm(self):
        original = f.x.request({"model": "fixture"}, self.case, "explicit_context", 0)
        for key, value in (
            ("messages", []),
            ("response_format", {}),
            ("stream", True),
            ("max_completion_tokens", 1),
        ):
            with self.assertRaisesRegex(ValueError, "protocol"):
                f.request(
                    dict(original, **{key: value}),
                    f.x.binding(self.case),
                    self.candidate(),
                    self.binary,
                    "raw",
                )
        with self.assertRaisesRegex(ValueError, "Unknown"):
            f.feedback(f.x.binding(self.case), self.candidate(), self.binary, "guess")


if __name__ == "__main__":
    unittest.main()
