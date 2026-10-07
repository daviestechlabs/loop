"""A routing failure must not hide later independent challenge pairs."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import coupled_dialogue_eval as evaluation


class CollectionTests(unittest.TestCase):
    def test_bad_route_collects_later_pairs_but_cannot_pass(self):
        visited = []

        def fake_trial(endpoint, directory, case_id, message, scene, condition):
            visited.append((case_id, condition))
            directory.mkdir()
            return {
                "case_id": case_id,
                "condition": condition,
                "completed": True,
                "answer": "observed reply",
                "model_calls": 0 if len(visited) == 1 else 1,
            }

        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "results"
            with (
                patch.object(evaluation, "trial", side_effect=fake_trial),
                patch.object(evaluation, "model_identity", return_value={"data": []}),
                contextlib.redirect_stdout(io.StringIO()),
                self.assertRaisesRegex(AssertionError, "Unexpected model call counts"),
            ):
                evaluation.run("http://unused", output)
            rows = json.loads((output / "results.json").read_text())
            self.assertEqual(len(rows), 12)
            self.assertEqual(rows[0]["model_calls"], 0)
            self.assertEqual(visited[-1], ("changed_scene", "retained_conversation"))
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["trials"], 12)
            self.assertIn("models-after.json", manifest["files"])

    def test_transport_failure_stops_collection(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "results"
            with (
                patch.object(evaluation, "trial", side_effect=TimeoutError) as trial,
                patch.object(evaluation, "model_identity", return_value={"data": []}),
                self.assertRaises(TimeoutError),
            ):
                evaluation.run("http://unused", output)
            self.assertEqual(trial.call_count, 1)
            self.assertFalse((output / "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
