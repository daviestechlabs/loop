"""Candidate C conversation experiment with a real model and isolated game fixtures."""

import argparse
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import sys
import threading
import time
import urllib.parse
from dialogue_case_pack import load_pack

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "voice/c-runtime/tests"))
import test_dnd_dialogue_product as dialogue  # noqa: E402

CASES = (
    ("referents", "Would it hit her?", "hall"),
    ("recall", "Remind me which spell I was considering.", "hall"),
    ("safety", "Is that safe for her?", "hall"),
    ("execution", "Then go ahead and cast it.", "hall"),
    ("cancel_paraphrase", "Actually, drop the idea. What should I do instead?", "hall"),
    ("changed_scene", "Would it hit her here?", "courtyard"),
)
CONDITIONS = ("retained_conversation", "fresh_conversation")


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


class Cohort(dialogue.ProductDialogue):
    def start(self, name, binary, variable):
        self.environment.update(
            LLM_HTTP_URL=self.model_endpoint,
            LLM_MODEL="default",
            LLM_HTTP_TIMEOUT_MS="120000",
            LLM_MAX_COMPLETION_TOKENS="256",
            COMPANIONS_TURN_TIMEOUT_MS="130000",
        )
        super().start(name, binary, variable)

    def http(self, port, method, path, body=None, cookie=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=140)
        headers = {"Content-Type": "application/json"}
        if cookie:
            headers["Cookie"] = cookie
        try:
            connection.request(
                method, path, None if body is None else json.dumps(body), headers
            )
            response = connection.getresponse()
            return response.status, response.read(), response.getheaders()
        finally:
            connection.close()


def capture_server(endpoint, directory):
    target = urllib.parse.urlsplit(endpoint)
    if (
        target.scheme != "http"
        or not target.hostname
        or target.username
        or target.query
    ):
        raise ValueError("Use the explicit internal HTTP model endpoint")
    records = []

    class Proxy(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            record = {"path": self.path, "response_sse": ""}
            records.append(record)
            upstream = http.client.HTTPConnection(
                target.hostname, target.port or 80, timeout=120
            )
            try:
                body = self.rfile.read(int(self.headers["Content-Length"]))
                record["request"] = json.loads(body)
                write(directory / "model-exchanges.json", records)
                upstream.request(
                    "POST", self.path, body, {"Content-Type": "application/json"}
                )
                response = upstream.getresponse()
                record["status"] = response.status
                self.send_response(response.status)
                self.send_header(
                    "Content-Type",
                    response.getheader("Content-Type", "text/event-stream"),
                )
                self.send_header("Connection", "close")
                self.end_headers()
                chunks = []
                while chunk := response.read1(8192):
                    chunks.append(chunk)
                    self.wfile.write(chunk)
                    self.wfile.flush()
                record["response_sse"] = b"".join(chunks).decode("utf-8")
            except Exception as error:
                record["error"] = repr(error)
                raise
            finally:
                upstream.close()
                write(directory / "model-exchanges.json", records)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread, records


def model_identity(endpoint):
    target = urllib.parse.urlsplit(endpoint)
    connection = http.client.HTTPConnection(
        target.hostname, target.port or 80, timeout=15
    )
    try:
        connection.request("GET", "/v1/models")
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError("Model identity unavailable")
        return json.loads(response.read())
    finally:
        connection.close()


def trial(endpoint, directory, case_id, message, scene, condition):
    directory.mkdir()
    server, thread, exchanges = capture_server(endpoint, directory)
    case = Cohort()
    case.model_endpoint = f"http://127.0.0.1:{server.server_port}/v1/chat/completions"
    row = {
        "case_id": case_id,
        "message": message,
        "scene": scene,
        "condition": condition,
    }
    write(directory / "trial.json", row)
    try:
        case.setUp()
        before = case.install_scene()
        (directory / "campaign-before.json").write_bytes(before)
        prefix = case.question()
        prefix_final = case.turn(prefix)
        call, result = case.receipt(prefix_final)
        write(directory / "prefix-receipt.json", {"call": call, "output": result})
        write(
            directory / "prefix.json", {"request": prefix, "events": case.last_events}
        )
        body = case.question(message=message, scene=scene)
        for key in ("knowledge_scope", "retrieval_force"):
            body["metadata"].pop(key)
        if condition == "fresh_conversation":
            body["conversation_id"] = "fresh-room"
        started = time.monotonic()
        status, raw, _ = case.http(
            case.product_port, "POST", "/api/turns", body, case.cookie
        )
        elapsed = 1000 * (time.monotonic() - started)
        events = [json.loads(line) for line in raw.splitlines() if line]
        finals = [event for event in events if event["type"] == "text_completed"]
        write(
            directory / "followup.json",
            {"request": body, "status": status, "events": events},
        )
        recall = case.question(message="What spell am I holding?", scene=scene)
        recall["conversation_id"] = body["conversation_id"]
        recalled = case.turn(recall)
        write(
            directory / "recall.json", {"request": recall, "events": case.last_events}
        )
        unchanged = case.parent.read_bytes() == before
        (directory / "campaign-after.json").write_bytes(case.parent.read_bytes())
        if not unchanged:
            raise AssertionError("Unexpected campaign mutation")
        row.update(
            request_ms=elapsed,
            completed=bool(
                status == 200 and events and events[-1]["type"] == "completed"
            ),
            answer=finals[0]["text"] if len(finals) == 1 else None,
            terminal=events[-1] if events else None,
            recalled_proposal=recalled["text"],
            model_calls=len(exchanges),
            campaign_unchanged=unchanged,
            campaign_sha256=hashlib.sha256(before).hexdigest(),
        )
    except Exception as error:
        row["error"] = repr(error)
        raise
    finally:
        if hasattr(case, "logs"):
            for name in case.logs:
                (directory / f"{name}.log").write_text(case.log(name))
        row["cleanup_ok"] = case.doCleanups()
        server.shutdown()
        thread.join(timeout=3)
        server.server_close()
        write(directory / "trial.json", row)
    # A completed deterministic route is an observed routing failure in this
    # experiment. Retain it and collect the other independent pairs before
    # failing the run. Transport and cleanup failures still stop immediately.
    write(directory / "model-exchanges.json", exchanges)
    if not row["completed"] or not row["cleanup_ok"]:
        raise AssertionError("Incomplete C trial; inspect retained artifacts")
    return row


def run(endpoint, output, case_pack=None):
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    (output / "coupled_dialogue_eval.py").write_bytes(Path(__file__).read_bytes())
    cases = CASES
    pack_fields = {}
    if case_pack is not None:
        (output / "dialogue_case_pack.py").write_bytes(
            Path(__file__).with_name("dialogue_case_pack.py").read_bytes()
        )
        pack = load_pack(case_pack)
        cases = tuple((c["case_id"], c["message"], c["scene"]) for c in pack["cases"])
        (output / "case-pack.json").write_bytes(case_pack.read_bytes())
        pack_fields["case_pack_sha256"] = hashlib.sha256(
            case_pack.read_bytes()
        ).hexdigest()
    write(
        output / "protocol.json",
        {
            "context_contract": "cancellation-receipt-v1",
            "output_contract": "c-state-receipt-prefix-v1",
            "cases": cases,
            **pack_fields,
            "conditions": CONDITIONS,
            "order": "Serial pairs; reverse condition order on odd case indexes",
            "intervention": "Same repaired prefix; retain conversation ID or use a fresh ID for the follow-up",
            "scope": "Candidate C product and signed local scene tool; real model; no audio or live campaign writes",
            "generation": "Unmodified candidate C request; capture actual parameters per trial",
            "automatic_quality_scoring": False,
            "training_performed": False,
            "human_approved": False,
            "production_gate": False,
        },
    )
    write(output / "models-before.json", model_identity(endpoint))
    rows = []
    for index, (case_id, message, scene) in enumerate(cases):
        for condition in CONDITIONS if index % 2 == 0 else reversed(CONDITIONS):
            row = trial(
                endpoint,
                output / f"{case_id}-{condition}",
                case_id,
                message,
                scene,
                condition,
            )
            rows.append(row)
            write(output / "results.json", rows)
            print(
                json.dumps(
                    {
                        "trial": len(rows),
                        "case": case_id,
                        "condition": condition,
                        "completed": row["completed"],
                        "answer": row["answer"],
                    }
                ),
                flush=True,
            )
    write(output / "models-after.json", model_identity(endpoint))
    hashes = {
        str(path.relative_to(output)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(output.rglob("*"))
        if path.is_file()
    }
    write(output / "manifest.json", {"files": hashes, "trials": len(rows)})
    route_failures = [
        (row["case_id"], row["condition"], row["model_calls"])
        for row in rows
        if row["model_calls"] != 1
    ]
    if route_failures:
        raise AssertionError(f"Unexpected model call counts: {route_failures}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case-pack", type=Path)
    arguments = parser.parse_args()
    run(arguments.endpoint, arguments.output, arguments.case_pack)
