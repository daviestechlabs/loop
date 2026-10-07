import { test, expect } from 'bun:test';
import { VoiceTurn, type VoiceOptions } from '../src/voice';
import { FRAME_CONTROL_JSON, writeFrame } from '../../../contracts/ai-sdk/src/turnStream';
test('release during pending microphone permission stops late media tracks', async () => {
  const globals = ['isSecureContext', 'WebTransport', 'AudioContext', 'AudioWorkletNode', 'navigator'] as const;
  const saved = new Map(globals.map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  let deliver!: (stream: MediaStream) => void; let requested!: () => void; let stopped = 0; let closed = 0;
  const started = new Promise<void>((resolve) => { requested = resolve; });
  class Context { async resume() {} async close() { closed++; } }
  const values = { isSecureContext: true, WebTransport: class {}, AudioContext: Context, AudioWorkletNode: class {},
    navigator: { mediaDevices: { getUserMedia: () => { requested(); return new Promise<MediaStream>((resolve) => { deliver = resolve; }); } } } };
  for (const key of globals) Object.defineProperty(globalThis, key, { value: values[key], configurable: true });
  const controller = new AbortController(); const phases: string[] = [];
  const options: VoiceOptions = { requestId: 'late-permission', signal: controller.signal, identity: 'fixture', endpoint: 'https://test.invalid/v1/voice/turns',
    onPhase: (p) => phases.push(p), onObservation() {}, onTranscript() {}, onText() {}, onAudio() {} };
  try {
    const turn = new VoiceTurn(options); const run = turn.run(); const failed = run.then(() => { throw new Error('Expected cancellation'); }, (error: Error) => error);
    await started; controller.abort(); expect((await failed).message).toBe('Turn interrupted');
    deliver({ getTracks: () => [{ stop: () => { stopped++; } }] } as unknown as MediaStream);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(stopped).toBe(1); expect(closed).toBe(1); expect(phases.includes('listening')).toBe(false);
  } finally {
    for (const key of globals) { const descriptor = saved.get(key); if (descriptor) Object.defineProperty(globalThis, key, descriptor); else Reflect.deleteProperty(globalThis, key); }
  }
});

for (const datagramAPI of ['writable', 'createWritable', 'missing'] as const)
for (const earlyEndpoint of [false, true]) test(`saved input preserves frames without opening a microphone (early endpoint=${earlyEndpoint}, datagrams=${datagramAPI})`, async () => {
  const globals = ['isSecureContext', 'WebTransport', 'AudioContext', 'AudioWorkletNode', 'navigator'] as const;
  const saved = new Map(globals.map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  let response!: ReadableStreamDefaultController<Uint8Array>; let micCalls = 0;
  const packets: Uint8Array[] = [], recorded: Uint8Array[] = [], controls: Record<string, unknown>[] = [];
  const send = (value: object) => response.enqueue(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({ request_id: 'saved-fixture', protocol_version: 'turnstream.v1alpha1', ...value })));
  class Context { async resume() {} async close() {} }
  let datagramFactories = 0;
  class Transport {
    ready = Promise.resolve(); closed = Promise.resolve(); close() {}
    outgoing = new WritableStream<Uint8Array>({ write: (packet) => {
      if (new TextDecoder().decode(packet.slice(0, 5)) === 'DTVP1') {
        packets.push(new Uint8Array(packet));
        if (earlyEndpoint && packets.length === 1) send({ type: 'input_endpoint' });
      } else {
        const value = JSON.parse(new TextDecoder().decode(packet.slice(6))) as Record<string, unknown>; controls.push(value);
        if (value.type === 'end') {
          send({ type: 'input_committed', audio_datagrams: 2, audio_bytes: 1280, lost_datagrams: 0 });
          send({ type: 'failed', error: 'synthetic response stop' });
        }
      }
    } });
    datagrams = datagramAPI === 'missing' ? {} : datagramAPI === 'createWritable'
      ? { createWritable: () => { datagramFactories++; return this.outgoing; } }
      : { writable: this.outgoing };
    async createBidirectionalStream() {
      return { readable: new ReadableStream<Uint8Array>({ start(controller) {
        response = controller;
        send({ type: 'accepted', metadata: { audio_endpointing: 'server', audio_endpoint_feedback: 'control-v1', audio_datagram_protocol: 'dtvp1' } });
      } }), writable: new WritableStream<Uint8Array>() };
    }
  }
  const values = { isSecureContext: true, WebTransport: Transport, AudioContext: Context, AudioWorkletNode: undefined,
    navigator: { mediaDevices: { getUserMedia() { micCalls++; throw new Error('Microphone must not open'); } } } };
  for (const key of globals) Object.defineProperty(globalThis, key, { value: values[key], configurable: true });
  const input = new Uint8Array(1280); input[0] = 42; input[640] = 81;
  try {
    const turn = new VoiceTurn({ requestId: 'saved-fixture', signal: new AbortController().signal, identity: 'fixture', endpoint: 'https://test.invalid', recordedInput: input,
      onPhase() {}, onObservation() {}, onTranscript() {}, onText() {}, onAudio(kind, pcm) { if (kind === 'input') recorded.push(new Uint8Array(pcm)); } });
    input[0] = 0;
    const error = await turn.run().then(() => new Error('Unexpected success'), (value: Error) => value);
    if (datagramAPI === 'missing') { expect(error.message).toBe('WebTransport datagram sending is unavailable'); expect(packets).toHaveLength(0); expect(micCalls).toBe(0); return; }
    expect(datagramFactories).toBe(datagramAPI === 'createWritable' ? 1 : 0);
    expect(error.message).toBe(earlyEndpoint ? 'Gateway ended saved input before complete delivery' : 'synthetic response stop');
    expect(micCalls).toBe(0); expect(packets.length).toBe(earlyEndpoint ? 1 : 2);
    expect(recorded[0]?.[0]).toBe(42);
    if (!earlyEndpoint) { expect(recorded[1]?.[0]).toBe(81); expect(controls.find((value) => value.type === 'end')).toMatchObject({ packet_count: 2, audio_bytes: 1280 }); }
    else expect(controls.some((value) => value.type === 'end')).toBe(false);
  } finally {
    for (const key of globals) { const descriptor = saved.get(key); if (descriptor) Object.defineProperty(globalThis, key, descriptor); else Reflect.deleteProperty(globalThis, key); }
  }
});

for (const outcome of ['success', 'denied', 'constraint', 'abort'] as const) test(`microphone constraint retry: ${outcome}`, async () => {
  const saved = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  const controller = new AbortController(), observations: { name: string; payload: Record<string, unknown> }[] = [];
  const calls: MediaStreamConstraints[] = [];
  const stream = {} as MediaStream;
  Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { mediaDevices: {
    async getUserMedia(constraints: MediaStreamConstraints) {
      calls.push(constraints);
      if (outcome === 'denied') throw new DOMException('Permission denied', 'NotAllowedError');
      if (calls.length === 1) {
        if (outcome === 'abort') controller.abort();
        throw Object.assign(new DOMException('Invalid constraint', 'OverconstrainedError'), { constraint: 'channelCount' });
      }
      if (outcome === 'constraint') throw new DOMException('Invalid constraint', 'OverconstrainedError');
      return stream;
    },
  } } });
  const turn = new VoiceTurn({ requestId: 'constraints', signal: controller.signal, identity: 'fixture', endpoint: 'https://test.invalid',
    onPhase() {}, onObservation: (event) => observations.push(event), onTranscript() {}, onText() {}, onAudio() {} });
  try {
    const request = (turn as unknown as { requestMicrophone(): Promise<MediaStream> }).requestMicrophone();
    if (outcome === 'success') expect(await request).toBe(stream);
    else await expect(request).rejects.toThrow(outcome === 'abort' ? 'Turn interrupted'
      : outcome === 'denied' ? 'Allow microphone access' : 'browser rejected default microphone settings');
    expect(calls.length).toBe(outcome === 'denied' || outcome === 'abort' ? 1 : 2);
    if (calls.length === 2) {
      expect(calls[1]).toEqual({ audio: true });
      expect(observations.some((e) => e.name === 'microphone_settings_retry')).toBe(true);
      expect(observations[0]?.payload.constraint).toBe('channelCount');
    }
  } finally {
    if (saved) Object.defineProperty(globalThis, 'navigator', saved); else Reflect.deleteProperty(globalThis, 'navigator');
  }
});

test('live input commits on server endpoint and interruption preserves the shared microphone context', async () => {
  const { LiveFrames } = await import('../src/live');
  const globals = ['isSecureContext', 'WebTransport', 'AudioContext', 'AudioWorkletNode', 'navigator'] as const;
  const saved = new Map(globals.map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  let response!: ReadableStreamDefaultController<Uint8Array>;
  const controls: Record<string, unknown>[] = [], packets: Uint8Array[] = [];
  let contextCloses = 0, micRequests = 0, playbackStops = 0;
  const controller = new AbortController(), input = new LiveFrames();
  const send = (value: object) => response.enqueue(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({ request_id: 'live-fixture', protocol_version: 'turnstream.v1alpha1', ...value })));
  class Context { async resume() {} async close() { contextCloses++; } }
  class Transport {
    ready = Promise.resolve(); closed = new Promise<void>(() => {}); close() {}
    datagrams = { writable: new WritableStream<Uint8Array>({ write(packet) {
      if (new TextDecoder().decode(packet.slice(0, 5)) === 'DTVP1') {
        packets.push(packet); send({ type: 'input_endpoint' });
      } else {
        const value = JSON.parse(new TextDecoder().decode(packet.slice(6))); controls.push(value);
        if (value.type === 'end') {
          send({ type: 'input_committed', audio_datagrams: 1, audio_bytes: 640, lost_datagrams: 0 });
          send({ type: 'transcript', text: 'First utterance', is_final: true });
        }
      }
    } }) };
    async createBidirectionalStream() {
      return { readable: new ReadableStream<Uint8Array>({ start(value) {
        response = value;
        send({ type: 'accepted', metadata: { audio_endpointing: 'server', audio_endpoint_feedback: 'control-v1', audio_datagram_protocol: 'dtvp1' } });
      } }), writable: new WritableStream<Uint8Array>() };
    }
  }
  const values = { isSecureContext: true, WebTransport: Transport, AudioContext: Context, AudioWorkletNode: class {},
    navigator: { mediaDevices: { getUserMedia() { micRequests++; throw new Error('Do not recapture'); } } } };
  for (const key of globals) Object.defineProperty(globalThis, key, { value: values[key], configurable: true });
  const context = new Context() as unknown as AudioContext;
  const turn = new VoiceTurn({ requestId: 'live-fixture', identity: 'fixture', endpoint: 'https://test.invalid', signal: controller.signal,
    liveInput: input, audioContext: context, onPhase() {}, onObservation() {}, onText() {}, onAudio() {},
    onTranscript() { controller.abort(); } });
  (turn as unknown as { sources: Set<{ stop(): void; disconnect(): void }> }).sources.add({ stop() { playbackStops++; }, disconnect() {} });
  input.push(new Uint8Array(640));
  try {
    await expect(turn.run()).rejects.toThrow('Turn interrupted');
    expect(packets).toHaveLength(1); expect(micRequests).toBe(0); expect(contextCloses).toBe(0); expect(playbackStops).toBe(1);
    expect(controls[0]).toEqual({ type: 'end', request_id: 'live-fixture', packet_count: 1, audio_bytes: 640 });
    expect(controls[1]?.type).toBe('interrupt'); expect(await input.read()).toBeNull();
  } finally {
    for (const key of globals) { const descriptor = saved.get(key); if (descriptor) Object.defineProperty(globalThis, key, descriptor); else Reflect.deleteProperty(globalThis, key); }
  }
});
