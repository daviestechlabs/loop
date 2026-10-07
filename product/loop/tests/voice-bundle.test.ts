import { test, expect } from 'bun:test';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { createHash } from 'node:crypto';
import { TURN_PROVENANCE_WASM_BASE64 } from '../../../contracts/ai-sdk/src/turnProvenanceWasm';

test('served Loop bundle preserves complete passage witnesses with the current C decoder', () => {
  const bundle = readFileSync(new URL('../static/app.js', import.meta.url), 'utf8');
  // Run the shipped protocol modules before the application starts DOM setup.
  // Do not rebuild here: a stale checked-in asset must fail this test.
  const application = bundle.search(/^\/\/ (?:product\/loop\/)?src\/app\.ts$/m);
  expect(application).toBeGreaterThan(0);
  const shipped = runInNewContext(bundle.slice(0, application) +
    '\n({ parseTurnEvent, wasm: TURN_PROVENANCE_WASM_BASE64 })',
    { TextEncoder, TextDecoder, WebAssembly, atob }, { timeout: 1000 });
  const hash = (value: string) => createHash('sha256').update(Buffer.from(value, 'base64')).digest('hex');
  expect(hash(shipped.wasm)).toBe(hash(TURN_PROVENANCE_WASM_BASE64));
  const fixtures = JSON.parse(readFileSync(new URL(
    '../../../contracts/ai-sdk/tests/fixtures/provenance-wire.json', import.meta.url), 'utf8'));
  const wire = new Uint8Array(Buffer.from(fixtures.passage, 'base64'));
  const event = shipped.parseTurnEvent(wire, 'req-browser-dnd');
  const [citation] = JSON.parse(event.metadata.cascade_retrieval_citations);
  expect(citation.kind).toBe('complete_passage');
  expect(citation.passage_id).toBe('a'.repeat(64));
  expect(citation.witnesses).toEqual([
    { record_id: '1'.repeat(64), content_hash: '2'.repeat(64), page: 96,
      chunk_index: 2, begin: 2, end: 10, record_length: 30 },
    { record_id: '3'.repeat(64), content_hash: '4'.repeat(64), page: 97,
      chunk_index: 0, begin: 0, end: 12, record_length: 40 },
  ]);
  expect(() => shipped.parseTurnEvent(wire, 'another-turn')).toThrow(/mismatched/);
  expect(() => shipped.parseTurnEvent(wire.subarray(0, wire.length - 1),
    'req-browser-dnd')).toThrow(/provenance/);
});
