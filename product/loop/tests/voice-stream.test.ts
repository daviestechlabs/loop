import { test, expect } from 'bun:test';
import { VoiceTurn } from '../src/voice';
import { FRAME_CONTROL_JSON, FRAME_AUDIO_PACKET, writeFrame } from '../../../contracts/ai-sdk/src/turnStream';

for (const [admitted, acknowledgement] of [[true, 'valid'], [true, 'foreign'], [true, 'absent'], [false, 'absent']] as const) {
  test(`saved Loop input requires reliable admission (${admitted}), complete PCM, and ${acknowledgement} interrupt acknowledgement`, async () => {
    const keys = ['isSecureContext', 'WebTransport', 'AudioContext', 'navigator'] as const;
    const saved = new Map(keys.map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
    let response!: ReadableStreamDefaultController<Uint8Array>;
    let unreliable: boolean | undefined, closed = 0, writerClosed = 0, captures = 0;
    const packets: Uint8Array[] = [], controls: any[] = [], observations: any[] = [];
    const input = new Uint8Array(1280); input[0] = 42; input[640] = 81;
    const send = (value: object) => response.enqueue(writeFrame(FRAME_CONTROL_JSON,
      JSON.stringify({ request_id: 'stream-fixture', protocol_version: 'turnstream.v1alpha1', ...value })));
    class Context { async resume() {} async close() {} }
    class Transport {
      ready = Promise.resolve(); closed = new Promise<void>(() => {});
      constructor(_endpoint: string, options: { requireUnreliable: boolean }) { unreliable = options.requireUnreliable; }
      close() { closed++; }
      get datagrams() { throw new Error('Reliable input must not acquire a datagram writer'); }
      async createBidirectionalStream() {
        return { readable: new ReadableStream<Uint8Array>({ start(controller) { response = controller; } }),
          writable: new WritableStream<Uint8Array>({ close() { writerClosed++; }, write(frame) {
            expect(new DataView(frame.buffer, frame.byteOffset).getUint32(1, false)).toBe(frame.length - 5);
            if (frame[0] === FRAME_AUDIO_PACKET) {
              expect(admitted).toBe(true);
              const body = frame.slice(5);
              expect(new TextDecoder().decode(body.slice(0, 5))).toBe('DTVP1');
              expect(new DataView(body.buffer).getUint32(5, false)).toBe(packets.length);
              packets.push(body.slice(9));
              return;
            }
            expect(frame[0]).toBe(FRAME_CONTROL_JSON);
            const value = JSON.parse(new TextDecoder().decode(frame.slice(5))); controls.push(value);
            if (value.identity_token) {
              expect(value.request_id).toBe('stream-fixture');
              expect(value.session_id).toBe('shared-conversation');
              expect(value.metadata.input_mode).toBe('webtransport_audio_stream_v1');
              send({ type: 'accepted', metadata: { audio_endpointing: 'server', audio_endpoint_feedback: 'control-v1',
                audio_datagram_protocol: 'dtvp1', audio_input_transport: admitted ? 'stream-v1' : 'datagram-v1' } });
            } else if (value.type === 'end') {
              expect(value).toEqual({ type: 'end', request_id: 'stream-fixture', packet_count: 2, audio_bytes: 1280 });
              send({ type: 'input_committed', audio_datagrams: 2, audio_bytes: 1280, lost_datagrams: 0 });
              send({ type: 'failed', error: 'intentional fixture stop' });
            } else if (value.type === 'interrupt' && admitted && acknowledgement !== 'absent') {
              // A resolved write alone must not close QUIC. A foreign owner cannot acknowledge it.
              setTimeout(() => {
                expect(closed).toBe(0);
                send({ type: 'interrupt_ack', delivery: 'vbus_publish',
                  request_id: acknowledgement === 'valid' ? 'stream-fixture' : 'other-turn' });
              }, 10);
            }
          } }) };
      }
    }
    const values = { isSecureContext: true, WebTransport: Transport, AudioContext: Context,
      navigator: { mediaDevices: { getUserMedia() { captures++; throw new Error('Unexpected microphone'); } } } };
    for (const key of keys) Object.defineProperty(globalThis, key, { configurable: true, value: values[key] });
    try {
      const turn = new VoiceTurn({ requestId: 'stream-fixture', sessionId: 'shared-conversation', signal: new AbortController().signal,
        identity: 'fixture', endpoint: 'https://fixture.invalid', recordedInput: input, inputTransport: 'stream',
        onPhase() {}, onObservation: (event) => observations.push(event), onAudio() {}, onText() {}, onTranscript() {} });
      await expect(turn.run()).rejects.toThrow(admitted ? 'intentional fixture stop' : 'Reliable voice input was not accepted');
      expect(unreliable).toBe(false); expect(captures).toBe(0); expect(closed).toBe(1); expect(writerClosed).toBe(0);
      expect(packets.length).toBe(admitted ? 2 : 0);
      if (admitted) {
        expect(packets[0]).toEqual(input.slice(0, 640)); expect(packets[1]).toEqual(input.slice(640));
        expect(observations.find((event) => event.name === 'input_committed')).toBeDefined();
      }
      expect(controls.at(-1)).toMatchObject({ type: 'interrupt', request_id: 'stream-fixture' });
      expect(observations.find((event) => event.name === 'interrupt_sent')?.payload.delivery).toBe('stream_write_not_server_ack');
      expect(observations.filter((event) => event.name === 'interrupt_ack')).toHaveLength(acknowledgement === 'valid' ? 1 : 0);
    } finally {
      for (const key of keys) {
        const descriptor = saved.get(key);
        if (descriptor) Object.defineProperty(globalThis, key, descriptor); else Reflect.deleteProperty(globalThis, key);
      }
    }
  });
}
