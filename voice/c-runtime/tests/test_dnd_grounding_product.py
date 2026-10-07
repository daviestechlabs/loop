"""Real C login, signed turn, indexed retrieval, generation and public citations.

Python supplies isolated model and Milvus fixtures. The rule text is fictional.
"""

import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import signal
import subprocess
import threading
import unittest
import uuid

import test_dnd_product as product
import test_rag_source as source


QUESTION = "How often can I use Warding Step in a round?"
ANSWER = (
    "Warding Step works once per turn when you see the target and remain within "
    "15 feet. A reaction can occur on another creature's turn in the same round."
)


class ProductGrounding(product.ProductProcess):
    def setUp(self):
        super().setUp()
        self.stop("cascade")
        self.path = self.host.root / "source"
        self.path.mkdir()
        subprocess.run(
            [str(source.RUNTIME / ".build/dnd-source-artifact/emit"), str(self.path)],
            check=True,
            capture_output=True,
        )
        self.records = json.loads((self.path / "records.json").read_text())
        self.rows = {record["record_id"]: source.row(record) for record in self.records}
        self.baseline = [self.rows[self.records[0]["record_id"]]]
        self.mutation = None
        origin, self.searches = self.enterContext(
            source.backend(
                self.baseline,
                self.rows,
                mutate=lambda hits: self.mutation(hits) if self.mutation else hits,
            )
        )
        self.backend_origin = origin
        # Copy only backend settings, never ambient production destinations.
        settings = source.source_environment(self.path, origin)
        self.environment.update(
            (key, value)
            for key, value in settings.items()
            if key.startswith(("RAG_", "MILVUS_", "EMBED_", "EMBEDDING_"))
        )
        self.model_requests = []
        self.finish_reason = "stop"
        self.model_started = threading.Event()
        self.model_release = threading.Event()
        self.model_release.set()
        self.start_model()
        self.environment["PROMPT_LIBRARY_ROOT"] = str(
            product.ROOT / "contracts/prompt-library"
        )
        self.start("rag", source.RUNTIME / "c-rag-gateway", "RAG_GATEWAY_BIN")
        self.wait_for(lambda: "enabled=1 source_index=1" in self.log("rag"))
        self.start("cascade", source.RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN")
        self.wait_for(lambda: self.log("cascade").count("pure-C direct admission") == 2)

    def start_model(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                size = int(self.headers["Content-Length"])
                assert 0 < size <= 65536
                owner.model_requests.append(json.loads(self.rfile.read(size)))
                owner.model_started.set()
                if not owner.model_release.wait(timeout=4):
                    return
                deltas = ["**Warding", " Step**", ANSWER[len("Warding Step") :]]
                if owner.finish_reason != "stop":
                    deltas.append(" **unfinished_marker")
                frames = [
                    {"choices": [{"delta": {"content": delta}, "finish_reason": None}]}
                    for delta in deltas
                ]
                frames.append(
                    {"choices": [{"delta": {}, "finish_reason": owner.finish_reason}]}
                )
                encoded = (
                    "".join("data: " + json.dumps(frame) + "\n\n" for frame in frames)
                    + "data: [DONE]\n\n"
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                try:
                    self.wfile.write(encoded)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        worker = threading.Thread(
            target=lambda: server.serve_forever(poll_interval=0.01), daemon=True
        )
        worker.start()

        def close():
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)

        self.addCleanup(close)
        self.environment["LLM_HTTP_URL"] = (
            f"http://127.0.0.1:{server.server_port}/v1/chat/completions"
        )

    def body(self):
        return {
            "message": QUESTION,
            "request_id": str(uuid.uuid4()),
            "enable_tts": False,
            "enable_rag": False,
            "metadata": {
                "interaction_profile": "dnd_app",
                "knowledge_scope": "shared_rulebook",
                "retrieval_force": "true",
                "campaign_id": "table",
            },
        }

    def assert_grounding(self, final):
        self.assertEqual(final["text"], ANSWER)
        self.assertEqual(len(self.model_requests), 1)
        request = self.model_requests[0]
        self.assertTrue(request["stream"])
        messages = request["messages"]
        self.assertEqual([m["role"] for m in messages], ["system", "user"])
        canonical = (
            (
                product.ROOT
                / "contracts/prompt-library/prompts/rag/grounded-response.system.txt"
            )
            .read_text()
            .rstrip("\r\n")
        )
        style = (
            (
                product.ROOT
                / "contracts/prompt-library/prompts/voice/product-response-style.system.txt"
            )
            .read_text()
            .strip()
        )
        self.assertEqual(
            messages[0]["content"],
            style
            + "\n\n"
            + (
                product.ROOT
                / "contracts/prompt-library/prompts/voice/dnd-dialogue.system.txt"
            )
            .read_text()
            .strip()
            + "\n\n"
            + canonical,
        )
        self.assertIn(QUESTION, messages[1]["content"])
        for record in self.records[:3]:
            self.assertIn(record["content"], messages[1]["content"])
            self.assertIn(record["record_id"], messages[1]["content"])
        metadata = final["metadata"]
        self.assertEqual(metadata["cascade_route"], "retrieve_then_escalate")
        self.assertEqual(metadata["cascade_retrieval_used"], "true")
        self.assertEqual(metadata["cascade_retrieved_documents"], "3")
        self.assertEqual(
            metadata["cascade_grounding_prompt_sha256"],
            hashlib.sha256(canonical.encode()).hexdigest(),
        )
        citations = json.loads(metadata["cascade_retrieval_citations"])
        self.assertEqual(len(citations), 3)
        for citation, record in zip(citations, self.records[:3], strict=True):
            for key in (
                "record_id",
                "document_id",
                "book_slug",
                "content_hash",
                "source_sha256",
            ):
                self.assertEqual(citation[key], record[key])
            self.assertEqual(citation["page_start"], record["page"])
            self.assertEqual(citation["page_end"], record["page"])
            self.assertEqual(citation["chunk_index"], record["chunk"])
            self.assertEqual(citation["corpus_version"], source.CORPUS)
            self.assertEqual(citation["collection"], source.COLLECTION)
            self.assertEqual(citation["score_metric"], "none")
            self.assertEqual(citation["score"], 0)
            self.assertNotIn("content", citation)
            self.assertNotIn("owner_user_id", citation)
        self.assertEqual(len(self.searches), 2)
        for path, authorization, query in self.searches:
            self.assertIn(path.rsplit("/", 1)[-1], ("search", "query"))
            self.assertEqual(authorization, "Bearer fixture-token")
            self.assertEqual(query["collectionName"], source.COLLECTION)
            for required in (
                'owner_user_id == ""',
                'visibility == "public"',
                source.CORPUS,
                'ruleset == "dnd-5e-2014"',
            ):
                self.assertIn(required, query["filter"])
            self.assertNotIn("tashas-cauldron-of-everything", query["filter"])

    def test_direct_answer_uses_shared_style_without_retrieval(self):
        body = self.body()
        body["message"] = "Describe a mysterious tavern in two short sentences."
        body["metadata"] = {"interaction_profile": "dnd_app"}
        self.turn(body)
        self.assertEqual(len(self.model_requests), 1)
        messages = self.model_requests[0]["messages"]
        style = (
            (
                product.ROOT
                / "contracts/prompt-library/prompts/voice/product-response-style.system.txt"
            )
            .read_text()
            .strip()
        )
        self.assertEqual(
            messages,
            [
                {
                    "role": "system",
                    "content": style
                    + "\n\n"
                    + (
                        product.ROOT
                        / "contracts/prompt-library/prompts/voice/dnd-dialogue.system.txt"
                    )
                    .read_text()
                    .strip(),
                },
                {"role": "user", "content": body["message"]},
            ],
        )
        self.assertFalse(self.searches)

    def test_general_profile_keeps_generic_style(self):
        body = self.body()
        body["message"] = "Describe a rainy forest in two sentences."
        body["metadata"] = {}
        self.turn(body)
        style = (
            (
                product.ROOT
                / "contracts/prompt-library/prompts/voice/product-response-style.system.txt"
            )
            .read_text()
            .strip()
        )
        self.assertEqual(
            self.model_requests[0]["messages"][0], {"role": "system", "content": style}
        )
        self.assertFalse(self.searches)

    def test_guest_login_to_indexed_sources_and_public_citations(self):
        self.assert_grounding(self.turn(self.body()))

    def configure_passages(self):
        self.stop("rag")
        for name in ("index.dndsidx", "manifest.json", "records.json"):
            (self.path / name).unlink()
        subprocess.run(
            [
                str(source.RUNTIME / ".build/dnd-source-artifact/emit"),
                str(self.path),
                "--passages",
            ],
            check=True,
            capture_output=True,
        )
        self.records = json.loads((self.path / "records.json").read_text())
        self.rows.clear()
        self.rows.update({r["record_id"]: source.row(r) for r in self.records})
        self.baseline[:] = [self.rows[self.records[4]["record_id"]]]
        self.environment.update(
            (k, v)
            for k, v in source.source_environment(
                self.path, self.backend_origin
            ).items()
            if k.startswith(("RAG_", "MILVUS_", "EMBED_", "EMBEDDING_"))
        )
        self.start("rag", source.RUNTIME / "c-rag-gateway", "RAG_GATEWAY_BIN")
        self.wait_for(lambda: self.log("rag").count("enabled=1 source_index=1") == 2)

    def test_complete_passages_reach_model_and_public_product_citations(self):
        self.configure_passages()
        body = self.body()
        body["message"] = "Can I cast Aegis or Beacon?"
        final = self.turn(body)
        self.assertEqual(len(self.model_requests), 1)
        prompt = self.model_requests[0]["messages"][1]["content"]
        citations = json.loads(final["metadata"]["cascade_retrieval_citations"])
        self.assertEqual(final["metadata"]["cascade_retrieved_documents"], "2")
        self.assertEqual([len(c["witnesses"]) for c in citations], [5, 1])
        for c in citations:
            self.assertEqual(c["kind"], "complete_passage")
            self.assertEqual(c["record_id"], "")
            assembled = bytearray()
            previous = None
            for w in c["witnesses"]:
                raw = self.rows[w["record_id"]]["content"].encode()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), w["content_hash"])
                self.assertEqual(len(raw), w["record_length"])
                if previous is not None and previous != w["page"]:
                    assembled.extend(b"\n")
                assembled.extend(raw[w["begin"] : w["end"]])
                previous = w["page"]
            self.assertEqual(hashlib.sha256(assembled).hexdigest(), c["content_hash"])
            self.assertIn(assembled.decode(), prompt)
            self.assertIn(c["passage_id"], prompt)
        self.assertNotIn("LAMPLIGHT", prompt)
        self.assertEqual(
            [q["limit"] for path, _, q in self.searches if path.endswith("/query")],
            [4, 2],
        )

    def test_failed_second_passage_batch_never_calls_model(self):
        self.configure_passages()
        calls = 0

        def corrupt(hits):
            nonlocal calls
            calls += 1
            if calls == 2:
                hits[0]["content"] += " forged"
            return hits

        self.mutation = corrupt
        body = self.body()
        body["message"] = "Can I cast Aegis or Beacon?"
        self.turn(body, completed=False)
        self.assertEqual(calls, 2)
        self.assertFalse(self.model_requests)

    def test_social_prefix_does_not_swallow_the_model_request(self):
        utterances = (
            "Thanks, is Mira still here",
            "Thanks, is Mira still here?",
            "Sounds good, roll initiative",
            "No thanks, do not cast fireball",
            "How are you tracking my spell slots?",
            "Hello, is Mira still here?",
            "unfamiliar neutral utterance",
        )
        for utterance in utterances:
            with self.subTest(utterance=utterance):
                before = len(self.model_requests)
                body = self.body()
                body["message"] = utterance
                body["metadata"] = {"interaction_profile": "dnd_app"}
                final = self.turn(body)
                self.assertEqual(final["text"], ANSWER)
                self.assertEqual(len(self.model_requests), before + 1)
                self.assertEqual(
                    self.model_requests[-1]["messages"][-1],
                    {"role": "user", "content": utterance},
                )
        self.assertFalse(self.searches)

    def test_complete_social_turn_keeps_canned_reply_without_model(self):
        for utterance, answer in (
            ("Thanks, that was helpful.", "You're welcome."),
            ("Sounds good!", "Got it."),
            ("How are you?", "Doing well — ready when you are."),
        ):
            with self.subTest(utterance=utterance):
                body = self.body()
                body["message"] = utterance
                body["metadata"] = {"interaction_profile": "dnd_app"}
                self.assertEqual(self.turn(body)["text"], answer)
        self.assertFalse(self.model_requests)
        self.assertFalse(self.searches)

    def test_grounded_speech_has_plain_text_pcm_and_matching_citations(self):
        provider, pcm = self.start_tts()
        body = self.body()
        body["enable_tts"] = True
        self.assert_grounding(self.turn(body))
        self.assertTrue(provider)
        self.assertTrue(all("*" not in call["text"] for call in provider))
        # Provider lanes may start out of order; publication carries segment indices.
        spoken = sorted((call["text"] for call in provider), key=ANSWER.index)
        self.assertEqual(" ".join(spoken), ANSWER)
        self.assertTrue(all(call["turn_id"] == body["request_id"] for call in provider))
        chunks = [event for event in self.last_events if event["type"] == "pcm_chunk"]
        self.assertTrue(chunks)
        self.assertEqual(sum(bool(chunk["is_final"]) for chunk in chunks), 1)
        self.assertTrue(chunks[-1]["is_final"])
        self.assertEqual(
            [chunk["segment_index"] for chunk in chunks],
            sorted(chunk["segment_index"] for chunk in chunks),
        )
        self.assertTrue(
            all(
                (chunk["sample_rate"], chunk["channels"], chunk["bit_depth"])
                == (24000, 1, 16)
                for chunk in chunks
            )
        )
        self.assertEqual(
            b"".join(
                base64.b64decode(chunk["audio_base64"], validate=True)
                for chunk in chunks
            ),
            pcm * len(provider),
        )

    def stalled_speech(self, stall_seconds, *, completed):
        process = self.processes["product"]
        guard = threading.Lock()
        timers = []

        def stall_once():
            with guard:
                if timers:
                    return
                os.kill(process.pid, signal.SIGSTOP)
                timer = threading.Timer(
                    stall_seconds, os.kill, (process.pid, signal.SIGCONT)
                )
                timers.append(timer)
                timer.start()

        try:
            provider, pcm = self.start_tts(
                pcm_bytes=2880 * 1024, before_write=stall_once
            )
            body = self.body()
            body["enable_tts"] = True
            if completed:
                final = self.turn(body)
            else:
                status, raw, _ = self.http(
                    self.product_port, "POST", "/api/turns", body, self.cookie
                )
                self.assertEqual(status, 200)
                self.last_events = [json.loads(line) for line in raw.splitlines()]
                self.assertTrue(self.last_events)
                self.assertTrue(
                    all(e["request_id"] == body["request_id"] for e in self.last_events)
                )
            self.assertEqual(len(timers), 1)
            chunks = [e for e in self.last_events if e["type"] == "pcm_chunk"]
            self.assertTrue(chunks)
            if completed:
                self.assert_grounding(final)
                self.assertEqual(len(provider), 2)
                self.assertEqual(
                    b"".join(
                        base64.b64decode(e["audio_base64"], validate=True)
                        for e in chunks
                    ),
                    pcm * len(provider),
                )
                self.assertEqual(sum(bool(e["is_final"]) for e in chunks), 1)
                self.assertTrue(chunks[-1]["is_final"])
                segments = sorted({e["segment_index"] for e in chunks})
                self.assertEqual(segments, list(range(len(provider))))
                for segment in segments:
                    sequence = [
                        e["sequence"] for e in chunks if e["segment_index"] == segment
                    ]
                    self.assertEqual(sequence, list(range(len(sequence))))
            else:
                self.assertEqual(self.last_events[-1]["type"], "failed")
                self.assertFalse(
                    any(e["type"] == "completed" for e in self.last_events)
                )
                self.assertEqual(
                    sum(e["type"] == "failed" for e in self.last_events), 1
                )
                self.assertEqual(
                    sum(e["type"] == "text_completed" for e in self.last_events), 1
                )
        finally:
            if process.poll() is None:
                os.kill(process.pid, signal.SIGCONT)
            for timer in timers:
                timer.join(timeout=3)

    def test_short_reader_stall_preserves_all_grounded_speech(self):
        self.stalled_speech(0.5, completed=True)

    def test_unresponsive_reader_has_one_failed_terminal_after_recovery(self):
        self.stalled_speech(1.5, completed=False)

    def test_changed_source_content_cannot_reach_model_or_final_answer(self):
        def corrupt(hits):
            hits[0]["content"] += " Untrusted changed rule."
            return hits

        self.mutation = corrupt
        self.turn(self.body(), completed=False)
        self.assertEqual(len(self.searches), 2)
        self.assertEqual(self.model_requests, [])

    def test_unentitled_backend_hit_cannot_be_used_by_free_guest(self):
        self.baseline[:] = [self.rows[self.records[3]["record_id"]]]
        body = self.body()
        body["message"] = "What is the Iron Badger's Armor Class?"
        body["enable_rag"] = True
        self.turn(body, completed=False)
        self.assertEqual(len(self.searches), 1)
        self.assertNotIn("tashas-cauldron-of-everything", self.searches[0][2]["filter"])
        self.assertEqual(self.model_requests, [])

    def test_exact_fetch_rejects_changed_owner_ruleset_or_corpus(self):
        for changes in (
            {"owner_user_id": "another-user"},
            {"ruleset": "dnd-5e-2024"},
            {"corpus_version": "sha256:" + "f" * 64},
        ):
            with self.subTest(changes=changes):

                def corrupt(hits):
                    source.change_metadata(hits[0], **changes)
                    return hits

                self.mutation = corrupt
                self.searches.clear()
                self.turn(self.body(), completed=False)
                self.assertEqual(len(self.searches), 2)
                self.assertEqual(self.model_requests, [])

    def test_exhausted_model_does_not_publish_final_text_or_sources(self):
        self.finish_reason = "length"
        self.turn(self.body(), completed=False)
        self.assertEqual(len(self.model_requests), 1)
        self.assertTrue(
            all(
                "cascade_retrieval_citations" not in event.get("metadata", {})
                for event in self.last_events
            )
        )
        self.assertNotIn("unfinished_marker", json.dumps(self.last_events))

    def test_anonymous_and_forged_identity_fail_before_retrieval(self):
        status, _, _ = self.http(self.product_port, "POST", "/api/turns", self.body())
        self.assertEqual(status, 401)
        for key, value in (("user_id", "another-user"), ("premium", True)):
            body = self.body()
            body[key] = value
            status, _, _ = self.http(
                self.product_port, "POST", "/api/turns", body, self.cookie
            )
            self.assertEqual(status, 400)
        self.assertEqual(self.searches, [])
        self.assertEqual(self.model_requests, [])

    def test_cancel_during_generation_does_not_publish_answer_or_sources(self):
        body = self.body()
        self.model_release.clear()
        with ThreadPoolExecutor(max_workers=1) as worker:
            pending = worker.submit(
                self.http, self.product_port, "POST", "/api/turns", body, self.cookie
            )
            try:
                self.assertTrue(self.model_started.wait(timeout=3))
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
                    all(event["request_id"] == body["request_id"] for event in events)
                )
                self.assertTrue(
                    all(
                        event["type"]
                        not in ("completed", "text_completed", "pcm_chunk")
                        for event in events
                    )
                )
                self.assertNotIn("cascade_retrieval_citations", response.decode())
            finally:
                self.model_release.set()


if __name__ == "__main__":
    unittest.main()
