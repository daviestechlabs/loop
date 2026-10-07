import { test, expect, afterEach } from 'bun:test';
import { anvilBundle, gatewaySTTInterval, gatewayMetadata, gatewayTokenUsage, reportedMilliseconds, CaptureSubmission, Recorder, api, type FinalRecord, type TurnRecord } from '../src/evidence';
const originalFetch = globalThis.fetch;
afterEach(() => { globalThis.fetch = originalFetch; });
const final: FinalRecord = { status: 'failed', event_count: 2, transcript: '', answer: '', error: 'test failure', recording_error: null };
test('recording retries the unacknowledged event without skipping or duplicating its sequence', async () => {
  const sent: number[] = []; let failOnce = true;
  globalThis.fetch = (async (_url: unknown, options: RequestInit) => {
    const value = JSON.parse(String(options.body)) as { seq: number };
    sent.push(value.seq);
    if (value.seq === 1 && failOnce) { failOnce = false; return Response.json({ error: 'store_unavailable' }, { status: 503 }); }
    return Response.json({ stored: true });
  }) as typeof fetch;
  const recorder = new Recorder('retry');
  recorder.record({ name: 'listening', source: 'browser', payload: {} });
  recorder.record({ name: 'committing', source: 'browser', payload: {} });
  await expect(recorder.flush()).rejects.toThrow('store_unavailable');
  await recorder.flush(); expect(sent).toEqual([0, 1, 1]);
});
test('finish saves events then audio then final, and failed audio prevents finalization', async () => {
  const calls: string[] = []; let failAudio = true;
  globalThis.fetch = (async (url: string) => {
    calls.push(url);
    if (url.includes('/audio/input/') && failAudio) return Response.json({}, { status: 503 });
    return Response.json({ stored: true });
  }) as typeof fetch;
  const recorder = new Recorder('saved');
  recorder.record({ name: 'listening', source: 'browser', payload: {} });
  recorder.addAudio('input', new Uint8Array([1, 2, 3, 4]), 16000);
  await expect(recorder.finish({ ...final, event_count: 1 })).rejects.toThrow('Could not save input');
  expect(calls.some((s) => s.endsWith('/finish'))).toBe(false);
  failAudio = false; await recorder.finish({ ...final, event_count: 1 });
  expect(calls.at(-1)).toBe('/api/turns/saved/finish');
  expect(calls.filter((s) => s.endsWith('/events')).length).toBe(1);
});
test('recorder copies audio bytes and rejects a changed format or oversized recording', async () => {
  const recorder = new Recorder('audio'); const pcm = new Uint8Array([1, 2]);
  recorder.addAudio('input', pcm, 16000); pcm[0] = 9;
  globalThis.fetch = (async (url: string, options: RequestInit) => {
    if (url.includes('/audio/input/')) expect(new Uint8Array(await (options.body as Blob).arrayBuffer())).toEqual(new Uint8Array([1, 2]));
    return Response.json({ stored: true });
  }) as typeof fetch;
  await recorder.finish({ ...final, event_count: 0 });
  expect(() => recorder.addAudio('input', pcm, 24000)).toThrow('format changed');
  expect(() => recorder.addAudio('output', new Uint8Array(4 * 1024 * 1024 + 2), 24000)).toThrow('capacity exceeded');
});
test('identity and API failures remain explicit', async () => {
  globalThis.fetch = (async () => Response.json({ error: 'sign_in_required' }, { status: 401 })) as typeof fetch;
  await expect(api('/api/session')).rejects.toThrow('sign_in_required');
});
test('failure capture retries keep immutable source, ID, and fields and coalesce concurrent saves', async () => {
  const record = { id: 'source', status: 'failed', manifest_sha256: 'a'.repeat(64), final_sha256: 'b'.repeat(64) } as TurnRecord;
  const fields = { title: 'Transport failed', category: 'transport', expected: 'Receive a complete response' };
  const capture = new CaptureSubmission(record, fields);
  record.id = 'different-source'; fields.expected = 'changed after submission';
  const bodies: string[] = []; let first = true;
  globalThis.fetch = (async (url: string, options: RequestInit) => {
    expect(url).toBe('/api/captures'); bodies.push(String(options.body));
    if (first) { first = false; throw new TypeError('Response lost after write'); }
    return Response.json({ stored: true });
  }) as typeof fetch;
  await expect(capture.save()).rejects.toThrow('Response lost');
  expect(capture.saved).toBe(false);
  const retry = capture.save(); expect(capture.save()).toBe(retry); await retry;
  await capture.save(); expect(bodies.length).toBe(2); expect(bodies[0]).toBe(bodies[1]);
  expect(JSON.parse(bodies[1]!)).toEqual({ id: capture.definition.id, source_turn: 'source', title: 'Transport failed', category: 'transport', expected: 'Receive a complete response' });
  expect(capture.saved).toBe(true);
});
test('failure capture requires a persisted final recording and an explicit save acknowledgement', async () => {
  const record = { id: 'source', status: 'failed', manifest_sha256: 'a'.repeat(64), final_sha256: 'b'.repeat(64) } as TurnRecord;
  const fields = { title: 'Failure', category: 'voice', expected: 'Respond' };
  expect(() => new CaptureSubmission({ ...record, status: 'recording' }, fields)).toThrow('finalized recording');
  expect(() => new CaptureSubmission({ ...record, final_sha256: null }, fields)).toThrow('finalized recording');
  expect(() => new CaptureSubmission(record, { ...fields, title: ' ' })).toThrow('Enter a title');
  expect(() => new CaptureSubmission(record, { ...fields, category: 'é'.repeat(41) })).toThrow('Shorten');
  globalThis.fetch = (async () => Response.json({})) as typeof fetch;
  const capture = new CaptureSubmission(record, fields);
  await expect(capture.save()).rejects.toThrow('did not confirm'); expect(capture.saved).toBe(false);
});

test('displayed metadata keeps gateway provenance and rejects conflicting observations', () => {
  const events: TurnRecord['events'] = [];
  const add = (source: 'browser' | 'gateway_observed', metadata: Record<string, unknown>) => events.push({
    event: { seq: events.length, elapsed_ms: events.length, name: 'completed', source, payload: { metadata } }, sha256: '', received: 0,
  });
  add('browser', { stt_ms: 999, model_id: 'invented-browser-model' });
  expect(gatewayMetadata(events).stt_ms).toBeUndefined();
  expect(gatewayMetadata(events).model_id).toBeUndefined();
  add('gateway_observed', { stt_ms: '0', llm_ms: '12.5', model_id: 'observed-model' });
  add('browser', { stt_ms: 42, model_id: 'overwritten' });
  expect(gatewayMetadata(events)).toEqual({ stt_ms: '0', llm_ms: '12.5', model_id: 'observed-model' });
  add('gateway_observed', { stt_ms: '1', model_id: 'different-model' });
  add('gateway_observed', { stt_ms: '0', model_id: 'observed-model' });
  expect(gatewayMetadata(events)).toEqual({ stt_ms: null, llm_ms: '12.5', model_id: null });
});
test('model identity retains its provider source without promoting browser claims', () => {
  const event = (source: 'browser' | 'gateway_observed', metadata: Record<string, unknown>): TurnRecord['events'][number] => ({
    event: { seq: 0, elapsed_ms: 0, name: 'text_completed', source, payload: { metadata } }, sha256: '', received: 0,
  });
  const provider = event('gateway_observed', { model_id: 'served-name', model_identity_source: 'provider_reported' });
  expect(gatewayMetadata([provider, event('browser', { model_id: 'weights-attested', model_identity_source: 'verified' })]))
    .toEqual({ model_id: 'served-name', model_identity_source: 'provider_reported' });
});
test('timing display preserves zero and refuses coercion into invented measurements', () => {
  for (const invalid of [null, undefined, true, false, '', ' ', '0x10', 'Infinity', Infinity, NaN, -1, '-1', {}, []])
    expect(reportedMilliseconds(invalid)).toBeNull();
  expect(reportedMilliseconds(0)).toBe(0); expect(reportedMilliseconds('0')).toBe(0);
  expect(reportedMilliseconds('12.5')).toBe(12.5);
});

const exportRecord = { id: 'export-source', status: 'failed', manifest_sha256: 'a'.repeat(64), final_sha256: 'b'.repeat(64) } as TurnRecord;
const exportBinding = { runId: '4b196128-9dc2-4d65-8bc5-8c839875afe1', sourceRevision: 'c'.repeat(40), sequence: 1 };
const exportReport = { receiptSha256: 'd'.repeat(64), receipt: { turnId: exportRecord.id,
  manifestSha256: exportRecord.manifest_sha256, finalSha256: exportRecord.final_sha256 } };
async function bundleResponse(corrupt = false): Promise<Response> {
  const bytes = new Uint8Array(2560);
  const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), (v) => v.toString(16).padStart(2, '0')).join('');
  if (corrupt) bytes[0] = 1;
  return new Response(bytes, { headers: { 'Content-Type': 'application/x-tar', 'Content-Length': '2560', 'X-Content-SHA256': digest } });
}
test('Anvil download snapshots the binding and verifies the returned bytes', async () => {
  let resolve!: (response: Response) => void; let submitted: unknown;
  const binding = { ...exportBinding }, record = { ...exportRecord };
  globalThis.fetch = (async (_url: unknown, options: RequestInit) => {
    if (options.method === 'GET') return new Promise<Response>((done) => { resolve = done; });
    submitted = JSON.parse(String(options.body)); return bundleResponse();
  }) as typeof fetch;
  const download = anvilBundle(record, binding);
  binding.sourceRevision = 'e'.repeat(40); record.final_sha256 = 'e'.repeat(64);
  resolve(Response.json(exportReport));
  const blob = await download;
  expect(blob.size).toBe(2560); expect(blob.type).toBe('application/x-tar');
  expect(submitted).toEqual({ run_id: exportBinding.runId, source_revision: exportBinding.sourceRevision, sequence: 1, receipt_sha256: exportReport.receiptSha256 });
});
test('Anvil download refuses stale recording evidence before requesting a bundle', async () => {
  let calls = 0;
  globalThis.fetch = (async () => { calls++; return Response.json({ ...exportReport, receipt: { ...exportReport.receipt, finalSha256: 'e'.repeat(64) } }); }) as typeof fetch;
  await expect(anvilBundle(exportRecord, exportBinding)).rejects.toThrow('does not match'); expect(calls).toBe(1);
});
test('Anvil download refuses altered bytes, failed exports, and unfinished sources', async () => {
  for (const response of [await bundleResponse(true), Response.json({}, { status: 409 })]) {
    globalThis.fetch = (async (_url: unknown, options: RequestInit) => options.method === 'GET' ? Response.json(exportReport) : response) as typeof fetch;
    await expect(anvilBundle(exportRecord, exportBinding)).rejects.toThrow(response.status === 409 ? '409' : 'digest mismatch');
  }
  globalThis.fetch = (async () => { throw new Error('Invalid bindings must not fetch'); }) as typeof fetch;
  await expect(anvilBundle({ ...exportRecord, status: 'recording' }, exportBinding)).rejects.toThrow('finalized recording');
  await expect(anvilBundle(exportRecord, { ...exportBinding, sequence: 0 })).rejects.toThrow('valid report sequence');
});

function stageEvent(start: unknown, end: unknown, source: 'browser' | 'gateway_observed' = 'gateway_observed'): TurnRecord['events'][number] {
  return { event: { seq: 0, elapsed_ms: 0, name: 'pcm_started', source, payload: { metadata: {
    stage_stt_request_received_at_ms: start, stage_stt_transcript_published_at_ms: end,
  } } }, sha256: '', received: 0 };
}
test('STT display uses the observed C interval once across repeated speech segments', () => {
  const stage = stageEvent('1790162493844', '1790162495529');
  expect(gatewaySTTInterval([stage, stage, stage])).toBe(1685);
  expect(gatewaySTTInterval([stageEvent('100', '100')])).toBe(0);
  expect(gatewaySTTInterval([stageEvent('1', '9999', 'browser'), stage])).toBe(1685);
  expect(gatewaySTTInterval([])).toBeNull();
});
test('STT display refuses malformed, incomplete, reversed, or conflicting timestamps', () => {
  for (const bad of [undefined, null, true, '', '1.5', '0', '-1', '9007199254740992', Infinity]) {
    expect(gatewaySTTInterval([stageEvent(bad, '200')])).toBeNull();
    expect(gatewaySTTInterval([stageEvent('100', bad)])).toBeNull();
  }
  expect(gatewaySTTInterval([stageEvent('200', '100')])).toBeNull();
  expect(gatewaySTTInterval([stageEvent('100', '200'), stageEvent('101', '201')])).toBeNull();
  expect(gatewaySTTInterval([stageEvent('100', undefined), stageEvent(undefined, '200')])).toBeNull();
});

test('provider token usage preserves zero and rejects missing, conflicting, and browser claims', () => {
  const event = (metadata: Record<string, unknown>, source: 'gateway_observed' | 'browser' = 'gateway_observed'): TurnRecord['events'][number] => ({
    event: { seq: 0, elapsed_ms: 0, name: 'text_completed', source, payload: { metadata } }, sha256: '', received: 0,
  });
  const valid = { usage_source: 'provider_reported', prompt_tokens: '10', completion_tokens: '0', total_tokens: '10' };
  expect(gatewayTokenUsage([event(valid)])).toEqual({ input: 10, output: 0, total: 10 });
  expect(gatewayTokenUsage([event(valid, 'browser')])).toBeNull();
  for (const value of [null, undefined, '', '01', '-1', -1, '1.5', 1.5, true, '9007199254740992'])
    expect(gatewayTokenUsage([event({ ...valid, completion_tokens: value })])).toBeNull();
  expect(gatewayTokenUsage([event({ ...valid, total_tokens: '11' })])).toBeNull();
  expect(gatewayTokenUsage([event({ ...valid, usage_source: 'estimated' })])).toBeNull();
  expect(gatewayTokenUsage([event(valid), event({ ...valid, prompt_tokens: '9' }), event(valid)])).toBeNull();
  expect(gatewayTokenUsage([event(valid), event(valid)])).toEqual({ input: 10, output: 0, total: 10 });
});
