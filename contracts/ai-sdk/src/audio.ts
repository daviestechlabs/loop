export const INPUT_SAMPLE_RATE = 16000;
export const REALTIME_AUDIO_PACKET_MS = 20;

import { SIMD_AUDIO_KERNEL_BASE64, SCALAR_AUDIO_KERNEL_BASE64 } from './audioWasm.js';

export type RealtimeAudioBackend = "wasm-simd" | "scalar";

export type RealtimeAudioPacket = {
  pcm: Uint8Array;
  rms: number;
  backend: RealtimeAudioBackend;
};

type AudioKernelExports = {
  memory: WebAssembly.Memory;
  input_ptr(): number;
  output_ptr(): number;
  output_samples(): number;
  process(inputLength: number): number;
  playback_ptr(): number;
  decode_pcm(byteLength: number): number;
  clear_audio(): void;
};

function decodeBase64(value: string): Uint8Array {
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const clean = value.replace(/=+$/, "");
  const output = new Uint8Array(Math.floor((clean.length * 6) / 8));
  let accumulator = 0;
  let bits = 0;
  let offset = 0;
  for (const character of clean) {
    const digit = alphabet.indexOf(character);
    if (digit < 0) continue;
    accumulator = (accumulator << 6) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output[offset++] = (accumulator >> bits) & 0xff;
    }
  }
  return offset === output.length ? output : output.slice(0, offset);
}

function instantiateAudioKernel(encoded: string): AudioKernelExports | null {
  if (typeof WebAssembly === "undefined") return null;
  try {
    const module = new WebAssembly.Module(
      decodeBase64(encoded) as unknown as BufferSource,
    );
    const instance = new WebAssembly.Instance(module);
    return instance.exports as unknown as AudioKernelExports;
  } catch {
    return null;
  }
}

export function supportsWasmSIMDAudio(): boolean {
  return instantiateAudioKernel(SIMD_AUDIO_KERNEL_BASE64) !== null;
}

/**
 * Stateful 20 ms mono packetizer for the WebTransport mic uplink.
 *
 * It consumes browser-rate Float32 audio without dropping render-quantum
 * remainders, downsamples each exact-duration packet to 16 kHz, and emits
 * s16le PCM. The same C kernel supplies SIMD and scalar WebAssembly builds.
 * The canonical "scalar" backend label now refers to portable C.
 */
export class RealtimeAudioPacketizer {
  readonly backend: RealtimeAudioBackend;
  readonly inputSamplesPerPacket: number;

  private readonly kernel: AudioKernelExports;
  private readonly pending: Float32Array;
  private pendingLength = 0;

  constructor(
    sourceSampleRate: number,
    packetMs = REALTIME_AUDIO_PACKET_MS,
    preferSIMD = true,
  ) {
    this.inputSamplesPerPacket = sourceSampleRate * packetMs / 1000;
    if (packetMs !== REALTIME_AUDIO_PACKET_MS || !Number.isInteger(this.inputSamplesPerPacket) ||
        this.inputSamplesPerPacket < 1 || this.inputSamplesPerPacket > 4096) throw new RangeError('Unsupported audio packet duration');
    this.pending = new Float32Array(this.inputSamplesPerPacket);
    const simd = preferSIMD ? instantiateAudioKernel(SIMD_AUDIO_KERNEL_BASE64) : null;
    const kernel = simd ?? instantiateAudioKernel(SCALAR_AUDIO_KERNEL_BASE64);
    if (!kernel) throw new Error('C audio kernel unavailable');
    this.kernel = kernel;
    this.backend = simd ? "wasm-simd" : "scalar";
  }

  push(input: Float32Array): RealtimeAudioPacket[] {
    const packets: RealtimeAudioPacket[] = [];
    let inputOffset = 0;
    while (inputOffset < input.length) {
      const count = Math.min(
        input.length - inputOffset,
        this.inputSamplesPerPacket - this.pendingLength,
      );
      this.pending.set(input.subarray(inputOffset, inputOffset + count), this.pendingLength);
      this.pendingLength += count;
      inputOffset += count;
      if (this.pendingLength === this.inputSamplesPerPacket) {
        packets.push(this.packetize(this.pending.subarray(0, this.pendingLength)));
        this.pendingLength = 0;
      }
    }
    return packets;
  }

  reset(): void {
    this.pending.fill(0);
    this.pendingLength = 0;
    this.kernel.clear_audio();
  }

  private packetize(input: Float32Array): RealtimeAudioPacket {
    const inputPtr = this.kernel.input_ptr();
    const outputPtr = this.kernel.output_ptr();
    const outputSamples = this.kernel.output_samples();
    new Float32Array(this.kernel.memory.buffer, inputPtr, input.length).set(input);
    const energy = this.kernel.process(input.length);
    if (!(energy >= 0)) throw new RangeError('Invalid audio samples');
    const pcm = new Uint8Array(
      this.kernel.memory.buffer,
      outputPtr,
      outputSamples * Int16Array.BYTES_PER_ELEMENT,
    ).slice();
    return { pcm, rms: Math.sqrt(Math.max(0, energy)), backend: this.backend };
  }
}

/** C converts returned s16le PCM; Web Audio owns device-rate playback. */
export class PCM16Decoder {
  private readonly kernel: AudioKernelExports;
  constructor(preferSIMD = true) {
    const kernel = (preferSIMD ? instantiateAudioKernel(SIMD_AUDIO_KERNEL_BASE64) : null)
      ?? instantiateAudioKernel(SCALAR_AUDIO_KERNEL_BASE64);
    if (!kernel) throw new Error('C audio kernel unavailable');
    this.kernel = kernel;
  }
  decode(pcm: Uint8Array): Float32Array {
    try {
      if (pcm.byteLength > 16384) throw new RangeError('PCM frame exceeds C capacity');
      new Uint8Array(this.kernel.memory.buffer, this.kernel.input_ptr(), pcm.byteLength).set(pcm);
      const count = this.kernel.decode_pcm(pcm.byteLength);
      if (!count) throw new RangeError('Invalid PCM frame');
      return new Float32Array(this.kernel.memory.buffer, this.kernel.playback_ptr(), count).slice();
    } finally { this.kernel.clear_audio(); }
  }
}

export function realtimeMicWorkletSource(): string {
  return `
const SIMD = ${JSON.stringify(SIMD_AUDIO_KERNEL_BASE64)};
const SCALAR = ${JSON.stringify(SCALAR_AUDIO_KERNEL_BASE64)};
const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function decodeBase64(value) {
  const clean = value.replace(/=+$/, "");
  const output = new Uint8Array(Math.floor((clean.length * 6) / 8));
  let accumulator = 0, bits = 0, offset = 0;
  for (const character of clean) {
    const digit = ALPHABET.indexOf(character);
    if (digit < 0) continue;
    accumulator = (accumulator << 6) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output[offset++] = (accumulator >> bits) & 255;
    }
  }
  return offset === output.length ? output : output.slice(0, offset);
}
function createKernel(encoded) {
  try {
    return new WebAssembly.Instance(new WebAssembly.Module(decodeBase64(encoded))).exports;
  } catch (_) {
    return null;
  }
}
class DTLRealtimeMicProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.samplesPerPacket = sampleRate * ${REALTIME_AUDIO_PACKET_MS} / 1000;
    if (!Number.isInteger(this.samplesPerPacket) || this.samplesPerPacket < 1 || this.samplesPerPacket > 4096)
      throw new RangeError('Unsupported audio packet duration');
    this.pending = new Float32Array(this.samplesPerPacket);
    this.pendingLength = 0;
    const simd = options?.processorOptions?.preferSIMD === false ? null : createKernel(SIMD);
    this.kernel = simd || createKernel(SCALAR);
    if (!this.kernel) throw new Error('C audio kernel unavailable');
    this.backend = simd ? "wasm-simd" : "scalar";
    this.finished = false;
    this.port.onmessage = ({ data }) => {
      if (this.finished || !['finish', 'stop'].includes(data.type)) return;
      this.finished = true;
      if (data.type === 'finish' && this.pendingLength) {
        this.pending.fill(0, this.pendingLength);
        this.emitPacket(this.pending);
      }
      this.pending.fill(0);
      this.pendingLength = 0;
      this.kernel.clear_audio();
      this.port.postMessage({ type: 'finished' });
    };
    this.port.postMessage({ type: "ready", backend: this.backend, packetMs: ${REALTIME_AUDIO_PACKET_MS} });
  }
  emitPacket(input) {
    const inputPtr = this.kernel.input_ptr();
    const outputPtr = this.kernel.output_ptr();
    const outputSamples = this.kernel.output_samples();
    new Float32Array(this.kernel.memory.buffer, inputPtr, input.length).set(input);
    const energy = this.kernel.process(input.length);
    if (!(energy >= 0)) throw new RangeError('Invalid audio samples');
    const pcm = new Uint8Array(this.kernel.memory.buffer, outputPtr, outputSamples * 2).slice();
    const packet = { pcm, rms: Math.sqrt(energy), backend: this.backend };
    this.port.postMessage({ type: "packet", ...packet }, [packet.pcm.buffer]);
  }
  process(inputs, outputs) {
    if (this.finished) return false;
    const input = inputs[0] && inputs[0][0];
    if (input && input.length) {
      let inputOffset = 0;
      while (inputOffset < input.length) {
        const count = Math.min(input.length - inputOffset, this.samplesPerPacket - this.pendingLength);
        this.pending.set(input.subarray(inputOffset, inputOffset + count), this.pendingLength);
        this.pendingLength += count;
        inputOffset += count;
        if (this.pendingLength === this.samplesPerPacket) {
          this.emitPacket(this.pending.subarray(0, this.pendingLength));
          this.pendingLength = 0;
        }
      }
    }
    const output = outputs[0] && outputs[0][0];
    if (output) output.fill(0);
    return true;
  }
}
registerProcessor("dtl-realtime-mic", DTLRealtimeMicProcessor);
`;
}

export function makeRealtimeMicWorkletUrl(): string {
  return URL.createObjectURL(new Blob([realtimeMicWorkletSource()], { type: "text/javascript" }));
}

export function downsampleTo16k(float32: Float32Array, sourceRate: number): Float32Array {
  if (sourceRate === INPUT_SAMPLE_RATE) return float32;
  const ratio = sourceRate / INPUT_SAMPLE_RATE;
  const outputLength = Math.max(1, Math.floor(float32.length / ratio));
  const output = new Float32Array(outputLength);
  for (let index = 0; index < outputLength; index++) {
    const sourceIndex = index * ratio;
    const before = Math.floor(sourceIndex);
    const after = Math.min(before + 1, float32.length - 1);
    const weight = sourceIndex - before;
    const beforeSample = float32[before] ?? 0;
    const afterSample = float32[after] ?? beforeSample;
    output[index] = beforeSample * (1 - weight) + afterSample * weight;
  }
  return output;
}

export function floatToPcm16(float32: Float32Array): Uint8Array {
  const output = new Uint8Array(float32.length * 2);
  const view = new DataView(output.buffer);
  for (let index = 0; index < float32.length; index++) {
    const sample = Math.max(-1, Math.min(1, float32[index] ?? 0));
    view.setInt16(index * 2, sample < 0 ? sample * 0x8000 : sample * 0x7fff, true);
  }
  return output;
}

export function rms(float32: Float32Array): number {
  let sum = 0;
  for (let index = 0; index < float32.length; index++) {
    const sample = float32[index] ?? 0;
    sum += sample * sample;
  }
  return Math.sqrt(sum / Math.max(1, float32.length));
}

export function pcmLevel(bytes: Uint8Array): number {
  if (!bytes.byteLength) return 0;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const samples = Math.floor(bytes.byteLength / 2);
  let sum = 0;
  for (let index = 0; index < samples; index++) {
    const sample = view.getInt16(index * 2, true) / 32768;
    sum += sample * sample;
  }
  return Math.sqrt(sum / Math.max(1, samples));
}

export function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}
