"""Exercise the real C listener with signed requests and hostile TCP frames."""

import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import unittest


BINARY = Path(__file__).resolve().parent / "c-ptools"
SECRET = "local-test-tool-key-" + "x" * 32
EXECUTE = "/v1/tools/execute"


def encode(value):
    return json.dumps(value, separators=(",", ":")).encode()


def signed_headers(method, path, body, user="alice", nonce=None, timestamp=None):
    nonce = nonce or secrets.token_hex(16)
    timestamp = str(timestamp if timestamp is not None else int(time.time()))
    canonical = "\n".join(
        [
            "voice-hmac-v1",
            method,
            path,
            user,
            timestamp,
            nonce,
            hashlib.sha256(body).hexdigest(),
        ]
    )
    signature = hmac.new(
        SECRET.encode(), canonical.encode(), hashlib.sha256
    ).hexdigest()
    return {
        "Content-Type": "application/json",
        "X-Tool-User": user,
        "X-Tool-Timestamp": timestamp,
        "X-Tool-Nonce": nonce,
        "X-Tool-Signature": signature,
    }


def dice_request(**changes):
    request = {
        "tool_call_id": "dice-1",
        "idempotency_key": "roll-1",
        "parent_turn_id": "turn-1",
        "session_id": "table-1",
        "agent_id": "dnd-agent",
        "tool_id": "dnd-dice-roll",
        "input_json": '{"expression":"2d6+3"}',
    }
    request.update(changes)
    return encode(request)


class AuthenticatedTools(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="c-ptools-http-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.environment = os.environ.copy()
        self.environment.pop("NATS_URL", None)
        self.environment.update(
            TOOL_HTTP_ADDR=f"127.0.0.1:{self.port}",
            TOOL_HTTP_AUTH_SECRET=SECRET,
            TOOL_CALL_STORE_PATH=str(self.root / "calls.json"),
            TOOL_WORKSPACE_ROOT=str(self.root / "ws"),
            TOOL_ARTIFACT_DIR=str(self.root / "artifacts"),
            TOOL_DND_STATE_DIR=str(self.root / "dnd"),
        )
        self.process = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.process = subprocess.Popen(
            [str(BINARY)],
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                output = self.process.communicate()
                self.fail(f"listener exited: {output!r}")
            try:
                status, body = self.request("GET", "/healthz", headers={})
                if status == 200:
                    self.assertEqual(body["auth"], "configured")
                    return
            except OSError:
                time.sleep(0.01)
        self.fail("listener did not become ready")

    def stop(self):
        if self.process is None:
            return
        process, self.process = self.process, None
        process.terminate()
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            self.fail("listener did not stop after SIGTERM")
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        self.assertNotIn(SECRET.encode(), stdout + stderr)

    def request(self, method, path, body=b"", user="alice", headers=None):
        if headers is None:
            headers = signed_headers(method, path, body, user=user)
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            data = response.read()
            self.assertEqual(int(response.getheader("Content-Length")), len(data))
            return self.checked_response(response.status, data, path, headers)
        finally:
            connection.close()

    def checked_response(self, status, data, path, headers):
        payload = json.loads(data)
        if status == 200 and "call" in payload:
            # Verify the exact signed bytes, including Unicode and control escapes.
            text = data.decode("utf-8")
            start = text.index('"call":') + len('"call":')
            _, end = json.JSONDecoder().raw_decode(text, start)
            expected = signed_headers(
                "RESULT",
                path,
                text[start:end].encode("utf-8"),
                user=headers["X-Tool-User"],
                nonce=headers["X-Tool-Nonce"],
                timestamp=headers["X-Tool-Timestamp"],
            )["X-Tool-Signature"]
            self.assertTrue(hmac.compare_digest(payload.pop("signature"), expected))
        return status, payload

    def frame(self, body, extra=(), headers=None, method="POST", path=EXECUTE):
        if headers is None:
            headers = signed_headers(method, path, body)
        lines = [
            f"{method} {path} HTTP/1.1",
            "Host: localhost",
            f"Content-Length: {len(body)}",
        ]
        lines.extend(f"{key}: {value}" for key, value in headers.items())
        lines.extend(extra)
        return ("\r\n".join(lines) + "\r\n\r\n").encode() + body

    def raw(self, frame, fragment=False):
        with socket.create_connection(
            ("127.0.0.1", self.port), timeout=5
        ) as connection:
            if fragment:
                # Both the headers and body cross several TCP writes.
                for offset in range(0, len(frame), 31):
                    connection.sendall(frame[offset : offset + 31])
                    time.sleep(0.001)
            else:
                connection.sendall(frame)
            connection.shutdown(socket.SHUT_WR)
            response = http.client.HTTPResponse(connection)
            response.begin()
            head = frame.split(b"\r\n\r\n", 1)[0].decode()
            lines = head.split("\r\n")
            headers = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
            return self.checked_response(
                response.status, response.read(), lines[0].split()[1], headers
            )

    def assert_no_calls(self):
        self.assertFalse(list(self.root.glob("calls.d/*.json")))

    def test_history_exceeds_cache_without_rerolling_old_requests(self):
        first_body = dice_request()
        status, first = self.request("POST", EXECUTE, first_body)
        self.assertEqual(status, 200, first)
        for number in range(1, 260):
            status, result = self.request("POST", EXECUTE, dice_request(
                tool_call_id=f"history-{number}", idempotency_key=f"history-{number}",
                input_json=encode({"expression": f"2d6+{number}"}).decode(),
            ))
            self.assertEqual(status, 200, (number, result))
            self.assertEqual(result["call"]["state"], "completed")
        self.assertEqual(len(list(self.root.glob("calls.d/*.json"))), 260)
        artifact = self.root / "artifacts" / (first["call"]["output_sha256"] + ".json")
        original_artifact = artifact.read_bytes()
        artifact.chmod(0o660)
        self.stop()
        self.assertFalse(list(self.root.glob("calls.json.lookup-*")))
        self.start()
        self.assertEqual(artifact.stat().st_mode & 0o777, 0o600)
        self.assertEqual(artifact.read_bytes(), original_artifact)
        status, repeated = self.request("POST", EXECUTE, first_body)
        self.assertEqual(status, 200, repeated)
        self.assertEqual(repeated, first)
        self.assertEqual(self.request("GET", "/v1/tools/calls/dice-1", user="bob")[0], 404)
        self.assertEqual(self.request("POST", EXECUTE, dice_request(input_json='{"expression":"1d20"}'))[0], 409)
        self.assertEqual(len(list(self.root.glob("calls.d/*.json"))), 260)

    def test_store_failure_changes_health_until_validated_restart(self):
        status, original = self.request("POST", EXECUTE, dice_request())
        self.assertEqual(status, 200, original)
        records = self.root / "calls.d"
        saved = self.root / "calls.saved"
        records.rename(saved)
        records.write_text("temporary write failure")
        failed = dice_request(tool_call_id="failed-write", idempotency_key="failed-write")
        self.assertEqual(self.request("POST", EXECUTE, failed)[0], 409)
        self.assertEqual(self.request("GET", "/healthz", headers={})[0], 503)
        records.unlink()
        saved.rename(records)
        self.assertEqual(self.request("GET", "/healthz", headers={})[0], 503)
        self.assertEqual(len(list(records.glob("*.json"))), 1)
        self.stop()
        self.start()
        self.assertEqual(self.request("POST", EXECUTE, dice_request()), (200, original))
        self.assertEqual(self.request("POST", EXECUTE, failed)[0], 200)
        self.assertEqual(len(list(records.glob("*.json"))), 2)

    def test_startup_requires_exclusive_authenticated_transport(self):
        self.stop()
        for changes in (
            {"TOOL_HTTP_AUTH_SECRET": ""},
            {"TOOL_HTTP_AUTH_SECRET": "short"},
            {"TOOL_HTTP_AUTH_SECRET": "x" * 256},
            {"NATS_URL": "nats://127.0.0.1:1"},
            {"TOOL_HTTP_ADDR": ":0"},
            {"TOOL_HTTP_ADDR": ":65536"},
            {"TOOL_HTTP_ADDR": ":8081junk"},
            {"TOOL_HTTP_ADDR": "localhost:8081"},
        ):
            with self.subTest(changes=list(changes)):
                result = subprocess.run(
                    [str(BINARY)],
                    env=self.environment | changes,
                    capture_output=True,
                    timeout=5,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(SECRET.encode(), result.stdout + result.stderr)
        self.assert_no_calls()

    def test_authentication_binds_request_and_rejects_replay(self):
        body = dice_request(user_id="alice")
        headers = signed_headers("POST", EXECUTE, body)
        attempts = [
            (EXECUTE, body, {}),
            (EXECUTE, dice_request(user_id="bob"), headers),
            ("/v1/tools/calls", body, headers),
            (EXECUTE, body, headers | {"X-Tool-User": "bob"}),
            (
                EXECUTE,
                body,
                signed_headers("POST", EXECUTE, body, timestamp=int(time.time()) - 60),
            ),
            (
                EXECUTE,
                body,
                signed_headers("POST", EXECUTE, body, timestamp=int(time.time()) + 60),
            ),
        ]
        for path, attempt, auth in attempts:
            self.assertEqual(self.request("POST", path, attempt, headers=auth)[0], 401)
        self.assert_no_calls()
        self.assertEqual(self.request("POST", EXECUTE, body, headers=headers)[0], 200)
        self.assertEqual(self.request("POST", EXECUTE, body, headers=headers)[0], 401)
        self.assertEqual(self.request("POST", EXECUTE, body)[0], 200)

    def test_owned_result_and_retry_survive_restart(self):
        body = dice_request()
        status, original = self.raw(self.frame(body), fragment=True)
        self.assertEqual(status, 200, original)
        call = original["call"]
        self.assertEqual(call["user_id"], "alice")
        self.assertEqual(call["parent_turn_id"], "turn-1")
        self.assertEqual(call["state"], "completed")
        result = json.loads(call["output_json"])
        self.assertEqual(result["roll_id"], call["tool_call_id"])
        output_hash = hashlib.sha256(call["output_json"].encode()).hexdigest()
        self.assertEqual(call["output_sha256"], output_hash)
        self.assertEqual(call["output_artifact"], "sha256:" + output_hash)
        artifact = self.root / "artifacts" / (output_hash + ".json")
        self.assertEqual(artifact.read_bytes(), call["output_json"].encode())
        self.assertEqual(artifact.stat().st_mode & 0o777, 0o600)
        self.assertEqual(result["expression"], "2d6+3")
        self.assertEqual(len(result["rolls"]), 2)
        self.assertTrue(
            all(type(value) is int and 1 <= value <= 6 for value in result["rolls"])
        )
        self.assertEqual(
            result["kept_rolls"], [result["rolls"][i] for i in result["kept_indexes"]]
        )
        self.assertEqual(result["total"], sum(result["kept_rolls"]) + 3)
        self.assertEqual(
            self.request("POST", EXECUTE, dice_request(tool_call_id="retry")),
            (200, original),
        )
        self.assertEqual(
            self.request(
                "POST", EXECUTE, dice_request(input_json='{"expression":"1d20"}')
            )[0],
            409,
        )
        self.assertEqual(
            self.request("POST", EXECUTE, dice_request(parent_turn_id="turn-2"))[0], 409
        )
        self.assertEqual(self.request("POST", EXECUTE, body, user="bob")[0], 409)
        read_path = "/v1/tools/calls/dice-1"
        self.assertEqual(self.request("GET", read_path, user="bob")[0], 404)
        self.assertEqual(
            self.request("POST", read_path + "/cancel", b"{}", user="bob")[0], 404
        )
        self.assertEqual(self.request("GET", read_path), (200, original))
        self.assertEqual(
            self.request("POST", read_path + "/cancel", b"{}"), (200, original)
        )
        self.stop()
        self.start()
        self.assertEqual(self.request("POST", EXECUTE, body), (200, original))
        self.assertEqual(self.request("GET", read_path), (200, original))

    def test_strict_dice_admission(self):
        invalid = [
            dice_request(user_id="bob"),
            dice_request(session_id="../table"),
            dice_request(tool_id="workspace-edit"),
            dice_request(agent_id="waterdeep-coder"),
            dice_request(unknown="ignored"),
            dice_request(deadline_unix_ms=int(time.time() * 1000) - 1),
            dice_request(deadline_unix_ms=int(time.time() * 1000) + 120000),
            dice_request(input_json={"expression": "1d20"}),
            dice_request(input_json='{"expression":"1d20","expression":"1d6"}'),
            dice_request(input_json='{"expression":"1d20","user_id":"bob"}'),
            dice_request(input_json='{"expression":"1d0"}'),
            dice_request()[:-1] + b',"tool_id":"dnd-dice-roll"}',
            dice_request()[:-1] + b',"user_id":"alice","user_id":"alice"}',
            dice_request()[:-1] + b',"session_id":"table\\u0000hidden"}',
        ]
        for body in invalid:
            with self.subTest(body=body):
                self.assertEqual(self.request("POST", EXECUTE, body)[0], 400)
        self.assert_no_calls()

    def test_http_framing_rejects_ambiguity(self):
        body = dice_request()
        cases = [
            self.frame(body, [f"Content-Length: {len(body)}"]),
            self.frame(body, ["Transfer-Encoding: chunked"]),
            self.frame(body, ["Host: other"]),
            self.frame(body).replace(
                b"Content-Length: " + str(len(body)).encode(), b"Content-Length: 20000"
            ),
            self.frame(body).replace(
                b"Content-Length: " + str(len(body)).encode(), b"Content-Length: -1"
            ),
            self.frame(body).replace(b"HTTP/1.1", b"HTTP/1.0", 1),
            self.frame(body).replace(b"POST ", b"POST  ", 1),
            self.frame(body)[:-1],
            self.frame(body) + b"unexpected",
            self.frame(body + b"\x00"),
            self.frame(body, ["Content-Type: application/json"]),
        ]
        for frame in cases:
            with self.subTest(frame=frame[:80]):
                self.assertEqual(self.raw(frame)[0], 400)
        self.assertEqual(self.raw(self.frame(body, ["X-Tool-User: alice"]))[0], 401)
        self.assert_no_calls()
        self.assertEqual(self.raw(self.frame(body), fragment=True)[0], 200)

    def test_private_artifact_survives_volume_group_permission_change(self):
        status, original = self.request("POST", EXECUTE, dice_request())
        self.assertEqual(status, 200)
        artifact = (
            self.root / "artifacts" / (original["call"]["output_sha256"] + ".json")
        )
        saved = artifact.read_bytes()
        self.stop()
        # Kubernetes adds the writable fsGroup mask when it remounts the volume.
        artifact.chmod(0o660)
        self.start()
        status, replayed = self.request("GET", "/v1/tools/calls/dice-1")
        self.assertEqual(status, 200, replayed)
        self.assertEqual(replayed, original)
        self.assertEqual(artifact.stat().st_mode & 0o7777, 0o600)
        self.assertEqual(artifact.read_bytes(), saved)
        self.assertEqual(self.request("POST", EXECUTE, dice_request()), (200, original))
        self.assertEqual(
            self.request("GET", "/v1/tools/calls/dice-1", user="bob")[0], 404
        )
        artifact.chmod(0o660)
        self.assertEqual(self.request("GET", "/v1/tools/calls/dice-1")[0], 503)
        self.assertEqual(artifact.stat().st_mode & 0o7777, 0o660)

    def test_startup_permission_repair_preserves_untrusted_artifacts(self):
        for kind in (
            "changed",
            "missing",
            "world",
            "executable",
            "setgid",
            "hardlink",
            "symlink",
            "fifo",
        ):
            with self.subTest(kind=kind):
                identifier = "unsafe-" + kind
                body = dice_request(tool_call_id=identifier, idempotency_key=identifier)
                status, original = self.request("POST", EXECUTE, body)
                self.assertEqual(status, 200)
                artifact = (
                    self.root
                    / "artifacts"
                    / (original["call"]["output_sha256"] + ".json")
                )
                saved = artifact.read_bytes()
                self.stop()
                artifact.chmod(0o660)
                checked = artifact
                if kind == "changed":
                    artifact.write_bytes(saved[:-1] + b" ")
                elif kind == "missing":
                    artifact.unlink()
                elif kind == "world":
                    artifact.chmod(0o664)
                elif kind == "executable":
                    artifact.chmod(0o760)
                elif kind == "setgid":
                    artifact.chmod(0o2660)
                elif kind == "hardlink":
                    os.link(artifact, self.root / (identifier + "-alias"))
                elif kind == "symlink":
                    checked = self.root / (identifier + "-target")
                    artifact.rename(checked)
                    artifact.symlink_to(checked)
                elif kind == "fifo":
                    artifact.unlink()
                    os.mkfifo(artifact, 0o660)
                mode = checked.stat().st_mode if kind != "missing" else None
                self.start()
                self.assertEqual(
                    self.request("GET", "/v1/tools/calls/" + identifier)[0], 503
                )
                self.assertEqual(self.request("POST", EXECUTE, body)[0], 503)
                if kind == "missing":
                    self.assertFalse(artifact.exists())
                else:
                    self.assertEqual(checked.stat().st_mode, mode)
                if kind not in ("fifo", "missing"):
                    expected = saved[:-1] + b" " if kind == "changed" else saved
                    self.assertEqual(checked.read_bytes(), expected)

    @unittest.skipUnless(os.geteuid() == 0, "requires changing file ownership")
    def test_permission_repair_rejects_foreign_owner_and_group(self):
        for kind in ("owner", "group"):
            with self.subTest(kind=kind):
                identifier = "foreign-" + kind
                body = dice_request(tool_call_id=identifier, idempotency_key=identifier)
                status, original = self.request("POST", EXECUTE, body)
                self.assertEqual(status, 200)
                artifact = (
                    self.root
                    / "artifacts"
                    / (original["call"]["output_sha256"] + ".json")
                )
                saved = artifact.read_bytes()
                self.stop()
                uid = 1 if kind == "owner" else 0
                gid = os.getegid() + 1 if kind == "group" else os.getegid()
                os.chown(artifact, uid, gid)
                artifact.chmod(0o660)
                self.start()
                self.assertEqual(
                    self.request("GET", "/v1/tools/calls/" + identifier)[0], 503
                )
                self.assertEqual(self.request("POST", EXECUTE, body)[0], 503)
                observed = artifact.stat()
                self.assertEqual(
                    (observed.st_uid, observed.st_gid, observed.st_mode & 0o7777),
                    (uid, gid, 0o660),
                )
                self.assertEqual(artifact.read_bytes(), saved)

    def test_corrupt_record_rejects_restart(self):
        self.assertEqual(self.request("POST", EXECUTE, dice_request())[0], 200)
        self.stop()
        records = list(self.root.glob("calls.d/*.json"))
        self.assertEqual(len(records), 1)
        records[0].write_text("{broken")
        result = subprocess.run(
            [str(BINARY)], env=self.environment, capture_output=True, timeout=5
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(records[0].read_text(), "{broken")

    def test_corrupt_artifact_is_never_replaced_or_returned(self):
        status, payload = self.request("POST", EXECUTE, dice_request())
        self.assertEqual(status, 200)
        artifact = (
            self.root / "artifacts" / (payload["call"]["output_sha256"] + ".json")
        )
        artifact.write_bytes(b"forged output")
        self.assertEqual(self.request("POST", EXECUTE, dice_request())[0], 503)
        self.assertEqual(self.request("GET", "/v1/tools/calls/dice-1")[0], 503)
        self.assertEqual(artifact.read_bytes(), b"forged output")
        self.stop()
        self.start()
        self.assertEqual(self.request("POST", EXECUTE, dice_request())[0], 503)
        self.assertEqual(artifact.read_bytes(), b"forged output")

    def test_failed_artifact_write_cannot_publish_a_roll(self):
        artifacts = self.root / "artifacts"
        artifacts.rmdir()
        artifacts.write_text("not a directory")
        self.assertEqual(self.request("POST", EXECUTE, dice_request())[0], 409)
        record = json.loads(next(self.root.glob("calls.d/*.json")).read_text())
        self.assertEqual(record["state"], "failed")
        self.assertEqual(record["output_json"], "")
        self.assertEqual(self.request("POST", EXECUTE, dice_request())[0], 409)
        self.assertEqual(
            json.loads(next(self.root.glob("calls.d/*.json")).read_text()), record
        )


if __name__ == "__main__":
    unittest.main()
