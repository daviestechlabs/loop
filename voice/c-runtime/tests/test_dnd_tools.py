"""Real C client/host proof, including a hostile signed-response fixture."""

import hashlib
import http.client
import http.server
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "tool_http_fixture", ROOT / "agents/platform-tools/c-ptools/test_http.py"
)
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)
CLIENT = Path(
    os.environ.get("DND_TOOLS_CLIENT_BIN", ROOT / "voice/c-runtime/c-dnd-tools-test")
)
PROMPT = "Roll 2d20 with advantage and add 5 for my Perception check. Tell me the actual dice and total."


class DiceClient(unittest.TestCase):
    def setUp(self):
        self.host = FIXTURE.AuthenticatedTools()
        self.addCleanup(self.host.doCleanups)
        self.host.setUp()
        self.url = f"http://127.0.0.1:{self.host.port}/v1/tools/execute"

    def execute(
        self, *, url=None, user="alice", prompt=PROMPT, deadline=None, cancel="never"
    ):
        result = subprocess.run(
            [
                str(CLIENT),
                "--execute",
                url or self.url,
                "turn-1",
                user,
                "table-1",
                prompt,
                str(
                    deadline if deadline is not None else int(time.time() * 1000) + 5000
                ),
                cancel,
            ],
            env=os.environ | {"DND_TOOLS_TEST_SECRET": FIXTURE.SECRET},
            capture_output=True,
            timeout=8,
        )
        self.assertNotIn(FIXTURE.SECRET.encode(), result.stdout + result.stderr)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        return json.loads(result.stdout)

    def proxy(self, mutate=None, *, resign=False, hold=None, contacted=None,
              response_delay=0):
        upstream = self.host.port

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                connection = http.client.HTTPConnection(
                    "127.0.0.1", upstream, timeout=5
                )
                try:
                    headers = {
                        key: value
                        for key, value in self.headers.items()
                        if key.lower() not in ("host", "content-length", "connection")
                    }
                    connection.request("POST", self.path, body=body, headers=headers)
                    response = connection.getresponse()
                    status, data = response.status, response.read()
                finally:
                    connection.close()
                if contacted:
                    contacted.set()
                if response_delay:
                    time.sleep(response_delay)
                if status == 200:
                    payload = json.loads(data)
                    if mutate:
                        mutate(payload)
                    if resign:
                        payload["signature"] = FIXTURE.signed_headers(
                            "RESULT",
                            self.path,
                            FIXTURE.encode(payload["call"]),
                            user=self.headers["X-Tool-User"],
                            nonce=self.headers["X-Tool-Nonce"],
                            timestamp=self.headers["X-Tool-Timestamp"],
                        )["X-Tool-Signature"]
                    data = FIXTURE.encode(payload)
                if hold:
                    hold.wait(timeout=2)
                try:
                    self.send_response(status)
                    self.send_header("Content-Length", str(len(data)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def close():
            if hold:
                hold.set()
            server.shutdown()
            thread.join(timeout=3)
            server.server_close()

        self.addCleanup(close)
        return f"http://127.0.0.1:{server.server_port}/v1/tools/execute"

    def test_real_client_verifies_durable_artifact_and_speech(self):
        result = self.execute()
        self.assertEqual(result["rc"], 0, result)
        status, record = self.host.request(
            "GET", "/v1/tools/calls/" + result["call_id"]
        )
        self.assertEqual(status, 200)
        call = record["call"]
        self.assertEqual(result["output_sha256"], call["output_sha256"])
        dice = json.loads(call["output_json"])
        self.assertEqual(
            result["text"],
            f"You rolled 2d20kh1+5 with advantage: {dice['rolls'][0]}, {dice['rolls'][1]}, "
            f"kept {max(dice['rolls'])}, with modifier +5, for a total of {max(dice['rolls']) + 5}.",
        )
        self.assertEqual(self.execute(), result)
        self.host.stop()
        self.host.start()
        self.assertEqual(self.execute(), result)
        self.assertNotEqual(self.execute(user="bob")["call_id"], result["call_id"])
        self.assertNotEqual(self.execute(prompt="Roll 2d20kh1+5.")["rc"], 0)

    def test_tampered_unsigned_or_replayed_response_fails(self):
        saved = []

        def replay(payload):
            if saved:
                payload.update(saved[0])
            else:
                saved.append(payload.copy())

        replay_url = self.proxy(replay)
        self.assertEqual(self.execute(url=replay_url)["rc"], 0)
        self.assertEqual(self.execute(url=replay_url)["rc"], -1)
        url = self.proxy(lambda payload: payload.update(signature="0" * 64))
        self.assertEqual(self.execute(url=url)["rc"], -1)
        url = self.proxy(lambda payload: payload["call"].update(user_id="bob"))
        self.assertEqual(self.execute(url=url)["rc"], -1)

    def test_even_signed_wrong_identity_or_arithmetic_fails(self):
        changes = [
            {"user_id": "bob"},
            {"session_id": "other-table"},
            {"parent_turn_id": "other-turn"},
            {"agent_id": "other-agent"},
            {"tool_id": "other-tool"},
            {"idempotency_key": "changed"},
            {"state": "queued"},
            {"output_sha256": "0" * 64},
            {"output_artifact": "sha256:" + "0" * 64},
        ]
        for change in changes:
            with self.subTest(change=change):
                url = self.proxy(
                    lambda payload, change=change: payload["call"].update(change),
                    resign=True,
                )
                self.assertEqual(self.execute(url=url)["rc"], -1)

        def invalid_math(payload):
            call = payload["call"]
            output = json.loads(call["output_json"])
            output["total"] += 1
            call["output_json"] = FIXTURE.encode(output).decode()
            call["output_sha256"] = hashlib.sha256(
                call["output_json"].encode()
            ).hexdigest()
            call["output_artifact"] = "sha256:" + call["output_sha256"]

        self.assertEqual(
            self.execute(url=self.proxy(invalid_math, resign=True))["rc"], -1
        )

    def test_cancellation_and_deadline_do_not_emit_unverified_text(self):
        self.assertEqual(self.execute(cancel="before"), {"rc": -3})
        self.assertEqual(self.execute(deadline=int(time.time() * 1000) - 1), {"rc": -4})
        self.host.assert_no_calls()
        hold, contacted = threading.Event(), threading.Event()
        url = self.proxy(hold=hold, contacted=contacted)
        started = time.monotonic()
        self.assertEqual(self.execute(url=url, cancel="during"), {"rc": -3})
        self.assertLess(time.monotonic() - started, 1)
        self.assertTrue(contacted.is_set())
        hold.set()
        # A completed roll remains in the audit store; cancellation cannot undo it.
        self.assertEqual(self.execute()["rc"], 0)


class EncounterClient(unittest.TestCase):
    proxy = DiceClient.proxy

    def setUp(self):
        self.host = FIXTURE.AuthenticatedTools()
        self.addCleanup(self.host.doCleanups)
        self.host.setUp()
        self.url = f"http://127.0.0.1:{self.host.port}/v1/tools/execute"
        commands = [
            (
                "campaign",
                {
                    "operation": "create",
                    "campaign_id": "table",
                    "expected_version": 0,
                    "campaign": {"name": "Table", "ruleset": "5e"},
                },
            ),
            ("encounter", {"operation": "start", "expected_version": 0}),
            (
                "encounter",
                {
                    "operation": "add",
                    "expected_version": 1,
                    "participant": {
                        "id": "aria",
                        "name": 'Aria "🐉"',
                        "initiative": 18,
                        "max_hp": 30,
                        "current_hp": 0,
                    },
                },
            ),
            ("encounter", {"operation": "advance", "expected_version": 2}),
        ]
        for index, (kind, command) in enumerate(commands):
            if kind == "encounter":
                command.update(campaign_id="table", encounter_id="battle")
            status, response = self.host.request(
                "POST",
                FIXTURE.EXECUTE,
                FIXTURE.dice_request(
                    tool_call_id=f"setup-{index}",
                    idempotency_key=f"setup-{index}",
                    tool_id=f"dnd-{kind}-state",
                    input_json=FIXTURE.encode(command).decode(),
                ),
            )
            self.assertEqual(status, 200, response)

    def execute(
        self,
        *,
        url=None,
        user="alice",
        campaign="table",
        encounter="battle",
        request="read-1",
        prompt="Show the initiative order.",
        cancel="never",
        deadline=None,
    ):
        process = subprocess.run(
            [
                str(CLIENT),
                "--execute",
                url or self.url,
                request,
                user,
                "session",
                prompt,
                str(
                    deadline if deadline is not None else int(time.time() * 1000) + 5000
                ),
                cancel,
                campaign,
                encounter,
            ],
            env=os.environ | {"DND_TOOLS_TEST_SECRET": FIXTURE.SECRET},
            capture_output=True,
            timeout=8,
        )
        self.assertIn(process.returncode, (0, 1), process.stderr)
        self.assertNotIn(FIXTURE.SECRET.encode(), process.stdout + process.stderr)
        return json.loads(process.stdout)

    def test_snapshot_owner_retry_restart_and_no_state_mutation(self):
        path = self.host.root / "dnd/encounters/table/battle.json"
        before = path.read_bytes()
        result = self.execute()
        self.assertEqual(result["rc"], 0, result)
        self.assertIn('Round 1. Aria "🐉" acts now.', result["text"])
        status, receipt = self.host.request(
            "GET", "/v1/tools/calls/" + result["call_id"]
        )
        self.assertEqual(status, 200)
        call = receipt["call"]
        self.assertEqual(call["tool_id"], "dnd-encounter-state")
        self.assertEqual(
            hashlib.sha256(call["output_json"].encode()).hexdigest(),
            result["output_sha256"],
        )
        self.assertEqual(self.execute(), result)
        self.host.stop()
        self.host.start()
        self.assertEqual(self.execute(), result)
        for changes in (
            {"user": "bob"},
            {"encounter": "other"},
            {"campaign": "other"},
            {"prompt": "Please show the initiative order."},
        ):
            self.assertNotEqual(self.execute(**changes)["rc"], 0)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(self.execute(request="read-2")["rc"], 0)

    def test_signed_invalid_snapshot_and_receipt_reject(self):
        def changed(changes):
            def mutate(payload):
                call = payload["call"]
                output = json.loads(call["output_json"])
                changes(output)
                call["output_json"] = FIXTURE.encode(output).decode()
                call["output_sha256"] = hashlib.sha256(
                    call["output_json"].encode()
                ).hexdigest()
                call["output_artifact"] = "sha256:" + call["output_sha256"]

            return mutate

        for mutate in [
            lambda o: o.update(campaign_id="other"),
            lambda o: o.update(encounter_id="other"),
            lambda o: o.update(operation_id="other"),
            lambda o: o.update(operation="start"),
            lambda o: o.update(version=0),
            lambda o: o.update(version=2**53),
            lambda o: o.update(active_participant_id="other"),
            lambda o: o.update(active_index=1),
            lambda o: o.update(owner_user_id="alice"),
            lambda o: o.update(status="ended"),
            lambda o: o["participants"][0].update(current_hp=31),
            lambda o: o["participants"][0].update(name="Bad\x00name"),
            lambda o: o["participants"][0].update(conditions=["prone", "prone"]),
            lambda o: o["participants"].append(o["participants"][0]),
        ]:
            self.assertEqual(
                self.execute(url=self.proxy(changed(mutate), resign=True))["rc"], -1
            )
        self.assertEqual(
            self.execute(
                url=self.proxy(lambda p: p["call"].update(user_id="bob"), resign=True)
            )["rc"],
            -1,
        )
        self.assertEqual(
            self.execute(url=self.proxy(lambda p: p.update(signature="0" * 64)))["rc"],
            -1,
        )

    def test_context_deadline_cancellation_and_parent_removal(self):
        self.assertEqual(self.execute(encounter="")["rc"], -1)
        self.assertEqual(self.execute(cancel="before")["rc"], -3)
        self.assertEqual(self.execute(deadline=int(time.time() * 1000) - 1)["rc"], -4)
        hold, contacted = threading.Event(), threading.Event()
        self.assertEqual(
            self.execute(
                url=self.proxy(hold=hold, contacted=contacted), cancel="during"
            )["rc"],
            -3,
        )
        self.assertTrue(contacted.is_set())
        hold.set()
        self.assertEqual(self.execute()["rc"], 0)
        (self.host.root / "dnd/campaigns/table.json").unlink()
        self.assertNotEqual(self.execute()["rc"], 0)

    def test_native_encoder_matches_canonical_protobuf(self):
        result = subprocess.run(
            [
                str(CLIENT),
                "--encounter-wire",
                self.url,
                "read-wire",
                "alice",
                "session",
                "Show the initiative order.",
                str(int(time.time() * 1000) + 5000),
                "never",
                "table",
                "battle",
            ],
            env=os.environ | {"DND_TOOLS_TEST_SECRET": FIXTURE.SECRET},
            capture_output=True,
            timeout=8,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        decoded = subprocess.run(
            [
                "protoc",
                f"--proto_path={ROOT / 'contracts/handler-base/proto'}",
                "--decode=messages.v1.DndEncounterState",
                "messages/v1/messages.proto",
            ],
            input=result.stdout,
            capture_output=True,
            timeout=5,
        )
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertIn(b"version: 3", decoded.stdout)
        self.assertIn(b'active_participant_id: "aria"', decoded.stdout)
        self.assertIn(b"initiative: 18", decoded.stdout)
        self.assertNotIn(b"current_hp:", decoded.stdout)  # Canonical proto3 zero.
        self.assertIn(b'operation: "get"', decoded.stdout)


if __name__ == "__main__":
    unittest.main()
