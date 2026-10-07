"""Check portable CSP and authenticated configuration through the native C fixture."""

import http.client
import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


class PortableHTTPTests(unittest.TestCase):
    def test_configured_voice_policy_and_private_config_route(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory, socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
            listener.close()
            env = dict(
                os.environ,
                LOOP_TEST_WEBTRANSPORT_URL="https://voice.example:9443/v1/voice/turns",
                LOOP_TEST_ANVIL_ORIGIN="https://anvil.example:9443",
            )
            with subprocess.Popen(
                [
                    str(root / "server/loop-test-http"),
                    directory + "/fixture.sqlite",
                    str(root / "static"),
                    str(port),
                ],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ) as process:
                try:
                    for _ in range(100):
                        self.assertIsNone(process.poll())
                        try:
                            with socket.create_connection(
                                ("127.0.0.1", port), timeout=0.1
                            ):
                                break
                        except OSError:
                            time.sleep(0.05)
                    else:
                        self.fail("Native fixture did not start")

                    def request(path, headers=None):
                        connection = http.client.HTTPConnection(
                            "127.0.0.1", port, timeout=5
                        )
                        connection.request("GET", path, headers=headers or {})
                        response = connection.getresponse()
                        result = (
                            response.status,
                            dict(response.getheaders()),
                            response.read(),
                        )
                        connection.close()
                        return result

                    status, headers, _ = request("/")
                    self.assertEqual(status, 200)
                    policy = headers["Content-Security-Policy"]
                    connect = next(
                        part.strip()
                        for part in policy.split(";")
                        if "connect-src" in part
                    )
                    self.assertEqual(
                        connect, "connect-src 'self' https://voice.example:9443"
                    )
                    self.assertNotIn("daviestechlabs", policy)
                    self.assertEqual(request("/api/config")[0], 401)
                    _, headers, _ = request("/api/test/session")
                    cookie = headers["Set-Cookie"].split(";")[0]
                    status, _, body = request("/api/config", {"Cookie": cookie})
                    self.assertEqual(status, 200)
                    self.assertEqual(
                        json.loads(body), {"anvil_origin": "https://anvil.example:9443"}
                    )
                finally:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
