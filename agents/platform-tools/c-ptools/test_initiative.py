"""Independent signed HTTP, dice arithmetic, and process recovery oracles."""

import copy
import hashlib
import json
import os
import re
import subprocess
import unittest
from unittest.mock import patch

import test_campaign as campaign
import test_encounter as encounter
import test_roster as roster
import test_state_authority as authority
import test_state_recovery as recovery

INPUT_SCHEMA = json.loads(
    (campaign.SCHEMAS / "dnd-encounter-state.input.schema.json").read_text()
)
OUTPUT_SCHEMA = json.loads(
    (campaign.SCHEMAS / "dnd-encounter-state.output.schema.json").read_text()
)


def command(**changes):
    return {
        "operation": "roll_initiative",
        "campaign_id": "table",
        "encounter_id": "battle",
        "expected_version": 2,
        "campaign_version": 2,
        "initiative": [
            {"character_id": "rogue", "expression": "1d20+3"},
            {"character_id": "npc", "expression": "2d20kh1+1"},
            {"character_id": "last", "expression": "2d20kl1-2"},
        ],
        **changes,
    }


def parent(**changes):
    return campaign.state(
        **{
            "characters": [
                campaign.character(),
                roster.player(id="rogue", name="Rogue 🐉", max_hp=40),
                roster.player(id="last", name="Last", max_hp=15),
            ],
            **changes,
        }
    )


class InitiativeTest(unittest.TestCase):
    def fixture(self, kind):
        result = kind()
        result.setUp()
        self.addCleanup(result.doCleanups)
        return result

    def case(self, *, new=False, initial=None, characters=None):
        host = self.fixture(recovery.StateRecoveryTest)
        case = host.case("encounter")
        case["command"] = command(expected_version=0 if new else 2)
        case["parent_path"] = case["root"] / "campaigns/table.json"
        case["parent"] = parent(**({"characters": characters} if characters else {}))
        if characters:
            case["parent"]["scene_director"] = {
                "active_npc_ids": [],
                "next_speaker_index": 0,
                "active_speaker_id": "",
                "active_turn_id": "",
                "lease_expires_unix_ms": 0,
            }
        case["parent_path"].write_bytes(campaign.encoded(case["parent"]))
        if initial:
            case["initial"] = initial
            case["path"].write_bytes(campaign.encoded(initial))
        if new:
            case["path"].unlink()
            case["initial"] = None
        return host, case

    def verify(self, host, case, record, *, deterministic=False):
        output = json.loads(record["output_json"])
        self.assertLessEqual(set(output), set(OUTPUT_SCHEMA["properties"]))
        self.assertEqual(
            output["campaign_version"], case["command"]["campaign_version"]
        )
        # Hash the complete state with the existing campaign roundtrip codec.
        raw = subprocess.run(
            [str(campaign.BINARY), "roundtrip"],
            input=campaign.encoded(case["parent"]),
            capture_output=True,
            check=True,
            timeout=20,
        ).stdout.rstrip(b"\n")
        self.assertEqual(output["campaign_sha256"], hashlib.sha256(raw).hexdigest())
        initial = case["initial"]
        expected = (
            copy.deepcopy(initial)
            if initial
            else encounter.state(version=0, active_index=-1, participants=[])
        )
        expected["version"] += 1
        active = (
            expected["participants"][expected["active_index"]]["id"]
            if expected["active_index"] >= 0
            else ""
        )
        players = {p["id"]: p for p in case["parent"]["characters"]}
        participants = {p["id"]: p for p in expected["participants"]}
        self.assertEqual(
            len(output["initiative_rolls"]), len(case["command"]["initiative"])
        )
        for selection, receipt in zip(
            case["command"]["initiative"], output["initiative_rolls"], strict=True
        ):
            schema = OUTPUT_SCHEMA["properties"]["initiative_rolls"]["items"]
            self.assertEqual(set(receipt), set(schema["required"]))
            self.assertRegex(
                receipt["expression"], schema["properties"]["expression"]["pattern"]
            )
            self.assertEqual(receipt["character_id"], selection["character_id"])
            self.assertEqual(receipt["expression"], selection["expression"])
            expression = re.fullmatch(
                r"(1|2)d20(k[hl]1)?([+-]\d+)?", selection["expression"]
            )
            self.assertIsNotNone(expression)
            rolls = receipt["rolls"]
            self.assertEqual(len(rolls), int(expression[1]))
            self.assertTrue(all(type(r) is int and 1 <= r <= 20 for r in rolls))
            chosen = (max if expression[2] == "kh1" else min)(rolls)
            kept = rolls.index(chosen)
            self.assertEqual(receipt["kept_indices"], [kept])
            self.assertEqual(receipt["total"], chosen + int(expression[3] or 0))
            self.assertEqual(receipt["entropy_source"], "getrandom")
            ident = selection["character_id"]
            if ident not in participants:
                p = players[ident]
                participants[ident] = encounter.participant(
                    id=ident, name=p["name"], max_hp=p["max_hp"], current_hp=p["max_hp"]
                )
            participants[ident]["initiative"] = receipt["total"]
        expected["participants"] = sorted(
            participants.values(), key=lambda p: (-p["initiative"], p["id"])
        )
        expected["active_index"] = next(
            (i for i, p in enumerate(expected["participants"]) if p["id"] == active), -1
        )
        host.assert_complete(case, record, expected)
        if deterministic:
            self.assertEqual(
                [r["rolls"] for r in output["initiative_rolls"]],
                [[1], [20, 5], [15, 2]],
            )
            self.assertEqual(
                [r["total"] for r in output["initiative_rolls"]], [4, 21, 0]
            )
        return output

    def test_signed_creation_reroll_owner_and_exact_retry(self):
        case = self.fixture(authority.StateHTTP)
        case.create()
        for version, p in enumerate(parent()["characters"], 1):
            case.execute("campaign", roster.upsert(p, version))
        cmd = command(expected_version=0, campaign_version=4)
        result = case.execute("encounter", cmd, call_id="roll")
        output = json.loads(result["call"]["output_json"])
        self.assertEqual(output["version"], 1)
        self.assertEqual(output["active_index"], -1)
        path = case.root / "encounters/table/battle.json"
        before = path.read_bytes()
        case.execute("encounter", cmd, user="bob", status=409)
        case.execute("encounter", {**cmd, "campaign_version": 3}, status=409)
        case.execute(
            "encounter", {**cmd, "expected_version": 1}, call_id="roll", status=409
        )
        for key in ["session_id", "parent_turn_id", "idempotency_key"]:
            body = json.loads(case.body("encounter", cmd, "roll"))
            body[key] = "changed"
            self.assertEqual(
                case.http.request(
                    "POST", authority.http.EXECUTE, campaign.encoded(body)
                )[0],
                409,
            )
        self.assertEqual(
            case.http.request("GET", "/v1/tools/calls/roll", user="bob")[0], 404
        )
        self.assertEqual(
            case.http.request("POST", "/v1/tools/calls/roll/cancel", b"{}", user="bob")[
                0
            ],
            404,
        )
        case.http.stop()
        case.http.start()
        self.assertEqual(case.execute("encounter", cmd, call_id="roll"), result)
        self.assertEqual(
            case.http.request("POST", "/v1/tools/calls/roll/cancel", b"{}"),
            (200, result),
        )
        self.assertEqual(path.read_bytes(), before)
        second = case.execute(
            "encounter", {**cmd, "expected_version": 1}, call_id="reroll"
        )
        self.assertEqual(json.loads(second["call"]["output_json"])["version"], 2)
        self.assertEqual(case.execute("encounter", cmd, call_id="roll"), result)

    def test_deterministic_draws_preserve_active_hp_conditions_and_round(self):
        initial = encounter.state(
            round=7,
            participants=[
                encounter.participant(
                    id="unselected", initiative=30, conditions=["prone"]
                ),
                encounter.participant(current_hp=3, conditions=["poisoned", "prone"]),
            ],
            active_index=1,
        )
        host, case = self.case(initial=initial)
        with patch.dict(os.environ, PT_TEST_ENTROPY="sequence"):
            record = host.run_call(case)
        self.verify(host, case, record, deterministic=True)
        self.assertEqual(
            json.loads(record["output_json"])["active_participant_id"], "rogue"
        )
        self.assertEqual(
            case["parent_path"].read_bytes(), campaign.encoded(case["parent"])
        )

    def test_recovers_all_six_boundaries_without_reroll_after_commit(self):
        for new in [False, True]:
            for stage in [
                "queued",
                "artifact",
                "journal",
                "state",
                "complete",
                "cleanup",
            ]:
                with self.subTest(new=new, stage=stage):
                    host, case = self.case(new=new)
                    with patch.dict(os.environ, PT_TEST_ENTROPY="sequence"):
                        host.run_call(case, crash=stage)
                    journal = (
                        json.loads(case["journal"].read_bytes())
                        if case["journal"].exists()
                        else None
                    )
                    if journal:
                        self.assertEqual(
                            journal["schema"], "dnd-encounter-transaction/v2"
                        )
                        self.assertEqual(journal["draws"], [1, 20, 5, 15, 2])
                    if stage in ["queued", "artifact"]:
                        self.assertEqual(case["path"].exists(), not new)
                        if not new:
                            self.assertEqual(
                                json.loads(case["path"].read_bytes()), case["initial"]
                            )
                    with patch.dict(
                        os.environ,
                        PT_TEST_ENTROPY="sequence"
                        if stage in ["queued", "artifact"]
                        else "fail",
                    ):
                        result = host.run_call(case)
                        self.assertEqual(host.run_call(case), result)
                    self.verify(host, case, result, deterministic=True)
                    if journal:
                        self.assertEqual(result, journal["call"])

    def test_entropy_failure_and_pending_sync_cancellation(self):
        host, case = self.case()
        before = case["path"].read_bytes()
        with patch.dict(os.environ, PT_TEST_ENTROPY="fail"):
            rejected = host.run_call(case, code=1)
        self.assertEqual(rejected["state"], "failed")
        self.assertEqual(case["path"].read_bytes(), before)
        self.assertFalse(case["journal"].exists())
        for fault in ["1", "2", "journal-dir", "complete"]:
            host, case = self.case()
            with patch.dict(os.environ, PT_TEST_ENTROPY="sequence"):
                host.run_call(case, fail_sync=fault, code=1)
            journal = json.loads(case["journal"].read_bytes())
            with patch.dict(os.environ, PT_TEST_ENTROPY="fail"):
                completed = host.run_call(case, action="cancel")
            self.assertEqual(completed, journal["call"])
            self.verify(host, case, completed, deterministic=True)
        for action in ["cancel-pending", "read-pending", "other-pending"]:
            host, case = self.case()
            self.verify(host, case, host.run_call(case, action=action, fail_sync="1"))
        host, case = self.case()
        host.run_call(case, crash="queued")
        self.assertEqual(host.run_call(case, action="cancel")["state"], "canceled")
        self.assertEqual(json.loads(case["path"].read_bytes()), case["initial"])

    def test_pending_campaign_changes_and_corrupt_v2_context_fail_closed(self):
        host, case = self.case()
        host.run_call(case, crash="journal")
        raw = case["journal"].read_bytes()
        tx = json.loads(raw)
        before = case["path"].read_bytes()
        variants = []
        for key in ["campaign", "draws"]:
            variants.append({k: v for k, v in tx.items() if k != key})
        for draws in [
            [],
            [1],
            [0, 1, 2, 3, 4],
            [21, 1, 2, 3, 4],
            [1.0, 2, 3, 4, 5],
            ["1", 2, 3, 4, 5],
            [1] * 129,
        ]:
            variants.append({**tx, "draws": draws})
        variants.append({**tx, "draws": [21 - tx["draws"][0], *tx["draws"][1:]]})
        variants.append({**tx, "schema": "dnd-encounter-transaction/v1"})
        for key, value in [
            ("version", 3),
            ("owner_user_id", "other"),
            ("status", "archived"),
        ]:
            variants.append({**tx, "campaign": {**tx["campaign"], key: value}})
        variants.extend(
            [
                raw[:-1],
                raw + b"{}",
                raw + b"\0",
                raw.replace(b'"draws":', b'"draws":[],"draws":', 1),
                raw.replace(b'"campaign":', b'"c\\u0061mpaign":', 1),
            ]
        )
        for invalid in variants:
            with self.subTest(invalid=str(invalid)[:90]):
                case["journal"].write_bytes(
                    invalid if isinstance(invalid, bytes) else campaign.encoded(invalid)
                )
                self.assertIsNone(host.run_call(case, code=1))
                self.assertEqual(case["path"].read_bytes(), before)
        case["journal"].write_bytes(raw)
        parent_raw = case["parent_path"].read_bytes()
        for change in [
            {**case["parent"], "version": 3},
            {**case["parent"], "version": 1},
            {**case["parent"], "owner_user_id": "other"},
            {
                **case["parent"],
                "campaign": {**case["parent"]["campaign"], "name": "Changed"},
            },
        ]:
            case["parent_path"].write_bytes(campaign.encoded(change))
            self.assertIsNone(host.run_call(case, code=1))
            self.assertEqual(case["path"].read_bytes(), before)
            self.assertEqual(case["journal"].read_bytes(), raw)
        case["parent_path"].unlink()
        self.assertIsNone(host.run_call(case, code=1))
        case["parent_path"].write_bytes(parent_raw)
        artifact = case["root"] / "artifacts" / (tx["call"]["output_sha256"] + ".json")
        contents = artifact.read_bytes()
        artifact.unlink()
        self.assertIsNone(host.run_call(case, code=1))
        artifact.write_bytes(contents)
        artifact.chmod(0o600)
        with patch.dict(os.environ, PT_TEST_ENTROPY="fail"):
            self.verify(host, case, host.run_call(case))

    def test_completed_cleanup_never_rewinds_later_roster_or_encounter(self):
        host, case = self.case()
        host.run_call(case, crash="complete")
        journal = case["journal"].read_bytes()
        first = host.run_call(case)
        later = {**case["command"], "expected_version": 3}
        host.run_call(case, command=later, call_id="later")
        saved = case["path"].read_bytes()
        case["parent"]["version"] += 1
        case["parent"]["characters"][0]["name"] = "Later Mirt"
        case["parent_path"].write_bytes(campaign.encoded(case["parent"]))
        parent_raw = case["parent_path"].read_bytes()
        case["journal"].write_bytes(journal)
        with patch.dict(os.environ, PT_TEST_ENTROPY="fail"):
            self.assertEqual(host.run_call(case), first)
        self.assertEqual(case["path"].read_bytes(), saved)
        self.assertEqual(case["parent_path"].read_bytes(), parent_raw)
        self.assertFalse(case["journal"].exists())

    def test_invalid_input_never_admits_a_call(self):
        case = self.fixture(authority.StateHTTP)
        case.create()
        cmd = command(expected_version=0, campaign_version=1)
        invalid = [{k: v for k, v in cmd.items() if k != omit} for omit in cmd]
        invalid.extend(
            {**cmd, key: "forged"}
            for key in ["owner_user_id", "participant", "premium", "amount"]
        )
        for key in ["campaign_version", "expected_version"]:
            invalid.extend({**cmd, key: v} for v in [True, "1", 1.0, -1, 2**63])
        invalid.append({**cmd, "campaign_version": 0})
        for expression in [
            "1d6",
            "2d20",
            "3d20kh1",
            "1d20kh1",
            "2d20kh2",
            "d20",
            "1D20",
            " 1d20",
            "1d20+0",
            "1d20+81",
            "1d20-102",
            "1d20+3\0x",
        ]:
            invalid.append(
                {
                    **cmd,
                    "initiative": [{"character_id": "npc", "expression": expression}],
                }
            )
        for selection in [
            [],
            [cmd["initiative"][0]] * 2,
            [{"character_id": "npc"}],
            [{"character_id": "../x", "expression": "1d20"}],
            [{"character_id": "n" * 65, "expression": "1d20"}],
            [{"character_id": f"p{i}", "expression": "1d20"} for i in range(65)],
        ]:
            invalid.append({**cmd, "initiative": selection})
        raw = campaign.encoded(cmd)
        invalid.extend(
            [
                raw[:-1],
                raw + b"{}",
                raw.replace(b'"expression":', b'"expression":"1d20","expression":', 1),
                raw.replace(b'"character_id":', b'"character_\\u0069d":', 1),
            ]
        )
        path = case.root / "campaigns/table.json"
        before = path.read_bytes()
        calls = sorted((case.root / "calls.d").iterdir())
        for i, value in enumerate(invalid):
            with self.subTest(index=i):
                body = json.loads(case.body("encounter", cmd, f"bad-{i}"))
                body["input_json"] = (
                    value.decode()
                    if isinstance(value, bytes)
                    else campaign.encoded(value).decode()
                )
                self.assertEqual(
                    case.http.request(
                        "POST", authority.http.EXECUTE, campaign.encoded(body)
                    )[0],
                    400,
                )
                self.assertEqual(sorted((case.root / "calls.d").iterdir()), calls)
        self.assertEqual(path.read_bytes(), before)

    def test_selection_state_and_public_projection_bounds(self):
        for change in [
            {"campaign_version": 3},
            {"expected_version": 0},
            {"initiative": [{"character_id": "missing", "expression": "1d20"}]},
        ]:
            host, case = self.case()
            host.run_call(case, command={**case["command"], **change}, code=1)
            self.assertEqual(json.loads(case["path"].read_bytes()), case["initial"])
        for initial in [
            encounter.state(status="ended"),
            encounter.state(version=9007199254740991),
        ]:
            host, case = self.case(initial=initial)
            host.run_call(
                case,
                command={**case["command"], "expected_version": initial["version"]},
                code=1,
            )
            self.assertEqual(json.loads(case["path"].read_bytes()), initial)
        # Every retained character is valid campaign data, but not every name
        # fits the bounded public encounter projection.
        for name in ["x" * 121, "🐉" * 31, "line\nbreak"]:
            host, case = self.case(characters=[campaign.character(name=name)])
            cmd = command(initiative=[{"character_id": "npc", "expression": "1d20"}])
            host.run_call(case, command=cmd, code=1)
            self.assertEqual(json.loads(case["path"].read_bytes()), case["initial"])
        for count, name, good in [
            (64, "x", True),
            (65, "x", False),
            (64, "x" * 120, False),
        ]:
            players = [
                campaign.character(id=f"p{i:02d}", name=name, safety_rules=[])
                for i in range(count)
            ]
            host, case = self.case(
                characters=players,
                initial=encounter.state(
                    participants=[
                        encounter.participant(id=f"p{i:02d}", name=name)
                        for i in range(count)
                    ]
                ),
            )
            cmd = command(initiative=[{"character_id": "p00", "expression": "1d20"}])
            result = host.run_call(case, command=cmd, code=0 if good else 1)
            if good:
                self.assertEqual(
                    len(json.loads(result["output_json"])["participants"]), 64
                )
            else:
                self.assertEqual(json.loads(case["path"].read_bytes()), case["initial"])

    def test_extreme_totals_and_maximum_draw_count(self):
        host, case = self.case(new=True)
        case["command"]["initiative"] = [
            {"character_id": "rogue", "expression": "1d20-101"},
            {"character_id": "npc", "expression": "1d20+80"},
        ]
        with patch.dict(os.environ, PT_TEST_ENTROPY="sequence"):
            result = host.run_call(case)
        output = self.verify(host, case, result)
        self.assertEqual([r["total"] for r in output["initiative_rolls"]], [-100, 100])
        pattern = INPUT_SCHEMA["properties"]["initiative"]["items"]["properties"][
            "expression"
        ]["pattern"]
        for base in ["1d20", "2d20kh1", "2d20kl1"]:
            for modifier in range(-102, 82):
                expression = base + (f"{modifier:+}" if modifier else "")
                self.assertEqual(
                    bool(re.fullmatch(pattern, expression)), -101 <= modifier <= 80
                )
        players = [
            campaign.character(id=f"p{i:02d}", name="P", safety_rules=[])
            for i in range(64)
        ]
        host, case = self.case(new=True, characters=players)
        case["command"]["initiative"] = [
            {"character_id": p["id"], "expression": "2d20kl1"} for p in players
        ]
        with patch.dict(os.environ, PT_TEST_ENTROPY="sequence"):
            host.run_call(case, crash="journal")
        tx = json.loads(case["journal"].read_bytes())
        self.assertEqual(len(tx["draws"]), 128)
        with patch.dict(os.environ, PT_TEST_ENTROPY="fail"):
            self.verify(host, case, host.run_call(case))


if __name__ == "__main__":
    unittest.main()
