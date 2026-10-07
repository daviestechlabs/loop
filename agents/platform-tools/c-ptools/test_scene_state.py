"""Scene observations through signed HTTP and the durable Linux state owner."""

import copy
import hashlib
import json
import os
import unittest
from unittest.mock import patch

import test_campaign as campaign
import test_scene_observations as scene
import test_state_authority as authority
import test_state_recovery as recovery


class SceneState(unittest.TestCase):
    def fixture(self, kind):
        case = kind()
        case.setUp()
        self.addCleanup(case.doCleanups)
        return case

    def test_signed_presence_owner_retry_restart_and_stale_receipt(self):
        case = self.fixture(authority.StateHTTP)
        case.create()
        case.execute(
            "campaign",
            {
                "operation": "add_character",
                "campaign_id": "table",
                "expected_version": 1,
                "character": campaign.character(),
            },
        )
        command = scene.write_command()
        written = case.execute("campaign", command, call_id="scene-write")
        query = scene.read_command()
        read = case.execute("campaign", query, call_id="scene-read")
        output = json.loads(read["call"]["output_json"])
        self.assertEqual(output["presence"], "present")
        self.assertEqual(output["source_turn_id"], "mira-enters-hall")
        self.assertEqual(len(output), 9)
        case.execute("campaign", query, user="bob", status=409)
        case.execute("campaign", command, user="bob", status=409)
        self.assertEqual(
            case.http.request("GET", "/v1/tools/calls/scene-read", user="bob")[0], 404
        )
        case.http.stop()
        case.http.start()
        self.assertEqual(
            case.execute("campaign", command, call_id="scene-write"), written
        )
        self.assertEqual(case.execute("campaign", query, call_id="scene-read"), read)
        case.execute(
            "campaign",
            {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": 3,
                "scene": "Courtyard",
            },
        )
        case.execute("campaign", query, status=409)
        current = case.execute("campaign", scene.read_command(expected_version=4))
        current_output = json.loads(current["call"]["output_json"])
        self.assertEqual(
            (
                current_output["version"],
                current_output["presence"],
                current_output["source_turn_id"],
            ),
            (4, "unknown", ""),
        )
        # A retry is an immutable receipt, not a fresh world-state query.
        self.assertEqual(case.execute("campaign", query, call_id="scene-read"), read)

    def test_observation_write_recovers_at_each_existing_crash_boundary(self):
        case = self.fixture(recovery.StateRecoveryTest)
        for stage in ("queued", "artifact", "journal", "state", "complete", "cleanup"):
            with self.subTest(stage=stage):
                item = case.case()
                item["command"] = scene.write_command()
                item["expected"] = {
                    **copy.deepcopy(item["initial"]),
                    "version": 3,
                    "scene_observations": {
                        **scene.write_command()["scene_observations"],
                        "version": 3,
                    },
                }
                case.run_call(item, crash=stage)
                record = case.run_call(item)
                case.assert_complete(item, record)
                self.assertEqual(case.run_call(item), record)

    def test_private_read_recovers_without_rewriting_or_leaking_state(self):
        case = self.fixture(recovery.StateRecoveryTest)
        item = case.case()
        initial = item["initial"]
        initial["scene_observations"] = {
            "version": 2,
            "scene_id": "hall",
            "entries": [
                scene.observation(visibility="dm", source_turn_id="secret-exit")
            ],
        }
        item["path"].write_text(json.dumps(initial, indent=2))
        before = item["path"].read_bytes()
        item["command"] = scene.read_command(expected_version=2)
        case.run_call(item, crash="journal")
        record = case.run_call(item)
        self.assertEqual(record["state"], "completed")
        self.assertEqual(item["path"].read_bytes(), before)
        self.assertFalse(item["journal"].exists())
        output = json.loads(record["output_json"])
        self.assertEqual(output["presence"], "unknown")
        self.assertEqual(output["source_turn_id"], "")
        self.assertEqual(len(output), 9)
        self.assertNotIn("secret-exit", record["output_json"])
        digest = hashlib.sha256(record["output_json"].encode()).hexdigest()
        self.assertEqual(record["output_sha256"], digest)
        self.assertEqual(
            (item["root"] / "artifacts" / (digest + ".json")).read_text(),
            record["output_json"],
        )
        self.assertEqual(case.run_call(item), record)

    def test_dedicated_scene_tool_is_owned_read_only_and_fresh(self):
        case = self.fixture(authority.StateHTTP)
        case.create()
        case.execute(
            "campaign",
            {
                "operation": "add_character",
                "campaign_id": "table",
                "expected_version": 1,
                "character": campaign.character(),
            },
        )
        case.execute("campaign", scene.write_command())
        path = case.root / "campaigns/table.json"
        before = path.read_bytes()
        first = case.execute("scene", scene.resolve_command(), call_id="query-first")
        self.assertEqual(
            json.loads(first["call"]["output_json"])["presence"], "present"
        )
        self.assertEqual(path.read_bytes(), before)
        case.execute("scene", scene.resolve_command(), user="bob", status=409)
        for operation in (
            scene.write_command(),
            {"operation": "get", "campaign_id": "table"},
        ):
            case.execute("scene", operation, status=400)
        case.http.stop()
        case.http.start()
        self.assertEqual(
            case.execute("scene", scene.resolve_command(), call_id="query-first"), first
        )
        case.execute(
            "campaign",
            {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": 3,
                "scene": "Courtyard",
            },
        )
        current = case.execute(
            "scene", scene.resolve_command(), call_id="query-current"
        )
        self.assertEqual(
            json.loads(current["call"]["output_json"])["presence"], "unknown"
        )
        self.assertEqual(
            case.execute("scene", scene.resolve_command(), call_id="query-first"), first
        )

    def test_dedicated_read_recovers_all_five_boundaries_without_state_write(self):
        case = self.fixture(recovery.StateRecoveryTest)
        for stage in ("queued", "artifact", "journal", "complete", "cleanup"):
            with (
                self.subTest(stage=stage),
                patch.dict(os.environ, PT_TEST_SCENE_TOOL="1"),
            ):
                item = case.case()
                initial = item["initial"]
                initial["scene_observations"] = {
                    "version": 2,
                    "scene_id": "hall",
                    "entries": [
                        scene.observation(visibility="dm", source_turn_id="secret-exit")
                    ],
                }
                item["path"].write_text(json.dumps(initial, indent=2))
                before = item["path"].read_bytes()
                item["command"] = scene.resolve_command()
                item["journal"] = item["root"] / "scene-query-transaction-v1.json"
                case.run_call(item, crash=stage)
                record = case.run_call(item)
                self.assertEqual(record["state"], "completed")
                self.assertEqual(record["tool_id"], "dnd-scene-presence")
                self.assertEqual(item["path"].read_bytes(), before)
                self.assertFalse(item["journal"].exists())
                output = json.loads(record["output_json"])
                self.assertEqual(
                    (output["presence"], output["source_turn_id"]), ("unknown", "")
                )
                self.assertNotIn("secret-exit", record["output_json"])
                self.assertEqual(case.run_call(item), record)


if __name__ == "__main__":
    unittest.main()
