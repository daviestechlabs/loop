"""Independent JSON and file oracles for the native C campaign host."""

import copy
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

BINARY = Path(__file__).resolve().with_name("c-ptools-campaign-fixture")
SCHEMAS = (
    Path(__file__).resolve().parents[2]
    / "interaction-control-plane/examples/schemas/tools"
)
INPUT_SCHEMA = json.loads(
    (SCHEMAS / "dnd-campaign-state.input.schema.json").read_text()
)
OUTPUT_SCHEMA = json.loads(
    (SCHEMAS / "dnd-campaign-state.output.schema.json").read_text()
)


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def metadata(**changes):
    return {
        "name": 'Laeral\'s "table" 🐉',
        "ruleset": "5e",
        "description": "Road\\river\nKeep",
        "current_scene": "Tavern",
        "house_rules": {"Potion": "Bonus action"},
        **changes,
    }


def character(**changes):
    return {
        "id": "npc",
        "name": "Mirt",
        "kind": "npc",
        "species": "human",
        "class": "rogue",
        "level": 5,
        "armor_class": 15,
        "max_hp": 30,
        "ability_scores": dict.fromkeys(
            [
                "strength",
                "dexterity",
                "constitution",
                "intelligence",
                "wisdom",
                "charisma",
            ],
            10,
        ),
        "sheet_refs": {"local": "sha256:synthetic"},
        "persona": "Guide",
        "voice_id": "mirt",
        "speaking_style": "Measured",
        "pronunciation": "MEERT",
        "allowed_knowledge_scopes": ["campaign_canon", "owned_rulebook"],
        "safety_rules": ["x" * 500, "Keep table boundaries"],
        **changes,
    }


def state(**changes):
    return {
        "campaign_id": "table",
        "owner_user_id": "user",
        "version": 2,
        "status": "active",
        "campaign": metadata(),
        "characters": [character()],
        "session_recaps": [
            {
                "session_id": "session-1",
                "source_turn_id": "turn-1",
                "summary": "🐉" * 1200,
                "created_at_unix_ms": 10,
            }
        ],
        "scene_director": {
            "active_npc_ids": ["npc"],
            "next_speaker_index": 0,
            "active_speaker_id": "npc",
            "active_turn_id": "turn-2",
            "lease_expires_unix_ms": 20,
        },
        **changes,
    }


class CampaignTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="c-campaign-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "campaigns/table.json"
        self.call_count = 0

    def install(self, value=None):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_bytes(encoded(state() if value is None else value))
        return self.path.read_bytes()

    def run_call(self, command, *, user="user", good=True, fail_sync=None, root=None):
        env = os.environ.copy()
        env.pop("PT_TEST_FAIL_FSYNC", None)
        if fail_sync:
            env["PT_TEST_FAIL_FSYNC"] = fail_sync
        self.call_count += 1
        call_id = f"campaign-test-{self.call_count}"
        response = subprocess.run(
            [str(BINARY), str(root or self.root), user, call_id],
            input=command if isinstance(command, bytes) else encoded(command),
            capture_output=True,
            check=False,
            timeout=15,
            env=env,
        )
        self.assertEqual(response.stderr, b"")
        if not good:
            self.assertNotEqual(response.returncode, 0)
            self.assertEqual(response.stdout, b"")
            return None
        self.assertEqual(response.returncode, 0, response.stderr)
        result = json.loads(response.stdout)
        self.assertEqual(
            set(result),
            set(OUTPUT_SCHEMA["required"])
            | set(OUTPUT_SCHEMA["allOf"][0]["else"]["else"]["required"]),
        )
        self.assertEqual(
            set(result["campaign"]), set(INPUT_SCHEMA["$defs"]["campaign"]["required"])
        )
        self.assertEqual(
            set(result["scene_director"]),
            set(INPUT_SCHEMA["$defs"]["scene_director"]["required"]),
        )
        self.assertEqual(result["operation_id"], call_id)
        return result

    def get(self, **options):
        return self.run_call({"operation": "get", "campaign_id": "table"}, **options)

    def mutation(self, **changes):
        return {
            "operation": "set_scene",
            "campaign_id": "table",
            "expected_version": 2,
            "scene": "New scene",
            **changes,
        }

    def test_create_get_and_full_metadata_limits(self):
        campaign = metadata(
            name="x" * 200,
            ruleset="r" * 120,
            description="d" * 4000,
            current_scene="s" * 500,
            house_rules={"a": "b"},
        )
        result = self.run_call(
            {
                "operation": "create",
                "campaign_id": "table",
                "expected_version": 0,
                "campaign": campaign,
            }
        )
        self.assertEqual(result["campaign"], campaign)
        self.assertEqual(result["version"], 1)
        self.assertEqual(result["characters"], [])
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        before = self.path.read_bytes()
        self.assertEqual(self.get()["campaign"], campaign)
        self.assertEqual(self.path.read_bytes(), before)
        self.run_call(
            {
                "operation": "create",
                "campaign_id": "table",
                "expected_version": 0,
                "campaign": campaign,
            },
            good=False,
        )
        self.assertEqual(self.path.read_bytes(), before)

    def test_read_and_mutation_preserve_all_rich_state(self):
        original = state()
        before = self.install(original)
        read = self.get()
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(
            {k: read[k] for k in original if k != "owner_user_id"},
            {k: v for k, v in original.items() if k != "owner_user_id"},
        )
        result = self.run_call(self.mutation(scene='Hall\n"Rope\\stairs" 🐉'))
        wanted = copy.deepcopy(original)
        wanted["version"] = 3
        wanted["campaign"]["current_scene"] = 'Hall\n"Rope\\stairs" 🐉'
        self.assertEqual(json.loads(self.path.read_bytes()), wanted)
        self.assertEqual(result["characters"], original["characters"])
        self.assertEqual(self.get()["session_recaps"], original["session_recaps"])

    def test_update_preserves_omitted_optional_metadata(self):
        original = state()
        self.install(original)
        update = {
            "operation": "update",
            "campaign_id": "table",
            "expected_version": 2,
            "campaign": {"name": "New name", "ruleset": "5e-2024"},
        }
        result = self.run_call(update)
        self.assertEqual(
            result["campaign"], {**original["campaign"], **update["campaign"]}
        )
        result = self.run_call(
            {
                **update,
                "expected_version": 3,
                "campaign": {
                    **update["campaign"],
                    "description": "",
                    "current_scene": "",
                    "house_rules": {},
                },
            }
        )
        self.assertEqual(result["campaign"]["house_rules"], {})
        self.assertEqual(result["campaign"]["description"], "")
        self.assertEqual(result["characters"], original["characters"])

    def test_complete_command_shapes(self):
        original = self.install()
        commands = [
            self.mutation(),
            {"operation": "get", "campaign_id": "table"},
            {
                "operation": "update",
                "campaign_id": "table",
                "expected_version": 2,
                "campaign": metadata(),
            },
            {
                "operation": "create",
                "campaign_id": "new",
                "expected_version": 0,
                "campaign": metadata(),
            },
        ]
        for command in commands:
            for field in command:
                with self.subTest(operation=command["operation"], missing=field):
                    self.run_call(
                        {k: v for k, v in command.items() if k != field}, good=False
                    )
            self.run_call({"wrapper": command}, good=False)
            self.run_call({**command, "unknown": True}, good=False)
        raw = encoded(self.mutation())
        for invalid in [
            raw[:-1],
            raw + b"{}",
            raw + b"\0",
            raw.replace(b'"scene":', b'"scene":"other","scene":'),
            raw.replace(b'"operation"', b'"oper\\u0061tion"'),
            raw.replace(b'"New scene"', b'"\\ud800"'),
            raw.replace(b'"New scene"', b'"\\u0000"'),
            raw.replace(b'"New scene"', b'"\xff"'),
        ]:
            self.run_call(invalid, good=False)
        self.assertEqual(self.path.read_bytes(), original)

    def test_metadata_types_bounds_and_no_silent_truncation(self):
        original = self.install()
        for key, value in [
            ("name", ""),
            ("name", "x" * 201),
            ("ruleset", ""),
            ("ruleset", "r" * 121),
            ("description", "d" * 4001),
            ("current_scene", "s" * 501),
            ("house_rules", []),
            ("house_rules", {"a": ""}),
            ("house_rules", {"a": "x" * 1001}),
            ("house_rules", {"a": 1}),
            ("description", None),
            ("name", {"name": "nested"}),
        ]:
            with self.subTest(field=key):
                self.run_call(
                    {
                        "operation": "update",
                        "campaign_id": "table",
                        "expected_version": 2,
                        "campaign": metadata(**{key: value}),
                    },
                    good=False,
                )
        self.assertEqual(self.path.read_bytes(), original)

    def test_house_rule_map_100_entries_and_decoded_duplicate_keys(self):
        self.install()
        rules = {f"rule-{i}": "value" for i in range(100)}
        update = {
            "operation": "update",
            "campaign_id": "table",
            "expected_version": 2,
            "campaign": metadata(house_rules=rules),
        }
        self.assertEqual(self.run_call(update)["campaign"]["house_rules"], rules)
        before = self.path.read_bytes()
        self.run_call(
            {
                **update,
                "expected_version": 3,
                "campaign": metadata(house_rules={**rules, "overflow": "x"}),
            },
            good=False,
        )
        update = {
            **update,
            "expected_version": 3,
            "campaign": metadata(house_rules={"rule": "value"}),
        }
        raw = encoded(update).replace(
            b'{"rule":"value"}', b'{"rule":"a","r\\u0075le":"b"}'
        )
        self.run_call(raw, good=False)
        self.assertEqual(self.path.read_bytes(), before)
        raw = encoded(update).replace(b'"rule":"value"', b'"r\\u0075le":"value"')
        self.assertEqual(
            self.run_call(raw)["campaign"]["house_rules"], {"rule": "value"}
        )

    def test_identity_versions_status_and_paths(self):
        before = self.install()
        for invalid in [-1, 0, 1, 3, 2.5, "2", True, 9223372036854775808]:
            self.run_call(self.mutation(expected_version=invalid), good=False)
        for invalid in ["", "../escape", "/absolute", "a/b", "..", "x" * 65]:
            self.run_call(self.mutation(campaign_id=invalid), good=False)
        for owner in ["other", "", " user", "user ", "user\n"]:
            self.get(user=owner, good=False)
        self.assertEqual(self.path.read_bytes(), before)
        self.install(state(version=9223372036854775807))
        self.get()
        self.run_call(self.mutation(expected_version=9223372036854775807), good=False)
        self.install(state(status="archived"))
        self.get()
        self.run_call(self.mutation(), good=False)
        self.install(state(campaign_id="other"))
        self.get(good=False)
        self.assertFalse((self.root / "escape.json").exists())

    def test_every_retained_field_is_required_and_unknown_fields_fail(self):
        original = state()
        scopes = [
            (),
            ("campaign",),
            ("characters", 0),
            ("characters", 0, "ability_scores"),
            ("session_recaps", 0),
            ("scene_director",),
        ]
        for scope in scopes:
            subject = original
            for part in scope:
                subject = subject[part]
            for key in [*subject, "unknown"]:
                invalid = copy.deepcopy(original)
                target = invalid
                for part in scope:
                    target = target[part]
                if key == "unknown":
                    target[key] = True
                else:
                    del target[key]
                with self.subTest(scope=scope, key=key):
                    before = self.install(invalid)
                    self.get(good=False)
                    self.run_call(self.mutation(), good=False)
                    self.assertEqual(self.path.read_bytes(), before)

    def test_retained_semantic_validation(self):
        invalid = [
            state(version=1.5),
            state(version=0),
            state(version="2"),
            state(status="unknown"),
            state(owner_user_id="user\n"),
            state(characters=[character(), character()]),
            state(characters=[character(level=21)]),
            state(characters=[character(kind="player")]),
            state(characters=[character(pronunciation=" leading")]),
            state(characters=[character(safety_rules=["a", "b", "a"])]),
            state(characters=[character(safety_rules=["x" * 501])]),
            state(characters=[character(sheet_refs={str(i): "x" for i in range(21)})]),
            state(characters=[character(voice_id="")]),
            state(
                characters=[
                    character(
                        allowed_knowledge_scopes=["owned_rulebook", "campaign_canon"]
                    )
                ]
            ),
        ]
        for changes in [
            {"active_npc_ids": ["missing"]},
            {"next_speaker_index": 1},
            {"active_speaker_id": "missing"},
            {"lease_expires_unix_ms": 0},
            {"active_turn_id": ""},
        ]:
            invalid.append(
                state(scene_director={**state()["scene_director"], **changes})
            )
        for changes in [
            {"summary": "x" * 1201},
            {"summary": ""},
            {"created_at_unix_ms": 0},
            {"created_at_unix_ms": 1.5},
        ]:
            invalid.append(
                state(session_recaps=[{**state()["session_recaps"][0], **changes}])
            )
        recap = state()["session_recaps"][0]
        invalid += [
            state(session_recaps=[recap, recap]),
            state(
                session_recaps=[
                    recap,
                    {**recap, "session_id": "later", "created_at_unix_ms": 9},
                ]
            ),
            state(
                session_recaps=[
                    {**recap, "session_id": f"s-{i}", "summary": "short"}
                    for i in range(25)
                ]
            ),
        ]
        for index, value in enumerate(invalid):
            with self.subTest(index=index):
                before = self.install(value)
                self.get(good=False)
                self.assertEqual(self.path.read_bytes(), before)

    def test_legacy_empty_director_and_codec_roundtrip(self):
        original = state(
            characters=[], session_recaps=[], scene_director={"active_npc_ids": []}
        )
        before = self.install(original)
        result = self.get()
        self.assertEqual(result["scene_director"]["active_npc_ids"], [])
        self.assertEqual(self.path.read_bytes(), before)
        for value in [original, state()]:
            response = subprocess.run(
                [str(BINARY), "roundtrip"],
                input=encoded(value),
                capture_output=True,
                timeout=15,
                check=False,
            )
            self.assertEqual(response.returncode, 0, response.stderr)
            decoded = json.loads(response.stdout)
            self.assertEqual(decoded["campaign"], value["campaign"])
            self.assertEqual(decoded["characters"], value["characters"])

    def test_unicode_maps_safety_and_escaped_owner(self):
        owner = 'user "quote" 🐉'
        original = state(
            owner_user_id=owner,
            characters=[character(safety_rules=["🐉" * 500])],
            campaign=metadata(house_rules={"🐉": "🐉" * 1000}),
        )
        self.install(original)
        self.assertEqual(self.get(user=owner)["campaign"], original["campaign"])
        self.run_call(self.mutation(), user=owner)
        after = json.loads(self.path.read_bytes())
        self.assertEqual(after["owner_user_id"], owner)
        self.assertEqual(after["characters"], original["characters"])
        self.assertEqual(
            after["campaign"]["house_rules"], original["campaign"]["house_rules"]
        )

    def test_invalid_retained_documents_and_file_types(self):
        original = self.install()
        invalid = [
            original[:-1],
            original + b"{}",
            original + b"\0",
            b" " * 65536 + original,
            original.replace(b'"version":2', b'"version":2,"version":2'),
            original.replace(b'"campaign_id"', b'"campaign\\u005fid"'),
        ]
        for raw in invalid:
            self.path.write_bytes(raw)
            self.get(good=False)
            self.assertEqual(self.path.read_bytes(), raw)
        self.path.unlink()
        target = self.root / "outside.json"
        target.write_bytes(original)
        for kind in ["symlink", "hardlink", "fifo", "directory"]:
            if kind == "symlink":
                self.path.symlink_to(target)
            elif kind == "hardlink":
                os.link(target, self.path)
            elif kind == "fifo":
                os.mkfifo(self.path)
            else:
                self.path.mkdir()
            self.get(good=False)
            self.run_call(self.mutation(), good=False)
            if kind == "directory":
                self.path.rmdir()
            else:
                self.path.unlink()
            self.assertEqual(target.read_bytes(), original)
        self.get(root="x" * 1100, good=False)

    def test_capacity_failure_precedes_mutation(self):
        original = state(
            characters=[],
            session_recaps=[],
            scene_director={"active_npc_ids": []},
            campaign=metadata(house_rules={str(i): "v" * 1000 for i in range(63)}),
        )
        before = self.install(original)
        self.assertLess(len(before), 65536)
        self.get()
        self.run_call(self.mutation(scene="\x01" * 500), good=False)
        self.assertEqual(self.path.read_bytes(), before)

    def test_add_character_preserves_other_records_and_refuses_replacement(self):
        before = state()
        self.install(before)
        added = character(id="new", name="New NPC")
        result = self.run_call(
            {
                "operation": "add_character",
                "campaign_id": "table",
                "expected_version": 2,
                "character": added,
            }
        )
        self.assertEqual(result["characters"], before["characters"] + [added])
        self.assertEqual(result["campaign"], before["campaign"])
        self.assertEqual(result["scene_director"], before["scene_director"])
        self.assertEqual(result["session_recaps"], before["session_recaps"])
        retained = self.path.read_bytes()
        self.run_call(
            {
                "operation": "add_character",
                "campaign_id": "table",
                "expected_version": 3,
                "character": added | {"name": "Overwrite"},
            },
            good=False,
        )
        self.assertEqual(self.path.read_bytes(), retained)

    def test_synchronization_failures_clear_response(self):
        original = self.install()
        self.run_call(self.mutation(), fail_sync="1", good=False)
        self.assertEqual(self.path.read_bytes(), original)
        self.run_call(self.mutation(), fail_sync="2", good=False)
        self.assertEqual(json.loads(self.path.read_bytes())["version"], 3)


if __name__ == "__main__":
    unittest.main()
