import { test, expect } from 'bun:test';
import { VoiceTurn, type Observation } from '../src/voice';
import { PCM16Decoder } from '../../../contracts/ai-sdk/src/audio';
import { writeFrame, FRAME_TURN_EVENT_PROTO, type TurnEvent } from '../../../contracts/ai-sdk/src/turnStream';

function harness(holdPlayback = false) {
  const observations: Observation[] = [], recorded: Uint8Array[] = [], scheduled: number[] = [];
  let stopped = 0;
  const controller = new AbortController();
  const turn = new VoiceTurn({ requestId: 'audio', signal: controller.signal, identity: 'fixture', endpoint: 'https://test.invalid',
    onPhase() {}, onTranscript() {}, onText() {}, onObservation: (event) => observations.push(event),
    onAudio: (_kind, pcm) => recorded.push(pcm.slice()) });
  const internal = turn as unknown as { play: (event: TurnEvent) => Promise<void>; inputCommitted: boolean; transcriptFinal: boolean };
  Object.assign(internal, { inputCommitted: true, transcriptFinal: true, decoder: new PCM16Decoder(false), context: {
    currentTime: 0, destination: {}, close: async () => {},
    createBuffer: (_channels: number, length: number, rate: number) => ({ duration: length / rate, copyToChannel() {} }),
    createBufferSource: () => {
      const source = { onended: undefined as (() => void) | undefined, connect() {}, disconnect() {},
        start(time: number) { scheduled.push(time); if (!holdPlayback) source.onended?.(); },
        stop() { stopped++; } };
      return source;
    },
  } });
  const event: TurnEvent = { type: 8, name: 'pcm_chunk', text: '', speechText: '', displayText: '', error: '', metadata: {},
    audio: new Uint8Array([1, 0, 255, 127]), sampleRate: 24000, channels: 1, bitDepth: 16 };
  return { internal, controller, observations, recorded, scheduled, event, stopped: () => stopped };
}

for (const bytes of [0, 1, 3, 16386]) test(`invalid PCM never becomes recording or timing evidence (${bytes} bytes)`, async () => {
  const h = harness();
  await expect(h.internal.play({ ...h.event, audio: new Uint8Array(bytes) })).rejects.toThrow();
  expect(h.recorded).toEqual([]); expect(h.observations).toEqual([]); expect(h.scheduled).toEqual([]);
});
for (const patch of [{ sampleRate: 8000 }, { sampleRate: 24000.5 }, { channels: 2 }, { bitDepth: 8 }])
  test(`unsupported PCM format fails before evidence (${JSON.stringify(patch)})`, async () => {
    const h = harness();
    await expect(h.internal.play({ ...h.event, ...patch })).rejects.toThrow('Invalid response audio');
    expect(h.recorded).toEqual([]); expect(h.observations).toEqual([]); expect(h.scheduled).toEqual([]);
  });
test('valid PCM preserves bytes and scheduling evidence, but a changed rate fails', async () => {
  const h = harness();
  await h.internal.play(h.event); await h.internal.play(h.event);
  expect(h.recorded).toEqual([h.event.audio, h.event.audio]); expect(h.scheduled.length).toBe(2);
  expect(h.observations.filter((event) => event.name === 'first_audio_received').length).toBe(1);
  expect(h.observations.find((event) => event.name === 'first_playback_scheduled')?.payload.scope).toBe('scheduled_not_acoustic');
  await expect(h.internal.play({ ...h.event, sampleRate: 48000 })).rejects.toThrow('Invalid response audio');
  expect(h.recorded.length).toBe(2); expect(h.scheduled.length).toBe(2);
});
test('response audio requires the final transcript and ignores late audio after abort', async () => {
  const h = harness(); h.internal.transcriptFinal = false;
  await expect(h.internal.play(h.event)).rejects.toThrow('Invalid response audio');
  h.internal.transcriptFinal = true; h.controller.abort();
  await expect(h.internal.play(h.event)).rejects.toThrow('Turn interrupted');
  expect(h.recorded).toEqual([]); expect(h.observations).toEqual([]); expect(h.scheduled).toEqual([]);
});

test('streamed PCM absorbs burst arrival jitter without gaps between scheduled samples', async () => {
  const h = harness();
  const context = (h.internal as unknown as { context: { currentTime: number } }).context;
  const event = { ...h.event, audio: new Uint8Array(960) }; // Actual 20 ms production frames.
  for (const arrival of [0, 0.086, 0.181, 0.260, 0.347, 0.441, 0.522]) {
    context.currentTime = arrival;
    for (let frame = 0; frame < 4; frame++) await h.internal.play(event);
  }
  for (let i = 1; i < h.scheduled.length; i++)
    expect(h.scheduled[i]! - h.scheduled[i - 1]!).toBeCloseTo(0.020, 8);
  expect(h.observations.some((event) => event.name === 'playback_buffer_starved')).toBe(false);
});

test('late PCM replenishes a bounded buffer and reports browser starvation separately from TTS work', async () => {
  const h = harness();
  const context = (h.internal as unknown as { context: { currentTime: number } }).context;
  const event = { ...h.event, audio: new Uint8Array(960) };
  await h.internal.play(event);
  context.currentTime = 0.500;
  await h.internal.play(event);
  expect(h.scheduled[1]! - context.currentTime).toBeGreaterThanOrEqual(0.100);
  expect(h.scheduled[1]! - context.currentTime).toBeLessThanOrEqual(0.200);
  expect(h.observations.filter((event) => event.name === 'playback_buffer_starved')).toEqual([
    expect.objectContaining({ source: 'browser', payload: expect.objectContaining({
      scope: 'scheduled_not_acoustic', count: 1,
    }) }),
  ]);
  context.currentTime = 0.510;
  await h.internal.play(event);
  expect(h.scheduled[2]! - h.scheduled[1]!).toBeCloseTo(0.020, 8);
});

test('interruption cancels audio scheduled inside the startup buffer', async () => {
  const h = harness(true);
  await h.internal.play({ ...h.event, audio: new Uint8Array(960) });
  expect(h.scheduled[0]).toBeGreaterThan(0);
  h.controller.abort();
  await (h.internal as unknown as { dispose: (completed: boolean) => Promise<void> }).dispose(false);
  expect(h.stopped()).toBe(1);
  await expect(h.internal.play(h.event)).rejects.toThrow('Turn interrupted');
  expect(h.scheduled).toHaveLength(1);
  expect(h.observations.some((event) => event.name === 'playback_finished')).toBe(false);
});

for (const mode of ['complete', 'missing_final_pcm', 'missing_completion', 'malformed_pcm'] as const)
  test(`response summary requires complete validated protocol (${mode})`, async () => {
    const h = harness();
    const internal = h.internal as unknown as { readResponse: () => Promise<TurnEvent>; reader: ReadableStreamDefaultReader<Uint8Array> };
    const id = [...new TextEncoder().encode('audio')];
    const event = (bytes: number[]) => writeFrame(FRAME_TURN_EVENT_PROTO, new Uint8Array([10, id.length, ...id, ...bytes]));
    const pcm = mode === 'malformed_pcm' ? [1] : [1, 0, 255, 127];
    internal.reader = new ReadableStream<Uint8Array>({ start(controller) {
      controller.enqueue(event([24, 5, 42, 2, 72, 105, 96, 1])); // Final text.
      controller.enqueue(event([24, 8, 50, pcm.length, ...pcm, 56, 192, 187, 1, 96, mode === 'missing_final_pcm' ? 0 : 1]));
      if (mode !== 'missing_completion') controller.enqueue(event([24, 10]));
      controller.close();
    } }).getReader();
    if (mode === 'complete') {
      expect((await internal.readResponse()).text).toBe('Hi');
      const summary = h.observations.find((value) => value.name === 'output_received_complete');
      expect(summary).toEqual({ name: 'output_received_complete', source: 'browser', payload: { bytes: 4, packets: 1, sample_rate: 24000 } });
      const names = h.observations.map((value) => value.name);
      expect(names.indexOf('output_received_complete')).toBeGreaterThan(names.indexOf('completed'));
      expect(names.indexOf('playback_finished')).toBeGreaterThan(names.indexOf('output_received_complete'));
    } else {
      await expect(internal.readResponse()).rejects.toThrow();
      expect(h.observations.some((value) => value.name === 'output_received_complete')).toBe(false);
      expect(h.observations.some((value) => value.name === 'playback_finished')).toBe(false);
    }
  });
