"""Scene evidence checks against production C campaign adapters on any host."""

import copy
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_campaign import character, encoded, state

BINARY = Path(
    os.environ.get(
        "CAMPAIGN_CONTRACT_BIN", Path(__file__).with_name("c-ptools-campaign-contract")
    )
).resolve()


def observation(**changes):
    return {
        "character_id": "npc",
        "presence": "present",
        "visibility": "table",
        "source_turn_id": "mira-enters-hall",
        **changes,
    }


def write_command(entries=None, **changes):
    return {
        "operation": "set_scene_observations",
        "campaign_id": "table",
        "expected_version": 2,
        "scene_observations": {
            "scene_id": "hall",
            "entries": [observation()] if entries is None else entries,
        },
        **changes,
    }


def read_command(**changes):
    return {
        "operation": "get_scene_presence",
        "campaign_id": "table",
        "expected_version": 3,
        "scene_id": "hall",
        "character_id": "npc",
        **changes,
    }


def resolve_command(**changes):
    return {
        "operation": "resolve_scene_presence",
        "campaign_id": "table",
        "scene_id": "hall",
        "character_name": "Mirt",
        **changes,
    }


class SceneObservations(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="scene-contract-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.calls = 0

    def execute(self, before, command, *, user="user", good=True, tool=None):
        self.calls += 1
        path = self.root / "before.json"
        if before is not None:
            path.write_bytes(before if isinstance(before, bytes) else encoded(before))
        response = subprocess.run(
            [
                str(BINARY),
                str(path) if before is not None else "-",
                user,
                f"scene-{self.calls}",
                *([tool] if tool else []),
            ],
            input=command if isinstance(command, bytes) else encoded(command),
            capture_output=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(response.stderr, b"")
        if not good:
            self.assertNotEqual(response.returncode, 0)
            self.assertEqual(response.stdout, b"")
            return None
        self.assertEqual(response.returncode, 0, response.stderr)
        result = json.loads(response.stdout)
        self.assertEqual(result["output"]["operation_id"], f"scene-{self.calls}")
        return result

    def installed(self, entries=None):
        return self.execute(state(), write_command(entries))["state"]

    def assert_presence(self, before, presence, *, source="", **query):
        result = self.execute(before, read_command(**query))
        self.assertTrue(result["read_only"])
        self.assertEqual(result["state"], before)
        self.assertEqual(
            result["output"],
            {
                "operation": "get_scene_presence",
                "operation_id": f"scene-{self.calls}",
                "campaign_id": "table",
                "version": before["version"],
                "status": before["status"],
                "scene_id": query.get("scene_id", "hall"),
                "character_id": query.get("character_id", "npc"),
                "presence": presence,
                "source_turn_id": source,
            },
        )

    def test_present_absent_and_unknown_are_distinct(self):
        for presence in ("present", "absent", "unknown"):
            with self.subTest(presence=presence):
                before = self.installed([observation(presence=presence)])
                self.assert_presence(
                    before,
                    presence,
                    source="" if presence == "unknown" else "mira-enters-hall",
                )

    def test_write_is_versioned_and_preserves_existing_collections(self):
        original = state()
        result = self.execute(original, write_command())
        self.assertFalse(result["read_only"])
        expected = {
            **original,
            "version": 3,
            "scene_observations": {
                **write_command()["scene_observations"],
                "version": 3,
            },
        }
        self.assertEqual(result["state"], expected)
        self.assertEqual(
            result["output"]["scene_observations"], expected["scene_observations"]
        )
        self.assertNotIn("owner_user_id", result["output"])

    def test_hidden_missing_and_unknown_have_the_same_projection(self):
        # Neither an active speaking NPC nor prose in current_scene is evidence.
        candidates = [
            state(version=3),
            self.installed([]),
            self.installed(
                [observation(visibility="dm", source_turn_id="secret-passage")]
            ),
            self.installed(
                [observation(presence="unknown", source_turn_id="hidden-reference")]
            ),
        ]
        for before in candidates:
            self.assert_presence(before, "unknown")
        self.assert_presence(self.installed(), "unknown", scene_id="other-room")
        self.assert_presence(self.installed(), "unknown", character_id="other-person")

    def test_scene_change_invalidates_evidence_and_old_query_version(self):
        before = self.installed()
        changed = self.execute(
            before,
            {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": 3,
                "scene": "Another room",
            },
        )["state"]
        self.execute(changed, read_command(), good=False)
        self.assert_presence(changed, "unknown", expected_version=4)
        self.assertEqual(changed["scene_observations"], before["scene_observations"])
        refreshed = self.execute(
            changed, write_command([observation(presence="absent")], expected_version=4)
        )["state"]
        self.assert_presence(
            refreshed, "absent", expected_version=5, source="mira-enters-hall"
        )

    def test_metadata_and_roster_changes_do_not_refresh_observations(self):
        before = self.installed()
        for command in (
            {
                "operation": "update",
                "campaign_id": "table",
                "expected_version": 3,
                "campaign": {"name": "Changed", "ruleset": "5e"},
            },
            {
                "operation": "add_character",
                "campaign_id": "table",
                "expected_version": 3,
                "character": character(id="new"),
            },
        ):
            changed = self.execute(before, command)
            self.assertEqual(
                changed["state"]["scene_observations"], before["scene_observations"]
            )
            self.assertNotIn("scene_observations", changed["output"])
            self.assert_presence(changed["state"], "unknown", expected_version=4)
        removable = copy.deepcopy(before)
        removable["scene_director"] = {
            "active_npc_ids": [],
            "next_speaker_index": 0,
            "active_speaker_id": "",
            "active_turn_id": "",
            "lease_expires_unix_ms": 0,
        }
        changed = self.execute(
            removable,
            {
                "operation": "remove_character",
                "campaign_id": "table",
                "expected_version": 3,
                "character_id": "npc",
            },
        )["state"]
        self.assert_presence(changed, "unknown", expected_version=4)

    def test_existing_read_shapes_stay_stable_and_do_not_expose_observations(self):
        for operation in ("get", "get_roster"):
            before = self.installed([observation(visibility="dm")])
            result = self.execute(
                before, {"operation": operation, "campaign_id": "table"}
            )
            self.assertEqual(result["state"], before)
            self.assertNotIn("scene_observations", result["output"])
            self.assertNotIn(
                "source_turn_id",
                encoded(result["output"]).decode() if operation == "get_roster" else "",
            )

    def test_owner_version_and_archival_boundaries(self):
        before = self.installed()
        self.execute(before, read_command(), user="other", good=False)
        self.execute(state(), write_command(), user="other", good=False)
        for version in (0, 1, 3, 9223372036854775807):
            self.execute(state(), write_command(expected_version=version), good=False)
        for version in (0, 2, 4):
            self.execute(before, read_command(expected_version=version), good=False)
        self.assert_presence({**before, "status": "archived"}, "unknown")
        self.execute(state(status="archived"), write_command(), good=False)
        self.execute(
            state(version=9223372036854775807),
            write_command(expected_version=9223372036854775807),
            good=False,
        )

    def test_snapshot_replace_clear_and_maximum(self):
        characters = [character(id=f"npc-{i}") for i in range(20)]
        before = state(characters=[character(), *characters])
        entries = [observation(character_id=c["id"]) for c in characters]
        full = self.execute(before, write_command(entries))["state"]
        self.assertEqual(len(full["scene_observations"]["entries"]), 20)
        self.assert_presence(
            full, "present", character_id="npc-19", source="mira-enters-hall"
        )
        cleared = self.execute(full, write_command([], expected_version=3))["state"]
        self.assert_presence(
            cleared, "unknown", character_id="npc-19", expected_version=4
        )
        self.execute(before, write_command([*entries, observation()]), good=False)
        self.execute(
            state(),
            write_command([observation(character_id="not-in-roster")]),
            good=False,
        )

    def test_strict_commands_entries_and_duplicate_keys(self):
        commands = []
        for key, values in {
            "scene_id": (None, "", "../hall", "x" * 128),
            "entries": (None, {}, "present"),
        }.items():
            for value in values:
                command = write_command()
                command["scene_observations"][key] = value
                commands.append(command)
        for key in read_command():
            value = read_command()
            del value[key]
            commands.append(value)
        commands.extend(
            [
                read_command(scene_id="../hall"),
                read_command(character_id="npc/other"),
                read_command(untrusted="x"),
            ]
        )
        for key in ("version", "unknown"):
            command = write_command()
            command["scene_observations"][key] = 3
            commands.append(command)
        for key, values in {
            "presence": ("here", "PRESENT", True, None, "present\x00hidden"),
            "visibility": ("public", "DM", False, None),
            "source_turn_id": ("", "../secret", "x" * 65, "x" * 128, None),
            "character_id": ("", "x" * 65, "x" * 128, None),
        }.items():
            commands.extend(
                write_command([observation(**{key: value})]) for value in values
            )
        for key in observation():
            entry = observation()
            del entry[key]
            commands.append(write_command([entry]))
        commands.extend(
            [
                write_command([observation(), observation()]),
                write_command([observation(radius=20)]),
            ]
        )
        raw = encoded(write_command())
        commands.extend(
            [
                raw.replace(
                    b'"presence":"present"', b'"presence":"present","presence":"absent"'
                ),
                raw.replace(b'"visibility"', b'"visibilit\\u0079"'),
                raw[:-1],
                raw + b"garbage",
                raw + b"\x00",
                b"[]",
            ]
        )
        for command in commands:
            with self.subTest(command=command):
                self.execute(state(), command, good=False)

    def test_corrupt_retained_evidence_fails_closed(self):
        before = self.installed()
        invalid = []
        for version in (0, -1, 4, 1.5, "3"):
            changed = copy.deepcopy(before)
            changed["scene_observations"]["version"] = version
            invalid.append(changed)
        for key in ("version", "scene_id", "entries"):
            changed = copy.deepcopy(before)
            del changed["scene_observations"][key]
            invalid.append(changed)
        changed = copy.deepcopy(before)
        changed["scene_observations"]["entries"][0]["character_id"] = "not-in-roster"
        invalid.append(changed)
        invalid.append({**before, "scene_observations": None})
        for changed in invalid:
            self.execute(changed, read_command(), good=False)
            self.execute(
                changed, {"operation": "get_roster", "campaign_id": "table"}, good=False
            )

    def test_name_resolution_reads_one_current_owned_version(self):
        before = self.installed()
        for name in ("Mirt", "mirt", "MIRT"):
            result = self.execute(
                before, resolve_command(character_name=name), tool="scene"
            )
            self.assertTrue(result["read_only"])
            self.assertEqual(result["state"], before)
            self.assertEqual(
                result["output"],
                {
                    "operation": "resolve_scene_presence",
                    "operation_id": f"scene-{self.calls}",
                    "campaign_id": "table",
                    "version": 3,
                    "status": "active",
                    "scene_id": "hall",
                    "character_name": name,
                    "presence": "present",
                    "source_turn_id": "mira-enters-hall",
                },
            )
        self.execute(
            before, resolve_command(), tool="scene", user="intruder", good=False
        )

    def test_name_resolution_ambiguity_and_ineligible_evidence(self):
        before = self.installed()
        duplicate = copy.deepcopy(before)
        duplicate["characters"].append(character(id="duplicate", name="mirt"))
        for candidate in (
            duplicate,
            {**before, "version": 4},
            self.installed([observation(visibility="dm")]),
            self.installed([]),
            {**before, "status": "archived"},
        ):
            result = self.execute(candidate, resolve_command(), tool="scene")
            self.assertEqual(result["state"], candidate)
            self.assertEqual(result["output"]["presence"], "unknown")
            self.assertEqual(result["output"]["source_turn_id"], "")
        for query in (
            resolve_command(scene_id="other"),
            resolve_command(character_name="Mira"),
        ):
            result = self.execute(before, query, tool="scene")
            self.assertEqual(result["output"]["presence"], "unknown")

    def test_scene_tool_cannot_mutate_or_read_private_campaign(self):
        for command in (
            write_command(),
            read_command(),
            {"operation": "get", "campaign_id": "table"},
            {"operation": "get_roster", "campaign_id": "table"},
            {
                "operation": "set_scene",
                "campaign_id": "table",
                "expected_version": 3,
                "scene": "Elsewhere",
            },
            resolve_command(expected_version=3),
            resolve_command(character_id="npc"),
        ):
            self.execute(self.installed(), command, tool="scene", good=False)
        for name in ("", " Mirt", "Mirt ", "Mir\nt", "M" * 201, None):
            self.execute(
                self.installed(),
                resolve_command(character_name=name),
                tool="scene",
                good=False,
            )

    def test_name_resolution_preserves_utf8_without_guessing_aliases(self):
        before = self.installed()
        before["characters"][0]["name"] = "Míra of Waterdeep"
        for name, expected in (
            ("Míra of Waterdeep", "present"),
            ("MÍra of Waterdeep", "unknown"),
            ("Mira", "unknown"),
        ):
            result = self.execute(
                before, resolve_command(character_name=name), tool="scene"
            )
            self.assertEqual(result["output"]["presence"], expected)


if __name__ == "__main__":
    unittest.main()
