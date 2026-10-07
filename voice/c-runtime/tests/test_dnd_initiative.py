"""Native product client against the signed C state host and hostile receipts."""

import copy
import hashlib
import importlib.util
import json
import os
import subprocess
import time
import unittest

import test_dnd_tools as client


ROOT = client.ROOT
SPEC = importlib.util.spec_from_file_location(
    "campaign_fixture", ROOT / "agents/platform-tools/c-ptools/test_campaign.py"
)
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)
PROTO = ROOT / "contracts/handler-base/proto"


class InitiativeClient(unittest.TestCase):
    proxy = client.DiceClient.proxy

    def setUp(self):
        self.host = client.FIXTURE.AuthenticatedTools()
        self.addCleanup(self.host.doCleanups)
        self.host.setUp()
        self.url = f"http://127.0.0.1:{self.host.port}/v1/tools/execute"
        self.characters = [
            CAMPAIGN.character(id="rogue", name='Rógué "🐉"', safety_rules=[]),
            CAMPAIGN.character(id="npc", name="Mirt", safety_rules=[]),
            CAMPAIGN.character(id="last", name="Last", safety_rules=[]),
        ]
        self.install_roster(self.characters)
        self.selection = {
            "operation_id": "roll-1",
            "campaign_version": 2,
            "expected_version": 0,
            "selections": [
                {"character_id": "rogue", "expression": "1d20+3"},
                {"character_id": "npc", "expression": "2d20kh1+1"},
                {"character_id": "last", "expression": "2d20kl1-2"},
            ],
        }

    def install_roster(self, characters):
        # Trusted retained-state setup stays local. Requests use the real signed host.
        self.parent = self.host.root / "dnd/campaigns/table.json"
        self.parent.parent.mkdir(parents=True, exist_ok=True)
        state = CAMPAIGN.state(
            owner_user_id="alice",
            characters=characters,
            session_recaps=[],
            scene_director={
                "active_npc_ids": [],
                "next_speaker_index": 0,
                "active_speaker_id": "",
                "active_turn_id": "",
                "lease_expires_unix_ms": 0,
            },
        )
        self.parent.write_bytes(client.FIXTURE.encode(state))
        self.parent.chmod(0o600)

    def execute(
        self,
        *,
        roster=False,
        selection=None,
        request="roll-1",
        user="alice",
        url=None,
        prompt=None,
        mode="--execute",
        cancel="never",
    ):
        args = [
            str(client.CLIENT),
            mode,
            url or self.url,
            request,
            user,
            "session",
            prompt
            or (
                "Show the campaign roster."
                if roster
                else "Roll initiative for the party."
            ),
            str(int(time.time() * 1000) + 5000),
            cancel,
            "table",
        ]
        if not roster:
            chosen = self.selection if selection is None else selection
            text = f"campaign_version: {chosen['campaign_version']}\nexpected_version: {chosen['expected_version']}\n"
            text += "operation_id: " + json.dumps(chosen["operation_id"]) + "\n"
            for item in chosen["selections"]:
                text += "selections { character_id: " + json.dumps(item["character_id"])
                text += " expression: " + json.dumps(item["expression"]) + " }\n"
            path = self.host.root / "selections.pb"
            path.write_bytes(
                subprocess.run(
                    [
                        "protoc",
                        f"--proto_path={PROTO}",
                        "--encode=messages.v1.DndInitiativeRequest",
                        "messages/v1/messages.proto",
                    ],
                    input=text.encode(),
                    capture_output=True,
                    check=True,
                    timeout=10,
                ).stdout
            )
            args.extend(["battle", str(path)])
        process = subprocess.run(
            args,
            capture_output=True,
            timeout=8,
            env=os.environ | {"DND_TOOLS_TEST_SECRET": client.FIXTURE.SECRET},
        )
        self.assertIn(process.returncode, (0, 1), process.stderr)
        self.assertNotIn(
            client.FIXTURE.SECRET.encode(), process.stdout + process.stderr
        )
        if mode == "--execute" or process.returncode:
            return json.loads(process.stdout)
        return subprocess.run(
            [
                "protoc",
                f"--proto_path={PROTO}",
                "--decode=messages.v1.TurnEvent",
                "messages/v1/messages.proto",
            ],
            input=process.stdout,
            capture_output=True,
            check=True,
            timeout=10,
        ).stdout.decode()

    def receipt(self, result):
        self.assertEqual(result["rc"], 0, result)
        status, response = self.host.request(
            "GET", "/v1/tools/calls/" + result["call_id"]
        )
        self.assertEqual(status, 200, response)
        call = response["call"]
        self.assertEqual(result["output_sha256"], call["output_sha256"])
        self.assertEqual(
            hashlib.sha256(call["output_json"].encode()).hexdigest(),
            result["output_sha256"],
        )
        return json.loads(call["output_json"])

    def test_owned_roster_is_minimal_and_does_not_change_campaign(self):
        before = self.parent.read_bytes()
        result = self.execute(roster=True, request="roster-1")
        output = self.receipt(result)
        self.assertEqual(
            set(output),
            {
                "operation",
                "operation_id",
                "campaign_id",
                "version",
                "status",
                "characters",
            },
        )
        self.assertEqual(
            output["characters"],
            [
                {key: p[key] for key in ["id", "name", "kind", "max_hp"]}
                for p in self.characters
            ],
        )
        self.assertEqual(self.parent.read_bytes(), before)
        self.assertEqual(self.execute(roster=True, request="roster-1"), result)
        self.host.stop()
        self.host.start()
        self.assertEqual(self.execute(roster=True, request="roster-1"), result)
        self.assertNotEqual(self.execute(roster=True, user="bob")["rc"], 0)
        wire = self.execute(roster=True, request="roster-1", mode="--turn-wire")
        self.assertIn("dnd_campaign_roster {", wire)
        self.assertNotIn("player_user_id", wire)
        self.assertNotIn("safety_rules", wire)

    def test_roll_saved_order_and_receipts_survive_retry_and_restart(self):
        result = self.execute()
        output = self.receipt(result)
        participants = {p["id"]: p for p in output["participants"]}
        for selection, roll in zip(
            self.selection["selections"], output["initiative_rolls"], strict=True
        ):
            self.assertEqual(roll["character_id"], selection["character_id"])
            self.assertEqual(roll["expression"], selection["expression"])
            choice = (max if "kh" in roll["expression"] else min)(roll["rolls"])
            self.assertEqual(roll["kept_indices"], [roll["rolls"].index(choice)])
            modifier = {"rogue": 3, "npc": 1, "last": -2}[roll["character_id"]]
            self.assertEqual(roll["total"], choice + modifier)
            self.assertEqual(
                participants[roll["character_id"]]["initiative"], roll["total"]
            )
        path = self.host.root / "dnd/encounters/table/battle.json"
        saved = path.read_bytes()
        self.assertEqual(self.execute(), result)
        self.host.stop()
        self.host.start()
        self.assertEqual(self.execute(), result)
        self.assertEqual(path.read_bytes(), saved)
        wire = self.execute(mode="--turn-wire")
        self.assertIn('operation: "roll_initiative"', wire)
        self.assertIn("dnd_initiative_result {", wire)
        self.assertEqual(wire.count('entropy_source: "getrandom"'), 3)

    def test_stale_versions_wrong_owner_and_changed_retry_preserve_state(self):
        self.receipt(self.execute())
        path = self.host.root / "dnd/encounters/table/battle.json"
        before = path.read_bytes()
        for kwargs in [
            {"user": "bob"},
            {"selection": self.selection | {"operation_id": "stale-encounter"}},
            {
                "selection": self.selection | {"campaign_version": 1},
                "request": "stale-campaign",
            },
            {"selection": self.selection | {"expected_version": 1}},
            {"prompt": "Show the initiative order."},
            {"cancel": "before"},
        ]:
            with self.subTest(kwargs=kwargs):
                self.assertNotEqual(self.execute(**kwargs)["rc"], 0)
                self.assertEqual(path.read_bytes(), before)

    def test_resigned_wrong_roll_or_context_is_rejected(self):
        mutations = [
            lambda o: o.update(campaign_version=3),
            lambda o: o.update(version=2),
            lambda o: o.update(campaign_sha256="x" * 64),
            lambda o: o["initiative_rolls"][0].update(total=99),
            lambda o: o["initiative_rolls"][0].update(expression="1d20+4"),
            lambda o: o["initiative_rolls"][0].update(character_id="other"),
            lambda o: o["initiative_rolls"][0].update(rolls=[0]),
            lambda o: o["initiative_rolls"][0].update(kept_indices=[1]),
            lambda o: o["initiative_rolls"][0].update(entropy_source="model"),
            lambda o: o["initiative_rolls"].reverse(),
            lambda o: o["initiative_rolls"].pop(),
            lambda o: o.update(private_owner="alice"),
        ]
        for mutate in mutations:

            def changed(payload, mutate=mutate):
                call = payload["call"]
                output = json.loads(call["output_json"])
                mutate(output)
                call["output_json"] = client.FIXTURE.encode(output).decode()
                call["output_sha256"] = hashlib.sha256(
                    call["output_json"].encode()
                ).hexdigest()
                call["output_artifact"] = "sha256:" + call["output_sha256"]

            with self.subTest(mutate=mutate):
                self.assertEqual(
                    self.execute(url=self.proxy(changed, resign=True))["rc"], -1
                )

    def test_maximum_selections_reach_signed_tool_and_canonical_event(self):
        characters = [
            CAMPAIGN.character(id=f"pc-{i:02}", name=f"Character {i}", safety_rules=[])
            for i in range(64)
        ]
        self.install_roster(characters)
        chosen = copy.deepcopy(self.selection)
        chosen["selections"] = [
            {"character_id": p["id"], "expression": "2d20kh1+5"} for p in characters
        ]
        result = self.execute(selection=chosen)
        output = self.receipt(result)
        self.assertEqual(len(output["participants"]), 64)
        self.assertEqual(len(output["initiative_rolls"]), 64)
        wire = self.execute(selection=chosen, mode="--turn-wire")
        self.assertEqual(wire.count('entropy_source: "getrandom"'), 64)


if __name__ == "__main__":
    unittest.main()
