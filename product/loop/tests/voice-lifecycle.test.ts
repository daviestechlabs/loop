import { test, expect } from 'bun:test';
import { VoiceTurn, type Observation } from '../src/voice';
import { FRAME_CONTROL_JSON, FRAME_TURN_EVENT_PROTO, writeFrame, type TurnEvent } from '../../../contracts/ai-sdk/src/turnStream';

const flush = async () => { for (let i = 0; i < 30; i++) await Promise.resolve(); };
// Exercise the shipped response loop and cleanup with controlled time and transport I/O.
function harness(responseOnly = true) {
  const originalSet = globalThis.setTimeout, originalClear = globalThis.clearTimeout;
  let now = 0, next = 0;
  const timers = new Map<number, { at: number; run: () => void }>();
  globalThis.setTimeout = ((run: () => void, delay = 0) => { const id = ++next; timers.set(id, { at: now + delay, run }); return id; }) as unknown as typeof setTimeout;
  globalThis.clearTimeout = ((id: number) => { timers.delete(id); }) as unknown as typeof clearTimeout;
  const controller = new AbortController(), observations: Observation[] = [], datagrams: Record<string, unknown>[] = [], texts: string[] = [];
  let closes = 0, pending: ((value: ReadableStreamReadResult<Uint8Array>) => void) | undefined;
  const frames: Uint8Array[] = [];
  const turn = new VoiceTurn({ requestId: 'lifecycle', signal: controller.signal, identity: 'synthetic', endpoint: 'https://test.invalid',
    onPhase() {}, onObservation: (value) => observations.push(value), onText: (value) => texts.push(value), onTranscript() {}, onAudio() {} });
  // This seam replaces browser I/O, not the implementation being tested.
  const internal = turn as unknown as {
    execute: () => Promise<TurnEvent>; readResponse: () => Promise<TurnEvent>; armSttFinalWatch: () => void;
    inputEnded: boolean; inputCommitted: boolean; transcriptFinal: boolean; writes: Promise<void>;
    sources: Set<{ stop: () => void; disconnect: () => void }>;
    writer: { write: (packet: Uint8Array) => Promise<void> }; transport: { close: () => void };
    reader: { read: () => Promise<ReadableStreamReadResult<Uint8Array>>; cancel: () => Promise<void> };
  };
  internal.writer = { async write(packet) { datagrams.push(JSON.parse(new TextDecoder().decode(packet.slice(6)))); } };
  internal.transport = { close() { closes++; } };
  internal.reader = { read: () => frames.length ? Promise.resolve({ done: false, value: frames.shift()! }) : new Promise((resolve) => { pending = resolve; }), async cancel() {} };
  if (responseOnly) { internal.execute = () => internal.readResponse(); internal.inputEnded = true; }
  function send(frame: Uint8Array) { if (pending) { const resolve = pending; pending = undefined; resolve({ done: false, value: frame }); } else frames.push(frame); }
  return { turn, internal, controller, observations, datagrams, texts, timers, send, closes: () => closes,
    control(value: Record<string, unknown>) { send(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({ request_id: 'lifecycle', protocol_version: 'turnstream.v1alpha1', ...value }))); },
    async tick(ms: number) {
      const end = now + ms;
      for (;;) {
        const due = [...timers.entries()].filter(([, timer]) => timer.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
        if (!due) break;
        now = due[1].at; timers.delete(due[0]); due[1].run(); await flush();
      }
      now = end; await flush();
    },
    restore() { globalThis.setTimeout = originalSet; globalThis.clearTimeout = originalClear; },
  };
}

test('commit acknowledgement cannot extend the twelve-second final transcript deadline', async () => {
  const h = harness();
  try {
    let failure: Error | undefined;
    const run = h.turn.run().catch((error: Error) => { failure = error; }); h.internal.armSttFinalWatch();
    await h.tick(11000); h.control({ type: 'input_committed', audio_datagrams: 0, audio_bytes: 0, lost_datagrams: 0 }); await flush();
    h.internal.armSttFinalWatch(); await h.tick(1000); await run;
    expect(failure?.message).toBe('No final transcript within 12 seconds'); expect(h.closes()).toBe(1); expect(h.timers.size).toBe(0);
  } finally { h.restore(); }
});
for (const text of ['', '  ', null]) test(`empty final transcript fails without disabling the deadline (${JSON.stringify(text)})`, async () => {
  const h = harness();
  try {
    const run = h.turn.run().catch((error: Error) => error); h.internal.armSttFinalWatch();
    h.control({ type: 'transcript', is_final: true, text });
    expect((await run as Error).message).toBe('Final transcript is empty'); expect(h.internal.transcriptFinal).toBe(false);
    expect(h.timers.size).toBe(0);
  } finally { h.restore(); }
});
test('a nonempty final transcript clears the deadline', async () => {
  const h = harness();
  try {
    let failure: Error | undefined;
    const run = h.turn.run().catch((error: Error) => { failure = error; }); h.internal.armSttFinalWatch();
    h.control({ type: 'transcript', is_final: true, text: 'Is Mira still in the room?' }); await flush(); await h.tick(12000);
    expect(failure).toBeUndefined(); expect(h.internal.transcriptFinal).toBe(true);
    h.controller.abort(); await run; expect(failure?.name).toBe('AbortError'); expect(h.timers.size).toBe(0);
  } finally { h.restore(); }
});
test('abort during a read stops playback and ignores the late answer', async () => {
  const h = harness();
  try {
    let stopped = 0, disconnected = 0;
    h.internal.sources.add({ stop() { stopped++; }, disconnect() { disconnected++; } });
    const run = h.turn.run().catch((error: Error) => error); h.controller.abort(); expect((await run as Error).name).toBe('AbortError');
    h.send(writeFrame(FRAME_TURN_EVENT_PROTO, new Uint8Array([1]))); await flush();
    expect(h.texts).toEqual([]); expect(stopped).toBe(1); expect(disconnected).toBe(1);
    expect(h.datagrams).toEqual([{ type: 'interrupt', request_id: 'lifecycle', reason: 'user_interrupt' }]); expect(h.closes()).toBe(1);
  } finally { h.restore(); }
});
test('stalled datagram writes cannot hold cancellation past 250 ms', async () => {
  const h = harness();
  try {
    h.internal.writes = new Promise(() => {});
    const run = h.turn.run().catch((error: Error) => error); h.controller.abort(); await flush();
    await h.tick(249); expect(h.closes()).toBe(0); await h.tick(1);
    expect((await run as Error).name).toBe('AbortError'); expect(h.closes()).toBe(1); expect(h.timers.size).toBe(0);
  } finally { h.restore(); }
});
test('pre-session error frame with no request_id fails with invalid_turn', async () => {
  const h = harness();
  try {
    const run = h.turn.run().catch((error: Error) => error);
    h.send(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({
      type: 'error', protocol_version: 'turnstream.v1alpha1', error: 'invalid_turn',
    })));
    expect((await run as Error).message).toBe('invalid_turn');
    expect(h.observations.filter((event) => event.name === 'gateway_error')).toEqual([
      { name: 'gateway_error', source: 'gateway_observed', payload: { error: 'invalid_turn', request_bound: false } },
    ]);
  } finally { h.restore(); }
});
test('a frame whose request_id is not the turn id fails with Voice control belongs to another turn', async () => {
  const h = harness();
  try {
    const run = h.turn.run().catch((error: Error) => error);
    h.send(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({
      type: 'error', protocol_version: 'turnstream.v1alpha1', error: 'invalid_turn', request_id: 'other-turn',
    })));
    expect((await run as Error).message).toBe('Voice control belongs to another turn');
    expect(h.observations.some((event) => event.name === 'gateway_error')).toBe(false);
  } finally { h.restore(); }
});
test('audio gap retains the matching gateway error before cleanup', async () => {
  const h = harness();
  try {
    const run = h.turn.run().catch((error: Error) => error);
    h.control({ type: 'error', error: 'audio_datagram_gap' });
    expect((await run as Error).message).toBe('audio_datagram_gap');
    expect(h.observations.filter((event) => event.name === 'gateway_error')).toEqual([
      { name: 'gateway_error', source: 'gateway_observed', payload: { error: 'audio_datagram_gap', request_bound: true } },
    ]);
    expect(h.observations.findIndex((event) => event.name === 'gateway_error'))
      .toBeLessThan(h.observations.findIndex((event) => event.name === 'playback_stopped'));
  } finally { h.restore(); }
});
test('a foreign transcript cannot alter transcript state or observations', async () => {
  const h = harness();
  try {
    const run = h.turn.run().catch((error: Error) => error);
    h.control({ type: 'transcript', request_id: 'other-turn', is_final: true, text: 'Mira left.' });
    expect((await run as Error).message).toBe('Voice control belongs to another turn'); expect(h.internal.transcriptFinal).toBe(false);
    expect(h.observations.some((value) => value.name === 'transcript')).toBe(false);
  } finally { h.restore(); }
});

for (const committed of [false, true]) test(`text needs committed input and final transcript (committed=${committed})`, async () => {
  const h = harness();
  try {
    h.internal.inputCommitted = committed;
    const run = h.turn.run().catch((error: Error) => error);
    const id = new TextEncoder().encode('lifecycle');
    // TurnEvent: request_id, text_delta, text. Exercise the real protobuf decoder.
    h.send(writeFrame(FRAME_TURN_EVENT_PROTO, new Uint8Array([10, id.length, ...id, 24, 4, 42, 2, 72, 105])));
    expect((await run as Error).message).toBe('Voice text arrived before committed input and final transcript');
    expect(h.texts).toEqual([]); expect(h.closes()).toBe(1);
  } finally { h.restore(); }
});

for (const stage of ['ready', 'stream', 'accepted', 'connection_failure', 'unsupported'] as const) test(`WebTransport fails closed at ${stage}`, async () => {
  const globals = ['isSecureContext', 'WebTransport', 'AudioContext', 'fetch'] as const;
  const saved = new Map(globals.map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  const h = harness(false);
  Object.assign(h.internal, { writer: undefined, transport: undefined, reader: undefined });
  let transports = 0, closes = 0, httpCalls = 0, rejectClosed!: (error: Error) => void;
  class Context { async resume() {} async close() {} }
  class Transport {
    constructor(_url: string, options: WebTransportOptions) {
      transports++; expect(options.requireUnreliable).toBe(true);
    }
    ready = stage === 'ready' ? new Promise<void>(() => {}) : Promise.resolve();
    closed = new Promise<void>((_, reject) => { rejectClosed = reject; });
    datagrams = { writable: new WritableStream<Uint8Array>() };
    close() { closes++; }
    async createBidirectionalStream() {
      if (stage === 'stream') return new Promise<WebTransportBidirectionalStream>(() => {});
      return { readable: new ReadableStream<Uint8Array>(), writable: new WritableStream<Uint8Array>() };
    }
  }
  const values = { isSecureContext: true, WebTransport: stage === 'unsupported' ? undefined : Transport, AudioContext: Context,
    fetch: () => { httpCalls++; throw new Error('No voice fallback allowed'); } };
  for (const key of globals) Object.defineProperty(globalThis, key, { value: values[key], configurable: true });
  // Saved PCM skips microphone permissions; transport and admission use the shipped implementation.
  (h.turn as unknown as { savedInput: Uint8Array }).savedInput = new Uint8Array(640);
  try {
    let failure: Error | undefined;
    const run = h.turn.run().catch((error: Error) => { failure = error; }); await flush();
    if (stage === 'connection_failure') { rejectClosed(new Error('synthetic QUIC loss')); await flush(); }
    else if (stage !== 'unsupported') {
      await h.tick(9999); expect(failure).toBeUndefined(); await h.tick(1);
    }
    await run;
    expect(failure?.message).toBe(stage === 'unsupported'
      ? 'This browser does not provide WebTransport. Loop voice requires WebTransport; no fallback is available.'
      : stage === 'connection_failure' ? 'WebTransport connection failed' : 'WebTransport voice admission exceeded 10 seconds');
    expect(transports).toBe(stage === 'unsupported' ? 0 : 1);
    expect(closes).toBe(stage === 'unsupported' ? 0 : 1);
    expect(httpCalls).toBe(0); expect(h.timers.size).toBe(0);
  } finally {
    h.restore();
    for (const key of globals) { const descriptor = saved.get(key); if (descriptor) Object.defineProperty(globalThis, key, descriptor); else Reflect.deleteProperty(globalThis, key); }
  }
});

test('committed input and final transcript permit WebTransport response text', async () => {
  const h = harness();
  try {
    h.internal.inputCommitted = true;
    const run = h.turn.run().catch((error: Error) => error);
    h.control({ type: 'transcript', is_final: true, text: 'Hello' }); await flush();
    const id = new TextEncoder().encode('lifecycle');
    h.send(writeFrame(FRAME_TURN_EVENT_PROTO, new Uint8Array([10, id.length, ...id, 24, 4, 42, 2, 72, 105])));
    await flush(); expect(h.texts).toEqual(['Hi']);
    h.controller.abort(); expect((await run as Error).name).toBe('AbortError');
  } finally { h.restore(); }
});


test('response fragments grow one body and completion replaces the draft', async () => {
  const h = harness();
  try {
    h.internal.inputCommitted = true;
    const run = h.turn.run().catch((error: Error) => error);
    h.control({ type: 'transcript', is_final: true, text: 'Hello' }); await flush();
    const id = new TextEncoder().encode('lifecycle');
    function textEvent(type: number, value: string, display = false) {
      const bytes = new TextEncoder().encode(value);
      h.send(writeFrame(FRAME_TURN_EVENT_PROTO, new Uint8Array([
        10, id.length, ...id, 24, type, ...(display ? [146, 1] : [42]), bytes.length, ...bytes,
        ...(type === 5 ? [96, 1] : []),
      ])));
    }
    for (const fragment of ['Hello', ' ', 'world', '!', ' café']) { textEvent(4, fragment); await flush(); }
    expect(h.texts).toEqual(['Hello', 'Hello ', 'Hello world', 'Hello world!', 'Hello world! café']);
    textEvent(4, ' again', true); await flush();
    expect(h.texts.at(-1)).toBe('Hello world! café again');
    textEvent(5, 'Hello world!'); await flush();
    expect(h.texts.at(-1)).toBe('Hello world!');
    h.controller.abort(); expect((await run as Error).name).toBe('AbortError');
    textEvent(4, ' late'); await flush();
    expect(h.texts.at(-1)).toBe('Hello world!');
  } finally { h.restore(); }
});
