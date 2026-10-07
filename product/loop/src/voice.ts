import { audioDatagram, controlDatagram, readFrame, writeFrame, parseTurnEvent,
  FRAME_CONTROL_JSON, FRAME_TURN_EVENT_PROTO, FRAME_AUDIO_PACKET, type TurnEvent, type TurnStreamFrame,
} from '../../../contracts/ai-sdk/src/turnStream';
import { PCM16Decoder } from '../../../contracts/ai-sdk/src/audio';
import { ModelRequestCapture } from './model-request';

export interface LiveInput { read(): Promise<Uint8Array | null>; close(): void; }
export type Phase = 'connecting' | 'listening' | 'committing' | 'thinking' | 'speaking' | 'closed';
export type Observation = { name: string; source: 'browser' | 'gateway_observed'; payload: Record<string, unknown> };
type Control = {
  request_id?: string; protocol_version: string; type: string;
  metadata?: Record<string, string>; text?: string; is_final?: boolean;
  audio_datagrams?: number; audio_bytes?: number; lost_datagrams?: number; error?: string; delivery?: string;
};
export type VoiceOptions = {
  requestId: string; sessionId?: string; signal: AbortSignal; identity: string; endpoint: string;
  recordedInput?: Uint8Array;
  liveInput?: LiveInput;
  audioContext?: AudioContext;
  inputTransport?: 'datagram' | 'stream';
  captureModelRequest?: boolean;
  onPhase: (phase: Phase) => void;
  onObservation: (event: Observation) => void;
  onTranscript: (text: string) => void;
  onText: (text: string) => void;
  onAudio: (kind: 'input' | 'output', pcm: Uint8Array, sampleRate: number) => void;
};
export function voiceCapability(recordedInput = false): string | null {
  if (!isSecureContext) return 'A secure browser context is required.';
  if (!globalThis.WebTransport) return 'This browser does not provide WebTransport. Loop voice requires WebTransport; no fallback is available.';
  if (!globalThis.AudioContext) return 'Audio playback is unavailable in this browser.';
  if (!recordedInput && (!globalThis.AudioWorkletNode || !navigator.mediaDevices?.getUserMedia)) return 'AudioWorklet and microphone access are required.';
  return null;
}
const canceled = () => new DOMException('Turn interrupted', 'AbortError');
// Orpheus returns roughly 85 ms bursts; keep a bounded lead across burst jitter.
const PLAYBACK_BUFFER_SECONDS = 0.120;
const PLAYBACK_SCHEDULING_LEAD_SECONDS = 0.010;
export class VoiceTurn {
  phase: Phase = 'connecting';
  private context: AudioContext | undefined;
  private stream: MediaStream | undefined;
  private source: MediaStreamAudioSourceNode | undefined;
  private worklet: AudioWorkletNode | undefined;
  private transport: WebTransport | undefined;
  private reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
  private writer: WritableStreamDefaultWriter<Uint8Array> | undefined;
  private decoder: PCM16Decoder | undefined;
  private sequence = 0;
  private bytes = 0;
  private queued = 0;
  private writes = Promise.resolve();
  private responseTask: Promise<TurnEvent> | undefined;
  private interruptAcknowledged = false;
  private finishWorklet: (() => void) | undefined;
  private workletFinished: Promise<void> = Promise.resolve();
  private finishing: Promise<void> | undefined;
  private inputEnded = false;
  private inputCommitted = false;
  private transcriptFinal = false;
  private closed = false;
  private playbackEnd = 0;
  private firstPlayback = false;
  private playbackStarvations = 0;
  private longestPlaybackLatenessMs = 0;
  private outputSampleRate: number | undefined;
  private sources = new Set<AudioBufferSourceNode>();
  private connectionWatch: ReturnType<typeof setTimeout> | undefined;
  private captureLimit: ReturnType<typeof setTimeout> | undefined;
  private sttWatch: ReturnType<typeof setTimeout> | undefined;
  private rejectFailure!: (error: Error) => void;
  private failure = new Promise<never>((_, reject) => { this.rejectFailure = reject; });
  private abort = () => this.rejectFailure(canceled());
  private savedInput: Uint8Array | undefined;
  private modelRequest: ModelRequestCapture | undefined;
  private get streamInput(): boolean { return this.options.inputTransport === 'stream'; }
  constructor(private options: VoiceOptions) {
    this.savedInput = options.recordedInput ? new Uint8Array(options.recordedInput) : undefined;
    if (options.captureModelRequest) this.modelRequest = new ModelRequestCapture(options.requestId);
  }
  private guard(): void { if (this.closed || this.options.signal.aborted) throw canceled(); }
  private observe(name: string, source: Observation['source'], payload: Record<string, unknown> = {}): void {
    this.options.onObservation({ name, source, payload });
  }
  private setPhase(phase: Phase): void { this.guard(); this.phase = phase; this.options.onPhase(phase); this.observe(phase, 'browser'); }
  fail(error: Error): void { this.rejectFailure(error); }
  private armSttFinalWatch(): void {
    if (this.sttWatch !== undefined || this.transcriptFinal || this.closed) return;
    this.sttWatch = setTimeout(() => this.fail(new Error('No final transcript within 12 seconds')), 12000);
  }
  async run(): Promise<TurnEvent> {
    this.options.signal.addEventListener('abort', this.abort, { once: true });
    const deadline = setTimeout(() => this.fail(new Error('Voice turn exceeded 90 seconds')), 90000);
    let complete = false;
    try {
      const result = await Promise.race([this.execute(), this.failure]); this.guard(); complete = true; return result;
    } finally {
      clearTimeout(deadline); clearTimeout(this.connectionWatch); clearTimeout(this.sttWatch); clearTimeout(this.captureLimit);
      await this.dispose(complete);
    }
  }
  private async execute(): Promise<TurnEvent> {
    this.guard(); const missing = voiceCapability(Boolean(this.savedInput)); if (missing) throw new Error(missing);
    this.setPhase('connecting');
    this.context = this.options.audioContext ?? new AudioContext({ latencyHint: 'interactive' });
    await this.context.resume(); this.guard();
    this.decoder = new PCM16Decoder();
    if (this.savedInput && (!this.savedInput.byteLength || this.savedInput.byteLength > 960000 || this.savedInput.byteLength % 640)) throw new Error('Saved input requires complete 20 ms PCM16 frames, at most 30 seconds');
    const backend = this.savedInput ? 'recorded_pcm16' : this.options.liveInput ? 'live_c_worklet' : await this.prepareMicrophone();
    this.connectionWatch = setTimeout(() => this.fail(new Error('WebTransport voice admission exceeded 10 seconds')), 10000);
    this.transport = new WebTransport(this.options.endpoint, { congestionControl: 'low-latency', requireUnreliable: !this.streamInput });
    this.transport.closed.catch(() => {
      if (!this.closed) this.fail(new Error('WebTransport connection failed'));
    });
    await this.transport.ready; this.guard();
    this.observe('transport_ready', 'browser', { endpoint: this.options.endpoint });
    // New WebTransport implementations expose createWritable instead of writable.
    if (!this.streamInput) {
      const datagrams = this.transport.datagrams as WebTransportDatagramDuplexStream & {
        createWritable?: () => WritableStream<Uint8Array>;
      };
      const outgoing = typeof datagrams.createWritable === 'function' ? datagrams.createWritable() : datagrams.writable;
      if (!outgoing) throw new Error('WebTransport datagram sending is unavailable');
      this.writer = outgoing.getWriter();
    }
    const control = await this.transport.createBidirectionalStream(); this.guard();
    this.reader = control.readable.getReader();
    const writer = control.writable.getWriter();
    if (this.streamInput) this.writer = writer;
    try {
      await writer.write(writeFrame(FRAME_CONTROL_JSON, JSON.stringify({
        request_id: this.options.requestId, session_id: this.options.sessionId ?? this.options.requestId, identity_token: this.options.identity, text: '', enable_tts: true,
        metadata: { interaction_profile: 'realtime_voice', client_surface: 'loop', product: 'loop',
          turn_kind: 'voice', client_transport: 'webtransport-turn-stream',
          input_mode: this.streamInput ? 'webtransport_audio_stream_v1' : 'webtransport_audio',
          client_audio_datagram_protocol: 'dtvp1', client_audio_kernel: backend, client_audio_packet_ms: '20' },
      })));
      if (!this.streamInput) await writer.close();
    } finally { if (!this.streamInput) writer.releaseLock(); }
    const accepted = this.control(await readFrame(this.reader)); this.guard();
    if (accepted.type !== 'accepted' || accepted.metadata?.audio_endpointing !== 'server' ||
        accepted.metadata.audio_endpoint_feedback !== 'control-v1' || accepted.metadata.audio_datagram_protocol !== 'dtvp1') throw new Error('Gateway voice protocol was not accepted');
    if (this.streamInput && accepted.metadata.audio_input_transport !== 'stream-v1') throw new Error('Reliable voice input was not accepted');
    if (this.modelRequest && accepted.metadata.model_request_capture !== 'prepared-request-v1') throw new Error('Model request capture was not accepted');
    clearTimeout(this.connectionWatch);
    this.observe('accepted', 'gateway_observed', { metadata: accepted.metadata });
    this.setPhase('listening');
    if (this.savedInput) {
      this.observe('recorded_input_started', 'browser', { bytes: this.savedInput.byteLength, microphone_recaptured: false });
      void this.sendRecording().catch((e: Error) => this.fail(e));
    } else if (this.options.liveInput) {
      void this.sendLiveInput().catch((e: Error) => this.fail(e));
      this.captureLimit = setTimeout(() => { void this.finishInput().catch((e: Error) => this.fail(e)); }, 30000);
    } else {
      this.source = this.context.createMediaStreamSource(this.stream!); this.source.connect(this.worklet!);
      this.captureLimit = setTimeout(() => { void this.finishInput().catch((e: Error) => this.fail(e)); }, 30000);
    }
    this.responseTask = this.readResponse();
    return this.responseTask;
  }
  private requestMicrophone(): Promise<MediaStream> {
    return requestMicrophone(() => this.guard(), this.options.onObservation);
  }
  private async prepareMicrophone(): Promise<string> {
    const stream = await this.requestMicrophone();
    if (this.closed || this.options.signal.aborted) { stream.getTracks().forEach((t) => t.stop()); throw canceled(); }
    this.stream = stream;
    await this.context!.audioWorklet.addModule('/mic-worklet.js');
    this.guard();
    this.worklet = new AudioWorkletNode(this.context!, 'dtl-realtime-mic', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1], channelCount: 1, channelCountMode: 'explicit' });
    this.worklet.onprocessorerror = () => this.fail(new Error('Audio capture processor failed'));
    let ready!: (backend: string) => void;
    const prepared = new Promise<string>((resolve) => { ready = resolve; });
    this.workletFinished = new Promise<void>((resolve) => { this.finishWorklet = resolve; });
    this.worklet.port.onmessage = ({ data }: MessageEvent<{ type: string; backend: string; pcm: Uint8Array }>) => {
      if (this.closed) return;
      try {
        if (data.type === 'ready') ready(data.backend);
        else if (data.type === 'finished') this.finishWorklet?.();
        else if (data.type === 'packet') this.queue(data.pcm);
      } catch (error) { this.fail(error instanceof Error ? error : new Error(String(error))); }
    };
    this.worklet.connect(this.context!.destination);
    const backend = await prepared; this.guard(); return backend;
  }
  private async sendLiveInput(): Promise<void> {
    while (!this.closed && !this.finishing) {
      const pcm = await this.options.liveInput!.read();
      if (!pcm || this.closed || this.finishing) return;
      this.guard(); this.queue(pcm); await this.writes;
    }
  }
  private async sendRecording(): Promise<void> {
    const input = this.savedInput!;
    for (let offset = 0; offset < input.byteLength; offset += 640) {
      this.guard();
      if (this.finishing) throw new Error('Gateway ended saved input before complete delivery');
      this.queue(input.slice(offset, offset + 640)); await this.writes;
      // Queue and write stalls cannot consume the next frame's spacing.
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    this.guard(); await this.finishInput();
  }
  private control(frame: TurnStreamFrame | null): Control {
    if (!frame || frame.frameType !== FRAME_CONTROL_JSON) throw new Error('Missing voice control');
    const value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(frame.payload)) as Control;
    const foreignRequest = value.request_id !== undefined && value.request_id !== this.options.requestId;
    if (foreignRequest || value.protocol_version !== 'turnstream.v1alpha1') throw new Error('Voice control belongs to another turn');
    if (value.type === 'error') {
      if (!this.closed) this.observe('gateway_error', 'gateway_observed', {
        error: value.error || 'Voice turn failed', request_bound: value.request_id === this.options.requestId,
      });
      throw new Error(value.error || 'Voice turn failed');
    }
    if (value.request_id !== this.options.requestId) throw new Error('Voice control belongs to another turn');
    return value;
  }
  private queue(pcm: Uint8Array): void {
    if (this.closed || !this.writer) return;
    if (!(pcm instanceof Uint8Array) || pcm.byteLength !== 640 || this.queued >= 32) throw new Error('Microphone transport cannot keep up');
    this.options.onAudio('input', pcm, 16000);
    const raw = audioDatagram(this.sequence++, pcm);
    const packet = this.streamInput ? writeFrame(FRAME_AUDIO_PACKET, raw) : raw;
    this.bytes += pcm.byteLength; this.queued++;
    this.writes = this.writes.then(async () => { this.guard(); await this.writer!.write(packet); }).finally(() => { this.queued--; });
    this.writes.catch((e: Error) => this.fail(e));
  }
  finishInput(serverEndpoint = false): Promise<void> {
    if (!this.finishing) this.finishing = this.commit(serverEndpoint);
    return this.finishing;
  }
  private async commit(serverEndpoint: boolean): Promise<void> {
    this.guard(); if (this.phase !== 'listening') throw new Error('Microphone is not listening');
    this.setPhase('committing'); clearTimeout(this.captureLimit);
    if (this.savedInput) {
      if (this.bytes !== this.savedInput.byteLength) throw new Error('Gateway ended saved input before complete delivery');
    } else if (this.options.liveInput) {
      this.options.liveInput.close();
    } else {
      this.source!.disconnect(); this.stream!.getTracks().forEach((t) => t.stop());
      this.worklet!.port.postMessage({ type: serverEndpoint ? 'stop' : 'finish' });
      await this.workletFinished; this.worklet!.disconnect();
    }
    await this.writes; this.guard();
    await this.writer!.write(this.inputControl({ type: 'end', request_id: this.options.requestId,
      packet_count: this.sequence, audio_bytes: this.bytes }));
    this.inputEnded = true;
    this.observe('input_end_sent', 'browser', { packets: this.sequence, bytes: this.bytes, server_endpoint: serverEndpoint });
    this.armSttFinalWatch();
  }
  private inputControl(value: Record<string, unknown>): Uint8Array {
    return this.streamInput ? writeFrame(FRAME_CONTROL_JSON, JSON.stringify(value)) : controlDatagram(value);
  }
  private async play(event: TurnEvent): Promise<void> {
    this.guard();
    if (!this.inputCommitted || !this.transcriptFinal || event.channels !== 1 || event.bitDepth !== 16 ||
        ![16000, 22050, 24000, 44100, 48000].includes(event.sampleRate) ||
        (this.outputSampleRate !== undefined && this.outputSampleRate !== event.sampleRate)) throw new Error('Invalid response audio');
    // Decode before accepting bytes as recording or first-audio evidence.
    const samples = this.decoder!.decode(event.audio);
    if (this.outputSampleRate === undefined) {
      this.outputSampleRate = event.sampleRate;
      this.observe('first_audio_received', 'browser', { sample_rate: event.sampleRate });
    }
    this.options.onAudio('output', event.audio, event.sampleRate);
    const context = this.context!;
    while (this.playbackEnd - context.currentTime > 2) { await new Promise((resolve) => setTimeout(resolve, 20)); this.guard(); }
    const buffer = context.createBuffer(1, samples.length, event.sampleRate); buffer.copyToChannel(new Float32Array(samples), 0);
    const source = context.createBufferSource(); source.buffer = buffer; source.connect(context.destination);
    this.sources.add(source); source.onended = () => { source.disconnect(); this.sources.delete(source); };
    const minimumStart = context.currentTime + PLAYBACK_SCHEDULING_LEAD_SECONDS;
    let start = this.playbackEnd;
    if (!this.firstPlayback || start < minimumStart) {
      if (this.firstPlayback) {
        const lateMs = (minimumStart - start) * 1000;
        this.playbackStarvations++;
        this.longestPlaybackLatenessMs = Math.max(this.longestPlaybackLatenessMs, lateMs);
        this.observe('playback_buffer_starved', 'browser', { count: this.playbackStarvations,
          late_ms: lateMs, buffer_ms: PLAYBACK_BUFFER_SECONDS * 1000,
          boundary: 'insufficient_scheduling_lead', scope: 'scheduled_not_acoustic' });
      }
      start = context.currentTime + PLAYBACK_BUFFER_SECONDS;
    }
    this.playbackEnd = start + buffer.duration;
    if (!this.firstPlayback) { this.firstPlayback = true; this.observe('first_playback_scheduled', 'browser', {
      audio_context_time: start, buffer_ms: PLAYBACK_BUFFER_SECONDS * 1000, scope: 'scheduled_not_acoustic' }); }
    if (this.phase !== 'speaking') this.setPhase('speaking'); source.start(start);
  }
  private async readResponse(): Promise<TurnEvent> {
    let finalText: TurnEvent | undefined; let completed = false; let finalPCM = false; let firstText = false;
    let outputBytes = 0, outputPackets = 0, responseText = '';
    for (;;) {
      const frame = await readFrame(this.reader!);
      if (this.closed) this.acceptInterruptAck(frame);
      this.guard(); if (!frame) break;
      if (frame.frameType === FRAME_CONTROL_JSON) {
        const value = this.control(frame);
        // Signed capture controls can drain after the terminal voice event.
        if (value.type === 'model_request_chunk') {
          if (!this.modelRequest) throw new Error('Unsolicited model request capture');
          const chunk = await this.modelRequest.append(value); this.guard();
          this.observe(value.type, 'gateway_observed', { ...chunk });
          continue;
        }
        if (completed) throw new Error('Event after voice completion');
        this.observe(value.type, 'gateway_observed', { text: value.text ?? '', is_final: value.is_final ?? false,
          audio_datagrams: value.audio_datagrams ?? null, audio_bytes: value.audio_bytes ?? null, lost_datagrams: value.lost_datagrams ?? null, error: value.error ?? '' });
        if (value.type === 'input_endpoint') await this.finishInput(true);
        else if (value.type === 'input_committed') {
          await this.finishing;
          if (!this.inputEnded || this.inputCommitted || value.audio_datagrams !== this.sequence || value.audio_bytes !== this.bytes || value.lost_datagrams !== 0) throw new Error('Incomplete microphone delivery');
          this.inputCommitted = true; this.setPhase('thinking');
        } else if (value.type === 'transcript') {
          if (value.is_final) {
            if (typeof value.text !== 'string' || !value.text.trim()) throw new Error('Final transcript is empty');
            this.transcriptFinal = true; clearTimeout(this.sttWatch);
          }
          this.options.onTranscript(value.text ?? '');
        } else if (value.type !== 'keepalive') throw new Error(value.error || 'Voice turn failed');
        continue;
      }
      if (completed) throw new Error('Event after voice completion');
      if (frame.frameType !== FRAME_TURN_EVENT_PROTO) throw new Error('Unknown voice frame');
      const event = parseTurnEvent(frame.payload, this.options.requestId);
      if (event.requestId !== this.options.requestId) throw new Error('Voice event belongs to another turn');
      const { audio, ...data } = event;
      // One evidence record per semantic event; audio bytes are stored separately.
      if (event.name !== 'pcm_chunk') this.observe(event.name, 'gateway_observed', data);
      if (event.name === 'failed' || event.name === 'canceled') throw new Error(event.error || 'Voice turn did not complete');
      if (event.name === 'text_delta' || event.name === 'text_completed') {
        if (!this.inputCommitted || !this.transcriptFinal) throw new Error('Voice text arrived before committed input and final transcript');
        if (!firstText) { firstText = true; this.observe('first_text_received', 'browser'); }
        // Deltas contain fragments; completion contains the authoritative full body.
        const fragment = event.displayText || event.text;
        responseText = event.name === 'text_completed' ? fragment : responseText + fragment;
        this.options.onText(responseText);
      }
      if (event.name === 'text_completed') {
        if (finalText || !event.isFinal || !(event.displayText || event.text).trim()) throw new Error('Invalid final voice text');
        finalText = event;
      } else if (event.name === 'pcm_chunk') {
        if (finalPCM) throw new Error('Audio after final audio packet');
        await this.play(event); outputBytes += event.audio.byteLength; outputPackets++;
        finalPCM = event.isFinal === true;
      } else if (event.name === 'completed') completed = true;
    }
    this.modelRequest?.assertComplete();
    if (!completed || !finalText || !finalPCM || !this.inputCommitted || !this.transcriptFinal) throw new Error('Incomplete voice turn');
    this.observe('output_received_complete', 'browser', { bytes: outputBytes, packets: outputPackets, sample_rate: this.outputSampleRate });
    this.observe('playback_buffer_summary', 'browser', { starvations: this.playbackStarvations,
      longest_lateness_ms: this.longestPlaybackLatenessMs, buffer_ms: PLAYBACK_BUFFER_SECONDS * 1000,
      scope: 'scheduled_not_acoustic' });
    while (this.sources.size) { await new Promise((resolve) => setTimeout(resolve, 20)); this.guard(); }
    this.observe('playback_finished', 'browser'); return finalText;
  }
  private acceptInterruptAck(frame: TurnStreamFrame | null): void {
    if (!this.streamInput || frame?.frameType !== FRAME_CONTROL_JSON) return;
    try {
      const control = this.control(frame);
      if (control.type === 'interrupt_ack' && control.delivery === 'vbus_publish' && !this.interruptAcknowledged) {
        this.interruptAcknowledged = true;
        this.observe('interrupt_ack', 'gateway_observed', { delivery: 'vbus_publish' });
      }
    } catch { /* A foreign or failed control cannot acknowledge cancellation. */ }
  }
  private async awaitInterruptAck(): Promise<void> {
    await this.responseTask?.catch(() => {});
    while (!this.interruptAcknowledged && this.reader) {
      const frame = await readFrame(this.reader);
      if (!frame) return;
      this.acceptInterruptAck(frame);
    }
  }
  private async dispose(completed: boolean): Promise<void> {
    this.closed = true; this.options.signal.removeEventListener('abort', this.abort);
    this.modelRequest?.dispose();
    this.source?.disconnect(); this.stream?.getTracks().forEach((track) => track.stop());
    if (this.worklet) { this.worklet.port.postMessage({ type: 'stop' }); this.worklet.port.onmessage = null; this.worklet.disconnect(); }
    for (const source of this.sources) { source.stop(); source.disconnect(); } this.sources.clear();
    if (!completed) this.observe('playback_stopped', 'browser');
    this.options.liveInput?.close();
    if (!this.options.audioContext) void this.context?.close().catch(() => {});
    if (this.transport) {
      let timeout: ReturnType<typeof setTimeout> | undefined;
      try {
        if (!completed && this.writer) {
          await Promise.race([
            this.writes.catch(() => {}).then(async () => {
              await this.writer!.write(this.inputControl({ type: 'interrupt', request_id: this.options.requestId, reason: 'user_interrupt' }));
              this.observe('interrupt_sent', 'browser', { delivery: this.streamInput ? 'stream_write_not_server_ack' : 'datagram_write_not_server_ack' });
              if (this.streamInput && this.responseTask) await this.awaitInterruptAck();
            }).catch(() => {}),
            new Promise<void>((resolve) => { timeout = setTimeout(resolve, 250); }),
          ]);
        }
      } finally { clearTimeout(timeout); this.transport.close({ closeCode: 0, reason: completed ? 'turn-complete' : 'turn-canceled' }); }
    }
    void this.reader?.cancel().catch(() => {}); this.phase = 'closed';
    this.options.onPhase('closed'); this.observe('transport_closed', 'browser');
  }
}

export async function requestMicrophone(guard: () => void, observe: (event: Observation) => void): Promise<MediaStream> {
    const request = async (defaults: boolean): Promise<MediaStream> => {
      guard();
      try {
        return await navigator.mediaDevices.getUserMedia({ audio: defaults ? true : {
          channelCount: { ideal: 1 }, echoCancellation: { ideal: true },
          noiseSuppression: { ideal: true }, autoGainControl: { ideal: true },
        } });
      } catch (error) {
        guard();
        const name = error instanceof Error || error instanceof DOMException ? error.name : 'UnknownError';
        const rawConstraint = (error as { constraint?: unknown } | null)?.constraint;
        const constraint = typeof rawConstraint === 'string' &&
          ['channelCount', 'echoCancellation', 'noiseSuppression', 'autoGainControl', 'sampleRate'].includes(rawConstraint)
          ? rawConstraint : 'unspecified';
        observe({ name: 'microphone_request_failed', source: 'browser', payload: { error_name: name, constraint, settings: defaults ? 'default' : 'preferred' } });
        if (!defaults && name === 'OverconstrainedError') {
          observe({ name: 'microphone_settings_retry', source: 'browser', payload: { settings: 'default' } });
          return request(true);
        }
        const detail = name === 'NotAllowedError' ? 'Allow microphone access for Loop in the browser and system settings.'
          : name === 'NotFoundError' ? 'Connect or select an available microphone.'
          : name === 'OverconstrainedError' ? 'The browser rejected default microphone settings. Check the selected input device.'
          : name === 'NotReadableError' ? 'The browser could not open the microphone. Check the input device and other audio applications.'
          : 'Check microphone permissions and the selected input device.';
        throw new Error(`Microphone setup failed (${name}): ${detail}`);
      }
    };
    return request(false);
  }
