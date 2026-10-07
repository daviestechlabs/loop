"""Independent HTTP fixtures drive the native RAG service, bus, and probe."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = ROOT / 'voice/c-runtime'
COLLECTION = 'dnd_text_chunks_c_' + 'a' * 64
CORPUS = 'sha256:' + 'a' * 64


@contextmanager
def backend(response):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            size = int(self.headers['Content-Length'])
            assert 0 < size <= 65536
            requests.append((self.path, self.headers.get('Authorization'), json.loads(self.rfile.read(size))))
            body = json.dumps(response).encode()
            self.send_response(200 if self.path.endswith('/search') else 500)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}', requests
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


class NativeBm25(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='c-rag-bm25-build-')
        cls.addClassCleanup(cls.build.cleanup)
        cls.probe = Path(cls.build.name) / 'probe'
        built = subprocess.run(
            [
                'bash',
                str(ROOT / 'benchmarks/retrieval-quality/scripts/build-c-probe.sh'),
                str(cls.probe),
            ],
            capture_output=True,
            text=True,
        )
        if built.returncode != 0:
            raise AssertionError(built.stderr or built.stdout or 'build-c-probe.sh failed')

    def run_case(self, premium=0, replacement=None, score=42.25, ruleset='', scalar='dnd-5e-2014', metadata='dnd-5e-2014', later_wrong=False):
        raw = (RUNTIME / 'tests/fuzz-corpus/dnd-retrieval/search.json').read_text()
        if replacement:
            raw = raw.replace('players-handbook', replacement)
        response = json.loads(raw)
        response['data'][0]['distance'] = score
        if ruleset:
            row = response['data'][0]
            if scalar is not None:
                row['ruleset'] = scalar
            nested = json.loads(row['metadata_json'])
            if metadata is None:
                nested.pop('ruleset')
            else:
                nested['ruleset'] = metadata
            row['metadata_json'] = json.dumps(nested)
            if later_wrong:
                other = dict(row, record_id='d' * 64, ruleset='lotr-5e')
                other['metadata_json'] = json.dumps(dict(nested, record_id='d' * 64, ruleset='lotr-5e'))
                response['data'].append(other)
        query = 'Which "雪" rule applies?\nKeep the complete question.'
        with tempfile.TemporaryDirectory(prefix='c-rag-bm25-') as directory, backend(response) as (url, requests):
            path = Path(directory)
            socket = str(path / 'bus.sock')
            environment = {k: v for k, v in os.environ.items()
                if not k.startswith(('RAG_', 'EMBED_', 'EMBEDDING_', 'MILVUS_', 'VBUS_'))}
            environment.update(VBUS_PATH=socket, EMBED_HTTP_URL=url + '/embeddings',
                MILVUS_SEARCH_URL=url + '/v2/vectordb/entities/search', EMBEDDING_MODEL_ID='bge-m3',
                EMBEDDING_DIMENSIONS='3', RAG_SHARED_RULEBOOK_COLLECTION=COLLECTION,
                RAG_SHARED_RULEBOOK_SEARCH='bm25', RAG_SHARED_RULEBOOK_RULESET=ruleset,
                MILVUS_HTTP_AUTH_TOKEN='fixture-token')
            processes = []
            try:
                for binary, args, marker in [('vbus-broker', [socket], 'vbus broker listening'),
                                             ('c-rag-gateway', [], 'enabled=1')]:
                    log = path / (binary + '.log')
                    with log.open('w') as output:
                        process = subprocess.Popen([str(RUNTIME / binary), *args], env=environment,
                            stdout=output, stderr=output)
                    processes.append(process)
                    deadline = time.monotonic() + 5
                    while marker not in log.read_text():
                        self.assertIsNone(process.poll())
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                result = subprocess.run([str(self.probe), socket, 'bm25-case', COLLECTION, query, CORPUS,
                    str(premium)], capture_output=True, text=True, timeout=5)
                self.assertIn(result.returncode, (0, 1), result.stderr)
                value = json.loads(result.stdout)
                self.assertEqual(len(requests), 1, 'Lexical retrieval must not call the embedding endpoint')
                endpoint, authorization, body = requests[0]
                self.assertEqual(endpoint, '/v2/vectordb/entities/search')
                self.assertEqual(authorization, 'Bearer fixture-token')
                self.assertEqual(body['data'], [query])
                self.assertEqual(body['annsField'], 'sparse')
                self.assertEqual(body['searchParams']['metricType'], 'BM25')
                self.assertEqual(body['collectionName'], COLLECTION)
                self.assertEqual(body['limit'], 4)
                self.assertIn('owner_user_id == ""', body['filter'])
                self.assertIn('visibility == "public"', body['filter'])
                self.assertEqual('tashas-cauldron-of-everything' in body['filter'], bool(premium))
                self.assertEqual('ruleset' in body['outputFields'], bool(ruleset))
                if ruleset:
                    self.assertIn(' and ruleset == "' + ruleset + '"', body['filter'])
                else:
                    self.assertNotIn('ruleset', body['filter'])
                return result.returncode, value
            finally:
                for process in reversed(processes):
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_free_core_and_raw_metric(self):
        code, value = self.run_case()
        self.assertEqual(code, 0)
        self.assertEqual(value['documents'][0]['score'], 42.25)
        self.assertEqual(value['documents'][0]['score_metric'], 'bm25')

    def test_premium_catalog(self):
        code, value = self.run_case(premium=1, replacement='tashas-cauldron-of-everything')
        self.assertEqual(code, 0)
        self.assertEqual(value['documents'][0]['book_slug'], 'tashas-cauldron-of-everything')

    def test_free_denies_unentitled_hit(self):
        code, value = self.run_case(replacement='tashas-cauldron-of-everything')
        self.assertEqual(code, 1)
        self.assertEqual(value['documents'], [])
        self.assertEqual(value['error'], 'search provenance rejected')

    def test_negative_bm25_rejects(self):
        code, value = self.run_case(score=-0.5)
        self.assertEqual(code, 1)
        self.assertEqual(value['documents'], [])
        self.assertEqual(value['error'], 'search provenance rejected')

    def test_matching_ruleset_and_entitlement(self):
        code, value = self.run_case(ruleset='dnd-5e-2014')
        self.assertEqual(code, 0)
        self.assertEqual(len(value['documents']), 1)
        code, value = self.run_case(ruleset='dnd-5e-2014', replacement='tashas-cauldron-of-everything')
        self.assertEqual(code, 1)
        self.assertEqual(value['documents'], [])

    def test_backend_cannot_widen_ruleset(self):
        for scalar, metadata in [('lotr-5e', 'lotr-5e'), ('lotr-5e', 'dnd-5e-2014'),
                                 ('dnd-5e-2014', 'lotr-5e'), (None, 'dnd-5e-2014'), ('dnd-5e-2014', None)]:
            with self.subTest(scalar=scalar, metadata=metadata):
                code, value = self.run_case(ruleset='dnd-5e-2014', scalar=scalar, metadata=metadata)
                self.assertEqual(code, 1)
                self.assertEqual(value['documents'], [])
                self.assertEqual(value['error'], 'search provenance rejected')

    def test_later_wrong_ruleset_erases_admitted_hit(self):
        code, value = self.run_case(ruleset='dnd-5e-2014', later_wrong=True)
        self.assertEqual(code, 1)
        self.assertEqual(value['documents'], [])
        self.assertEqual(value['error'], 'search provenance rejected')


if __name__ == '__main__':
    unittest.main()
