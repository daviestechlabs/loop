"""Independent process and file oracles for shared C state recovery."""

import copy
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_campaign as campaign
import test_encounter as encounter


class StateRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="c-state-recovery-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.case_count = 0

    def case(self, kind="campaign"):
        self.case_count += 1
        root = self.root / str(self.case_count)
        if kind == "encounter":
            encounter.install_campaign(root)
        initial = campaign.state() if kind == "campaign" else encounter.state()
        path = root / (
            "campaigns/table.json"
            if kind == "campaign"
            else "encounters/table/battle.json"
        )
        path.parent.mkdir(parents=True)
        path.write_bytes(campaign.encoded(initial))
        command = (
            {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": 2,
                "scene": "Cave",
            }
            if kind == "campaign"
            else {
                "operation": "heal",
                "campaign_id": "table",
                "encounter_id": "battle",
                "expected_version": 2,
                "participant_id": "rogue",
                "amount": 1,
            }
        )
        expected = copy.deepcopy(initial)
        expected["version"] = 3
        if kind == "campaign":
            expected["campaign"]["current_scene"] = "Cave"
        else:
            expected["participants"][0]["current_hp"] = 1
        return {
            "kind": kind,
            "root": root,
            "path": path,
            "initial": initial,
            "expected": expected,
            "command": command,
            "journal": root / f"{kind}-transaction-v1.json",
            "binary": campaign.BINARY if kind == "campaign" else encounter.BINARY,
        }

    def run_call(
        self,
        case,
        *,
        command=None,
        call_id="state-call",
        user="user",
        action="record",
        crash=None,
        fail_sync=None,
        code=0,
    ):
        env = os.environ.copy()
        env.pop("PT_TEST_CRASH_AT", None)
        env.pop("PT_TEST_FAIL_FSYNC", None)
        if crash:
            env["PT_TEST_CRASH_AT"] = crash
            code = 77
        if fail_sync:
            env["PT_TEST_FAIL_FSYNC"] = fail_sync
        request = (
            command
            if isinstance(command, str)
            else campaign.encoded(command or case["command"])
        )
        response = subprocess.run(
            [str(case["binary"]), str(case["root"]), user, call_id, action],
            input=request.encode() if isinstance(request, str) else request,
            capture_output=True,
            check=False,
            timeout=20,
            env=env,
        )
        self.assertEqual(response.returncode, code, response.stderr)
        self.assertEqual(response.stderr, b"")
        if crash:
            self.assertEqual(response.stdout, b"")
            return None
        return json.loads(response.stdout) if response.stdout else None

    def stored_call_path(self, case, call_id="state-call"):
        return (
            case["root"]
            / "calls.d"
            / (hashlib.sha256(call_id.encode()).hexdigest() + ".json")
        )

    def assert_complete(self, case, record, expected=None):
        expected = expected or case["expected"]
        self.assertEqual(record["state"], "completed")
        raw = record["output_json"].encode()
        digest = hashlib.sha256(raw).hexdigest()
        self.assertEqual(record["output_sha256"], digest)
        self.assertEqual(record["output_artifact"], "sha256:" + digest)
        artifact = case["root"] / "artifacts" / (digest + ".json")
        self.assertEqual(artifact.read_bytes(), raw)
        self.assertEqual(artifact.stat().st_mode & 0o777, 0o600)
        output = json.loads(raw)
        self.assertEqual(output["operation_id"], record["tool_call_id"])
        self.assertEqual(
            {k: output[k] for k in expected if k != "owner_user_id"},
            {k: v for k, v in expected.items() if k != "owner_user_id"},
        )
        self.assertEqual(json.loads(case["path"].read_bytes()), expected)
        self.assertFalse(case["journal"].exists())

    def test_campaign_recovers_each_crash_boundary(self):
        for stage in ["queued", "artifact", "journal", "state", "complete", "cleanup"]:
            with self.subTest(stage=stage):
                case = self.case()
                self.run_call(case, crash=stage)
                changed = stage in ["state", "complete", "cleanup"]
                self.assertEqual(
                    json.loads(case["path"].read_bytes())["version"],
                    3 if changed else 2,
                )
                completed = self.run_call(case)
                self.assert_complete(case, completed)
                self.assertEqual(self.run_call(case), completed)

    def test_campaign_update_and_read_recovery_preserve_collections(self):
        case = self.case()
        command = {
            "operation": "update",
            "campaign_id": "table",
            "expected_version": 2,
            "campaign": {"name": "Renamed", "ruleset": "2024"},
        }
        expected = copy.deepcopy(case["initial"])
        expected["version"] = 3
        expected["campaign"].update(name="Renamed", ruleset="2024")
        self.run_call(case, command=command, crash="state")
        self.assert_complete(case, self.run_call(case, command=command), expected)
        # Retain different whitespace to prove a read does not rewrite state.
        case["path"].write_text(json.dumps(expected, ensure_ascii=False, indent=2))
        before = case["path"].read_bytes()
        read = {"operation": "get", "campaign_id": "table"}
        self.run_call(case, command=read, call_id="read", crash="journal")
        completed = self.run_call(case, command=read, call_id="read")
        self.assert_complete(case, completed, expected)
        self.assertEqual(case["path"].read_bytes(), before)

    def test_campaign_create_recovers_once(self):
        case = self.case()
        case["path"].unlink()
        command = {
            "operation": "create",
            "campaign_id": "table",
            "expected_version": 0,
            "campaign": campaign.metadata(),
        }
        expected = campaign.state(
            version=1,
            characters=[],
            session_recaps=[],
            scene_director={
                "active_npc_ids": [],
                "next_speaker_index": 0,
                "active_speaker_id": "",
                "active_turn_id": "",
                "lease_expires_unix_ms": 0,
            },
        )
        self.run_call(case, command=command, crash="journal")
        self.assertFalse(case["path"].exists())
        completed = self.run_call(case, command=command)
        self.assert_complete(case, completed, expected)
        self.assertEqual(self.run_call(case, command=command), completed)

    def test_campaign_corrupt_journal_never_changes_state(self):
        case = self.case()
        self.run_call(case, crash="journal")
        original_bytes = case["journal"].read_bytes()
        original = json.loads(original_bytes)
        before = case["path"].read_bytes()
        changes = [
            ("before", "owner_user_id", "other"),
            ("before", "version", 4),
            ("after", "version", 9),
            ("call", "user_id", "other"),
            ("call", "tool_id", "dnd-encounter-state"),
            ("call", "output_sha256", "0" * 64),
            ("call", "created_at", 0),
            ("call", "parent_turn_id", "other-turn"),
        ]
        for scope, key, value in changes:
            with self.subTest(scope=scope, key=key):
                # Keep every unrelated byte, including canonical control escapes.
                begin = original_bytes.index(('"' + scope + '":').encode())
                end = (
                    original_bytes.index(b',"after":')
                    if scope == "before"
                    else original_bytes.index(b',"call":')
                    if scope == "after"
                    else len(original_bytes)
                )
                member = ('"' + key + '":').encode()
                old = member + campaign.encoded(original[scope][key])
                section = original_bytes[begin:end]
                self.assertEqual(section.count(old), 1)
                raw = (
                    original_bytes[:begin]
                    + section.replace(old, member + campaign.encoded(value), 1)
                    + original_bytes[end:]
                )
                case["journal"].write_bytes(raw)
                self.assertIsNone(self.run_call(case, code=1))
                self.assertEqual(case["journal"].read_bytes(), raw)
                self.assertEqual(case["path"].read_bytes(), before)
        raw = original_bytes
        for invalid in [
            raw[:-1],
            raw + b"{}",
            raw + b"\0",
            b" " * 1048576 + raw,
            raw.replace(b'"schema":', b'"schema":"other","schema":', 1),
        ]:
            case["journal"].write_bytes(invalid)
            self.assertIsNone(self.run_call(case, code=1))
            self.assertEqual(case["path"].read_bytes(), before)
        case["journal"].write_bytes(raw)
        self.assert_complete(case, self.run_call(case))

    def test_pending_io_keeps_queued_calls_for_both_tools(self):
        for kind in ["encounter", "campaign"]:
            for fault in ["1", "2", "journal", "journal-dir", "complete"]:
                with self.subTest(kind=kind, fault=fault):
                    case = self.case(kind)
                    self.run_call(case, fail_sync=fault, code=1)
                    stored = json.loads(self.stored_call_path(case).read_bytes())
                    self.assertEqual(stored["state"], "queued")
                    self.assertEqual(case["journal"].exists(), fault != "journal")
                    self.assertEqual(
                        json.loads(case["path"].read_bytes())["version"],
                        3 if fault in ["2", "complete"] else 2,
                    )
                    completed = self.run_call(case)
                    self.assert_complete(case, completed)
                    self.assertEqual(self.run_call(case), completed)

    def test_cancel_read_and_other_tool_wait_for_committed_work(self):
        for kind in ["encounter", "campaign"]:
            for action in ["cancel-pending", "read-pending", "other-pending"]:
                with self.subTest(kind=kind, action=action):
                    case = self.case(kind)
                    completed = self.run_call(case, action=action, fail_sync="1")
                    self.assert_complete(case, completed)
                    self.assertEqual(self.run_call(case), completed)
                    if action == "other-pending":
                        other = json.loads(
                            self.stored_call_path(case, "other-state").read_bytes()
                        )
                        self.assertEqual(other["state"], "completed")
                        self.assertTrue(other["output_artifact"].startswith("sha256:"))

    def test_campaign_cancel_before_journal_commit(self):
        case = self.case()
        self.run_call(case, crash="queued")
        before = case["path"].read_bytes()
        self.assertIsNone(self.run_call(case, user="other", action="cancel", code=1))
        canceled = self.run_call(case, action="cancel")
        self.assertEqual(canceled["state"], "canceled")
        self.assertEqual(self.run_call(case, code=1), canceled)
        self.assertEqual(case["path"].read_bytes(), before)
        self.assertFalse(case["journal"].exists())

    def test_cleanup_sync_failure_keeps_completed_receipt(self):
        for kind in ["encounter", "campaign"]:
            case = self.case(kind)
            completed = self.run_call(case, fail_sync="cleanup")
            self.assert_complete(case, completed)
            self.assertEqual(self.run_call(case), completed)

    def test_artifacts_and_startup_permission_repair(self):
        for kind in ["encounter", "campaign"]:
            case = self.case(kind)
            self.run_call(case, crash="journal")
            tx = json.loads(case["journal"].read_bytes())
            path = case["root"] / "artifacts" / (tx["call"]["output_sha256"] + ".json")
            original = path.read_bytes()
            before = case["path"].read_bytes()
            path.write_bytes(b"corrupt")
            self.assertIsNone(self.run_call(case, code=1))
            self.assertEqual(path.read_bytes(), b"corrupt")
            path.unlink()
            self.assertIsNone(self.run_call(case, code=1))
            self.assertFalse(path.exists())
            self.assertEqual(case["path"].read_bytes(), before)
            path.write_bytes(original)
            path.chmod(0o644)
            self.assertIsNone(self.run_call(case, code=1))
            self.assertEqual(path.stat().st_mode & 0o777, 0o644)
            path.chmod(0o660)
            self.assert_complete(case, self.run_call(case))

    def test_completed_and_legacy_campaign_records_require_artifacts(self):
        case = self.case()
        completed = self.run_call(case)
        self.assert_complete(case, completed)
        before = case["path"].read_bytes()
        artifact = case["root"] / "artifacts" / (completed["output_sha256"] + ".json")
        artifact.unlink()
        self.assertIsNone(self.run_call(case, code=1))
        self.assertIsNone(self.run_call(case, action="cancel", code=1))
        record = self.stored_call_path(case)
        legacy = json.loads(record.read_bytes())
        legacy["output_sha256"] = legacy["output_artifact"] = ""
        record.write_bytes(campaign.encoded(legacy))
        self.assertIsNone(self.run_call(case, code=1))
        self.assertFalse(artifact.exists())
        self.assertEqual(case["path"].read_bytes(), before)

    def test_campaign_retry_identity_and_old_cleanup_journal(self):
        case = self.case()
        self.run_call(case, crash="complete")
        journal = case["journal"].read_bytes()
        first = self.run_call(case)
        self.assert_complete(case, first)
        self.assertIsNone(self.run_call(case, user="other", code=1))
        self.assertEqual(
            self.run_call(
                case, command={**case["command"], "scene": "Changed"}, code=1
            ),
            first,
        )
        later = {**case["command"], "expected_version": 3, "scene": "Later"}
        self.run_call(case, command=later, call_id="later")
        before = case["path"].read_bytes()
        case["journal"].write_bytes(journal)
        self.assertEqual(self.run_call(case), first)
        self.assertEqual(case["path"].read_bytes(), before)
        self.assertEqual(json.loads(before)["version"], 4)
        self.assertFalse(case["journal"].exists())

    def test_conflicting_state_and_wrong_journal_kind(self):
        for kind in ["encounter", "campaign"]:
            case = self.case(kind)
            self.run_call(case, crash="journal")
            before = case["path"].read_bytes()
            changed = {**case["initial"], "version": 4}
            case["path"].write_bytes(campaign.encoded(changed))
            self.assertIsNone(self.run_call(case, code=1))
            self.assertEqual(json.loads(case["path"].read_bytes()), changed)
            case["path"].write_bytes(before)
            other = case["root"] / (
                "campaign-transaction-v1.json"
                if kind == "encounter"
                else "encounter-transaction-v1.json"
            )
            case["journal"].rename(other)
            self.assertIsNone(self.run_call(case, code=1))
            self.assertEqual(case["path"].read_bytes(), before)
            other.rename(case["journal"])
            self.assert_complete(case, self.run_call(case))

    def test_previously_landed_encounter_journal_remains_compatible(self):
        # This unchanged seed comes from the encounter-only journal implementation.
        seed = Path(__file__).with_name("fuzz-corpus") / "state/encounter.json"
        seed_bytes = seed.read_bytes()
        tx = json.loads(seed_bytes)
        case = self.case("encounter")
        case["path"].write_bytes(campaign.encoded(tx["before"]))
        case["journal"].write_bytes(seed_bytes)
        complete = tx["call"]
        queued = {
            **complete,
            "state": "queued",
            "updated_at": complete["created_at"],
            "output_json": "",
            "output_sha256": "",
            "output_artifact": "",
            "summary": "",
            "error": "",
        }
        record = self.stored_call_path(case, complete["tool_call_id"])
        record.parent.mkdir()
        record.write_bytes(campaign.encoded(queued))
        artifact = case["root"] / "artifacts" / (complete["output_sha256"] + ".json")
        artifact.parent.mkdir()
        artifact.write_bytes(complete["output_json"].encode())
        artifact.chmod(0o600)
        result = self.run_call(
            case, command=complete["input_json"], call_id=complete["tool_call_id"]
        )
        self.assertEqual(result, complete)
        self.assert_complete(case, result, tx["after"])


if __name__ == "__main__":
    unittest.main()
