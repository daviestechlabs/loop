"""Authenticated C turns preserve proposals without granting spell execution."""

import base64
import http.server
from concurrent.futures import ThreadPoolExecutor
import json
import threading
import unittest

import test_dnd_product as product
import test_dnd_scene_product as scene


class ProductDialogue(product.ProductProcess):
    seed_roster = True
    install_scene = scene.ProductScene.install_scene
    question = scene.ProductScene.question
    proxy = scene.ProductScene.proxy

    def restart_cascade(self):
        ready = self.log("cascade").count("pure-C direct admission")
        self.stop("cascade")
        self.start(
            "cascade", product.RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN"
        )
        self.wait_for(
            lambda: self.log("cascade").count("pure-C direct admission") > ready
        )

    def prepare(self):
        before = self.install_scene()
        result = self.turn(self.question())
        self.assertEqual(
            result["text"], "The current scene record places Mira in this room."
        )
        self.assertEqual(self.parent.read_bytes(), before)
        return before

    def followup(self, message, expected, **changes):
        body = self.question(message=message)
        body.update(changes)
        result = self.turn(body)
        self.assertEqual(result["text"], expected)
        self.assertNotIn("cascade_tool_call_id", result.get("metadata", {}))
        return result

    def test_recall_refuse_execution_and_cancel_without_game_mutation(self):
        before = self.prepare()
        self.followup("What spell am I holding?", "Your fireball proposal is on hold.")
        for message in ("yes", "Okay, cast it now."):
            self.followup(
                message,
                "Your fireball proposal is on hold. This voice path cannot execute spells.",
            )
        self.followup(
            "Never mind, cancel that spell.", "Canceled your fireball proposal."
        )
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)
        self.assertFalse((self.host.root / "dnd/encounters/table/battle.json").exists())

    def test_request_form_retains_proposal_until_cancel_and_never_executes(self):
        before = self.install_scene()
        body = self.question(
            message="I cast Moon Ward. Actually, wait—can you check if Mira is here?"
        )
        result = self.turn(body)
        self.assertEqual(result["text"], "The current scene record places Mira in this room.")
        _, receipt = self.receipt(result)
        self.assertEqual(receipt["operation"], "resolve_scene_presence")
        self.followup("What spell am I holding?", "Your Moon Ward proposal is on hold.")
        self.followup(
            "Okay, cast it now.",
            "Your Moon Ward proposal is on hold. This voice path cannot execute spells.",
        )
        self.followup("Cancel that spell.", "Canceled your Moon Ward proposal.")
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)
        self.assertFalse((self.host.root / "dnd/encounters/table/battle.json").exists())

    def test_owner_session_and_campaign_isolation(self):
        before = self.prepare()
        cookie, _ = self.login()
        result = self.turn(
            self.question(message="What spell am I holding?"), cookie=cookie
        )
        self.assertEqual(
            result["text"], "No spell proposal is on hold in this session."
        )
        self.followup(
            "What spell am I holding?",
            "No spell proposal is on hold in this session.",
            conversation_id="different-conversation",
        )
        self.followup("What spell am I holding?", "Your fireball proposal is on hold.")
        body = self.question(message="What spell am I holding?")
        body["metadata"]["campaign_id"] = "different-campaign"
        self.assertEqual(
            self.turn(body)["text"], "No spell proposal is on hold in this session."
        )
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def start_model(
        self,
        answer="Please choose a spell origin.",
        finish_reason="stop",
        before_write=None,
    ):
        requests = []

        class Model(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                requests.append(
                    json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                )
                events = [
                    {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"content": answer},
                                "finish_reason": None,
                            }
                        ]
                    },
                    {
                        "choices": [
                            {"index": 0, "delta": {}, "finish_reason": finish_reason}
                        ]
                    },
                ]
                data = (
                    "".join("data: " + json.dumps(e) + "\n\n" for e in events)
                    + "data: [DONE]\n\n"
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if before_write:
                    before_write()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            thread.join(timeout=3)
            server.server_close()

        self.addCleanup(stop)
        self.environment["LLM_HTTP_URL"] = (
            f"http://127.0.0.1:{server.server_port}/v1/chat/completions"
        )
        self.restart_cascade()
        return requests

    def test_current_model_input_contains_reference_not_cached_scene_facts(self):
        requests = self.start_model()
        self.prepare()
        state = json.loads(self.parent.read_text())
        state["version"] = 4
        state["scene_observations"]["version"] = 4
        self.parent.write_text(json.dumps(state))
        self.turn(self.question(message="Is Mira still in the room?"))
        body = self.question(message="Would it hit her?")
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        self.assertEqual(self.turn(body)["text"], "Please choose a spell origin.")
        self.assertEqual(len(requests), 1)
        current = requests[0]["messages"][-1]["content"]
        self.assertIn('"held_spell":"fireball"', current)
        self.assertIn('"last_question_character":"Mira"', current)
        self.assertIn('"scene_context_changed":true', current)
        self.assertIn("not scene evidence or an execution receipt", current)
        self.assertIn("Current input:\nWould it hit her?", current)
        self.assertNotIn("current scene record places", current)

    def test_pronoun_scene_question_reaches_model_with_prior_reference(self):
        requests = self.start_model()
        before = self.prepare()
        for message in (
            "Is she out of its area over here?",
            "Is she still here?",
            "Is Mira outside the blast area here?",
        ):
            body = self.question(message=message, scene="courtyard")
            body["metadata"].pop("knowledge_scope")
            body["metadata"].pop("retrieval_force")
            result = self.turn(body)
            self.assertEqual(result["text"], "Please choose a spell origin.")
            self.assertNotIn("cascade_tool_call_id", result.get("metadata", {}))
            content = requests[-1]["messages"][-1]["content"]
            self.assertIn('"held_spell":"fireball"', content)
            self.assertIn('"last_question_character":"Mira"', content)
            self.assertIn('"scene_context_changed":true', content)
            self.assertTrue(content.endswith("Current input:\n" + message))
        self.assertEqual(len(requests), 3)
        self.assertEqual(self.parent.read_bytes(), before)

    def test_late_scene_result_cannot_restore_canceled_proposal(self):
        before = self.install_scene()
        contacted, release = threading.Event(), threading.Event()
        self.environment["TOOL_HTTP_URL"] = self.proxy(
            hold=release, contacted=contacted
        )
        self.restart_cascade()
        body = self.question()
        replies = []
        errors = []

        def send():
            try:
                replies.append(
                    self.http(
                        self.product_port, "POST", "/api/turns", body, self.cookie
                    )
                )
            except Exception as error:
                errors.append(error)

        thread = threading.Thread(target=send)
        thread.start()
        self.addCleanup(release.set)
        self.assertTrue(contacted.wait(timeout=3), self.log("cascade"))
        self.followup("Cancel that spell.", "Canceled your fireball proposal.")
        release.set()
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertFalse(errors, errors)
        self.assertEqual(len(replies), 1)
        self.assertEqual(replies[0][0], 200)
        events = [json.loads(line) for line in replies[0][1].splitlines() if line]
        self.assertFalse(
            any(e["type"] in ("text_completed", "pcm_chunk") for e in events), events
        )
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_cancel_then_question_clears_state_and_delivers_receipt_to_model(self):
        requests = self.start_model()
        before = self.prepare()
        body = self.question(
            message="Actually, drop the idea. What should I do instead?"
        )
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        result = self.turn(body)
        self.assertEqual(
            result["text"],
            "Canceled your fireball proposal. Please choose a spell origin.",
        )
        self.assertEqual(len(requests), 1)
        content = requests[0]["messages"][-1]["content"]
        self.assertIn('"canceled_proposal":"fireball"', content)
        self.assertIn('"held_spell":""', content)
        self.assertIn("Current input:\n" + body["message"], content)
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_quoted_negated_and_conditional_requests_keep_proposal(self):
        self.start_model()
        before = self.prepare()
        for message in (
            "Don't drop the idea.",
            "If Mira returns, cancel that spell.",
            'Mira said "drop the idea." What does she mean?',
            "Cancel that spell only if Mira is here.",
            "If I forgot that spell, could we talk our way past them instead?",
            "Don't forget the spell. Could we talk instead?",
            "Mira said forget the spell.",
        ):
            body = self.question(message=message)
            body["metadata"].pop("knowledge_scope")
            body["metadata"].pop("retrieval_force")
            result = self.turn(body)
            self.assertEqual(result["text"], "Please choose a spell origin.")
            self.followup(
                "What spell am I holding?", "Your fireball proposal is on hold."
            )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_forget_proposal_clears_state_before_negotiation_question(self):
        answer = "What do you know about the people blocking your way?"
        requests = self.start_model(answer=answer)
        cancellation = "Canceled your fireball proposal."
        cancellation_pcm = b"\x01\x00" * 1440
        answer_pcm = b"\x02\x00" * 1440
        answer_written = threading.Event()
        provider_order = []

        def pcm_for_request(request):
            text = request["text"].strip()
            if text == cancellation:
                self.assertTrue(answer_written.wait(timeout=3), "TTS did not prefetch the answer")
                return cancellation_pcm
            self.assertEqual(text, answer)
            return answer_pcm

        def after_write(request):
            text = request["text"].strip()
            provider_order.append(text)
            if text == answer:
                answer_written.set()

        speech, _ = self.start_tts(pcm_for_request=pcm_for_request, after_write=after_write)
        before = self.prepare()
        message = "Forget the spell. Could we talk our way past them instead?"
        body = self.question(message=message)
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        body["enable_tts"] = True
        result = self.turn(body)
        self.assertEqual(result["text"], "Canceled your fireball proposal. " + answer)
        # Synthesis may run concurrently. Provider arrival order is not playback
        # order. Give each segment distinct PCM and force the second response
        # to finish first, then verify the actual public audio stream.
        self.assertCountEqual([r["text"].strip() for r in speech], [cancellation, answer])
        self.assertEqual(provider_order, [answer, cancellation])
        chunks = [e for e in self.last_events if e["type"] == "pcm_chunk"]
        self.assertTrue(chunks)
        self.assertEqual(
            b"".join(base64.b64decode(c["audio_base64"], validate=True) for c in chunks),
            cancellation_pcm + answer_pcm,
        )
        self.assertEqual(sum(c.get("is_final", False) for c in chunks), 1)
        self.assertEqual(len(requests), 1)
        content = requests[0]["messages"][-1]["content"]
        self.assertIn('"canceled_proposal":"fireball"', content)
        self.assertIn('"held_spell":""', content)
        self.assertTrue(content.endswith("Current input:\n" + message))
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_cancel_then_question_without_prior_spell_does_not_invent_receipt(self):
        requests = self.start_model()
        before = self.install_scene()
        body = self.question(message="Forget the spell. Could we talk instead?")
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        result = self.turn(body)
        self.assertEqual(
            result["text"],
            "No spell proposal is on hold in this session. Please choose a spell origin.",
        )
        self.assertEqual(len(requests), 1)
        self.assertIn('"canceled_proposal":""', requests[0]["messages"][-1]["content"])
        self.assertEqual(self.parent.read_bytes(), before)

    def test_cancel_then_empty_model_response_fails_without_receipt_only_success(self):
        self.assert_empty_cancel_answer_fails("")

    def test_cancel_then_whitespace_response_fails_without_receipt_only_success(self):
        self.assert_empty_cancel_answer_fails(" \n\t ")

    def assert_empty_cancel_answer_fails(self, answer):
        self.start_model(answer=answer)
        before = self.prepare()
        body = self.question(message="Forget the spell. Could we talk instead?")
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        self.turn(body, completed=False)
        self.assertFalse(any(e.get("text") for e in self.last_events))
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_cancel_then_truncated_answer_remains_failed_with_cleared_state(self):
        self.start_model(finish_reason="length")
        before = self.prepare()
        body = self.question(message="Forget the spell. Could we talk instead?")
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        self.turn(body, completed=False)
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_superseded_cancel_question_does_not_publish_delayed_receipt(self):
        contacted, release = threading.Event(), threading.Event()

        def hold_model():
            contacted.set()
            release.wait(timeout=5)

        self.start_model(before_write=hold_model)
        before = self.prepare()
        body = self.question(message="Forget the spell. Could we talk instead?")
        body["metadata"].pop("knowledge_scope")
        body["metadata"].pop("retrieval_force")
        with ThreadPoolExecutor(max_workers=1) as worker:
            pending = worker.submit(
                self.http, self.product_port, "POST", "/api/turns", body, self.cookie
            )
            try:
                self.assertTrue(contacted.wait(timeout=3), self.log("cascade"))
                self.followup(
                    "What spell am I holding?",
                    "No spell proposal is on hold in this session.",
                )
                release.set()
                status, response, _ = pending.result(timeout=3)
                self.assertEqual(status, 200, response)
                events = [json.loads(line) for line in response.splitlines() if line]
                self.assertTrue(events)
                self.assertFalse(
                    any(
                        e["type"] in ("completed", "text_completed", "pcm_chunk")
                        or e.get("text")
                        for e in events
                    ),
                    events,
                )
            finally:
                release.set()
        self.assertEqual(self.parent.read_bytes(), before)

    def test_transport_interrupt_preserves_proposal_and_rejects_delayed_answer(self):
        before = self.install_scene()
        contacted, release = threading.Event(), threading.Event()
        self.environment["TOOL_HTTP_URL"] = self.proxy(
            hold=release, contacted=contacted
        )
        self.restart_cascade()
        body = self.question()
        with ThreadPoolExecutor(max_workers=1) as worker:
            pending = worker.submit(
                self.http, self.product_port, "POST", "/api/turns", body, self.cookie
            )
            try:
                self.assertTrue(contacted.wait(timeout=3), self.log("cascade"))
                status, response, _ = self.http(
                    self.product_port,
                    "POST",
                    "/api/turns/cancel",
                    {"request_id": body["request_id"]},
                    self.cookie,
                )
                self.assertEqual(status, 200, response)
                self.assertTrue(json.loads(response)["cancelled"])
                status, response, _ = pending.result(timeout=3)
                self.assertEqual(status, 200, response)
                events = [json.loads(line) for line in response.splitlines() if line]
                self.assertTrue(events)
                self.assertTrue(
                    all(e["request_id"] == body["request_id"] for e in events)
                )
                self.assertFalse(
                    any(
                        e["type"] in ("completed", "text_completed", "pcm_chunk")
                        for e in events
                    ),
                    events,
                )
            finally:
                release.set()
        self.followup("What spell am I holding?", "Your fireball proposal is on hold.")
        self.assertEqual(self.parent.read_bytes(), before)

    def test_transport_interrupt_while_speech_waits_preserves_proposal(self):
        before = self.prepare()
        contacted, release = threading.Event(), threading.Event()

        def hold_audio():
            contacted.set()
            release.wait(timeout=5)

        self.start_tts(before_write=hold_audio)
        body = self.question(message="What spell am I holding?")
        body["enable_tts"] = True
        with ThreadPoolExecutor(max_workers=1) as worker:
            pending = worker.submit(
                self.http, self.product_port, "POST", "/api/turns", body, self.cookie
            )
            try:
                self.assertTrue(contacted.wait(timeout=3), self.log("tts"))
                status, response, _ = self.http(
                    self.product_port,
                    "POST",
                    "/api/turns/cancel",
                    {"request_id": body["request_id"]},
                    self.cookie,
                )
                self.assertEqual(status, 200, response)
                self.assertTrue(json.loads(response)["cancelled"])
                release.set()
                status, response, _ = pending.result(timeout=3)
                self.assertEqual(status, 200, response)
                events = [json.loads(line) for line in response.splitlines() if line]
                self.assertTrue(events)
                self.assertFalse(
                    any(e["type"] in ("completed", "pcm_chunk") for e in events),
                    events,
                )
            finally:
                release.set()
        self.followup("What spell am I holding?", "Your fireball proposal is on hold.")
        self.assertEqual(self.parent.read_bytes(), before)

    def test_new_turn_cancels_previous_queued_speech_after_cascade_finishes(self):
        before = self.prepare()
        contacted, release = threading.Event(), threading.Event()

        def hold_audio():
            contacted.set()
            release.wait(timeout=5)

        self.start_tts(before_write=hold_audio)
        body = self.question(message="What spell am I holding?")
        body["enable_tts"] = True
        with ThreadPoolExecutor(max_workers=1) as worker:
            pending = worker.submit(
                self.http, self.product_port, "POST", "/api/turns", body, self.cookie
            )
            try:
                self.assertTrue(contacted.wait(timeout=3), self.log("tts"))
                self.followup("Cancel that spell.", "Canceled your fireball proposal.")
                status, response, _ = pending.result(timeout=3)
                self.assertEqual(status, 200, response)
                events = [json.loads(line) for line in response.splitlines() if line]
                self.assertTrue(events)
                self.assertFalse(
                    any(e["type"] in ("completed", "pcm_chunk") for e in events),
                    events,
                )
            finally:
                release.set()
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_restart_discards_proposal_and_attached_mutation_cannot_cancel_it(self):
        before = self.prepare()
        body = self.body()
        body["conversation_id"] = "scene-room"
        body["message"] = "Cancel that spell."
        body["metadata"]["scene_id"] = "hall"
        self.turn(body, completed=False)
        self.followup("What spell am I holding?", "Your fireball proposal is on hold.")
        self.restart_cascade()
        self.followup(
            "What spell am I holding?", "No spell proposal is on hold in this session."
        )
        self.assertEqual(self.parent.read_bytes(), before)

    def test_dialogue_speech_has_matching_text_and_final_audio(self):
        self.prepare()
        requests, _ = self.start_tts()
        body = self.question(message="What spell am I holding?")
        body["enable_tts"] = True
        final = self.turn(body)
        self.assertEqual(final["text"], "Your fireball proposal is on hold.")
        self.assertTrue(any(r["turn_id"] == body["request_id"] for r in requests))
        chunks = [e for e in self.last_events if e["type"] == "pcm_chunk"]
        self.assertTrue(chunks)
        self.assertEqual(sum(c.get("is_final", False) for c in chunks), 1)


if __name__ == "__main__":
    unittest.main()
