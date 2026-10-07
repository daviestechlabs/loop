import { expect, test } from 'bun:test';
import { VoiceTurn, type Observation } from '../src/voice';
import { writeFrame, FRAME_CONTROL_JSON, FRAME_TURN_EVENT_PROTO, type TurnEvent } from '../../../contracts/ai-sdk/src/turnStream';

// Real product response loop; substitute only stream I/O and acoustic playback.
async function fixture(capture: boolean, mode: 'valid' | 'partial' | 'tampered' | 'late-event') {
  const bytes = new TextEncoder().encode('x'.repeat(9000));
  const sha = Buffer.from(await crypto.subtle.digest('SHA-256', bytes)).toString('hex');
  const observations: Observation[] = [];
  const frames: Uint8Array[] = [];
  const id = new TextEncoder().encode('capture-turn');
  const event = (type: number, extra: number[] = []) => writeFrame(FRAME_TURN_EVENT_PROTO,
    new Uint8Array([10, id.length, ...id, 24, type, ...extra]));
  frames.push(event(5, [42, 2, 72, 105, 96, 1]), event(8, [96, 1]), event(10));
  for (let offset = 0; offset < bytes.length; offset += 8192) {
    if (mode === 'partial' && offset) break;
    frames.push(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({
      type: 'model_request_chunk', protocol_version: 'turnstream.v1alpha1', request_id: 'capture-turn',
      sequence: offset / 8192, total_bytes: bytes.length,
      data: Buffer.from(bytes.subarray(offset, offset + 8192)).toString('base64'),
      final: offset + 8192 >= bytes.length, sha256: mode === 'tampered' ? '0'.repeat(64) : sha,
      captured_at: 1, nonce: 'a'.repeat(32), signature: 'b'.repeat(64),
    })));
  }
  if (mode === 'late-event') frames.push(event(10));
  const turn = new VoiceTurn({ requestId: 'capture-turn', captureModelRequest: capture,
    identity: 'synthetic', endpoint: 'https://test.invalid', signal: new AbortController().signal,
    onPhase() {}, onObservation(value) { observations.push(value); }, onTranscript() {}, onText() {}, onAudio() {} });
  const internal = turn as unknown as { readResponse(): Promise<TurnEvent> };
  Object.assign(turn, { inputCommitted: true, transcriptFinal: true, play: async () => {},
    reader: { async read() { const value = frames.shift(); return { done: !value, value }; } } });
  return { result: internal.readResponse(), observations };
}

test('product client records complete validated capture after voice completion', async () => {
  const f = await fixture(true, 'valid');
  expect((await f.result).text).toBe('Hi');
  const chunks = f.observations.filter((o) => o.name === 'model_request_chunk');
  expect(chunks).toHaveLength(2);
  expect(chunks[1]!.payload.final).toBe(true);
  expect(chunks[1]!.source).toBe('gateway_observed');
});
for (const mode of ['partial', 'tampered'] as const) test(`product client rejects ${mode} capture`, async () => {
  const f = await fixture(true, mode);
  await expect(f.result).rejects.toThrow('Invalid prepared model request capture');
});
test('product client rejects capture without opt-in', async () => {
  const f = await fixture(false, 'valid');
  await expect(f.result).rejects.toThrow('Unsolicited model request capture');
  expect(f.observations.some((o) => o.name === 'model_request_chunk')).toBe(false);
});
test('capture allowance does not permit other events after completion', async () => {
  const f = await fixture(true, 'late-event');
  await expect(f.result).rejects.toThrow('Event after voice completion');
});
