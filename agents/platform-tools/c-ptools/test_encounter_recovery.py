"""Crash and corruption oracles for the native C encounter transaction."""

import copy
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_encounter import BINARY, encoded, state, install_campaign


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="c-encounter-recovery-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        install_campaign(self.root)
        self.path = self.root / "encounters/table/battle.json"
        self.path.parent.mkdir(parents=True)
        self.path.write_bytes(encoded(state()))
        self.journal = self.root / "encounter-transaction-v1.json"
        self.command = {
            "operation": "heal",
            "campaign_id": "table",
            "encounter_id": "battle",
            "expected_version": 2,
            "participant_id": "rogue",
            "amount": 1,
        }

    def run_call(
        self,
        *,
        crash=None,
        command=None,
        call_id="heal-1",
        user="user",
        code=0,
        action="record",
        fail_sync=None,
    ):
        env = os.environ.copy()
        env.pop("PT_TEST_CRASH_AT", None)
        env.pop("PT_TEST_FAIL_FSYNC", None)
        if fail_sync:
            env["PT_TEST_FAIL_FSYNC"] = fail_sync
        if crash:
            env["PT_TEST_CRASH_AT"] = crash
            code = 77
        result = subprocess.run(
            [str(BINARY), str(self.root), user, call_id, action],
            input=encoded(command or self.command),
            capture_output=True,
            timeout=15,
            check=False,
            env=env,
        )
        self.assertEqual(result.returncode, code, result.stderr)
        self.assertEqual(result.stderr, b"")
        if crash:
            self.assertEqual(result.stdout, b"")
            return None
        return json.loads(result.stdout) if result.stdout else None

    def assert_complete(self, record, *, version=3, hp=1):
        self.assertEqual(record["state"], "completed")
        raw = record["output_json"].encode()
        self.assertEqual(record["output_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(record["output_artifact"], "sha256:" + record["output_sha256"])
        path = self.root / "artifacts" / (record["output_sha256"] + ".json")
        self.assertEqual(path.read_bytes(), raw)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        result = json.loads(raw)
        self.assertEqual(
            (result["version"], result["participants"][0]["current_hp"]), (version, hp)
        )
        self.assertEqual(result["operation_id"], record["tool_call_id"])
        self.assertFalse(self.journal.exists())

    def crash_and_resume(self, phase):
        self.run_call(crash=phase)
        before = json.loads(self.path.read_bytes())
        changed = phase in ["state", "complete", "cleanup"]
        self.assertEqual(
            (before["version"], before["participants"][0]["current_hp"]),
            (3, 1) if changed else (2, 0),
        )
        completed = self.run_call()
        self.assert_complete(completed)
        self.assertEqual(self.run_call(), completed)
        self.assertEqual(
            json.loads(self.path.read_bytes())["participants"][0]["current_hp"], 1
        )

    def test_crash_after_queued_call(self):
        self.crash_and_resume("queued")

    def test_crash_after_artifact_before_journal(self):
        self.crash_and_resume("artifact")

    def test_crash_after_journal_rename(self):
        self.crash_and_resume("journal")

    def test_crash_after_state_rename(self):
        self.crash_and_resume("state")

    def test_crash_after_complete_record_rename(self):
        self.crash_and_resume("complete")

    def test_crash_after_journal_cleanup(self):
        self.crash_and_resume("cleanup")

    def test_cancel_before_commit_preserves_state(self):
        self.run_call(crash="queued")
        before = self.path.read_bytes()
        self.assertIsNone(self.run_call(action="cancel", user="other-user", code=1))
        canceled = self.run_call(action="cancel")
        self.assertEqual(canceled["state"], "canceled")
        self.assertEqual(self.run_call(code=1), canceled)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertFalse(self.journal.exists())

    def test_cancel_cannot_discard_committed_pending_work(self):
        completed = self.run_call(action="cancel-pending", fail_sync="1")
        self.assert_complete(completed)
        self.assertEqual(self.run_call(action="cancel"), completed)
        self.assertEqual(
            json.loads(self.path.read_bytes())["participants"][0]["current_hp"], 1
        )

    def test_changed_retry_and_wrong_owner(self):
        completed = self.run_call()
        self.assert_complete(completed)
        before = self.path.read_bytes()
        self.run_call(command={**self.command, "amount": 2}, code=1)
        self.assertIsNone(self.run_call(user="other-user", code=1))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.run_call(), completed)

    def test_queued_retry_also_binds_owner_and_input(self):
        self.run_call(crash="queued")
        before = self.path.read_bytes()
        self.run_call(command={**self.command, "amount": 2}, code=1)
        self.assertIsNone(self.run_call(user="other-user", code=1))
        self.assertEqual(self.path.read_bytes(), before)
        self.assert_complete(self.run_call())

    def test_completed_cleanup_journal_cannot_rewind_later_state(self):
        self.run_call(crash="complete")
        old = self.journal.read_bytes()
        first = self.run_call()
        self.assert_complete(first)
        later = self.run_call(
            command={**self.command, "expected_version": 3}, call_id="heal-2"
        )
        self.assert_complete(later, version=4, hp=2)
        current = self.path.read_bytes()
        self.journal.write_bytes(old)
        self.assertEqual(self.run_call(), first)
        self.assertEqual(self.path.read_bytes(), current)
        self.assertFalse(self.journal.exists())

    def test_corrupt_transaction_never_changes_state(self):
        self.run_call(crash="journal")
        original = json.loads(self.journal.read_bytes())
        unchanged = self.path.read_bytes()
        changes = [
            ("before", "round", 4),
            ("before", "owner_user_id", "other-user"),
            ("after", "version", 9),
            ("call", "user_id", "other-user"),
            ("call", "input_json", encoded({**self.command, "amount": 2}).decode()),
            ("call", "output_json", "{}"),
            ("call", "output_sha256", "0" * 64),
            ("call", "created_at", 0),
        ]
        for scope, key, value in changes:
            with self.subTest(scope=scope, key=key):
                invalid = copy.deepcopy(original)
                invalid[scope][key] = value
                raw = encoded(invalid)
                self.journal.write_bytes(raw)
                self.assertIsNone(self.run_call(code=1))
                self.assertEqual(self.journal.read_bytes(), raw)
                self.assertEqual(self.path.read_bytes(), unchanged)
        raw = encoded(original)
        variants = [
            raw[:-1],
            raw + b"{}",
            raw + b"\0",
            b" " * 1048576 + raw,
            raw.replace(b'"schema":', b'"schema":"other","schema":', 1),
        ]
        for invalid in variants:
            self.journal.write_bytes(invalid)
            self.assertIsNone(self.run_call(code=1))
            self.assertEqual(self.path.read_bytes(), unchanged)
        self.journal.write_bytes(raw)
        self.assert_complete(self.run_call())

    def test_completed_receipt_requires_its_original_artifact(self):
        record = self.run_call()
        self.assert_complete(record)
        artifact = self.root / "artifacts" / (record["output_sha256"] + ".json")
        before = self.path.read_bytes()
        artifact.write_bytes(b"corrupt")
        self.assertIsNone(self.run_call(code=1))
        self.assertEqual(artifact.read_bytes(), b"corrupt")
        artifact.unlink()
        self.assertIsNone(self.run_call(code=1))
        self.assertFalse(artifact.exists())
        self.assertEqual(self.path.read_bytes(), before)

    def test_missing_or_changed_artifact_blocks_recovery(self):
        self.run_call(crash="journal")
        journal = json.loads(self.journal.read_bytes())
        artifact = (
            self.root / "artifacts" / (journal["call"]["output_sha256"] + ".json")
        )
        original = artifact.read_bytes()
        state_before = self.path.read_bytes()
        artifact.write_bytes(b"corrupt")
        self.assertIsNone(self.run_call(code=1))
        self.assertEqual(artifact.read_bytes(), b"corrupt")
        artifact.unlink()
        self.assertIsNone(self.run_call(code=1))
        self.assertFalse(artifact.exists())
        self.assertEqual(self.path.read_bytes(), state_before)
        artifact.write_bytes(original)
        artifact.chmod(0o600)
        self.assert_complete(self.run_call())

    def test_conflicting_current_state_is_not_overwritten(self):
        self.run_call(crash="journal")
        original = self.path.read_bytes()
        changed = encoded(state(version=4))
        self.path.write_bytes(changed)
        self.assertIsNone(self.run_call(code=1))
        self.assertEqual(self.path.read_bytes(), changed)
        self.path.write_bytes(original)
        self.assert_complete(self.run_call())

    def test_get_transaction_does_not_rewrite_legacy_state(self):
        before = json.dumps(state(), indent=2).encode() + b"\n"
        self.path.write_bytes(before)
        command = {"operation": "get", "campaign_id": "table", "encounter_id": "battle"}
        self.run_call(crash="journal", command=command, call_id="read-1")
        record = self.run_call(command=command, call_id="read-1")
        self.assert_complete(record, version=2, hp=0)
        self.assertEqual(self.path.read_bytes(), before)

    def test_start_transaction_recovers_missing_state(self):
        self.path.unlink()
        command = {
            "operation": "start",
            "campaign_id": "table",
            "encounter_id": "battle",
            "expected_version": 0,
        }
        self.run_call(crash="journal", command=command, call_id="start-1")
        self.assertFalse(self.path.exists())
        record = self.run_call(command=command, call_id="start-1")
        self.assertEqual(record["state"], "completed")
        self.assertEqual(json.loads(self.path.read_bytes())["version"], 1)
        self.assertEqual(self.run_call(command=command, call_id="start-1"), record)
        self.assertFalse(self.journal.exists())


if __name__ == "__main__":
    unittest.main()
