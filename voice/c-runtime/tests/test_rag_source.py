"""Local HTTP fixtures exercise the C manifest loader, selector, fetch and wire."""

from contextlib import contextmanager
import copy
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = ROOT / "voice/c-runtime"
BIN = Path(os.environ.get("RAG_TEST_RUNTIME_DIR", RUNTIME))
COLLECTION = "dnd_text_chunks_c_" + "a" * 64
CORPUS = "sha256:" + "a" * 64


def row(record):
    metadata = dict(
        record_id=record["record_id"],
        document_id=record["document_id"],
        book_slug=record["book_slug"],
        content_hash=record["content_hash"],
        source_sha256=record["source_sha256"],
        page_start=record["page"],
        page_end=record["page"],
        chunk_index=record["chunk"],
        knowledge_scope="shared_rulebook",
        source_kind="official_book",
        visibility="public",
        owner_user_id="",
        campaign_id="",
        session_id="",
        character_id="",
        corpus_version=CORPUS,
        source_etag="",
        embedding_model="bge-m3",
        ruleset="dnd-5e-2014",
        section="",
    )
    result = {
        key: metadata[key]
        for key in [
            "record_id",
            "document_id",
            "book_slug",
            "source_kind",
            "visibility",
            "owner_user_id",
            "campaign_id",
            "corpus_version",
            "source_etag",
            "page_start",
            "page_end",
            "ruleset",
        ]
    }
    result.update(
        content=record["content"],
        source="book://" + record["book_slug"],
        metadata_json=json.dumps(metadata),
    )
    return result


def change_metadata(hit, **changes):
    metadata = json.loads(hit["metadata_json"])
    metadata.update(changes)
    for key, value in changes.items():
        if key in hit:
            hit[key] = value
    hit["metadata_json"] = json.dumps(metadata)


@contextmanager
def backend(baseline, records, mutate=None, delays=(0, 0)):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            size = int(self.headers["Content-Length"])
            assert 0 < size <= 65536
            body = json.loads(self.rfile.read(size))
            requests.append((self.path, self.headers.get("Authorization"), body))
            if self.path.endswith("/search"):
                time.sleep(delays[0])
                data = copy.deepcopy(baseline)
                for hit in data:
                    hit["distance"] = 12.5
            else:
                assert self.path.endswith("/query"), (
                    "Source selection must not call embeddings"
                )
                time.sleep(delays[1])
                ids = json.loads(
                    re.search(r"record_id in (\[.*\])$", body["filter"])[1]
                )
                assert len(ids) == len(set(ids)) and 0 < len(ids) <= 4
                data = [copy.deepcopy(records[rid]) for rid in ids]
                if mutate:
                    data = mutate(data)
            encoded = json.dumps({"code": 0, "data": data}).encode()
            try:
                self.send_response(200)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(
        target=lambda: server.serve_forever(poll_interval=0.01), daemon=True
    )
    worker.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", requests
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def source_environment(path, url="http://127.0.0.1:1"):
    environment = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith(("RAG_", "EMBED_", "EMBEDDING_", "MILVUS_", "VBUS_"))
    }
    environment.update(
        VBUS_PATH=str(path / "bus.sock"),
        EMBED_HTTP_URL=url + "/embeddings",
        MILVUS_SEARCH_URL=url + "/v2/vectordb/entities/search",
        MILVUS_QUERY_URL=url + "/v2/vectordb/entities/query",
        EMBEDDING_MODEL_ID="bge-m3",
        EMBEDDING_DIMENSIONS="3",
        MILVUS_HTTP_AUTH_TOKEN="fixture-token",
        RAG_SHARED_RULEBOOK_COLLECTION=COLLECTION,
        RAG_SHARED_RULEBOOK_SEARCH="bm25",
        RAG_SHARED_RULEBOOK_RULESET="dnd-5e-2014",
        RAG_SHARED_RULEBOOK_CORPUS_VERSION=CORPUS,
        RAG_SOURCE_INDEX_PATH=str(path / "index.dndsidx"),
        RAG_SOURCE_INDEX_SHA256=hashlib.sha256(
            (path / "index.dndsidx").read_bytes()
        ).hexdigest(),
        RAG_SOURCE_MANIFEST_PATH=str(path / "manifest.json"),
        RAG_SOURCE_MANIFEST_SHA256=hashlib.sha256(
            (path / "manifest.json").read_bytes()
        ).hexdigest(),
        RAG_SOURCE_COMPILER_SHA256="d" * 64,
    )
    return environment


class NativeSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="c-rag-source-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.probe = Path(cls.build.name) / "probe"
        built = subprocess.run(
            [
                "bash",
                str(ROOT / "benchmarks/retrieval-quality/scripts/build-c-probe.sh"),
                str(cls.probe),
            ],
            capture_output=True,
            text=True,
        )
        if built.returncode != 0:
            raise AssertionError(
                built.stderr or built.stdout or "build-c-probe.sh failed"
            )

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="c-rag-source-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        subprocess.run(
            [str(RUNTIME / ".build/dnd-source-artifact/emit"), str(self.path)],
            check=True,
            capture_output=True,
        )
        self.records = json.loads((self.path / "records.json").read_text())
        self.rows = {r["record_id"]: row(r) for r in self.records}
        self.manifest = json.loads((self.path / "manifest.json").read_text())
        for record, source in [
            (self.records[0], self.manifest["sources"][0]),
            (self.records[3], self.manifest["sources"][1]),
        ]:
            identity = hashlib.sha256(
                (CORPUS + ":" + source["source_key"]).encode()
            ).hexdigest()[:24]
            self.assertEqual(record["document_id"], identity)

    def environment(self, url="http://127.0.0.1:1"):
        return source_environment(self.path, url)

    def run_case(
        self,
        query="How often can I use Warding Step in a round?",
        premium=1,
        baseline=None,
        mutate=None,
        delays=(0, 0),
        timeout=3000,
    ):
        if baseline is None:
            baseline = [self.rows[self.records[0]["record_id"]]]
        with backend(baseline, self.rows, mutate, delays) as (url, requests):
            environment = self.environment(url)
            environment["RAG_HTTP_TIMEOUT_MS"] = str(timeout)
            processes = []
            try:
                for binary, args, marker in [
                    (
                        "vbus-broker",
                        [environment["VBUS_PATH"]],
                        "vbus broker listening",
                    ),
                    ("c-rag-gateway", [], "enabled=1 source_index=1"),
                ]:
                    log = self.path / (binary + ".log")
                    with log.open("w") as output:
                        process = subprocess.Popen(
                            [str(BIN / binary), *args],
                            env=environment,
                            stdout=output,
                            stderr=output,
                        )
                    processes.append(process)
                    deadline = time.monotonic() + 8
                    while marker not in log.read_text():
                        self.assertIsNone(process.poll(), log.read_text())
                        self.assertLess(time.monotonic(), deadline, log.read_text())
                        time.sleep(0.01)
                started = time.monotonic()
                result = subprocess.run(
                    [
                        str(self.probe),
                        environment["VBUS_PATH"],
                        "source-case",
                        COLLECTION,
                        query,
                        CORPUS,
                        str(premium),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=8,
                )
                elapsed = time.monotonic() - started
                self.assertIn(result.returncode, (0, 1), result.stderr)
                self.assertNotRegex(
                    result.stderr, r"AddressSanitizer|LeakSanitizer|runtime error:"
                )
                value = json.loads(result.stdout)
                for endpoint, auth, body in requests:
                    self.assertEqual(auth, "Bearer fixture-token")
                    self.assertEqual(body["collectionName"], COLLECTION)
                    self.assertIn('owner_user_id == ""', body["filter"])
                    self.assertIn('visibility == "public"', body["filter"])
                    self.assertIn('corpus_version == "' + CORPUS + '"', body["filter"])
                    self.assertIn('ruleset == "dnd-5e-2014"', body["filter"])
                    self.assertEqual(
                        "tashas-cauldron-of-everything" in body["filter"], bool(premium)
                    )
                    self.assertIn("ruleset", body["outputFields"])
                    if endpoint.endswith("/query"):
                        self.assertNotIn("data", body)
                        self.assertNotIn("searchParams", body)
                        ids = json.loads(
                            re.search(r"record_id in (\[.*\])$", body["filter"])[1]
                        )
                        self.assertEqual(body["limit"], len(ids))
                return result.returncode, value, requests, elapsed
            finally:
                exits = []
                for process in reversed(processes):
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                    exits.append(process.returncode)
                for log in self.path.glob("*.log"):
                    self.assertNotRegex(
                        log.read_text(),
                        r"AddressSanitizer|LeakSanitizer|runtime error:",
                    )
                self.assertEqual(exits, [0] * len(processes))

    def passage_fixture(self):
        for name in ("index.dndsidx", "manifest.json", "records.json"):
            (self.path / name).unlink()
        subprocess.run(
            [
                str(RUNTIME / ".build/dnd-source-artifact/emit"),
                str(self.path),
                "--passages",
            ],
            check=True,
        )
        self.records = json.loads((self.path / "records.json").read_text())
        self.rows = {r["record_id"]: row(r) for r in self.records}

    def test_complete_passage_five_witnesses_in_two_authorized_batches(self):
        self.passage_fixture()
        baseline = [self.rows[self.records[4]["record_id"]]]
        code, value, requests, _ = self.run_case("Can I cast Aegis?", baseline=baseline)
        self.assertEqual(code, 0, value)
        self.assertEqual(len(value["documents"]), 1)
        citation = value["documents"][0]
        self.assertEqual(citation["kind"], "complete_passage")
        self.assertEqual(citation["record_id"], "")
        self.assertRegex(citation["passage_id"], r"^[a-f0-9]{64}$")
        self.assertEqual((citation["page_start"], citation["page_end"]), (40, 41))
        self.assertEqual(len(citation["witnesses"]), 5)
        assembled = bytearray()
        last = None
        for witness in citation["witnesses"]:
            original = next(
                r for r in self.records if r["record_id"] == witness["record_id"]
            )
            raw = original["content"].encode()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), witness["content_hash"])
            self.assertEqual(len(raw), witness["record_length"])
            if last is not None and last != witness["page"]:
                assembled.extend(b"\n")
            assembled.extend(raw[witness["begin"] : witness["end"]])
            last = witness["page"]
        self.assertEqual(
            hashlib.sha256(assembled).hexdigest(), citation["content_hash"]
        )
        identity = bytearray(
            b"dnd-complete-passage/v1\0" + citation["content_hash"].encode()
        )
        for witness in citation["witnesses"]:
            identity.extend(witness["record_id"].encode())
            identity.extend(
                struct.pack(
                    "<4I",
                    witness["page"],
                    witness["chunk_index"],
                    witness["begin"],
                    witness["end"],
                )
            )
        self.assertEqual(hashlib.sha256(identity).hexdigest(), citation["passage_id"])
        self.assertTrue(assembled.endswith(b" remains until dawn."))
        self.assertNotIn(b"LAMPLIGHT", assembled)
        queries = [body for path, _, body in requests if path.endswith("/query")]
        self.assertEqual([q["limit"] for q in queries], [4, 1])

    def test_two_complete_passages_recover_second_name_in_anchored_document(self):
        self.passage_fixture()
        code, value, requests, _ = self.run_case(
            "Can I cast Aegis or Beacon?",
            baseline=[self.rows[self.records[4]["record_id"]]],
        )
        self.assertEqual(code, 0, value)
        self.assertEqual(
            [d["section"] for d in value["documents"]], ["aegis", "beacon"]
        )
        self.assertEqual([len(d["witnesses"]) for d in value["documents"]], [5, 1])
        self.assertEqual(
            [b["limit"] for path, _, b in requests if path.endswith("/query")], [4, 2]
        )

    def test_second_witness_batch_rejection_exposes_no_partial_passage(self):
        self.passage_fixture()
        calls = 0

        def mutate(records):
            nonlocal calls
            calls += 1
            if calls == 2:
                records[0]["content"] += " forged"
            return records

        code, value, requests, _ = self.run_case(
            "Can I cast Aegis?",
            mutate=mutate,
            baseline=[self.rows[self.records[4]["record_id"]]],
        )
        self.assertEqual(code, 1, value)
        self.assertEqual(value["documents"], [])
        self.assertEqual(calls, 2)
        self.assertIn("passage witness", value["error"])

    def test_complete_rule_and_reaction_from_reordered_fetch(self):
        code, value, requests, _ = self.run_case(
            mutate=lambda hits: list(reversed(hits))
        )

        self.assertEqual(code, 0)
        self.assertEqual(len(requests), 2)
        self.assertEqual(
            [d["record_id"] for d in value["documents"]],
            [r["record_id"] for r in self.records[:3]],
        )
        self.assertTrue(
            all(
                d["score_metric"] == "none" and d["score"] == 0
                for d in value["documents"]
            )
        )

    def test_spell_excludes_neighbors_after_full_record_verification(self):
        self.path = self.path / "spell"
        self.path.mkdir()
        subprocess.run(
            [
                str(RUNTIME / ".build/dnd-source-artifact/emit"),
                str(self.path),
                "--spell",
            ],
            check=True,
        )
        self.records = json.loads((self.path / "records.json").read_text())
        self.rows = {record["record_id"]: row(record) for record in self.records}
        query = "How does the Aegis spell work?"
        code, value, requests, _ = self.run_case(query=query)
        self.assertEqual(code, 0)
        self.assertEqual(len(requests), 2)
        record = self.records[0]
        raw = record["content"].encode()
        begin, end = raw.index(b"AEGIS"), raw.index(b" NEXT RULE")
        citation = value["documents"][0]
        self.assertEqual(citation["content_hash"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(citation["excerpt_spans"], [{"begin": begin, "end": end}])
        prompt = (
            "Retrieved excerpts (source data):\n"
            f"\n[Source 1; name {citation['book_slug']}; record {citation['record_id']}; pages 7-7; section ; chunk 0]\n"
            + raw[begin:end].decode()
            + "\n[End source 1]\n\nQuestion: "
            + query
        )
        self.assertEqual(
            value["grounded_prompt_sha256"], hashlib.sha256(prompt.encode()).hexdigest()
        )

    def test_reaction_limit_requires_its_complete_second_record(self):
        self.path = self.path / "complete-reaction"
        self.path.mkdir()
        subprocess.run(
            [
                str(RUNTIME / ".build/dnd-source-artifact/emit"),
                str(self.path),
                "--complete-reaction",
            ],
            check=True,
            capture_output=True,
        )
        self.records = json.loads((self.path / "records.json").read_text())
        self.rows = {record["record_id"]: row(record) for record in self.records}
        for premium in (0, 1):
            with self.subTest(premium=premium):
                code, value, requests, _ = self.run_case(
                    premium=premium, mutate=lambda hits: list(reversed(hits))
                )
                self.assertEqual(code, 0)
                self.assertEqual(len(requests), 2)
                self.assertEqual(
                    [document["record_id"] for document in value["documents"]],
                    [self.records[i]["record_id"] for i in (0, 1, 2, 6)],
                )
                self.assertTrue(
                    all(
                        document["score_metric"] == "none"
                        for document in value["documents"]
                    )
                )
                self.assertIn(
                    "must wait until your next turn", self.records[6]["content"]
                )

    def test_wrong_page_and_opportunity_attack_use_original_source_records(self):
        baseline = [self.rows[self.records[2]["record_id"]]]
        for premium in [0, 1]:
            with self.subTest(premium=premium):
                code, value, requests, _ = self.run_case(
                    query="Can Warding Step happen on an opportunity attack?",
                    premium=premium,
                    baseline=baseline,
                )
                self.assertEqual(code, 0)
                self.assertEqual(len(requests), 2)
                self.assertEqual(
                    [d["record_id"] for d in value["documents"]],
                    [r["record_id"] for r in self.records[:3]],
                )
                self.assertTrue(
                    all(d["score_metric"] == "none" for d in value["documents"])
                )

    def test_ordinary_named_rule_does_not_fetch_reaction_context(self):
        code, value, requests, _ = self.run_case(
            query="What does Warding Step require?"
        )
        self.assertEqual(code, 0)
        self.assertEqual(len(requests), 2)
        self.assertEqual(
            [d["record_id"] for d in value["documents"]],
            [r["record_id"] for r in self.records[:2]],
        )

    def test_rule_does_not_fall_back_from_an_unindexed_document(self):
        hit = copy.deepcopy(next(iter(self.rows.values())))
        identity = hashlib.sha256((CORPUS + ":appendix-etag").encode()).hexdigest()[:24]
        change_metadata(
            hit, document_id=identity, record_id="e" * 64, source_etag="appendix-etag"
        )
        code, value, requests, _ = self.run_case(
            query="Can Warding Step happen on an opportunity attack?",
            baseline=[hit],
        )
        self.assertEqual(code, 0)
        self.assertEqual(len(requests), 1)
        self.assertEqual([d["record_id"] for d in value["documents"]], ["e" * 64])
        self.assertEqual(value["documents"][0]["score_metric"], "bm25")

    def test_subsection_keeps_each_entitled_source_without_baseline_anchors(self):
        for premium in [0, 1]:
            with self.subTest(premium=premium):
                code, value, requests, _ = self.run_case(
                    query="For concentration checks, what is the DC after damage?",
                    premium=premium,
                    baseline=[],
                )
                self.assertEqual(code, 0)
                expected = self.records[4:6] if premium else self.records[4:5]
                expected = sorted(expected, key=lambda r: r["document_id"])
                self.assertEqual(
                    [d["record_id"] for d in value["documents"]],
                    [r["record_id"] for r in expected],
                )
                self.assertEqual(
                    [d["content_hash"] for d in value["documents"]],
                    [r["content_hash"] for r in expected],
                )
                self.assertTrue(
                    all(d["score_metric"] == "none" for d in value["documents"])
                )
                self.assertEqual(len(requests), 2)

    def test_subsection_missing_variant_rejects_the_whole_source_group(self):
        code, value, requests, _ = self.run_case(
            query="What concentration saving throw follows taking damage?",
            baseline=[],
            mutate=lambda hits: hits[:-1],
        )
        self.assertEqual(code, 1)
        self.assertEqual(value["documents"], [])
        self.assertEqual(value["error"], "source record provenance rejected")
        self.assertEqual(len(requests), 2)

    def test_outgoing_damage_does_not_select_concentration_check_subsection(self):
        code, value, requests, _ = self.run_case(
            query="Does concentration increase my spell damage?",
        )
        self.assertEqual(code, 0)
        self.assertEqual(len(requests), 1)
        self.assertEqual(
            [d["record_id"] for d in value["documents"]], [self.records[0]["record_id"]]
        )
        self.assertEqual(value["documents"][0]["score_metric"], "bm25")

    def test_creature_replaces_distractor_and_does_not_need_search_hits(self):
        for baseline in [None, []]:
            with self.subTest(baseline=baseline):
                code, value, requests, _ = self.run_case(
                    query="What is the Iron Badger armor class?", baseline=baseline
                )
                self.assertEqual(code, 0)
                self.assertEqual(
                    [d["record_id"] for d in value["documents"]],
                    [self.records[3]["record_id"]],
                )
                self.assertEqual(len(requests), 2)

    def test_free_scope_never_fetches_premium_source(self):
        code, value, requests, _ = self.run_case(
            query="What is the Iron Badger armor class?", premium=0
        )
        self.assertEqual(code, 0)
        self.assertEqual(len(requests), 1)
        self.assertEqual(value["documents"][0]["book_slug"], "players-handbook")
        self.assertEqual(value["documents"][0]["score_metric"], "bm25")

    def test_reviewed_document_outside_index_uses_etag_identity(self):
        hit = copy.deepcopy(next(iter(self.rows.values())))
        identity = hashlib.sha256((CORPUS + ":appendix-etag").encode()).hexdigest()[:24]
        change_metadata(
            hit, document_id=identity, record_id="e" * 64, source_etag="appendix-etag"
        )
        code, value, requests, _ = self.run_case(query="What is cover?", baseline=[hit])
        self.assertEqual(code, 0)
        self.assertEqual(value["documents"][0]["document_id"], identity)
        self.assertEqual(len(requests), 1)

    def test_baseline_requires_reviewed_source_and_exact_corpus(self):
        for changes in [
            dict(source_sha256="e" * 64),
            dict(document_id="unreviewed"),
            dict(corpus_version="sha256:" + "e" * 64),
        ]:
            with self.subTest(changes=changes):
                hit = copy.deepcopy(next(iter(self.rows.values())))
                change_metadata(hit, **changes)
                code, value, requests, _ = self.run_case(baseline=[hit])
                self.assertEqual(code, 1)
                self.assertEqual(value["documents"], [])
                self.assertEqual(len(requests), 1)

    def test_missing_duplicate_extra_or_scored_fetch_rejects_whole_group(self):
        def extra(hits):
            unknown = copy.deepcopy(hits[0])
            change_metadata(unknown, record_id="e" * 64)
            return hits[:-1] + [unknown]

        def scored(hits):
            hits[-1]["distance"] = 12.5
            return hits

        for mutate in [
            lambda hits: hits[:-1],
            lambda hits: hits[:1] * len(hits),
            extra,
            scored,
            lambda hits: hits + hits[:1],
        ]:
            with self.subTest(mutate=mutate):
                code, value, requests, _ = self.run_case(mutate=mutate)
                self.assertEqual(code, 1)
                self.assertEqual(value["documents"], [])
                self.assertEqual(value["error"], "source record provenance rejected")
                self.assertEqual(len(requests), 2)

    def test_changed_record_fields_reject_even_with_consistent_backend_metadata(self):
        for changes in [
            dict(corpus_version="sha256:" + "e" * 64),
            dict(source_sha256="e" * 64),
            dict(page_start=23, page_end=23),
            dict(chunk_index=5),
            dict(owner_user_id="other-user"),
        ]:

            def mutate(hits):
                change_metadata(hits[-1], **changes)
                return hits

            with self.subTest(changes=changes):
                code, value, requests, _ = self.run_case(mutate=mutate)
                self.assertEqual(code, 1)
                self.assertEqual(value["documents"], [])
                self.assertEqual(len(requests), 2)

        def changed_text(hits):
            hits[-1]["content"] += " Changed."
            change_metadata(
                hits[-1],
                content_hash=hashlib.sha256(hits[-1]["content"].encode()).hexdigest(),
            )
            return hits

        code, value, _, _ = self.run_case(mutate=changed_text)
        self.assertEqual(code, 1)
        self.assertEqual(value["error"], "source record differs from index")
        self.assertEqual(value["documents"], [])

    def test_search_and_fetch_share_one_deadline(self):
        code, value, requests, elapsed = self.run_case(
            delays=(0.65, 0.65), timeout=1000
        )
        self.assertEqual(code, 1)
        self.assertEqual(value["documents"], [])
        self.assertEqual(value["error"], "source record response unavailable")
        self.assertEqual(len(requests), 2)
        self.assertLess(elapsed, 1.8)

    def reject_startup(self, environment):
        result = subprocess.run(
            [str(BIN / "c-rag-gateway")],
            env=environment,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("invalid retrieval backend configuration", result.stderr)
        self.assertNotRegex(
            result.stderr, r"AddressSanitizer|LeakSanitizer|runtime error:"
        )

    def reseal_manifest(self, raw):
        original = (self.path / "index.dndsidx").read_bytes()
        (self.path / "manifest.json").write_bytes(raw)
        (self.path / "index.dndsidx").write_bytes(
            original[:8] + hashlib.sha256(raw).digest() + original[40:]
        )

    def test_manifest_source_limit_includes_disabled_rows(self):
        self.manifest["sources"] += [{"enabled": False}] * 125
        self.reseal_manifest(json.dumps(self.manifest).encode())
        code, _, _, _ = self.run_case()
        self.assertEqual(code, 0)
        self.manifest["sources"].append({"enabled": False})
        self.reseal_manifest(json.dumps(self.manifest).encode())
        self.reject_startup(self.environment())

    def test_disabled_nonindexed_document_is_not_authorized(self):
        self.manifest["sources"][2]["enabled"] = False
        self.manifest["enabled_sources"] = 2
        self.reseal_manifest(json.dumps(self.manifest).encode())
        hit = copy.deepcopy(next(iter(self.rows.values())))
        identity = hashlib.sha256((CORPUS + ":appendix-etag").encode()).hexdigest()[:24]
        change_metadata(
            hit, document_id=identity, record_id="e" * 64, source_etag="appendix-etag"
        )
        code, value, requests, _ = self.run_case(query="What is cover?", baseline=[hit])
        self.assertEqual(code, 1)
        self.assertEqual(value["documents"], [])
        self.assertEqual(value["error"], "search source identity rejected")
        self.assertEqual(len(requests), 1)

    def test_manifest_invalid_bytes_and_duplicate_fields_reject(self):
        raw = (self.path / "manifest.json").read_bytes()
        for changed in [
            raw + b"\0",
            raw.replace(b"rules.pdf", b"rules\xff.pdf"),
            raw.replace(
                b'"enabled_sources":3', b'"enabled_sources":3,"enabled_sources":3'
            ),
            raw.replace(b'"etag":""', b'"etag":"","etag":""', 1),
            b" " * (256 * 1024 + 1),
        ]:
            with self.subTest(length=len(changed)):
                self.reseal_manifest(changed)
                self.reject_startup(self.environment())

    def test_partial_or_mismatched_configuration_rejects_startup(self):
        for key in [
            "RAG_SOURCE_INDEX_PATH",
            "RAG_SOURCE_INDEX_SHA256",
            "RAG_SOURCE_MANIFEST_PATH",
            "RAG_SOURCE_MANIFEST_SHA256",
            "RAG_SOURCE_COMPILER_SHA256",
            "MILVUS_QUERY_URL",
            "RAG_SHARED_RULEBOOK_RULESET",
            "RAG_SHARED_RULEBOOK_CORPUS_VERSION",
        ]:
            with self.subTest(missing=key):
                environment = self.environment()
                del environment[key]
                self.reject_startup(environment)
        for key, value in [
            ("RAG_SOURCE_INDEX_SHA256", "e" * 64),
            ("RAG_SOURCE_MANIFEST_SHA256", "e" * 64),
            ("RAG_SOURCE_COMPILER_SHA256", "e" * 64),
            ("RAG_SHARED_RULEBOOK_CORPUS_VERSION", "sha256:" + "e" * 64),
            ("RAG_SHARED_RULEBOOK_RULESET", "lotr-5e"),
        ]:
            with self.subTest(mismatch=key):
                environment = self.environment()
                environment[key] = value
                self.reject_startup(environment)

    def test_resealed_invalid_manifests_reject_startup(self):
        original = (self.path / "index.dndsidx").read_bytes()
        variants = []
        for key, value in [
            ("schema_version", "other"),
            ("corpus_id", "sha256:" + "e" * 64),
            ("enabled_sources", 2),
        ]:
            manifest = copy.deepcopy(self.manifest)
            manifest[key] = value
            variants.append(manifest)
        duplicate = copy.deepcopy(self.manifest)
        duplicate["sources"][1] = duplicate["sources"][0]
        variants.append(duplicate)
        disabled = copy.deepcopy(self.manifest)
        disabled["sources"][0]["enabled"] = False
        disabled["enabled_sources"] = 2
        variants.append(disabled)
        for key, value in [
            ("source_sha256", "e" * 64),
            ("source_key", "other.pdf"),
            ("etag", "other-etag"),
            ("enabled", False),
        ]:
            manifest = copy.deepcopy(self.manifest)
            manifest["sources"][0][key] = value
            variants.append(manifest)
        for manifest in variants:
            with self.subTest(manifest=manifest):
                raw = json.dumps(manifest).encode()
                (self.path / "manifest.json").write_bytes(raw)
                (self.path / "index.dndsidx").write_bytes(
                    original[:8] + hashlib.sha256(raw).digest() + original[40:]
                )
                self.reject_startup(self.environment())


if __name__ == "__main__":
    unittest.main()
