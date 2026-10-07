import { test, expect } from 'bun:test';
import { VoiceTurn } from '../src/voice';
import { FRAME_AUDIO_PACKET, FRAME_CONTROL_JSON, readFrame } from '../../../contracts/ai-sdk/src/turnStream';

const flush = async () => { for (let i = 0; i < 200; i++) await Promise.resolve(); };
for (const mode of ['stream', 'datagram'] as const)
for (const stall of ['writer', 'browser', 'observer'] as const)
test(`saved ${mode} input preserves spacing after a ${stall} stall`, async () => {
  const originalSet = globalThis.setTimeout, originalClear = globalThis.clearTimeout;
  const originalNow = Object.getOwnPropertyDescriptor(performance, 'now');
  let now = 0, next = 0, inputFrames = 0;
  const timers = new Map<number, { at: number; run: () => void }>();
  Object.defineProperty(performance, 'now', { configurable: true, value: () => now });
  globalThis.setTimeout = ((run: () => void, delay = 0) => {
    const id = ++next; timers.set(id, { at: now + delay, run }); return id;
  }) as unknown as typeof setTimeout;
  globalThis.clearTimeout = ((id: number) => { timers.delete(id); }) as unknown as typeof clearTimeout;
  const input = Uint8Array.from({ length: 3200 }, (_, i) => i % 251);
  const times: number[] = [], frames: Uint8Array[] = [], controls: Record<string, unknown>[] = [];
  const controller = new AbortController();
  const turn = new VoiceTurn({ requestId: 'pacing', identity: 'fixture', endpoint: 'https://test.invalid',
    signal: controller.signal, recordedInput: input, inputTransport: mode,
    onPhase() {}, onObservation() {}, onTranscript() {}, onText() {},
    onAudio(kind) { if (kind === 'input' && ++inputFrames === 2 && stall === 'observer') now += 600; } });
  const internal = turn as unknown as { sendRecording(): Promise<void>; writer: { write(packet: Uint8Array): Promise<void> } };
  turn.phase = 'listening';
  internal.writer = { async write(packet) {
    let raw = packet;
    if (mode === 'stream') {
      const frame = await readFrame(new ReadableStream<Uint8Array>({ start(c) { c.enqueue(packet); c.close(); } }).getReader());
      expect(frame).not.toBeNull();
      if (frame!.frameType === FRAME_CONTROL_JSON) { controls.push(JSON.parse(new TextDecoder().decode(frame!.payload))); return; }
      expect(frame!.frameType).toBe(FRAME_AUDIO_PACKET); raw = frame!.payload;
    } else if (new TextDecoder().decode(packet.slice(0, 5)) !== 'DTVP1') {
      controls.push(JSON.parse(new TextDecoder().decode(packet.slice(6)))); return;
    }
    times.push(now); frames.push(new Uint8Array(raw));
    if (stall === 'writer' && frames.length === 2) await new Promise<void>(resolve => setTimeout(resolve, 600));
  } };
  let done = false;
  try {
    const sending = internal.sendRecording().then(() => { done = true; }); await flush();
    for (let step = 0; !done && step < 20; step++) {
      const due = [...timers.entries()].sort((a, b) => a[1].at - b[1].at)[0];
      expect(due).toBeDefined();
      now = Math.max(now, due![1].at);
      if (stall === 'browser' && step === 1) now += 600;
      timers.delete(due![0]); due![1].run(); await flush();
    }
    await sending;
    expect(frames).toHaveLength(5);
    for (let i = 1; i < times.length; i++) expect(times[i]! - times[i - 1]!).toBeGreaterThanOrEqual(20);
    for (let i = 0; i < frames.length; i++) {
      expect(new DataView(frames[i]!.buffer).getUint32(5, false)).toBe(i);
      expect(frames[i]!.slice(9)).toEqual(input.slice(i * 640, (i + 1) * 640));
    }
    expect(controls).toEqual([{ type: 'end', request_id: 'pacing', packet_count: 5, audio_bytes: 3200 }]);
    expect(now - times[4]!).toBeGreaterThanOrEqual(20);
  } finally {
    globalThis.setTimeout = originalSet; globalThis.clearTimeout = originalClear;
    if (originalNow) Object.defineProperty(performance, 'now', originalNow); else Reflect.deleteProperty(performance, 'now');
  }
});
