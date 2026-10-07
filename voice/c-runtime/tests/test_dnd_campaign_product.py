"""Owned campaign setup through authenticated C processes, starting without seed state."""

import base64
import copy
import hashlib
import json
import os
import subprocess
import threading
import time
import unittest
import uuid

import test_dnd_product as product


class ProductCampaign(product.ProductProcess):
    proxy = product.initiative.client.DiceClient.proxy

    def setup_body(self, character=None, version=0):
        operation = str(uuid.uuid4())
        request = {
            "operation": "add_character" if character else "create",
            "operation_id": operation,
            "expected_version": version,
        }
        if character:
            request["character"] = character
        else:
            request["campaign"] = {"name": 'Laéral "🐉"', "ruleset": "5e"}
        return {
            "message": "Add a character to the campaign."
            if character
            else "Create the campaign.",
            "request_id": str(uuid.uuid4()),
            "conversation_id": "campaign-" + operation,
            "enable_tts": False,
            "metadata": {"interaction_profile": "dnd_app", "campaign_id": "table"},
            "dnd_campaign": request,
        }

    def character(self, **changes):
        return {
            "id": "pc",
            "name": 'Aria "🐉"',
            "kind": "player",
            "species": "elf",
            "class_name": "rogue",
            "level": 3,
            "armor_class": 14,
            "max_hp": 23,
            "ability_scores": dict(
                zip(
                    (
                        "strength",
                        "dexterity",
                        "constitution",
                        "intelligence",
                        "wisdom",
                        "charisma",
                    ),
                    (8, 16, 12, 13, 11, 14),
                    strict=True,
                )
            ),
            **changes,
        }

    def setup_turn(self, body):
        final = self.turn(body)
        call, output = self.receipt(final)
        self.assertEqual(call["parent_turn_id"], body["dnd_campaign"]["operation_id"])
        self.assertEqual(call["session_id"], body["conversation_id"])
        public = final["dnd_campaign_roster"]
        self.assertEqual(
            public["version"], body["dnd_campaign"]["expected_version"] + 1
        )
        self.assertEqual(public["output_sha256"], call["output_sha256"])
        self.assertEqual(
            public["characters"],
            [
                {key: character[key] for key in ("id", "name", "kind", "max_hp")}
                for character in output["characters"]
            ],
        )
        self.assertNotIn("player_user_id", json.dumps(final))
        self.assertNotIn("ability_scores", json.dumps(final))
        return final, call

    def test_empty_state_create_player_npc_and_saved_initiative_with_speech(self):
        self.assertFalse(self.parent.exists())
        created, _ = self.setup_turn(self.setup_body())
        self.assertEqual(created["dnd_campaign_roster"]["characters"], [])
        self.setup_turn(self.setup_body(self.character(), 1))
        self.setup_turn(
            self.setup_body(self.character(id="npc", name="Mirt", kind="npc"), 2)
        )
        retained = json.loads(self.parent.read_bytes())
        self.assertEqual(retained["owner_user_id"], self.user)
        self.assertEqual(retained["characters"][0]["player_user_id"], self.user)
        self.assertEqual(retained["characters"][1]["player_user_id"], "")
        self.assertEqual(retained["version"], 3)
        self.assertTrue(
            all(not c["allowed_knowledge_scopes"] for c in retained["characters"])
        )
        requests, expected_pcm = self.start_tts()
        body = self.body(
            selections=[
                {"character_id": "pc", "expression": "1d20+3"},
                {"character_id": "npc", "expression": "2d20kh1+1"},
            ]
        )
        body["dnd_initiative"]["campaign_version"] = 3
        del body["enable_tts"]
        final = self.turn(body)
        _, saved = self.receipt(final)
        self.assertEqual(
            final["dnd_encounter_state"]["participants"], saved["participants"]
        )
        chunks = [event for event in self.last_events if event["type"] == "pcm_chunk"]
        provider = [
            request for request in requests if request["turn_id"] == body["request_id"]
        ]
        self.assertTrue(chunks and provider)
        self.assertEqual(
            b"".join(
                base64.b64decode(c["audio_base64"], validate=True) for c in chunks
            ),
            expected_pcm * len(provider),
        )

    def test_create_and_add_retries_survive_restart_and_reject_changed_input(self):
        for body in (self.setup_body(), self.setup_body(self.character(), 1)):
            final, call = self.setup_turn(body)
            retained = self.parent.read_bytes()
            self.stop("product")
            self.host.stop()
            self.host.start()
            self.start_product()
            status, _, _ = self.http(
                self.product_port, "POST", "/api/turns", body, self.cookie
            )
            self.assertEqual(status, 502)
            body["request_id"] = str(uuid.uuid4())
            replay, replay_call = self.setup_turn(body)
            self.assertEqual(replay_call, call)
            self.assertEqual(
                replay["dnd_campaign_roster"], final["dnd_campaign_roster"]
            )
            changed = copy.deepcopy(body)
            changed["request_id"] = str(uuid.uuid4())
            data = changed["dnd_campaign"]
            data["campaign" if "campaign" in data else "character"]["name"] = "Changed"
            self.turn(changed, completed=False)
            self.assertEqual(self.parent.read_bytes(), retained)

    def test_signed_but_inconsistent_saved_character_never_reaches_public_output(self):
        self.setup_turn(self.setup_body())
        mutations = [
            lambda output: output.update(version=999),
            lambda output: output.update(operation="get"),
            lambda output: output.update(campaign_id="another"),
            lambda output: output["characters"][-1].update(name="Wrong"),
            lambda output: output["characters"][-1].update(player_user_id="victim"),
            lambda output: output["characters"][-1].update(max_hp=24),
            lambda output: output["characters"][-1].update({"class": "wizard"}),
            lambda output: output["characters"][-1]["ability_scores"].update(
                dexterity=1
            ),
            lambda output: output["characters"][-1].update(
                allowed_knowledge_scopes=["owned_rulebook"]
            ),
            lambda output: output.update(characters=[]),
        ]
        for index, mutation in enumerate(mutations):

            def corrupt(payload, mutation=mutation):
                call = payload["call"]
                output = json.loads(call["output_json"])
                mutation(output)
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
            body = self.setup_body(self.character(id=f"pc-{index}"), index + 1)
            self.turn(body, completed=False)
            retained = json.loads(self.parent.read_bytes())
            self.assertEqual(retained["version"], index + 2)
            self.assertEqual(retained["characters"][-1]["name"], 'Aria "🐉"')
            self.assertEqual(retained["characters"][-1]["player_user_id"], self.user)

    def test_tool_deadline_returns_terminal_failure_after_authorized_save(self):
        self.setup_turn(self.setup_body())
        self.stop("cascade")
        ready = self.log("cascade").count("pure-C direct admission")
        contacted = threading.Event()
        self.environment["TOOL_HTTP_URL"] = self.proxy(
            resign=True, contacted=contacted, response_delay=5.5
        )
        self.start(
            "cascade", product.RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN"
        )
        self.wait_for(
            lambda: self.log("cascade").count("pure-C direct admission") > ready
        )
        body = self.setup_body(self.character(id="deadline-pc"), 1)
        started = time.monotonic()
        self.turn(body, completed=False)
        elapsed = time.monotonic() - started
        self.assertTrue(contacted.is_set())
        self.assertGreaterEqual(elapsed, 4)
        self.assertLess(elapsed, 6.5)
        # A timeout does not undo or authorize retrying an already saved action.
        retained = json.loads(self.parent.read_bytes())
        self.assertEqual(retained["version"], 2)
        self.assertEqual(len(retained["characters"]), 1)
        self.assertEqual(retained["characters"][0]["id"], "deadline-pc")
        self.assertEqual(retained["characters"][0]["player_user_id"], self.user)

    def test_stale_duplicate_and_foreign_owner_cannot_overwrite(self):
        self.setup_turn(self.setup_body())
        self.setup_turn(self.setup_body(self.character(), 1))
        retained = self.parent.read_bytes()
        self.turn(self.setup_body(), completed=False)
        self.turn(self.setup_body(self.character(id="new"), 1), completed=False)
        self.turn(self.setup_body(self.character(name="Overwrite"), 2), completed=False)
        other, other_user = self.login()
        self.assertNotEqual(other_user, self.user)
        self.turn(
            self.setup_body(self.character(id="foreign"), 2),
            cookie=other,
            completed=False,
        )
        self.assertEqual(self.parent.read_bytes(), retained)

    def test_edge_rejects_forged_owner_unknown_fields_and_mixed_commands(self):
        good = self.setup_body(self.character(), 1)
        cases = []
        for key, value in (
            ("player_user_id", "victim"),
            ("allowed_knowledge_scopes", ["owned_rulebook"]),
            ("max_hp", 100001),
        ):
            body = copy.deepcopy(good)
            body["dnd_campaign"]["character"][key] = value
            cases.append(body)
        body = copy.deepcopy(good)
        body["dnd_initiative"] = self.body()["dnd_initiative"]
        cases.append(body)
        for body in cases:
            status, _, _ = self.http(
                self.product_port, "POST", "/api/turns", body, self.cookie
            )
            self.assertEqual(status, 400)
        self.assertFalse(self.parent.exists())
        body = self.setup_body()
        body["message"] = "Hello"
        self.turn(body, completed=False)
        self.assertFalse(self.parent.exists())

    @unittest.skipUnless(
        os.environ.get("DND_PRODUCT_BROWSER") == "1",
        "optional installed Chromium proof",
    )
    def test_served_browser_creates_campaign_and_characters_without_fixture_state(self):
        self.assertFalse(self.parent.exists())
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
                {"url": f"http://127.0.0.1:{self.product_port}", "cookie": self.cookie}
            ),
            text=True,
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        final = json.loads(process.stdout)
        _, saved = self.receipt(final)
        self.assertEqual(
            final["dnd_initiative_result"]["rolls"], saved["initiative_rolls"]
        )
        self.assertEqual(len(saved["participants"]), 2)
        state = json.loads(self.parent.read_bytes())
        self.assertEqual(state["version"], 3)
        self.assertEqual(state["owner_user_id"], self.user)
        self.assertEqual(state["characters"][0]["player_user_id"], self.user)


if __name__ == "__main__":
    unittest.main()
