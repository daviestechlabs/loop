"""Presence repair through authenticated product, C cascade, tool owner, and speech."""

import base64
import hashlib
import json
import unittest
import uuid

import test_dnd_product as product


class ProductScene(product.ProductProcess):
    seed_roster = True
    proxy = product.initiative.client.DiceClient.proxy

    def install_scene(
        self, *, presence="present", visibility="table", stale=False, duplicate=False
    ):
        characters = [
            product.initiative.CAMPAIGN.character(
                id="npc", name="Mira", safety_rules=[]
            )
        ]
        if duplicate:
            characters.append(
                product.initiative.CAMPAIGN.character(
                    id="other", name="mira", safety_rules=[]
                )
            )
        self.install_roster(characters)
        retained = json.loads(self.parent.read_text())
        retained["version"] = 4 if stale else 3
        retained["scene_observations"] = {
            "version": 3,
            "scene_id": "hall",
            "entries": [
                {
                    "character_id": "npc",
                    "presence": presence,
                    "visibility": visibility,
                    "source_turn_id": "mira-enters-hall",
                }
            ],
        }
        self.parent.write_text(json.dumps(retained))
        return self.parent.read_bytes()

    def question(
        self,
        *,
        scene="hall",
        message="I cast fireball, no wait is Mira still in the room?",
    ):
        return {
            "message": message,
            "request_id": str(uuid.uuid4()),
            "conversation_id": "scene-room",
            "enable_tts": False,
            "metadata": {
                "interaction_profile": "dnd_app",
                "campaign_id": "table",
                "scene_id": scene,
                "knowledge_scope": "shared_rulebook",
                "retrieval_force": "true",
            },
        }

    def assert_reply(self, body, expected):
        before = self.parent.read_bytes()
        final = self.turn(body)
        self.assertEqual(final["text"], expected)
        self.assertEqual(final["metadata"]["cascade_tool_id"], "dnd-scene-presence")
        self.assertTrue(final["metadata"]["cascade_tool_call_id"].startswith("scene-"))
        call, output = self.receipt(final)
        self.assertEqual(call["parent_turn_id"], body["request_id"])
        self.assertEqual(output["operation"], "resolve_scene_presence")
        self.assertEqual(output["character_name"], "Mira")
        self.assertEqual(output["scene_id"], body["metadata"]["scene_id"])
        self.assertEqual(self.parent.read_bytes(), before)
        self.assertFalse((self.host.root / "dnd/encounters/table/battle.json").exists())
        for field in (
            "dnd_campaign_roster",
            "dnd_encounter_state",
            "dnd_initiative_result",
            "dnd_retrieval",
        ):
            self.assertNotIn(field, final)
        return final, call

    def test_fireball_repair_speaks_only_current_presence_with_signed_receipt(self):
        self.install_scene()
        requests, expected_pcm = self.start_tts()
        body = self.question()
        del body["enable_tts"]
        final, _ = self.assert_reply(
            body, "The current scene record places Mira in this room."
        )
        provider = [r for r in requests if r["turn_id"] == body["request_id"]]
        chunks = [e for e in self.last_events if e["type"] == "pcm_chunk"]
        self.assertTrue(provider and chunks)
        self.assertEqual(
            b"".join(
                base64.b64decode(c["audio_base64"], validate=True) for c in chunks
            ),
            expected_pcm * len(provider),
        )
        self.assertEqual(sum(c.get("is_final", False) for c in chunks), 1)
        self.assertNotIn("fireball", final["text"].lower())

    def test_presence_request_forms_keep_authenticated_receipt_and_no_mutation(self):
        self.install_scene()
        for message in (
            "Can you tell me whether Mira is still here?",
            "Check whether Mira is in this room.",
            "Do we know if Mira is still here?",
            "Please can you check if Mira is in the room?",
            "Tell me whether Mira is here.",
            "Before I cast Fireball, is Mira here?",
            "I cast Fireball. Actually, wait—can you check if Mira is here?",
        ):
            with self.subTest(message=message):
                self.assert_reply(
                    self.question(message=message),
                    "The current scene record places Mira in this room.",
                )

    def test_absence_is_distinct_from_stale_private_ambiguous_and_wrong_scene(self):
        self.install_scene(presence="absent")
        self.assert_reply(
            self.question(), "The current scene record says Mira is not in this room."
        )
        for settings in (
            {"stale": True},
            {"visibility": "dm"},
            {"duplicate": True},
            {"presence": "unknown"},
        ):
            with self.subTest(settings=settings):
                self.install_scene(**settings)
                body = self.question(message="Can you check if Mira is in this room?")
                # Each fixture resets campaign versions. Use separate conversations
                # instead of making a retained revision move backward in one session.
                body["conversation_id"] = "fixture-" + str(uuid.uuid4())
                self.assert_reply(
                    body, "I can't confirm whether Mira is in this room."
                )
        self.install_scene()
        self.assert_reply(
            self.question(scene="courtyard"),
            "I can't confirm whether Mira is in this room.",
        )

    def test_repeated_parent_id_uses_fresh_read_after_state_changes(self):
        self.install_scene()
        body = self.question()
        _, first = self.assert_reply(
            body, "The current scene record places Mira in this room."
        )
        self.install_scene(stale=True)
        # Restart only the routing processes to remove transport replay state.
        # Durable tool receipts remain, so a reused tool ID would expose the old answer.
        self.stop("cascade")
        self.stop("gateway")
        self.start(
            "cascade", product.RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN"
        )
        self.wait_for(lambda: "pure-C direct admission" in self.log("cascade"))
        self.start(
            "gateway", product.RUNTIME / "c-voice-session-gateway", "GATEWAY_BIN"
        )
        self.wait_for(lambda: self.http(self.gateway_port, "GET", "/healthz")[0] == 200)
        # Product may retain a request cache; restart it with the same signing key.
        self.stop("product")
        self.start_product()
        _, current = self.assert_reply(
            body, "I can't confirm whether Mira is in this room."
        )
        self.assertNotEqual(first["tool_call_id"], current["tool_call_id"])

    def test_other_owner_missing_scene_and_attached_mutation_fail_without_write(self):
        before = self.install_scene()
        cookie, _ = self.login()
        self.turn(self.question(), cookie=cookie, completed=False)
        body = self.question()
        del body["metadata"]["scene_id"]
        self.turn(body, completed=False)
        body = self.body()
        body["message"] = self.question()["message"]
        body["metadata"]["scene_id"] = "hall"
        self.turn(body, completed=False)
        self.assertEqual(self.parent.read_bytes(), before)
        self.assertFalse((self.host.root / "dnd/encounters/table/battle.json").exists())

    def test_validly_signed_but_mismatched_output_never_becomes_speech(self):
        before = self.install_scene()
        for changes in (
            {"scene_id": "courtyard"},
            {"character_name": "Eli"},
            {"status": "archived"},
            {"presence": "unknown"},
            {"source_turn_id": ""},
        ):

            def corrupt(payload, changes=changes):
                call = payload["call"]
                output = json.loads(call["output_json"])
                output.update(changes)
                call["output_json"] = json.dumps(output, separators=(",", ":"))
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
                lambda ready=ready: (
                    self.log("cascade").count("pure-C direct admission") > ready
                )
            )
            self.turn(self.question(), completed=False)
            self.assertFalse(any(e["type"] == "pcm_chunk" for e in self.last_events))
            self.assertEqual(self.parent.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
