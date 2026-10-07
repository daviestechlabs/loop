"""Paired C product experiment against live model and read-only Milvus backends.

This is a diagnostic pilot, not a human voice or release acceptance test.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import uuid

from coupled_dialogue_eval import Cohort, capture_server, model_identity, write
from verify_coupled_dialogue import model_catalog

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "voice/c-runtime/tests"))
import test_dnd_product as product  # noqa: E402

CONDITIONS = ("direct", "live_retrieval")
CASES = (
    (
        "shield_ally",
        "Using the 2014 rules, can I cast Shield on Mira when an arrow hits her?",
        "Shield targets Self. Do not offer it as protection for another creature.",
    ),
    (
        "misty_step_ally",
        "Using the 2014 rules, can I use Misty Step to teleport Mira out of danger instead of myself?",
        "Misty Step teleports the caster, not another creature.",
    ),
    (
        "sanctuary_area",
        "Using the 2014 rules, will Sanctuary on Mira protect her from a Fireball explosion?",
        "Sanctuary does not protect against area effects. Do not invent invisibility or immunity.",
    ),
    (
        "shield_faith_save",
        "Using the 2014 rules, does Shield of Faith give Mira a bonus to Dexterity saving throws, and how much AC does it add?",
        "Shield of Faith adds 2 AC, not a saving-throw bonus.",
    ),
    (
        "unknown_capability",
        "Using the 2014 rules, should I protect Mira with Sanctuary or Shield of Faith? You do not know my class, prepared spells, slots, or the threat.",
        "Give conditional tradeoffs and ask for relevant missing information. Do not invent available spells or a threat.",
    ),
    (
        "unknown_geometry",
        "Using the 2014 rules, can I place Fireball so that Mira is safe? You do not know her position or where any enemies are.",
        "Require positions and area geometry before claiming safety; do not invent a safe placement.",
    ),
)


def load_cases(path):
    value = json.loads(path.read_text())
    if value.get("schema") != "grounded-rules-cases/v1" or set(value) != {
        "schema",
        "cases",
    }:
        raise ValueError("Unknown grounded rules case pack")
    rows = value["cases"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= 32:
        raise ValueError("Invalid case count")
    cases = []
    for row in rows:
        if (
            not isinstance(row, list)
            or len(row) != 3
            or not all(isinstance(v, str) and v for v in row)
        ):
            raise ValueError("Invalid authored case")
        if not re.fullmatch(r"[a-z0-9_]{1,64}", row[0]) or len(row[1].encode()) >= 2048:
            raise ValueError("Invalid case identity or message")
        cases.append(tuple(row))
    if len({c[0] for c in cases}) != len(cases):
        raise ValueError("Duplicate case identity")
    return tuple(cases)


class GroundedCohort(Cohort):
    def setUp(self):
        # Use the product's isolated auth/tool fixture, without scene facts.
        product.ProductProcess.setUp(self)
        self.stop("cascade")
        config = json.loads((self.source / "receipt.json").read_text())["config"]
        self.environment.update(config)
        self.environment.update(
            RAG_SOURCE_INDEX_PATH=str(self.source / "index.dndsidx"),
            RAG_SOURCE_MANIFEST_PATH=str(self.source / "manifest.json"),
            MILVUS_SEARCH_URL=self.backend_endpoint + "/v2/vectordb/entities/search",
            MILVUS_QUERY_URL=self.backend_endpoint + "/v2/vectordb/entities/query",
            EMBED_HTTP_URL=self.backend_endpoint + "/embeddings",
            EMBEDDING_MODEL_ID="bge-m3",
            EMBEDDING_DIMENSIONS="1024",
            RAG_SHARED_RULEBOOK_SEARCH="bm25",
            RAG_HTTP_TIMEOUT_MS="30000",
            RAG_TIMEOUT_MS="35000",
            LLM_GROUNDED_MAX_COMPLETION_TOKENS="256",
            PROMPT_LIBRARY_ROOT=str(ROOT / "contracts/prompt-library"),
        )
        self.start("rag", product.RUNTIME / "c-rag-gateway", "RAG_GATEWAY_BIN")
        self.wait_for(lambda: "enabled=1 source_index=1" in self.log("rag"))
        self.start(
            "cascade", product.RUNTIME / "c-cascade-router", "CASCADE_ROUTER_BIN"
        )
        self.wait_for(lambda: self.log("cascade").count("pure-C direct admission") == 2)


def trial(endpoint, source, directory, case_id, message, condition):
    directory.mkdir()
    backend_dir = directory / "retrieval"
    backend_dir.mkdir()
    model_server, model_thread, exchanges = capture_server(endpoint, directory)
    backend_server, backend_thread, retrieval = capture_server(endpoint, backend_dir)
    case = GroundedCohort()
    case.source = source
    case.model_endpoint = (
        f"http://127.0.0.1:{model_server.server_port}/v1/chat/completions"
    )
    case.backend_endpoint = f"http://127.0.0.1:{backend_server.server_port}"
    row = {"case_id": case_id, "condition": condition, "message": message}
    try:
        case.setUp()
        body = {
            "message": message,
            "request_id": str(uuid.uuid4()),
            "enable_tts": False,
            "enable_rag": False,
            "metadata": {"interaction_profile": "dnd_app", "campaign_id": "table"},
        }
        if condition == "live_retrieval":
            body["metadata"].update(
                knowledge_scope="shared_rulebook", retrieval_force="true"
            )
        write(directory / "request.json", body)
        status, raw, _ = case.http(
            case.product_port, "POST", "/api/turns", body, case.cookie
        )
        (directory / "response.ndjson").write_bytes(raw)
        events = [json.loads(line) for line in raw.splitlines() if line]
        finals = [event for event in events if event["type"] == "text_completed"]
        row.update(
            status=status,
            events=events,
            answer=finals[0]["text"] if len(finals) == 1 else None,
            completed=bool(
                status == 200 and events and events[-1]["type"] == "completed"
            ),
            model_calls=len(exchanges),
            retrieval_calls=len(retrieval),
        )
    except Exception as error:
        row["error"] = repr(error)
    finally:
        if hasattr(case, "logs"):
            for name in case.logs:
                (directory / f"{name}.log").write_text(case.log(name))
        row["cleanup_ok"] = case.doCleanups()
        for server, thread in (
            (model_server, model_thread),
            (backend_server, backend_thread),
        ):
            server.shutdown()
            thread.join(timeout=3)
            server.server_close()
        write(directory / "model-exchanges.json", exchanges)
        write(backend_dir / "model-exchanges.json", retrieval)
        write(directory / "trial.json", row)
    return row


def run(endpoint, source, output, case_pack=None):
    cases = CASES if case_pack is None else load_cases(case_pack)
    output.mkdir()
    source = source.resolve(strict=True)
    receipt = json.loads((source / "receipt.json").read_text())
    for filename in ("index.dndsidx", "manifest.json"):
        assert (
            hashlib.sha256((source / filename).read_bytes()).hexdigest()
            == receipt["files"][filename]["sha256"]
        )
    write(
        output / "design.json",
        {
            "conditions": CONDITIONS,
            "cases": cases,
            "case_pack_sha256": None
            if case_pack is None
            else hashlib.sha256(case_pack.read_bytes()).hexdigest(),
            "repetitions": 1,
            "order": "Alternate condition order for each successive case",
            "source": receipt,
            "audio": False,
            "training": False,
            "scope": "C authenticated product, gateway, cascade, reviewed source index, live Milvus BM25 and live model; isolated auth and tool fixture; no campaign scene facts",
            "limitations": "Diagnostic pilot; no population accuracy, latency, acoustic, or human acceptance claim. Longer backend timeouts accommodate the operator relay.",
        },
    )
    if case_pack is not None:
        (output / "case-pack.json").write_bytes(case_pack.read_bytes())
    before = model_identity(endpoint)
    write(output / "model-before.json", before)
    rows = []
    for index, (case_id, message, _rubric) in enumerate(cases):
        for condition in CONDITIONS[:: 1 if index % 2 == 0 else -1]:
            row = trial(
                endpoint,
                source,
                output / f"{case_id}-{condition}",
                case_id,
                message,
                condition,
            )
            rows.append(row)
            print(
                json.dumps(
                    {
                        k: row.get(k)
                        for k in (
                            "case_id",
                            "condition",
                            "completed",
                            "model_calls",
                            "retrieval_calls",
                            "error",
                        )
                    }
                ),
                flush=True,
            )
            write(output / "results.json", rows)
    after = model_identity(endpoint)
    write(output / "model-after.json", after)
    assert model_catalog(before) == model_catalog(after)
    assert len(rows) == 2 * len(cases) and all(
        r.get("completed") and r.get("cleanup_ok") for r in rows
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case-pack", type=Path)
    args = parser.parse_args()
    run(args.endpoint, args.source, args.output, args.case_pack)
