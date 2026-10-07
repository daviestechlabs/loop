"""Real C owner-store/queue/header checks; fixture transcripts are not quality proof."""

from __future__ import annotations

import hashlib
import json
import math
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def varint(value: int) -> bytes:
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def field(number: int, value: str | bytes | int) -> bytes:
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    raw = value.encode() if isinstance(value, str) else value
    return varint((number << 3) | 2) + varint(len(raw)) + raw


def fields(raw: bytes) -> dict[int, bytes | int]:
    position = 0

    def integer() -> int:
        nonlocal position
        value = shift = 0
        while True:
            byte = raw[position]
            position += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
            if shift > 63:
                raise ValueError("fixture varint overflow")

    result = {}
    while position < len(raw):
        tag = integer()
        if tag & 7 == 0:
            value = integer()
        elif tag & 7 == 2:
            length = integer()
            value = raw[position : position + length]
            position += length
        else:
            raise ValueError("unsupported fixture field")
        result[tag >> 3] = value
    return result


class Bus:
    def __init__(self, path: str):
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(3)
        try:
            self.connection.connect(path)
            self.subscriptions = 0
            self.send(1, "client")
            assert self.receive()[0] == 5
        except Exception:
            self.connection.close()
            raise

    def close(self):
        self.connection.close()

    def send(self, operation, topic, body=b"", reply="", identifier=0xFFFFFFFF):
        parts = [topic.encode(), b"", reply.encode(), body]
        header = struct.pack(
            "<8I", 0x56425553, 2, operation, identifier, *map(len, parts)
        )
        self.connection.sendall(header + b"".join(parts))

    def exact(self, length):
        out = bytearray()
        while len(out) < length:
            raw = self.connection.recv(length - len(out))
            if not raw:
                raise EOFError("owned fixture bus closed")
            out.extend(raw)
        return bytes(out)

    def receive(self):
        magic, version, operation, _identifier, topic, queue, reply, body = (
            struct.unpack("<8I", self.exact(32))
        )
        assert (magic, version) == (0x56425553, 2)
        assert topic + queue + reply + body < 4 * 1024 * 1024
        parts = [self.exact(length) for length in (topic, queue, reply, body)]
        return operation, parts[0].decode(), parts[3]

    def subscribe(self, topic):
        self.send(2, topic, identifier=self.subscriptions)
        self.subscriptions += 1
        assert self.receive()[0] == 5

    def request(self, topic, raw):
        return fields(self.request_raw(topic, raw))

    def request_raw(self, topic, raw):
        inbox = "_INBOX.fixture." + str(self.subscriptions)
        self.subscribe(inbox)
        self.send(3, topic, raw, reply=inbox)
        operation, subject, body = self.receive()
        assert operation == 4 and subject == inbox
        return body


class Rig:
    """Three real C processes on a private bus; never the production bus."""

    def __init__(self, backend: str, enabled=True):
        self.temporary = tempfile.TemporaryDirectory(
            prefix="c-stt-vocab-",
            dir="/private/tmp" if os.uname().sysname == "Darwin" else "/tmp",
        )
        self.directory = Path(self.temporary.name)
        self.socket = str(self.directory / "bus.sock")
        self.children = []
        self.logs = []
        self.bus = None
        self.environment = {
            **os.environ,
            "VBUS_PATH": self.socket,
            "STT_BACKEND_URL": backend,
            "STT_VOCABULARY_EVALUATION": "1" if enabled else "0",
            "STT_SPEECH_CONTEXT_MS": "-1",
        }

    def spawn(self, name):
        path = self.directory / (name + ".log")
        log = path.open("wb")
        child = subprocess.Popen(
            [str(ROOT / name)], env=self.environment, stdout=log, stderr=log
        )
        self.children.append(child)
        self.logs.append(log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise RuntimeError(path.read_text())
            marker = {
                "vbus-broker": "listening on",
                "c-session-manager": "canonical bounded session store",
                "c-audio-processor": "pure-C service running",
            }[name]
            if marker in path.read_text():
                if name == "vbus-broker":
                    # The broker prints its marker before bind/listen.
                    # An actual acknowledged client proves readiness.
                    try:
                        probe = Bus(self.socket)
                    except (FileNotFoundError, ConnectionRefusedError):
                        time.sleep(0.01)
                        continue
                    probe.close()
                return
            time.sleep(0.01)
        raise TimeoutError("owned C process startup timed out: " + path.read_text())

    def start(self):
        try:
            self.spawn("vbus-broker")
            self.bus = Bus(self.socket)
            self.spawn("c-session-manager")
            self.spawn("c-audio-processor")
        except (RuntimeError, TimeoutError, OSError):
            self.close()
            raise
        return self

    def close(self):
        if self.bus:
            self.bus.close()
        for child in reversed(self.children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
        for log in self.logs:
            log.close()
        self.temporary.cleanup()

    def append(self, session, owner, text, role="user"):
        message = (
            field(1, role) + field(2, text) + field(3, 12345) + field(4, "fixture")
        )
        response = self.bus.request(
            "ai.session.append", field(1, session) + field(2, owner) + field(3, message)
        )
        assert response.get(1) == session.encode() and response.get(2, 0) > 0

    def transcribe(self, session, owner, pcm=None, timeout=3):
        if pcm is None:
            pcm = struct.pack(
                "<16000h",
                *(
                    int(6000 * math.sin(i * math.tau * 440 / 16000))
                    for i in range(16000)
                ),
            )
        response = Bus(self.socket)
        response.connection.settimeout(timeout)
        response.subscribe("ai.voice.transcription." + session)
        common = field(5, 16000) + field(6, 1) + field(7, 16)
        start = field(1, "start") + common + (field(13, owner) if owner else b"")
        started = time.monotonic()
        try:
            self.bus.send(3, "ai.voice.stream." + session, start)
            for offset in range(0, len(pcm), 3200):
                self.bus.send(
                    3,
                    "ai.voice.stream." + session,
                    field(1, "chunk") + field(2, pcm[offset : offset + 3200]) + common,
                )
            self.bus.send(
                3,
                "ai.voice.stream." + session,
                field(1, "end") + common + field(9, int(time.time() * 1000)),
            )
            while True:
                operation, subject, raw = response.receive()
                decoded = fields(raw)
                if (
                    operation == 4
                    and subject == "ai.voice.transcription." + session
                    and decoded.get(17) == 1
                ):
                    return {
                        "text": decoded[2].decode(),
                        "wire": raw,
                        "wallMs": (time.monotonic() - started) * 1000,
                    }
        except TimeoutError as exc:
            logs = "\n".join(p.read_text() for p in self.directory.glob("*.log"))
            raise TimeoutError(logs) from exc
        finally:
            response.close()


class Fixture:
    def __init__(self):
        self.requests = []
        self.health_reads = 0
        self.health = {
            "status": "ready",
            "backend": "mlx-whisper",
            "device": "METAL",
            "model_artifacts_sha256": "a" * 64,
            "loaded": True,
            "evaluation_only": True,
            "spelling_hints_supported": True,
        }
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, body):
                raw = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self):
                fixture.health_reads += 1
                self.reply(fixture.health)

            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                fixture.requests.append(
                    {
                        "hint": self.headers.get("X-Whisper-Vocabulary"),
                        "PCMHash": hashlib.sha256(raw).hexdigest(),
                    }
                )
                self.reply({"status": "ok", "text": "bounded fixture transcript"})

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.url = "http://127.0.0.1:" + str(self.server.server_port)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class TestConversationSpelling(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)

    def test_owner_focus_topic_reset_and_queue_copy(self):
        rig = Rig(self.fixture.url).start()
        self.addCleanup(rig.close)
        rig.append(
            "fictional-a", "owner-a", "I cast fireball. No wait, is Mira still here?"
        )
        rig.transcribe("fictional-a", "owner-a")
        rig.transcribe("fictional-a", "other-owner")
        rig.transcribe("fictional-a", "")
        rig.append("fictional-a", "owner-a", "Is Elara still here?", "assistant")
        rig.transcribe("fictional-a", "owner-a")
        rig.append(
            "fictional-a",
            "owner-a",
            "What do we still need to establish before choosing a spell?",
        )
        rig.transcribe("fictional-a", "owner-a")
        rig.append("fictional-a", "owner-a", "Describe a fictional stone tower.")
        rig.transcribe("fictional-a", "owner-a")
        rig.append("fictional-a", "owner-a", "Is Elara still here?")
        rig.transcribe("fictional-a", "owner-a")
        rig.append("fictional-a", "owner-a", "Is Jorin Vale still here?")
        rig.transcribe("fictional-a", "owner-a")
        self.assertEqual(
            [x["hint"] for x in self.fixture.requests],
            ["Mira", None, None, "Mira", "Mira", None, "Elara", None],
        )
        self.assertEqual(self.fixture.health_reads, 1)

    def test_default_has_no_hint_or_health_probe(self):
        self.fixture.health = {"status": "ready", "backend": "openvino-genai"}
        rig = Rig(self.fixture.url, enabled=False).start()
        self.addCleanup(rig.close)
        rig.append("fictional-default", "owner-a", "Is Mira still here?")
        rig.transcribe("fictional-default", "owner-a")
        self.assertIsNone(self.fixture.requests[0]["hint"])
        self.assertEqual(self.fixture.health_reads, 0)

    def test_npu_cannot_enable_evaluation_hints(self):
        self.fixture.health["backend"] = "openvino-genai"
        result = subprocess.run(
            [str(ROOT / "c-audio-processor")],
            env={
                **os.environ,
                "STT_BACKEND_URL": self.fixture.url,
                "STT_VOCABULARY_EVALUATION": "1",
            },
            capture_output=True,
            timeout=3,
            check=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"requires the admitted evaluation engine", result.stderr)
        self.assertEqual(self.fixture.requests, [])


if __name__ == "__main__":
    unittest.main()
