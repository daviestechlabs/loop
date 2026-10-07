import { test, expect } from 'bun:test';
import { LiveFrames, LiveTurns } from '../src/live';
const flush = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };
const pcm = (value = 0) => new Uint8Array(640).fill(value);
function harness() {
  const inputs: LiveFrames[] = [], done: ((ok: boolean) => void)[] = [], errors: string[] = [];
  let interrupted = 0, idle = 0;
  const turns = new LiveTurns({ run: (input) => { inputs.push(input); return new Promise((resolve) => done.push(resolve)); },
    interrupt: () => { interrupted++; }, idle: () => { idle++; }, fail: (error) => errors.push(error.message) });
  const feed = (count: number, rms: number, value = 0) => { for (let i = 0; i < count; i++) turns.packet(pcm(value), rms); };
  return { inputs, done, errors, turns, feed, interrupted: () => interrupted, idle: () => idle };
}
test('silence and short noise do not create turns; sustained activity preserves pre-roll', async () => {
  const h = harness();
  h.feed(30, 0); h.feed(5, 0.1); expect(h.inputs).toHaveLength(0);
  h.feed(1, 0.1, 9); expect(h.inputs).toHaveLength(1);
  for (let i = 0; i < 14; i++) expect((await h.inputs[0]!.read())?.byteLength).toBe(640);
  expect((await h.inputs[0]!.read())?.[0]).toBe(9);
  h.turns.stop(); h.done[0]!(true); await flush();
});
test('speech during playback interrupts once and queues its audio until the old turn closes', async () => {
  const h = harness(); h.feed(6, 0.1);
  h.turns.setPhase(h.inputs[0]!, 'listening'); h.feed(15, 0);
  h.turns.setPhase(h.inputs[0]!, 'speaking'); h.feed(6, 0.1, 7);
  expect(h.interrupted()).toBe(1); expect(h.inputs).toHaveLength(1);
  h.feed(5, 0.1, 8); expect(h.interrupted()).toBe(1);
  // Late phases from the canceled turn must not overwrite the pending admission.
  h.turns.setPhase(h.inputs[0]!, 'closed'); h.feed(1, 0.1, 9);
  h.done[0]!(true); await flush(); expect(h.inputs).toHaveLength(2);
  const values = [];
  for (let i = 0; i < 21; i++) values.push((await h.inputs[1]!.read())?.[0]);
  expect(values.slice(-12)).toEqual([...Array(6).fill(7), ...Array(5).fill(8), 9]);
  h.turns.stop(); h.done[1]!(true); await flush(); expect(h.idle()).toBe(0);
});
test('completed replies keep the conversation ready for another utterance', async () => {
  const h = harness(); h.feed(6, 0.1); h.turns.setPhase(h.inputs[0]!, 'speaking'); h.feed(15, 0);
  h.done[0]!(true); await flush(); expect(h.idle()).toBe(1);
  h.feed(6, 0.1); expect(h.inputs).toHaveLength(2); expect(h.interrupted()).toBe(0);
  h.turns.stop(); h.done[1]!(true); await flush();
});
test('stop discards queued turns and late audio cannot restart capture', async () => {
  const h = harness(); h.feed(6, 0.1); h.turns.setPhase(h.inputs[0]!, 'speaking'); h.feed(15, 0); h.feed(6, 0.1);
  h.turns.stop(); h.done[0]!(true); await flush(); h.feed(100, 0.1);
  expect(h.inputs).toHaveLength(1); expect(await h.inputs[0]!.read()).toBeNull();
});
test('admission backpressure stops the conversation instead of silently dropping speech', async () => {
  const h = harness(); h.feed(6, 0.1); h.feed(501, 0.1);
  expect(h.errors).toHaveLength(1); expect(h.errors[0]).toContain('without dropping speech');
  h.done[0]!(true); await flush(); expect(h.idle()).toBe(0);
});
test('failed turns do not automatically retry or reopen the microphone', async () => {
  const h = harness(); h.feed(6, 0.1); h.done[0]!(false); await flush(); h.feed(15, 0); h.feed(6, 0.1);
  expect(h.inputs).toHaveLength(1); expect(h.idle()).toBe(0);
});
test('closing an input releases a blocked reader and discards stale packets', async () => {
  const input = new LiveFrames(), read = input.read(); input.close();
  expect(await read).toBeNull(); input.push(pcm()); expect(await input.read()).toBeNull();
});

test('stopping during microphone permission closes context and stops late tracks', async () => {
  const { LiveMicrophone } = await import('../src/live');
  const globals = ['AudioContext', 'navigator'] as const;
  const saved = new Map(globals.map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  let deliver!: (stream: MediaStream) => void, stopped = 0, closed = 0, packets = 0;
  class Context { async resume() {} async close() { closed++; } }
  Object.defineProperty(globalThis, 'AudioContext', { configurable: true, value: Context });
  Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { mediaDevices: {
    getUserMedia: () => new Promise<MediaStream>((resolve) => { deliver = resolve; }),
  } } });
  try {
    const microphone = new LiveMicrophone(() => { packets++; }, () => {});
    const result = microphone.start(() => {}).catch((error: Error) => error);
    await flush(); microphone.stop();
    deliver({ getTracks: () => [{ stop() { stopped++; } }] } as unknown as MediaStream);
    expect((await result as Error).name).toBe('AbortError');
    expect(stopped).toBe(1); expect(closed).toBe(1); expect(packets).toBe(0);
  } finally {
    for (const key of globals) { const descriptor = saved.get(key); if (descriptor) Object.defineProperty(globalThis, key, descriptor); else Reflect.deleteProperty(globalThis, key); }
  }
});
