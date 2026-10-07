"""Authenticated HTTP through the real C product, gateway, cascade and tool host.

Python supplies isolated setup, assertions, and a local speech-provider fixture.
"""

import copy
import base64
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import struct
import threading
import time
import unittest
import uuid

import test_dnd_initiative as initiative


ROOT = initiative.ROOT
RUNTIME = ROOT / "voice/c-runtime"
PRODUCT = ROOT / "product/companions-frontend/c-companions/c-companions"
TOKEN = "product-test-gateway-0123456789abcdef-0123456789abcdef"


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class ProductProcess(unittest.TestCase):
    seed_roster = False

    def setUp(self):
        self.host = initiative.client.FIXTURE.AuthenticatedTools()
        self.addCleanup(self.host.doCleanups)
        self.host.setUp()
        self.processes = {}
        self.logs = {}
        self.gateway_port = unused_port()
        # Do not inherit production model, telemetry or transport destinations.
        self.environment = {
            key: os.environ[key]
            for key in (
                "PATH",
                "LD_LIBRARY_PATH",
                "ASAN_OPTIONS",
                "UBSAN_OPTIONS",
                "TSAN_OPTIONS",
            )
            if key in os.environ
        }
        self.environment.update(
            VBUS_PATH=str(self.host.root / "bus.sock"),
            VOICE_GATEWAY_TOKEN=TOKEN,
            GATEWAY_HEALTH_PORT=str(self.gateway_port),
            TOOL_HTTP_URL=f"http://127.0.0.1:{self.host.port}/v1/tools/execute",
            TOOL_HTTP_AUTH_SECRET=initiative.client.FIXTURE.SECRET,
            TOOL_HTTP_TIMEOUT_MS="5000",
            LLM_HTTP_URL="",
            COMPANIONS_ROLE="product",
            COMPANIONS_ROOT=str(ROOT / "product/companions-frontend"),
            COMPANIONS_TURN_GATEWAY_URL=f"http://127.0.0.1:{self.gateway_port}",
            # Let the five-second tool deadline publish its terminal failure.
            # Equal inner and outer deadlines can truncate that event.
            COMPANIONS_TURN_TIMEOUT_MS="7000",
            GUEST_AUTH_ENABLED="true",
            COOKIE_SECURE="false",
            JWT_SECRET="product-test-access-0123456789abcdef-0123456789abcdef",
            REFRESH_SECRET="product-test-refresh-0123456789abcdef-0123456789abcdef",
        )
        self.start("broker", RUNTIME / "vbus-broker", "BROKER_BIN")
        self.wait_for(lambda: Path(self.environment["VBUS_PATH"]).exists())
        self.start("cascade", RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN")
        self.wait_for(lambda: "pure-C direct admission" in self.log("cascade"))
        self.start("gateway", RUNTIME / "c-voice-session-gateway", "GATEWAY_BIN")
        self.wait_for(lambda: self.http(self.gateway_port, "GET", "/healthz")[0] == 200)
        # Allocate after the gateway binds. Two released ephemeral ports can be equal.
        self.product_port = unused_port()
        self.environment["COMPANIONS_HTTP_ADDR"] = f"127.0.0.1:{self.product_port}"
        self.start_product()
        self.cookie, self.user = self.login()
        self.characters = [
            initiative.CAMPAIGN.character(
                id="rogue", name='Rógué "🐉"', safety_rules=[]
            ),
            initiative.CAMPAIGN.character(id="npc", name="Mirt", safety_rules=[]),
            initiative.CAMPAIGN.character(id="last", name="Last", safety_rules=[]),
        ]
        self.parent = self.host.root / "dnd/campaigns/table.json"
        if self.seed_roster:
            self.install_roster(self.characters)

    def log(self, name):
        return self.logs[name].read_text(errors="replace")

    def start(self, name, binary, variable):
        path = self.host.root / f"{name}.log"
        self.logs[name] = path
        with path.open("ab") as output:
            self.processes[name] = subprocess.Popen(
                [os.environ.get(variable, str(binary))],
                env=self.environment,
                cwd=ROOT,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
        self.addCleanup(self.stop, name)

    def stop(self, name):
        process = self.processes.pop(name, None)
        if process is None:
            return
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            self.fail(f"{name} did not stop after SIGTERM")
        self.assertEqual(process.returncode, 0, self.log(name))
        self.assertNotIn(TOKEN, self.log(name))
        self.assertNotIn(initiative.client.FIXTURE.SECRET, self.log(name))

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for name, process in self.processes.items():
                self.assertIsNone(process.poll(), self.log(name))
            try:
                if predicate():
                    return
            except OSError:
                pass
            time.sleep(0.01)
        self.fail("C service readiness timed out")

    def start_product(self):
        marker = "c-companions http listening on"
        previous = self.log("product").count(marker) if "product" in self.logs else 0
        self.start("product", PRODUCT, "COMPANIONS_BIN")
        # Require this process's startup, including on fixture restarts.
        self.wait_for(lambda: self.log("product").count(marker) > previous and
                      self.http(self.product_port, "GET", "/healthz")[0] == 200)

    def start_tts(self, pcm_bytes=2880, before_write=None, pcm_for_request=None, after_write=None):
        requests = []
        pattern = bytes((i * 131 + 17) % 256 for i in range(256))
        pcm = (pattern * ((pcm_bytes + 255) // 256))[:pcm_bytes]

        class Provider(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                request = json.loads(
                    self.rfile.read(int(self.headers["Content-Length"]))
                )
                requests.append(request)
                payload = pcm if pcm_for_request is None else pcm_for_request(request)
                body = struct.pack("<IHH", 24000, 1, 16) + payload
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if before_write:
                    before_write()
                try:
                    self.wfile.write(body)
                    self.wfile.flush()
                    if after_write:
                        after_write(request)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def close():
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)

        self.addCleanup(close)
        self.environment["TTS_HTTP_URL"] = f"http://127.0.0.1:{server.server_port}/pcm"
        self.start("tts", RUNTIME / "c-tts-module", "TTS_MODULE_BIN")
        self.wait_for(lambda: "pure-C" in self.log("tts"))
        return requests, pcm

    def http(self, port, method, path, body=None, cookie=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
        headers = {"Content-Type": "application/json"}
        if cookie:
            headers["Cookie"] = cookie
        try:
            connection.request(
                method,
                path,
                body=initiative.client.FIXTURE.encode(body)
                if body is not None
                else None,
                headers=headers,
            )
            response = connection.getresponse()
            return response.status, response.read(), response.getheaders()
        finally:
            connection.close()

    def login(self):
        status, body, headers = self.http(
            self.product_port, "POST", "/api/login", {"guest": True}
        )
        self.assertEqual(status, 200)
        cookie = "; ".join(
            value.split(";", 1)[0]
            for key, value in headers
            if key.lower() == "set-cookie"
        )
        status, body, _ = self.http(
            self.product_port, "GET", "/api/auth/check", cookie=cookie
        )
        self.assertEqual(status, 200)
        user = json.loads(body)["user"]
        self.assertFalse(user["premium"])
        self.assertTrue(user["sub"].startswith("guest_"))
        return cookie, user["sub"]

    def install_roster(self, characters):
        # Retained local fixture belongs to the principal minted by the C product.
        self.parent = self.host.root / "dnd/campaigns/table.json"
        self.parent.parent.mkdir(parents=True, exist_ok=True)
        state = initiative.CAMPAIGN.state(
            owner_user_id=self.user,
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
        self.parent.write_text(
            json.dumps(state, ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8",
        )
        self.parent.chmod(0o600)

    def body(self, *, roster=False, selections=None):
        request_id = str(uuid.uuid4())
        body = {
            "message": "Show the campaign roster."
            if roster
            else "Roll initiative for the party.",
            "request_id": request_id,
            "enable_tts": False,
            "metadata": {
                "interaction_profile": "dnd_app",
                "campaign_id": "table",
                "encounter_id": "battle",
            },
        }
        if not roster:
            body["conversation_id"] = "initiative-" + request_id
            body["dnd_initiative"] = {
                "operation_id": request_id,
                "campaign_version": 2,
                "expected_version": 0,
                "selections": selections
                or [
                    {"character_id": "rogue", "expression": "1d20+3"},
                    {"character_id": "npc", "expression": "2d20kh1+1"},
                    {"character_id": "last", "expression": "2d20kl1-2"},
                ],
            }
        return body

    def turn(self, body, *, cookie=None, completed=True):
        status, data, _ = self.http(
            self.product_port,
            "POST",
            "/api/turns",
            body,
            self.cookie if cookie is None else cookie,
        )
        self.assertEqual(status, 200, data)
        events = [json.loads(line) for line in data.splitlines() if line]
        self.last_events = events
        self.assertTrue(events, data)
        self.assertTrue(
            all(event["request_id"] == body["request_id"] for event in events)
        )
        self.assertEqual(
            events[-1]["type"], "completed" if completed else "failed", events
        )
        finals = [event for event in events if event["type"] == "text_completed"]
        if not completed:
            self.assertEqual(finals, [], events)
            return None
        self.assertEqual(len(finals), 1, events)
        final = finals[0]
        self.assertTrue(final["is_final"])
        self.assertTrue(final["text"])
        self.assertNotIn("dnd_retrieval", final)
        return final

    def receipt(self, event):
        metadata = event["metadata"]
        status, payload = self.host.request(
            "GET", "/v1/tools/calls/" + metadata["cascade_tool_call_id"], user=self.user
        )
        self.assertEqual(status, 200, payload)
        call = payload["call"]
        self.assertEqual(call["user_id"], self.user)
        self.assertEqual(call["output_sha256"], metadata["cascade_tool_output_hash"])
        self.assertEqual(
            hashlib.sha256(call["output_json"].encode()).hexdigest(),
            call["output_sha256"],
        )
        return call, json.loads(call["output_json"])


class ProductInitiative(ProductProcess):
    seed_roster = True

    def test_authenticated_roster_roll_and_exact_retry_after_both_hosts_restart(self):
        before = self.parent.read_bytes()
        roster = self.turn(self.body(roster=True))
        _, projected = self.receipt(roster)
        self.assertEqual(
            roster["dnd_campaign_roster"]["characters"], projected["characters"]
        )
        self.assertEqual(self.parent.read_bytes(), before)
        self.assertNotIn("owner_user_id", json.dumps(roster))
        body = self.body()
        result = self.turn(body)
        call, saved = self.receipt(result)
        self.assertEqual(call["parent_turn_id"], body["dnd_initiative"]["operation_id"])
        self.assertEqual(call["session_id"], body["conversation_id"])
        self.assertEqual(
            result["dnd_encounter_state"]["participants"], saved["participants"]
        )
        self.assertEqual(
            result["dnd_initiative_result"]["rolls"], saved["initiative_rolls"]
        )
        path = self.host.root / "dnd/encounters/table/battle.json"
        snapshot = path.read_bytes()
        self.stop("product")
        self.host.stop()
        self.host.start()
        # Cross the second boundary used by the product's default session ID.
        time.sleep(1.05)
        self.start_product()
        # Replayed transport IDs still fail; retry only the durable operation.
        status, _, _ = self.http(
            self.product_port, "POST", "/api/turns", body, self.cookie
        )
        self.assertEqual(status, 502)
        body["request_id"] = str(uuid.uuid4())
        retry = self.turn(body)
        retried, _ = self.receipt(retry)
        self.assertEqual(retried, call)
        self.assertEqual(
            retry["dnd_initiative_result"], result["dnd_initiative_result"]
        )
        self.assertEqual(retry["dnd_encounter_state"], result["dnd_encounter_state"])
        self.assertEqual(path.read_bytes(), snapshot)

    def test_anonymous_other_owner_stale_and_changed_retry_cannot_change_state(self):
        body = self.body()
        status, _, _ = self.http(self.product_port, "POST", "/api/turns", body)
        self.assertEqual(status, 401)
        other_cookie, other_user = self.login()
        self.assertNotEqual(other_user, self.user)
        self.turn(self.body(roster=True), cookie=other_cookie, completed=False)
        self.turn(body, cookie=other_cookie, completed=False)
        self.assertFalse((self.host.root / "dnd/encounters/table/battle.json").exists())
        body["request_id"] = str(uuid.uuid4())
        self.turn(body)
        path = self.host.root / "dnd/encounters/table/battle.json"
        before = path.read_bytes()
        self.turn(self.body(), completed=False)
        changed = copy.deepcopy(body)
        changed["request_id"] = str(uuid.uuid4())
        changed["dnd_initiative"]["expected_version"] = 1
        self.turn(changed, completed=False)
        forged = body | {"user_id": other_user, "premium": True}
        status, _, _ = self.http(
            self.product_port, "POST", "/api/turns", forged, self.cookie
        )
        self.assertEqual(status, 400)
        self.assertEqual(path.read_bytes(), before)

    def test_largest_retained_roster_and_all_64_choices_survive_http_and_wire(self):
        characters = [
            initiative.CAMPAIGN.character(
                id=f"pc-{i:03}", name="é" * 100, safety_rules=[]
            )
            for i in range(200)
        ]
        self.install_roster(characters)
        # The retained campaign has a separate 64 KiB byte limit. Prove that
        # overflow fails before exercising the largest valid local roster.
        self.turn(self.body(roster=True), completed=False)
        while self.parent.stat().st_size >= 65536:
            characters.pop()
            self.install_roster(characters)
        self.assertGreaterEqual(len(characters), 64)
        roster = self.turn(self.body(roster=True))
        self.assertEqual(
            len(roster["dnd_campaign_roster"]["characters"]), len(characters)
        )
        self.assertEqual(
            roster["dnd_campaign_roster"]["characters"][-1]["name"], "é" * 100
        )
        self.receipt(roster)
        # Encounter names have a stricter bound than retained campaign names.
        characters = [
            character | {"name": f"Character {i}"}
            for i, character in enumerate(characters)
        ]
        self.install_roster(characters)
        body = self.body(
            selections=[
                {"character_id": character["id"], "expression": "2d20kh1+5"}
                for character in characters[:64]
            ]
        )
        final = self.turn(body)
        _, saved = self.receipt(final)
        self.assertEqual(len(final["dnd_initiative_result"]["rolls"]), 64)
        self.assertEqual(
            final["dnd_encounter_state"]["participants"], saved["participants"]
        )
        self.assertEqual(
            final["dnd_initiative_result"]["rolls"], saved["initiative_rolls"]
        )

    def test_saved_initiative_text_produces_complete_native_tts_pcm(self):
        requests, expected_pcm = self.start_tts()
        body = self.body()
        del body["enable_tts"]
        final = self.turn(body)
        self.receipt(final)
        provider = [
            request for request in requests if request["turn_id"] == body["request_id"]
        ]
        self.assertTrue(provider)
        self.assertEqual(
            " ".join(request["text"] for request in provider), final["text"]
        )
        chunks = [event for event in self.last_events if event["type"] == "pcm_chunk"]
        self.assertTrue(chunks)
        self.assertEqual(sum(chunk["is_final"] for chunk in chunks), len(provider))
        self.assertTrue(
            all(
                chunk["sample_rate"] == 24000
                and chunk["channels"] == 1
                and chunk["bit_depth"] == 16
                for chunk in chunks
            )
        )
        self.assertEqual(
            b"".join(
                base64.b64decode(chunk["audio_base64"], validate=True)
                for chunk in chunks
            ),
            expected_pcm * len(provider),
        )

    @unittest.skipUnless(
        os.environ.get("DND_PRODUCT_BROWSER") == "1",
        "optional installed Chromium proof",
    )
    def test_served_table_uses_real_authenticated_c_turns(self):
        self.start_tts()
        process = subprocess.run(
            [
                os.environ.get("DND_PRODUCT_BROWSER_NODE", "node"),
                str(ROOT / "benchmarks/browser-e2e/scripts/dnd-product-initiative.mjs"),
            ],
            input=json.dumps(
                {"url": f"http://127.0.0.1:{self.product_port}", "cookie": self.cookie}
            ),
            text=True,
            capture_output=True,
            timeout=45,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        final = json.loads(process.stdout)
        _, saved = self.receipt(final)
        self.assertEqual(
            final["dnd_initiative_result"]["rolls"], saved["initiative_rolls"]
        )
        self.assertEqual(len(saved["participants"]), 3)


if __name__ == "__main__":
    unittest.main()
