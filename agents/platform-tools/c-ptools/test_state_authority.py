"""Campaign authority and signed state receipts through the real C host."""

import hashlib
import json
import os
import subprocess
import time
import unittest
from pathlib import Path

import test_campaign as campaign
import test_http as http
import test_state_recovery as recovery


class StateHTTP(unittest.TestCase):
    def setUp(self):
        self.http = http.AuthenticatedTools()
        self.http.setUp()
        self.addCleanup(self.http.doCleanups)
        self.http.stop()
        self.root = self.http.root
        self.http.environment["TOOL_DND_STATE_DIR"] = str(self.root)
        self.http.start()
        self.serial = 0

    def body(self, kind, command, call_id):
        return http.dice_request(
            tool_call_id=call_id,
            idempotency_key=call_id,
            session_id="table",
            tool_id="dnd-scene-presence" if kind == "scene" else f"dnd-{kind}-state",
            input_json=http.encode(command).decode(),
        )

    def execute(self, kind, command, *, user="alice", call_id=None, status=200):
        self.serial += 1
        call_id = call_id or f"state-{self.serial}"
        response_status, response = self.http.request(
            "POST", http.EXECUTE, self.body(kind, command, call_id), user=user
        )
        self.assertEqual(response_status, status, response)
        if status != 200:
            self.assertFalse(response["accepted"])
            return response
        record = response["call"]
        self.assertTrue(response["accepted"])
        self.assertEqual(record["user_id"], user)
        self.assertEqual(
            record["tool_id"],
            "dnd-scene-presence" if kind == "scene" else f"dnd-{kind}-state",
        )
        self.assertEqual(record["state"], "completed")
        raw = record["output_json"].encode()
        digest = hashlib.sha256(raw).hexdigest()
        self.assertEqual(record["output_sha256"], digest)
        self.assertEqual(record["output_artifact"], "sha256:" + digest)
        artifact = self.root / "artifacts" / (digest + ".json")
        self.assertEqual(artifact.read_bytes(), raw)
        self.assertEqual(artifact.stat().st_mode & 0o777, 0o600)
        output = json.loads(raw)
        self.assertEqual(output["operation_id"], call_id)
        self.assertEqual(output["operation"], command["operation"])
        self.assertEqual(output["campaign_id"], command["campaign_id"])
        return response

    def create(self):
        return self.execute(
            "campaign",
            {
                "operation": "create",
                "campaign_id": "table",
                "expected_version": 0,
                "campaign": {
                    "name": "Laeral 🐉",
                    "ruleset": "5e",
                    "description": 'Road\n"Keep"',
                    "house_rules": {"Potion": "Bonus action"},
                },
            },
            call_id="create",
        )

    def test_signed_campaign_lifecycle_and_exact_retry(self):
        created = self.create()
        self.assertEqual(json.loads(created["call"]["output_json"])["version"], 1)
        command = {
            "operation": "update",
            "campaign_id": "table",
            "expected_version": 1,
            "campaign": {"name": "Renamed", "ruleset": "2024"},
        }
        updated = self.execute("campaign", command, call_id="update")
        result = json.loads(updated["call"]["output_json"])
        self.assertEqual(result["version"], 2)
        self.assertEqual(result["campaign"]["description"], 'Road\n"Keep"')
        self.assertEqual(result["campaign"]["house_rules"], {"Potion": "Bonus action"})
        self.assertEqual(self.execute("campaign", command, call_id="update"), updated)
        self.execute(
            "campaign",
            {**command, "campaign": {"name": "Changed", "ruleset": "2024"}},
            call_id="update",
            status=409,
        )
        scene = self.execute(
            "campaign",
            {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": 2,
                "scene": "Cavern\n🐉",
            },
        )
        self.assertEqual(json.loads(scene["call"]["output_json"])["version"], 3)
        path = self.root / "campaigns/table.json"
        before = path.read_bytes()
        result = self.execute("campaign", {"operation": "get", "campaign_id": "table"})
        self.assertEqual(
            json.loads(result["call"]["output_json"])["campaign"]["current_scene"],
            "Cavern\n🐉",
        )
        self.assertEqual(path.read_bytes(), before)
        self.http.stop()
        self.http.start()
        self.assertEqual(self.execute("campaign", command, call_id="update"), updated)
        self.assertEqual(
            self.http.request("GET", "/v1/tools/calls/update"), (200, updated)
        )
        self.assertEqual(
            self.http.request("POST", "/v1/tools/calls/update/cancel", b"{}"),
            (200, updated),
        )
        self.assertEqual(path.read_bytes(), before)

    def test_signed_encounter_lifecycle(self):
        self.create()
        base = {"campaign_id": "table", "encounter_id": "battle"}
        self.execute("encounter", {**base, "operation": "start", "expected_version": 0})
        actions = [
            (
                "add",
                {
                    "participant": {
                        "id": "rogue",
                        "name": "Rógué 🐉",
                        "initiative": 12,
                        "max_hp": 20,
                        "current_hp": 0,
                    }
                },
            ),
            ("heal", {"participant_id": "rogue", "amount": 7}),
            ("damage", {"participant_id": "rogue", "amount": 2, "damage_type": "fire"}),
            ("condition_add", {"participant_id": "rogue", "condition": "prone"}),
            ("condition_remove", {"participant_id": "rogue", "condition": "prone"}),
            ("advance", {}),
            ("remove", {"participant_id": "rogue"}),
            ("end", {}),
        ]
        for version, (operation, fields) in enumerate(actions, 1):
            response = self.execute(
                "encounter",
                {**base, "operation": operation, "expected_version": version, **fields},
            )
            output = json.loads(response["call"]["output_json"])
            self.assertEqual(output["version"], version + 1)
            if operation == "heal":
                self.assertEqual(output["participants"][0]["current_hp"], 7)
            if operation == "damage":
                self.assertEqual(output["participants"][0]["current_hp"], 5)
            if operation == "condition_add":
                self.assertEqual(output["participants"][0]["conditions"], ["prone"])
            if operation == "condition_remove":
                self.assertEqual(output["participants"][0]["conditions"], [])
            if operation == "advance":
                self.assertEqual((output["round"], output["active_index"]), (1, 0))
        before = (self.root / "encounters/table/battle.json").read_bytes()
        result = self.execute("encounter", {**base, "operation": "get"})
        self.assertEqual(json.loads(result["call"]["output_json"])["status"], "ended")
        self.assertEqual(
            (self.root / "encounters/table/battle.json").read_bytes(), before
        )

    def test_other_owners_and_forged_authority_never_admit(self):
        self.create()
        initial = sorted(p.name for p in (self.root / "calls.d").iterdir())
        cases = [
            ("campaign", {"operation": "get", "campaign_id": "table"}),
            (
                "campaign",
                {
                    "operation": "set_scene",
                    "campaign_id": "table",
                    "expected_version": 1,
                    "scene": "Other",
                },
            ),
            (
                "campaign",
                {
                    "operation": "create",
                    "campaign_id": "table",
                    "expected_version": 0,
                    "campaign": {"name": "Other", "ruleset": "5e"},
                },
            ),
            (
                "encounter",
                {
                    "operation": "start",
                    "campaign_id": "table",
                    "encounter_id": "battle",
                    "expected_version": 0,
                },
            ),
        ]
        for kind, command in cases:
            self.execute(kind, command, user="bob", status=409)
        for suffix, method, body in [("", "GET", b""), ("/cancel", "POST", b"{}")]:
            self.assertEqual(
                self.http.request(
                    method, "/v1/tools/calls/create" + suffix, body, user="bob"
                )[0],
                404,
            )
        forged = http.dice_request(
            user_id="alice",
            tool_id="dnd-campaign-state",
            input_json='{"operation":"get","campaign_id":"table"}',
        )
        self.assertEqual(
            self.http.request("POST", http.EXECUTE, forged, user="bob")[0], 400
        )
        for field in ["owner_user_id", "authorized", "membership", "entitled"]:
            self.execute(
                "campaign",
                {"operation": "get", "campaign_id": "table", field: "alice"},
                status=400,
            )
        self.assertEqual(
            sorted(p.name for p in (self.root / "calls.d").iterdir()), initial
        )
        self.assertFalse((self.root / "encounters/table/battle.json").exists())

    def test_receipts_recheck_current_campaign_authority(self):
        original = self.create()
        path = self.root / "campaigns/table.json"
        before = path.read_bytes()
        other = json.loads(before)
        other["owner_user_id"] = "bob"
        for invalid in [
            http.encode(other),
            b'{"owner_user_id":"alice"}',
            before + b"\0",
        ]:
            path.write_bytes(invalid)
            self.assertEqual(self.http.request("GET", "/v1/tools/calls/create")[0], 404)
            self.assertEqual(
                self.http.request("POST", "/v1/tools/calls/create/cancel", b"{}")[0],
                404,
            )
            self.execute(
                "campaign", {"operation": "get", "campaign_id": "table"}, status=409
            )
            self.assertEqual(path.read_bytes(), invalid)
        path.unlink()
        self.assertEqual(self.http.request("GET", "/v1/tools/calls/create")[0], 404)
        self.execute(
            "encounter",
            {
                "operation": "start",
                "campaign_id": "table",
                "encounter_id": "battle",
                "expected_version": 0,
            },
            status=409,
        )
        path.write_bytes(before)
        self.assertEqual(
            self.http.request("GET", "/v1/tools/calls/create"), (200, original)
        )

    def test_invalid_state_commands_and_deadlines_never_create_calls(self):
        self.create()
        before = sorted(p.name for p in (self.root / "calls.d").iterdir())
        invalid = [
            "{}",
            '{"operation":"get","campaign_id":"table","operation":"create"}',
            '{"operation":"get","campaign_id":"table\\u0000other"}',
            '{"operation":"set_scene","campaign_id":"table","expected_version":1.0,"scene":"x"}',
            '{"operation":"set_scene","campaign_id":"table","expected_version":1,"scene":"x"}{}',
        ]
        for command in invalid:
            body = http.dice_request(tool_id="dnd-campaign-state", input_json=command)
            self.assertEqual(self.http.request("POST", http.EXECUTE, body)[0], 400)
        for deadline in [int(time.time() * 1000) - 1, int(time.time() * 1000) + 120000]:
            body = http.dice_request(
                tool_id="dnd-campaign-state",
                deadline_unix_ms=deadline,
                input_json='{"operation":"get","campaign_id":"table"}',
            )
            self.assertEqual(self.http.request("POST", http.EXECUTE, body)[0], 400)
        self.assertEqual(
            sorted(p.name for p in (self.root / "calls.d").iterdir()), before
        )

    def test_large_unicode_campaign_result_keeps_every_retained_field(self):
        self.create()
        retained = campaign.state(owner_user_id="alice")
        retained["session_recaps"] = [
            {
                **retained["session_recaps"][0],
                "session_id": f"session-{i}",
                "created_at_unix_ms": i + 10,
            }
            for i in range(5)
        ]
        path = self.root / "campaigns/table.json"
        before = campaign.encoded(retained)
        path.write_bytes(before)
        response = self.execute(
            "campaign", {"operation": "get", "campaign_id": "table"}
        )
        raw = response["call"]["output_json"]
        self.assertGreater(len(raw.encode()), 16384)
        output = json.loads(raw)
        for field in ["campaign", "characters", "session_recaps", "scene_director"]:
            self.assertEqual(output[field], retained[field])
        self.assertEqual(path.read_bytes(), before)

    def test_state_artifact_corruption_rejects_http_read_retry_and_cancel(self):
        original = self.create()
        artifact = (
            self.root / "artifacts" / (original["call"]["output_sha256"] + ".json")
        )
        before = artifact.read_bytes()
        artifact.write_bytes(b"{}")
        self.assertEqual(self.http.request("GET", "/v1/tools/calls/create")[0], 404)
        self.assertEqual(
            self.http.request("POST", "/v1/tools/calls/create/cancel", b"{}")[0], 404
        )
        self.execute(
            "campaign",
            json.loads(original["call"]["input_json"]),
            call_id="create",
            status=409,
        )
        self.assertEqual(artifact.read_bytes(), b"{}")
        artifact.write_bytes(before)
        self.assertEqual(
            self.http.request("GET", "/v1/tools/calls/create"), (200, original)
        )

    def test_crashed_campaign_transactions_resume_through_signed_http(self):
        self.create()
        binary = Path(__file__).with_name("c-ptools-campaign-fixture")
        for version, stage in enumerate(
            ["queued", "artifact", "journal", "state", "complete", "cleanup"], 1
        ):
            call_id = "crash-" + stage
            command = {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": version,
                "scene": stage,
            }
            self.http.stop()
            result = subprocess.run(
                [str(binary), str(self.root), "alice", call_id, "record"],
                check=False,
                input=http.encode(command),
                capture_output=True,
                timeout=20,
                env={
                    **os.environ,
                    "PT_TEST_CRASH_AT": stage,
                    "PT_TEST_PARENT_TURN_ID": "turn-1",
                },
            )
            self.assertEqual(result.returncode, 77, result.stderr)
            self.http.start()
            completed = self.execute("campaign", command, call_id=call_id)
            self.assertEqual(
                json.loads(completed["call"]["output_json"])["version"], version + 1
            )
            self.assertEqual(
                self.execute("campaign", command, call_id=call_id), completed
            )
            self.assertEqual(
                json.loads((self.root / "campaigns/table.json").read_bytes())[
                    "version"
                ],
                version + 1,
            )


class ParentAuthority(unittest.TestCase):
    def setUp(self):
        self.fixture = recovery.StateRecoveryTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def test_missing_or_conflicting_parent_denies_native_encounter_admission(self):
        case = self.fixture.case("encounter")
        parent = case["root"] / "campaigns/table.json"
        before = parent.read_bytes()
        initial = case["path"].read_bytes()
        for changes in [
            {"owner_user_id": "other"},
            {"campaign_id": "other"},
            {"version": 0},
        ]:
            parent.write_bytes(http.encode({**json.loads(before), **changes}))
            self.assertIsNone(self.fixture.run_call(case, code=1))
            self.assertEqual(case["path"].read_bytes(), initial)
            self.assertFalse(self.fixture.stored_call_path(case).exists())
        parent.unlink()
        self.assertIsNone(self.fixture.run_call(case, code=1))
        parent.symlink_to(case["path"])
        self.assertIsNone(self.fixture.run_call(case, code=1))
        parent.unlink()
        parent.write_bytes(before)
        self.fixture.assert_complete(case, self.fixture.run_call(case))

    def test_pending_encounter_cannot_recover_under_a_different_campaign_owner(self):
        for stage in ["journal", "state", "complete"]:
            with self.subTest(stage=stage):
                case = self.fixture.case("encounter")
                self.fixture.run_call(case, crash=stage)
                parent = case["root"] / "campaigns/table.json"
                original_parent = parent.read_bytes()
                before = case["path"].read_bytes()
                journal = case["journal"].read_bytes()
                record = self.fixture.stored_call_path(case).read_bytes()
                parent.write_bytes(
                    http.encode(
                        {**json.loads(original_parent), "owner_user_id": "other"}
                    )
                )
                for action in ["record", "cancel"]:
                    self.assertIsNone(
                        self.fixture.run_call(case, action=action, code=1)
                    )
                    self.assertEqual(case["path"].read_bytes(), before)
                    self.assertEqual(case["journal"].read_bytes(), journal)
                    self.assertEqual(
                        self.fixture.stored_call_path(case).read_bytes(), record
                    )
                parent.write_bytes(original_parent)
                self.fixture.assert_complete(case, self.fixture.run_call(case))


if __name__ == "__main__":
    unittest.main()
