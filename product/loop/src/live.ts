import { requestMicrophone, type LiveInput, type Observation, type Phase } from './voice';

// Ten seconds bounds identity/admission/save delays. Overflow fails rather than losing speech.
export class LiveFrames implements LiveInput {
  private packets: Uint8Array[] = [];
  private waiting: ((value: Uint8Array | null) => void) | undefined;
  private closed = false;
  push(pcm: Uint8Array): void {
    if (this.closed) return;
    if (pcm.byteLength !== 640) throw new Error('Invalid live microphone packet');
    if (this.packets.length >= 500) throw new Error('Live voice cannot keep up. Conversation stopped without dropping speech.');
    const packet = new Uint8Array(pcm);
    if (this.waiting) { const resolve = this.waiting; this.waiting = undefined; resolve(packet); }
    else this.packets.push(packet);
  }
  read(): Promise<Uint8Array | null> {
    if (this.closed) return Promise.resolve(null);
    const packet = this.packets.shift();
    return packet ? Promise.resolve(packet) : new Promise((resolve) => { this.waiting = resolve; });
  }
  close(): void { this.closed = true; this.packets = []; this.waiting?.(null); this.waiting = undefined; }
}

type Callbacks = {
  run: (input: LiveFrames) => Promise<boolean>;
  interrupt: (reason: 'speech' | 'stop') => void;
  idle: () => void;
  fail: (error: Error) => void;
};
// Activity detection is browser evidence, not server speech or acoustic proof.
export class LiveTurns {
  private input: LiveFrames | undefined;
  private pending: LiveFrames | undefined;
  private phase: Phase = 'closed';
  private busy = false;
  private stopped = false;
  private preRoll: Uint8Array[] = [];
  private loud = 0;
  private quiet = 0;
  private armed = true;
  constructor(private callbacks: Callbacks) {}
  setPhase(input: LiveFrames, phase: Phase): void { if (this.input === input) this.phase = phase; }
  packet(pcm: Uint8Array, rms: number): void {
    if (this.stopped) return;
    try {
      if (pcm.byteLength !== 640 || !Number.isFinite(rms) || rms < 0) throw new Error('Invalid live microphone packet');
      this.preRoll.push(new Uint8Array(pcm)); if (this.preRoll.length > 15) this.preRoll.shift();
      if (this.phase === 'connecting' || this.phase === 'listening') this.input?.push(pcm);
      if (rms < 0.015) { this.quiet++; this.loud = 0; if (this.quiet >= 15) this.armed = true; }
      else { this.quiet = 0; this.loud = rms >= 0.025 ? this.loud + 1 : 0; }
      if (!this.armed || this.loud < 6 || this.phase === 'connecting' || this.phase === 'listening') return;
      this.armed = false; this.loud = 0;
      const input = new LiveFrames(); for (const packet of this.preRoll) input.push(packet);
      this.input?.close(); this.input = input; this.pending = input; this.phase = 'connecting';
      if (this.busy) this.callbacks.interrupt('speech');
      void this.drain();
    } catch (error) { this.stop(); this.callbacks.fail(error instanceof Error ? error : new Error(String(error))); }
  }
  private async drain(): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      while (this.pending && !this.stopped) {
        const input = this.pending; this.pending = undefined;
        const ok = await this.callbacks.run(input); input.close();
        if (!ok) { this.stop(); return; }
        if (this.input === input) { this.input = undefined; this.phase = 'closed'; this.preRoll = []; }
      }
      if (!this.stopped) this.callbacks.idle();
    } catch (error) { this.stop(); this.callbacks.fail(error instanceof Error ? error : new Error(String(error))); }
    finally { this.busy = false; }
  }
  stop(): void {
    if (this.stopped) return;
    this.stopped = true; this.input?.close(); this.pending?.close(); this.pending = undefined; this.preRoll = [];
    this.callbacks.interrupt('stop');
  }
}

export class LiveMicrophone {
  readonly context = new AudioContext({ latencyHint: 'interactive' });
  private stream: MediaStream | undefined;
  private source: MediaStreamAudioSourceNode | undefined;
  private worklet: AudioWorkletNode | undefined;
  private stopped = false;
  private cancelReady: (() => void) | undefined;
  constructor(private packet: (pcm: Uint8Array, rms: number) => void, private fail: (error: Error) => void) {}
  private guard = () => { if (this.stopped) throw new DOMException('Conversation stopped', 'AbortError'); };
  async start(observe: (event: Observation) => void): Promise<void> {
    try {
      await this.context.resume(); this.guard();
      const stream = await requestMicrophone(this.guard, observe);
      if (this.stopped) { stream.getTracks().forEach((track) => track.stop()); this.guard(); }
      this.stream = stream;
      for (const track of stream.getAudioTracks()) track.onended = () => this.fail(new Error('Microphone disconnected. Conversation stopped.'));
      await this.context.audioWorklet.addModule('/mic-worklet.js');
      this.guard();
      this.worklet = new AudioWorkletNode(this.context, 'dtl-realtime-mic', { numberOfInputs: 1, numberOfOutputs: 1,
        outputChannelCount: [1], channelCount: 1, channelCountMode: 'explicit' });
      const worklet = this.worklet;
      let timer: ReturnType<typeof setTimeout> | undefined;
      try {
        await new Promise<void>((resolve, reject) => {
          this.cancelReady = () => reject(new DOMException('Conversation stopped', 'AbortError'));
          timer = setTimeout(() => reject(new Error('Microphone processor did not start')), 5000);
          worklet.onprocessorerror = () => { const error = new Error('Microphone processor failed'); reject(error); this.fail(error); };
          worklet.port.onmessage = ({ data }: MessageEvent<{ type: string; pcm: Uint8Array; rms: number }>) => {
            if (this.stopped) return;
            if (data.type === 'ready') resolve();
            else if (data.type === 'packet') this.packet(data.pcm, data.rms);
          };
          worklet.connect(this.context.destination);
        });
      } finally { clearTimeout(timer); this.cancelReady = undefined; }
      this.guard(); this.source = this.context.createMediaStreamSource(stream); this.source.connect(worklet);
    } catch (error) { this.stop(); throw error; }
  }
  stop(): void {
    if (this.stopped) return;
    this.stopped = true; this.cancelReady?.(); this.source?.disconnect();
    for (const track of this.stream?.getTracks() ?? []) { track.onended = null; track.stop(); }
    if (this.worklet) { this.worklet.onprocessorerror = null; this.worklet.port.onmessage = null; this.worklet.port.postMessage({ type: 'stop' }); this.worklet.disconnect(); }
    void this.context.close().catch(() => {});
  }
}
