"""Explicit encounter changes through authenticated C processes and signed receipts."""

import base64
import copy
import hashlib
import json
import os
import subprocess
import uuid
import unittest

import test_dnd_product as product
import test_dnd_campaign_product as campaign


class ProductEncounter(product.ProductProcess):
    setup_body = campaign.ProductCampaign.setup_body
    character = campaign.ProductCampaign.character
    setup_turn = campaign.ProductCampaign.setup_turn
    proxy = campaign.ProductCampaign.proxy

    def setUp(self):
        super().setUp()
        self.setup_turn(self.setup_body())
        self.setup_turn(self.setup_body(self.character(), 1))
        self.setup_turn(
            self.setup_body(self.character(id="npc", name="Mirt", kind="npc"), 2)
        )
        body = self.body(
            selections=[
                {"character_id": "pc", "expression": "1d20+3"},
                {"character_id": "npc", "expression": "1d20+1"},
            ]
        )
        body["dnd_initiative"]["campaign_version"] = 3
        self.current = self.turn(body)["dnd_encounter_state"]

    def action_body(self, operation, **fields):
        prompts = {
            "advance": "Advance the encounter.",
            "damage": "Apply damage in the encounter.",
            "heal": "Heal a participant in the encounter.",
            "condition_add": "Add a condition in the encounter.",
            "condition_remove": "Remove a condition in the encounter.",
            "end": "End the encounter.",
        }
        operation_id = str(uuid.uuid4())
        return {
            "message": prompts[operation],
            "request_id": str(uuid.uuid4()),
            "conversation_id": "action-" + operation_id,
            "enable_tts": False,
            "metadata": {
                "interaction_profile": "dnd_app",
                "campaign_id": "table",
                "encounter_id": "battle",
            },
            "dnd_encounter_action": {
                "operation": operation,
                "operation_id": operation_id,
                "expected_version": self.current["version"],
                **fields,
            },
        }

    def action(self, body):
        before = copy.deepcopy(self.current)
        final = self.turn(body)
        call, output = self.receipt(final)
        self.assertEqual(
            call["parent_turn_id"], body["dnd_encounter_action"]["operation_id"]
        )
        self.assertTrue(json.loads(call["input_json"])["include_previous"])
        self.assertEqual(
            output["previous_state"],
            {
                key: before[key]
                for key in (
                    "campaign_id",
                    "encounter_id",
                    "version",
                    "status",
                    "round",
                    "active_index",
                    "participants",
                )
            },
        )
        self.current = final["dnd_encounter_state"]
        self.assertEqual(self.current["version"], before["version"] + 1)
        self.assertEqual(self.current["participants"], output["participants"])
        self.assertNotIn("previous_state", json.dumps(final))
        return final, call

    def test_complete_encounter_changes_keep_other_participants_and_wrap_round(self):
        for _ in range(3):
            before = copy.deepcopy(self.current)
            self.action(self.action_body("advance"))
            expected_index = (before["active_index"] + 1) % len(before["participants"])
            self.assertEqual(self.current["active_index"], expected_index)
            self.assertEqual(
                self.current["round"],
                before["round"] + (before["active_index"] >= 0 and expected_index == 0),
            )
            self.assertEqual(self.current["participants"], before["participants"])
        npc = next(p for p in self.current["participants"] if p["id"] == "npc")
        for operation, fields, hp in [
            ("damage", {"amount": 7, "damage_type": "slashing"}, 16),
            ("heal", {"amount": 100000}, 23),
            ("damage", {"amount": 100000, "damage_type": ""}, 0),
        ]:
            self.action(self.action_body(operation, participant_id="pc", **fields))
            self.assertEqual(
                next(p for p in self.current["participants"] if p["id"] == "pc")[
                    "current_hp"
                ],
                hp,
            )
            self.assertEqual(
                next(p for p in self.current["participants"] if p["id"] == "npc"), npc
            )
        for operation, condition, expected in [
            ("condition_add", "prone", ["prone"]),
            ("condition_add", "blinded", ["blinded", "prone"]),
            ("condition_remove", "prone", ["blinded"]),
        ]:
            self.action(
                self.action_body(operation, participant_id="pc", condition=condition)
            )
            self.assertEqual(
                next(p for p in self.current["participants"] if p["id"] == "pc")[
                    "conditions"
                ],
                expected,
            )
        self.action(self.action_body("end"))
        self.assertEqual(
            (
                self.current["status"],
                self.current["active_index"],
                self.current["active_participant_id"],
            ),
            ("ended", -1, ""),
        )
        self.turn(self.action_body("advance"), completed=False)

    @unittest.skipUnless(
        os.environ.get("DND_PRODUCT_BROWSER") == "1",
        "optional installed Chromium proof",
    )
    def test_served_browser_creates_and_changes_encounter_with_provider_pcm(self):
        self.start_tts()
        process = subprocess.run(
            [
                os.environ.get("DND_PRODUCT_BROWSER_NODE", "node"),
                str(
                    product.ROOT
                    / "benchmarks/browser-e2e/scripts/dnd-product-campaign.mjs"
                ),
            ],
            input=json.dumps(
                {
                    "url": f"http://127.0.0.1:{self.product_port}",
                    "cookie": self.cookie,
                    "actions": True,
                    "campaign": "browser-table",
                    "encounter": "browser-battle",
                }
            ),
            text=True,
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        completed = json.loads(process.stdout)
        self.assertEqual(len(completed), 6)
        for final in completed:
            _, saved = self.receipt(final)
            self.assertEqual(final["dnd_encounter_state"]["version"], saved["version"])
            self.assertEqual(
                final["dnd_encounter_state"]["participants"], saved["participants"]
            )
        state = json.loads(
            (
                self.host.root / "dnd/encounters/browser-table/browser-battle.json"
            ).read_bytes()
        )
        self.assertEqual(
            (state["status"], state["version"], state["owner_user_id"]),
            ("ended", 7, self.user),
        )

    def test_changes_speak_verified_hp_and_conditions(self):
        requests, pcm = self.start_tts()
        for operation, fields, text in [
            ("damage", {"amount": 7, "damage_type": "fire"}, "16 of 23 hit points"),
            ("condition_add", {"condition": "prone"}, "is now prone"),
            ("condition_remove", {"condition": "prone"}, "Removed prone"),
        ]:
            body = self.action_body(operation, participant_id="pc", **fields)
            del body["enable_tts"]
            final, _ = self.action(body)
            self.assertIn(text, final["text"])
            chunks = [e for e in self.last_events if e["type"] == "pcm_chunk"]
            provider = [r for r in requests if r["turn_id"] == body["request_id"]]
            self.assertTrue(chunks and provider)
            self.assertEqual(
                b"".join(
                    base64.b64decode(c["audio_base64"], validate=True) for c in chunks
                ),
                pcm * len(provider),
            )

    def test_retry_retains_prior_receipt_after_later_changes_and_restart(self):
        body = self.action_body(
            "damage", participant_id="pc", amount=7, damage_type="fire"
        )
        first, call = self.action(body)
        self.action(self.action_body("heal", participant_id="pc", amount=4))
        state = (self.host.root / "dnd/encounters/table/battle.json").read_bytes()
        self.stop("product")
        self.host.stop()
        self.host.start()
        self.start_product()
        body["request_id"] = str(uuid.uuid4())
        replay = self.turn(body)
        replay_call, _ = self.receipt(replay)
        self.assertEqual(replay_call, call)
        self.assertEqual(replay["dnd_encounter_state"], first["dnd_encounter_state"])
        body["request_id"] = str(uuid.uuid4())
        body["dnd_encounter_action"]["amount"] = 8
        self.turn(body, completed=False)
        self.assertEqual(
            (self.host.root / "dnd/encounters/table/battle.json").read_bytes(), state
        )

    def test_foreign_owner_stale_version_and_unbound_choices_cannot_change_state(self):
        state = (self.host.root / "dnd/encounters/table/battle.json").read_bytes()
        other, _ = self.login()
        self.turn(self.action_body("advance"), cookie=other, completed=False)
        self.turn(self.action_body("advance", expected_version=999), completed=False)
        body = self.action_body(
            "damage", participant_id="missing", amount=1, damage_type=""
        )
        self.turn(body, completed=False)
        for change in (
            {"amount": 100001},
            {"owner_user_id": "victim"},
            {"damage_type": "invented"},
            {"amount": -1},
        ):
            body = self.action_body(
                "damage", participant_id="pc", amount=1, damage_type="fire"
            )
            body["dnd_encounter_action"].update(change)
            status, _, _ = self.http(
                self.product_port, "POST", "/api/turns", body, self.cookie
            )
            self.assertEqual(status, 400)
        body = self.action_body("advance")
        body["message"] = "Hello"
        self.turn(body, completed=False)
        self.assertEqual(
            (self.host.root / "dnd/encounters/table/battle.json").read_bytes(), state
        )

    def test_signed_inconsistent_transitions_fail_before_publication(self):
        mutations = [
            lambda o: o["previous_state"].update(version=999),
            lambda o: o["previous_state"].update(campaign_id="foreign"),
            lambda o: o["previous_state"]["participants"][0].update(name="Changed"),
            lambda o: o["participants"][0].update(max_hp=99),
            lambda o: o.update(round=999),
            lambda o: o.update(participants=[]),
            lambda o: o.pop("previous_state"),
        ]
        for mutate in mutations:

            def corrupt(payload, mutate=mutate):
                call = payload["call"]
                output = json.loads(call["output_json"])
                mutate(output)
                call["output_json"] = json.dumps(
                    output, ensure_ascii=False, separators=(",", ":")
                )
                call["output_sha256"] = hashlib.sha256(
                    call["output_json"].encode()
                ).hexdigest()
                call["output_artifact"] = "sha256:" + call["output_sha256"]

            self.stop("cascade")
            ready = self.log("cascade").count("pure-C direct admission")
            self.environment["TOOL_HTTP_URL"] = self.proxy(corrupt, resign=True)
            self.start(
                "cascade", product.RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN"
            )
            self.wait_for(
                lambda: self.log("cascade").count("pure-C direct admission") > ready
            )
            self.turn(self.action_body("advance"), completed=False)
            self.current = json.loads(
                (self.host.root / "dnd/encounters/table/battle.json").read_bytes()
            )


if __name__ == "__main__":
    unittest.main()
