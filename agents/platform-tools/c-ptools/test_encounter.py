"""Independent JSON and filesystem oracle for the compiled encounter host."""

import copy
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

BINARY = Path(__file__).resolve().parent / "c-ptools-encounter-fixture"


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def install_campaign(root, user="user"):
    path = root / "campaigns/table.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        encoded(
            {
                "campaign_id": "table",
                "owner_user_id": user,
                "version": 1,
                "status": "active",
                "campaign": {
                    "name": "Table",
                    "ruleset": "5e",
                    "description": "",
                    "current_scene": "",
                    "house_rules": {},
                },
                "characters": [],
                "session_recaps": [],
                "scene_director": {"active_npc_ids": []},
            }
        )
    )


def participant(**changes):
    value = {
        "id": "rogue",
        "name": "Rogue",
        "initiative": 12,
        "max_hp": 20,
        "current_hp": 0,
        "conditions": [],
    }
    value.update(changes)
    return value


def state(**changes):
    value = {
        "campaign_id": "table",
        "encounter_id": "battle",
        "owner_user_id": "user",
        "version": 2,
        "status": "active",
        "round": 1,
        "active_index": 0,
        "participants": [participant()],
    }
    value.update(changes)
    return value


class EncounterTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="c-encounter-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        install_campaign(self.root)
        self.path = self.root / "encounters/table/battle.json"

    def invoke(self, command, *, user="user", failure=False, fail_sync=None, root=None):
        raw = command if isinstance(command, bytes) else encoded(command)
        env = os.environ.copy()
        env.pop("PT_TEST_FAIL_FSYNC", None)
        if fail_sync is not None:
            env["PT_TEST_FAIL_FSYNC"] = str(fail_sync)
        result = subprocess.run(
            [str(BINARY), str(root or self.root), user],
            input=raw,
            capture_output=True,
            timeout=10,
            env=env,
            check=False,
        )
        self.assertEqual(result.returncode, 1 if failure else 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        if failure:
            self.assertEqual(result.stdout, b"")
            return None
        return json.loads(result.stdout)

    @staticmethod
    def command(operation, **fields):
        return {
            "operation": operation,
            "campaign_id": "table",
            "encounter_id": "battle",
            **fields,
        }

    def write_state(self, value):
        raw = value if isinstance(value, bytes) else encoded(value)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_bytes(raw)
        return raw

    def rejects_without_write(self, value, **kwargs):
        before = self.path.read_bytes() if self.path.exists() else None
        self.invoke(value, failure=True, **kwargs)
        self.assertEqual(self.path.read_bytes() if self.path.exists() else None, before)

    def test_previous_state_is_opt_in_and_preserves_legacy_output(self):
        for operation, fields in [
            ("advance", {}),
            ("damage", {"participant_id": "rogue", "amount": 7}),
            ("heal", {"participant_id": "rogue", "amount": 7}),
            ("condition_add", {"participant_id": "rogue", "condition": "blinded"}),
            ("condition_remove", {"participant_id": "rogue", "condition": "prone"}),
            ("end", {}),
        ]:
            before = state(
                participants=[participant(current_hp=10, conditions=["prone"])]
            )
            self.write_state(before)
            command = self.command(operation, expected_version=2, **fields)
            legacy = self.invoke(command)
            self.assertNotIn("previous_state", legacy)
            self.write_state(before)
            current = self.invoke({**command, "include_previous": True})
            self.assertEqual(
                current.pop("previous_state"),
                {key: value for key, value in before.items() if key != "owner_user_id"},
            )
            # The process fixture derives its tool call ID from its own PID.
            self.assertTrue(current.pop("operation_id").startswith("encounter-test-"))
            self.assertTrue(legacy.pop("operation_id").startswith("encounter-test-"))
            self.assertEqual(current, legacy)
            for invalid in [False, 1, "true", None]:
                self.rejects_without_write(
                    {**command, "expected_version": 3, "include_previous": invalid}
                )
        self.rejects_without_write(self.command("get", include_previous=True))
        self.rejects_without_write(
            self.command(
                "heal",
                expected_version=3,
                participant_id="rogue",
                amount=1,
                include_previous=True,
            )
        )

    def test_previous_state_capacity_failure_does_not_save_partial_output(self):
        conditions = [
            "blinded",
            "charmed",
            "deafened",
            "frightened",
            "grappled",
            "incapacitated",
            "invisible",
            "paralyzed",
            "petrified",
            "poisoned",
            "prone",
            "restrained",
            "stunned",
            "unconscious",
        ]
        before = state(
            participants=[
                participant(
                    id=f"pc-{i:03}",
                    name="a" * 120,
                    current_hp=10,
                    conditions=conditions,
                )
                for i in range(96)
            ]
        )
        self.write_state(before)
        command = self.command(
            "damage", expected_version=2, participant_id="pc-000", amount=1
        )
        self.invoke(command)
        self.write_state(before)
        self.rejects_without_write({**command, "include_previous": True})

    def test_full_lifecycle_and_restart(self):
        value = self.invoke(self.command("START", expected_version=0))
        self.assertEqual(
            (value["version"], value["round"], value["active_index"]), (1, 1, -1)
        )
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        name = 'Rogue "Mira" \\ {elf}\nÉowyn'
        value = self.invoke(
            self.command("add", expected_version=1, participant=participant(name=name))
        )
        self.assertEqual(value["participants"][0], participant(name=name))
        self.assertEqual(
            self.invoke(self.command("get"))["participants"], value["participants"]
        )
        value = self.invoke(self.command("advance", expected_version=2))
        self.assertEqual(value["active_participant_id"], "rogue")
        newcomer = participant(id="mage", name="Mage", initiative=99)
        del newcomer["current_hp"]
        value = self.invoke(
            self.command("add", expected_version=3, participant=newcomer)
        )
        self.assertEqual(value["active_participant_id"], "rogue")
        self.assertEqual(value["participants"][0]["current_hp"], 20)
        value = self.invoke(
            self.command(
                "heal", expected_version=4, participant_id="rogue", amount=2147483647
            )
        )
        self.assertEqual(value["participants"][1]["current_hp"], 20)
        value = self.invoke(
            self.command(
                "damage",
                expected_version=5,
                participant_id="rogue",
                amount=2147483647,
                damage_type="fire",
            )
        )
        self.assertEqual(value["participants"][1]["current_hp"], 0)
        value = self.invoke(
            self.command(
                "condition_add",
                expected_version=6,
                participant_id="rogue",
                condition="prone",
            )
        )
        self.assertEqual(value["participants"][1]["conditions"], ["prone"])
        value = self.invoke(
            self.command(
                "condition_remove",
                expected_version=7,
                participant_id="rogue",
                condition="prone",
            )
        )
        self.assertEqual(value["participants"][1]["conditions"], [])
        value = self.invoke(
            self.command("remove", expected_version=8, participant_id="mage")
        )
        self.assertEqual(
            (value["active_index"], value["active_participant_id"]), (0, "rogue")
        )
        value = self.invoke(self.command("advance", expected_version=9))
        self.assertEqual((value["round"], value["version"]), (2, 10))
        value = self.invoke(self.command("end", expected_version=10))
        self.assertEqual(
            (value["status"], value["active_index"], value["version"]),
            ("ended", -1, 11),
        )
        self.rejects_without_write(self.command("advance", expected_version=11))
        self.assertEqual(self.invoke(self.command("get"))["version"], 11)

    def test_rejects_ambiguous_command_json(self):
        valid = encoded(self.command("start", expected_version=0))
        cases = [
            b'{"nested":' + valid + b"}",
            b'{"operation":"get",' + valid[1:],
            valid[:-1] + b',"operation":"get"}',
            valid.replace(b'"operation"', b'"\\u006fperation"'),
            valid + b"{}",
            valid[:-1],
            valid[:-1] + b",}",
            valid.replace(b'"start"', b"null"),
            valid.replace(b":0}", b":0.0}"),
            valid.replace(b":0}", b":0e0}"),
            valid.replace(b":0}", b":9223372036854775808}"),
            valid.replace(b'"table"', b'"../table"'),
            valid.replace(b'"table"', b'"table\\u0000other"'),
            valid.replace(b'"table"', b'"\xff"'),
        ]
        for raw in cases:
            with self.subTest(raw=raw):
                self.rejects_without_write(raw)
        self.assertFalse(self.path.exists())

    def test_each_operation_requires_its_fields(self):
        self.write_state(state())
        commands = [
            self.command("start", expected_version=0),
            self.command("get"),
            self.command("add", expected_version=2, participant=participant(id="mage")),
            self.command("remove", expected_version=2, participant_id="rogue"),
            self.command("advance", expected_version=2),
            self.command("end", expected_version=2),
            self.command(
                "damage", expected_version=2, participant_id="rogue", amount=1
            ),
            self.command("heal", expected_version=2, participant_id="rogue", amount=1),
            self.command(
                "condition_add",
                expected_version=2,
                participant_id="rogue",
                condition="prone",
            ),
            self.command(
                "condition_remove",
                expected_version=2,
                participant_id="rogue",
                condition="prone",
            ),
        ]
        for command in commands:
            for key in command:
                with self.subTest(operation=command["operation"], missing=key):
                    changed = dict(command)
                    del changed[key]
                    self.rejects_without_write(changed)
            self.rejects_without_write({**command, "unused": "value"})
        self.rejects_without_write(self.command("get", expected_version=2))
        self.rejects_without_write(
            self.command(
                "heal",
                expected_version=2,
                participant_id="rogue",
                amount=1,
                damage_type="fire",
            )
        )

    def test_participant_bounds_and_types(self):
        self.write_state(state())
        invalid = []
        for key in ["id", "name", "initiative", "max_hp"]:
            p = participant(id="mage")
            del p[key]
            invalid.append(p)
        for key, values in {
            "id": ["", "x" * 65, "../mage", None],
            "name": ["", "x" * 121, " name", "name ", None],
            "initiative": [-101, 101, 4294967308, -4294967284, 12.5, True, "12", None],
            "max_hp": [-1, 100001, 4294967316, None],
            "current_hp": [-1, 21, 4294967296, 0.5, True, None],
            "conditions": [
                ["prone", "prone"],
                ["stunned", "prone"],
                ["unknown"],
                [None],
                {},
                None,
            ],
        }.items():
            invalid.extend(
                participant(id="mage", **{key: value})
                if key != "id"
                else participant(id=value)
                for value in values
            )
        invalid.append({**participant(id="mage"), "nested": {"initiative": 1}})
        for p in invalid:
            with self.subTest(participant=p):
                self.rejects_without_write(
                    self.command("add", expected_version=2, participant=p)
                )
        duplicate = encoded(
            self.command("add", expected_version=2, participant=participant(id="mage"))
        )
        self.rejects_without_write(
            duplicate.replace(b'"initiative":12', b'"initiative":12,"initiative":13')
        )

    def test_all_mutation_guards_preserve_state(self):
        self.write_state(state())
        self.rejects_without_write(self.command("get"), user="other-user")
        self.rejects_without_write(self.command("get"), user="x" * 128)
        self.rejects_without_write(self.command("start", expected_version=0))
        self.rejects_without_write(
            self.command("heal", expected_version=1, participant_id="rogue", amount=1)
        )
        for amount in [0, -1, 2147483648, 4294967297, 1.1, True, "1", None]:
            self.rejects_without_write(
                self.command(
                    "heal", expected_version=2, participant_id="rogue", amount=amount
                )
            )
        self.rejects_without_write(
            self.command(
                "damage",
                expected_version=2,
                participant_id="rogue",
                amount=1,
                damage_type="unknown",
            )
        )
        self.rejects_without_write(
            self.command("remove", expected_version=2, participant_id="absent")
        )

    def test_retained_state_requires_every_field(self):
        valid = state()
        variants = []
        for key in valid:
            v = copy.deepcopy(valid)
            del v[key]
            variants.append(v)
        for key in valid["participants"][0]:
            v = copy.deepcopy(valid)
            del v["participants"][0][key]
            variants.append(v)
        variants.extend(
            [
                state(campaign_id="other"),
                state(encounter_id="other"),
                state(version=0),
                state(version=9223372036854775808),
                state(round=2147483648),
                state(active_index=1),
                state(status="ended"),
                state(participants=[]),
                state(participants=[participant(), participant()]),
                state(
                    participants=[
                        participant(initiative=1),
                        participant(id="mage", initiative=2),
                    ]
                ),
                state(participants=[participant(conditions=["prone", "prone"])]),
                {**valid, "unknown": {}},
            ]
        )
        raw = encoded(valid)
        variants.extend(
            [
                raw[:-1],
                raw + b"{}",
                raw + b"\0",
                raw.replace(b'"round":1', b'"round":1,"round":2'),
                raw.replace(b'"id":"rogue"', b'"id":"rogue","id":"mage"'),
                raw.replace(
                    b'"owner_user_id":"user"', b'"owner_user_id":"user\\u0000"'
                ),
            ]
        )
        for value in variants:
            with self.subTest(state=value):
                self.write_state(value)
                self.rejects_without_write(self.command("get"))

    def test_serialization_preserves_unicode_and_escaped_strings(self):
        value = state(
            owner_user_id='issuer:"subject\\id',
            participants=[participant(name='Náme {x} "quoted" \\ line\nnext')],
        )
        result = subprocess.run(
            [str(BINARY), "roundtrip"],
            input=encoded(value),
            capture_output=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), value)
        self.write_state(value)
        install_campaign(self.root, value["owner_user_id"])
        output = self.invoke(self.command("get"), user=value["owner_user_id"])
        self.assertEqual(output["participants"], value["participants"])
        self.assertNotIn("owner_user_id", output)

    def test_counter_exhaustion(self):
        self.write_state(state(version=9223372036854775807))
        self.assertEqual(
            self.invoke(self.command("get"))["version"], 9223372036854775807
        )
        self.rejects_without_write(
            self.command("end", expected_version=9223372036854775807)
        )
        self.write_state(state(round=2147483647))
        self.rejects_without_write(self.command("advance", expected_version=2))
        value = self.invoke(
            self.command("heal", expected_version=2, participant_id="rogue", amount=1)
        )
        self.assertEqual(value["round"], 2147483647)

    def test_invalid_files_and_paths(self):
        self.path.parent.mkdir(parents=True)
        self.path.symlink_to(self.root / "missing")
        self.invoke(self.command("start", expected_version=0), failure=True)
        self.invoke(self.command("get"), failure=True)
        self.assertTrue(self.path.is_symlink())
        self.path.unlink()
        os.mkfifo(self.path)
        self.invoke(self.command("get"), failure=True)
        self.path.unlink()
        self.write_state(state())
        os.link(self.path, self.root / "second-link")
        self.rejects_without_write(self.command("get"))
        self.invoke(
            self.command("start", expected_version=0), failure=True, root="x" * 500
        )

    def test_full_output_must_fit_before_mutation(self):
        value = state(
            participants=[
                participant(id=f"p{i:03}", name="N" * 120) for i in range(256)
            ]
        )
        remaining = 65480 - len(encoded(value))
        self.assertGreater(remaining, 0)
        for p in value["participants"]:
            extra = min(120, remaining)
            p["name"] = '"' * extra + "N" * (120 - extra)
            remaining -= extra
        self.assertEqual(remaining, 0)
        raw = self.write_state(value)
        self.assertEqual(len(raw), 65480)
        self.rejects_without_write(
            self.command("heal", expected_version=2, participant_id="p000", amount=1)
        )

    def test_sync_failures_do_not_report_success(self):
        self.write_state(state())
        command = self.command(
            "heal", expected_version=2, participant_id="rogue", amount=1
        )
        self.rejects_without_write(command, fail_sync=1)
        self.assertEqual(list(self.path.parent.glob(".*.tmp-*")), [])
        self.invoke(command, failure=True, fail_sync=2)
        # Rename already happened. Failure of directory fsync is an uncertain
        # durability result, not a successful rollback or a successful call.
        after = json.loads(self.path.read_bytes())
        self.assertEqual(
            (after["version"], after["participants"][0]["current_hp"]), (3, 1)
        )
        self.assertEqual(self.invoke(self.command("get"))["version"], 3)


if __name__ == "__main__":
    unittest.main()
