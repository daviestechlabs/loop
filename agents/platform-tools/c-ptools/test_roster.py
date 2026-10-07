"""Roster admission through native state transactions and signed C HTTP."""

import copy
import json
import unittest

import test_campaign as campaign
import test_state_authority as authority
import test_state_recovery as recovery


def upsert(character, version=2):
    return {
        "operation": "upsert_character",
        "campaign_id": "table",
        "expected_version": version,
        "character": character,
    }


def remove(character_id, version=2):
    return {
        "operation": "remove_character",
        "campaign_id": "table",
        "expected_version": version,
        "character_id": character_id,
    }


def player(**changes):
    return campaign.character(
        **{
            "id": "pc",
            "kind": "player",
            "player_user_id": "bob",
            "name": 'Rógué "Rope\\stairs" 🐉',
            "max_hp": 0,
            "safety_rules": ["🐉" * 500, "Respect the table"],
            **changes,
        }
    )


class RosterTest(unittest.TestCase):
    def fixture(self, kind):
        fixture = kind()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        return fixture

    def test_signed_roster_lifecycle_retries_and_player_identity(self):
        case = self.fixture(authority.StateHTTP)
        case.create()
        command = upsert(player(), 1)
        added = case.execute("campaign", command, call_id="roster-add")
        self.assertEqual(
            json.loads(added["call"]["output_json"])["characters"], [player()]
        )
        second = player(id="second", name="Laeral", player_user_id="alice")
        case.execute("campaign", upsert(second, 2), call_id="second")
        replacement = player(level=6, sheet_refs={"local": "updated"})
        changed = case.execute("campaign", upsert(replacement, 3), call_id="replace")
        self.assertEqual(
            json.loads(changed["call"]["output_json"])["characters"],
            [replacement, second],
        )
        path = case.root / "campaigns/table.json"
        before = path.read_bytes()
        # A player reference and knowledge scopes do not grant campaign authority.
        case.execute("campaign", remove("pc", 4), user="bob", status=409)
        case.execute(
            "campaign",
            {"operation": "get", "campaign_id": "table"},
            user="bob",
            status=409,
        )
        self.assertEqual(
            case.http.request("GET", "/v1/tools/calls/roster-add", user="bob")[0], 404
        )
        self.assertEqual(
            case.http.request(
                "POST", "/v1/tools/calls/roster-add/cancel", b"{}", user="bob"
            )[0],
            404,
        )
        self.assertEqual(case.execute("campaign", command, call_id="roster-add"), added)
        case.execute(
            "campaign", upsert(replacement, 1), call_id="roster-add", status=409
        )
        for key in ["session_id", "parent_turn_id", "idempotency_key"]:
            body = json.loads(case.body("campaign", command, "roster-add"))
            body[key] = "changed"
            self.assertEqual(
                case.http.request(
                    "POST", authority.http.EXECUTE, campaign.encoded(body)
                )[0],
                409,
            )
        self.assertEqual(path.read_bytes(), before)
        removed = case.execute("campaign", remove("pc", 4), call_id="remove")
        self.assertEqual(
            json.loads(removed["call"]["output_json"])["characters"], [second]
        )
        before = path.read_bytes()
        case.http.stop()
        case.http.start()
        self.assertEqual(case.execute("campaign", command, call_id="roster-add"), added)
        self.assertEqual(
            case.execute("campaign", remove("pc", 4), call_id="remove"), removed
        )
        self.assertEqual(
            case.http.request("POST", "/v1/tools/calls/remove/cancel", b"{}"),
            (200, removed),
        )
        self.assertEqual(path.read_bytes(), before)
        case.execute("campaign", remove("second", 5))
        self.assertEqual(json.loads(path.read_bytes())["characters"], [])

    def test_replace_preserves_position_and_every_other_collection(self):
        case = self.fixture(campaign.CampaignTest)
        initial = campaign.state(
            characters=[campaign.character(), player(), player(id="last")]
        )
        case.install(initial)
        wanted = copy.deepcopy(initial)
        for index in [0, 1, 2]:
            character = {**wanted["characters"][index], "name": f"Updated {index} 🐉"}
            response = case.run_call(upsert(character, wanted["version"]))
            wanted["characters"][index] = character
            wanted["version"] += 1
            self.assertEqual(json.loads(case.path.read_bytes()), wanted)
            self.assertEqual(response["characters"], wanted["characters"])
        case.run_call(remove("pc", wanted["version"]))
        wanted["characters"].pop(1)
        wanted["version"] += 1
        self.assertEqual(json.loads(case.path.read_bytes()), wanted)

    def test_invalid_roster_commands_never_admit(self):
        case = self.fixture(authority.StateHTTP)
        case.create()
        path = case.root / "campaigns/table.json"
        before = path.read_bytes()
        calls = sorted(p.name for p in (case.root / "calls.d").iterdir())
        commands = [upsert(player(), 1), remove("pc", 1)]
        invalid = []
        for command in commands:
            invalid.extend(
                {k: v for k, v in command.items() if k != omitted}
                for omitted in command
            )
            invalid.extend(
                {**command, key: "forged"}
                for key in ["owner_user_id", "membership", "premium", "scene"]
            )
            invalid.extend(
                {**command, "expected_version": v}
                for v in [True, 1.0, "1", 0, -1, 2**63]
            )
            raw = campaign.encoded(command)
            invalid.extend(
                [
                    raw[:-1],
                    raw + b"{}",
                    raw.replace(b'"operation":', b'"operation":"get","operation":'),
                ]
            )
        character = player()
        for omitted in character:
            invalid.append(
                upsert({k: v for k, v in character.items() if k != omitted}, 1)
            )
        invalid.extend(
            upsert(player(**{key: value}), 1)
            for key, value in [
                ("name", "bad\0suffix"),
                ("id", "../escape"),
                ("level", 21),
                ("max_hp", -1),
                ("ability_scores", {"dexterity": 10}),
                ("safety_rules", ["a", "b", "a"]),
                ("safety_rules", ["🐉" * 501]),
                ("allowed_knowledge_scopes", ["owned_rulebook", "campaign_canon"]),
                ("owner_user_id", "bob"),
                ("player_user_id", ""),
            ]
        )
        invalid.extend(
            [
                campaign.encoded(upsert(player(), 1)).replace(
                    b'"dexterity":10', b'"dexterity":10,"dexterity":11'
                ),
                campaign.encoded(upsert(player(), 1)).replace(
                    b'"name":', b'"n\\u0061me":'
                ),
                campaign.encoded(upsert(player(), 1)).replace(
                    b'"kind":"player"', b'"kind":"player","kind":"npc"'
                ),
            ]
        )
        for command in invalid:
            with self.subTest(command=str(command)[:80]):
                body = json.loads(case.body("campaign", {}, "invalid"))
                body["input_json"] = (
                    command if isinstance(command, bytes) else campaign.encoded(command)
                ).decode()
                self.assertEqual(
                    case.http.request(
                        "POST", authority.http.EXECUTE, campaign.encoded(body)
                    )[0],
                    400,
                )
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(
            sorted(p.name for p in (case.root / "calls.d").iterdir()), calls
        )

    def test_scene_cast_and_lease_remain_valid(self):
        case = self.fixture(campaign.CampaignTest)
        before = case.install()
        for command in [
            remove("npc"),
            upsert(player(id="npc")),
            upsert(campaign.character(voice_id="")),
            upsert(campaign.character(allowed_knowledge_scopes=[])),
            remove("missing"),
        ]:
            case.run_call(command, good=False)
            self.assertEqual(case.path.read_bytes(), before)
        result = case.run_call(upsert(campaign.character(name="Mirt the guide")))
        self.assertEqual(result["scene_director"], campaign.state()["scene_director"])
        # Valid old rosters without a scene cast can lose their final NPC.
        case.install(campaign.state(scene_director={"active_npc_ids": []}))
        self.assertEqual(case.run_call(remove("npc"))["characters"], [])

    def test_version_owner_and_archived_guards_precede_roster_writes(self):
        case = self.fixture(campaign.CampaignTest)
        for original in [
            campaign.state(),
            campaign.state(status="archived"),
            campaign.state(version=2**63 - 1),
        ]:
            before = case.install(original)
            for version in [1, 3, 2**63 - 1]:
                case.run_call(upsert(player(), version), good=False)
                case.run_call(remove("npc", version), good=False)
            case.run_call(upsert(player()), user="other", good=False)
            if original["status"] == "archived":
                case.run_call(upsert(player(), original["version"]), good=False)
            self.assertEqual(case.path.read_bytes(), before)

    def test_output_capacity_rejects_complete_roster_without_truncation(self):
        case = self.fixture(campaign.CampaignTest)
        small = campaign.character(
            name="X",
            species="",
            **{"class": ""},
            level=0,
            armor_class=0,
            max_hp=0,
            sheet_refs={},
            persona="",
            voice_id="",
            speaking_style="",
            pronunciation="",
            allowed_knowledge_scopes=[],
            safety_rules=[],
        )
        characters = [{**small, "id": f"p{i}"} for i in range(180)]
        original = campaign.state(
            characters=characters,
            session_recaps=[],
            scene_director={"active_npc_ids": []},
        )
        before = case.install(original)
        self.assertLess(len(before), 65536)
        self.assertEqual(len(case.get()["characters"]), 180)
        case.run_call(upsert(player(persona="x" * 4000)), good=False)
        self.assertEqual(case.path.read_bytes(), before)
        self.assertEqual(len(case.run_call(remove("p179"))["characters"]), 179)

    def test_each_roster_transition_recovers_every_crash_boundary(self):
        fixture = self.fixture(recovery.StateRecoveryTest)
        for operation in ["append", "replace", "remove"]:
            for stage in [
                "queued",
                "artifact",
                "journal",
                "state",
                "complete",
                "cleanup",
            ]:
                with self.subTest(operation=operation, stage=stage):
                    case = fixture.case()
                    if operation == "append":
                        case["command"] = upsert(player())
                        case["expected"]["characters"].append(player())
                    elif operation == "replace":
                        incoming = campaign.character(name="Changed 🐉")
                        case["command"] = upsert(incoming)
                        case["expected"]["characters"] = [incoming]
                    else:
                        case["initial"]["characters"].append(player())
                        case["path"].write_bytes(campaign.encoded(case["initial"]))
                        case["command"] = remove("pc")
                    case["expected"]["campaign"] = case["initial"]["campaign"]
                    fixture.run_call(case, crash=stage)
                    completed = fixture.run_call(case)
                    fixture.assert_complete(case, completed)
                    self.assertEqual(fixture.run_call(case), completed)

    def test_roster_pending_journal_keeps_original_input_and_authority(self):
        fixture = self.fixture(recovery.StateRecoveryTest)
        case = fixture.case()
        case["command"] = upsert(player())
        case["expected"]["characters"].append(player())
        case["expected"]["campaign"] = case["initial"]["campaign"]
        fixture.run_call(case, crash="journal")
        before = case["path"].read_bytes()
        changed = {**case["initial"], "owner_user_id": "other"}
        case["path"].write_bytes(campaign.encoded(changed))
        fixture.run_call(case, code=1)
        self.assertEqual(json.loads(case["path"].read_bytes()), changed)
        self.assertTrue(case["journal"].exists())
        case["path"].write_bytes(before)
        # Recovery finishes the original command; the changed retry remains rejected.
        fixture.run_call(case, command=upsert(player(level=7)), code=1)
        fixture.assert_complete(case, fixture.run_call(case))


if __name__ == "__main__":
    unittest.main()
