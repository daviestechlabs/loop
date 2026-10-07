
const SIMD = "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DQELIAIgA2shAiADQQF0QYCIgIAAaiEAIANBAnRBgI2BgABqIQUDQCAFIAAvAQAiA0GAgHxyIAMgA0H//wFLG7JDAAAAOJQ4AgAgAEECaiEAIAVBBGohBSACQX9qIgINAAsLIAELigQBAX9BgIB/IQADQCAAQYCIgYAAakEAOgAAIABBgYiBgABqQQA6AAAgAEGCiIGAAGpBADoAACAAQYOIgYAAakEAOgAAIABBhIiBgABqQQA6AAAgAEGFiIGAAGpBADoAACAAQYaIgYAAakEAOgAAIABBh4iBgABqQQA6AAAgAEEIaiIADQALQYB2IQADQCAAQYCXg4AAakEAOgAAIABBgZeDgABqQQA6AAAgAEGCl4OAAGpBADoAACAAQYOXg4AAakEAOgAAIABBhJeDgABqQQA6AAAgAEGFl4OAAGpBADoAACAAQYaXg4AAakEAOgAAIABBh5eDgABqQQA6AAAgAEEIaiIADQALQYB7IQADQCAAQYCNgYAAakEAOgAAIABBgY2BgABqQQA6AAAgAEGCjYGAAGpBADoAACAAQYONgYAAakEAOgAAIABBhI2BgABqQQA6AAAgAEGFjYGAAGpBADoAACAAQYaNgYAAakEAOgAAIABBh42BgABqQQA6AAAgAEEIaiIADQALQYCAfiEAA0AgAEGAjYOAAGpBADoAACAAQYGNg4AAakEAOgAAIABBgo2DgABqQQA6AAAgAEGDjYOAAGpBADoAACAAQYSNg4AAakEAOgAAIABBhY2DgABqQQA6AAAgAEGGjYOAAGpBADoAACAAQYeNg4AAakEAOgAAIABBCGoiAA0ACws=";
const SCALAR = "AGFzbQEAAAABEgRgAAF/YAF/AX1gAX8Bf2AAAAMIBwAAAAEAAgMEBQFwAQEBBQQBAQICBggBfwFBgJcHCwdoCAZtZW1vcnkCAAlpbnB1dF9wdHIAAApvdXRwdXRfcHRyAAEOb3V0cHV0X3NhbXBsZXMAAgdwcm9jZXNzAAMMcGxheWJhY2tfcHRyAAQKZGVjb2RlX3BjbQAFC2NsZWFyX2F1ZGlvAAYKyg0HCABBgIiAgAALCABBgIiBgAALBQBBwAILnAgGAX0CfwF9AX8DfQd/QwAAgL8hAQJAIABB/19qQYBgSQ0AQYCIgIAAIQIgACEDA0BDAACAvyEBIAIqAgAiBEMAAIC/YEUNASAEQwAAgD9fRQ0BIAJBBGohAiADQX9qIgMNAAsCQAJAIABBBE8NAEMAAAAAIQFBACEFDAELIABB/D9xIQVDAAAAACEEQYCIgIAAIQJDAAAAACEBQwAAAAAhBkMAAAAAIQdBBCEDA0AgBCACKgIAIgggCJSSIQQgByACQQxqKgIAIgggCJSSIQcgBiACQQhqKgIAIgggCJSSIQYgASACQQRqKgIAIgggCJSSIQEgAkEQaiECIANBBGoiAyAATQ0ACyAEIAGSIAaSIAeSIQELAkAgBSAATw0AAkACQCAAQQNxIgkNACAFIQMMAQsgBUECdEGAiICAAGohAiAFIQMDQCADQQFqIQMgASACKgIAIgQgBJSSIQEgAkEEaiECIAlBf2oiCQ0ACwsgBSAAa0F8Sw0AIAAgA2shCSADQQJ0QYCIgIAAaiECA0AgASACKgIAIgQgBJSSIAJBBGoqAgAiBCAElJIgAkEIaioCACIEIASUkiACQQxqKgIAIgQgBJSSIQEgAkEQaiECIAlBfGoiCQ0ACwsgALMhBkEAIQpBACELA0AgCyIMQQFqIgsgAGxBwAJuIgIgDCAAbCIJQcACbiINQQFqIAIgDUsbIgIgACACIABJGyEFIApBwAJuIQNDAAAAACEEAkAgDSAATw0AAkACQCAFIAlBwAJuIg5BAWoiAiAFIAJLGyIPIA5rQQNxDQBDAAAAACEEIA0hAwwBCyADQQJ0QYCIgIAAaiECIAUgA0EBaiIJIAUgCUsbIANrQQNxQQJ0IQlDAAAAACEEIA0hAwNAIANBAWohAyAEIAIqAgCSIQQgAkEEaiECIAlBfGoiCQ0ACwsgDiAPa0F8Sw0AIANBAnRBgIiAgABqIQIDQCAEIAIqAgCSIAJBBGoqAgCSIAJBCGoqAgCSIAJBDGoqAgCSIQQgAkEQaiECIANBBGoiAyAFSQ0ACwsgDEECdEGAjYOAAGogBCAFIA1rs5U4AgAgCiAAaiEKIAtBwAJHDQALQYCNg4AAIQJBgHshAwNAAkACQEMAAIA/QwAAgL8gAioCACIEIARDAACAv10bIgQgBEMAAIA/XhtDAP7/RpQiBItDAAAAT11FDQAgBKghCQwBC0GAgICAeCEJCyADQYCNgYAAaiAJOwEAAkACQEMAAIA/QwAAgL8gAkEEaioCACIEIARDAACAv10bIgQgBEMAAIA/XhtDAP7/RpQiBItDAAAAT11FDQAgBKghCQwBC0GAgICAeCEJCyADQYKNgYAAaiAJOwEAIAJBCGohAiADQQRqIgMNAAsgASAGlSEBCyABCwgAQYCNgYAAC30BBH9BACEBAkAgAEF/akH//wBLDQAgAEEBcQ0AIABBAXYiAUEBIAFBAUsbIQJBgI2BgAAhAEGAiICAACEDA0AgACADLwEAIgRBgIB8ciAEIARB//8BSxuyQwAAADiUOAIAIANBAmohAyAAQQRqIQAgAkF/aiICDQALCyABC4oEAQF/QYCAfyEAA0AgAEGAiIGAAGpBADoAACAAQYGIgYAAakEAOgAAIABBgoiBgABqQQA6AAAgAEGDiIGAAGpBADoAACAAQYSIgYAAakEAOgAAIABBhYiBgABqQQA6AAAgAEGGiIGAAGpBADoAACAAQYeIgYAAakEAOgAAIABBCGoiAA0AC0GAdiEAA0AgAEGAl4OAAGpBADoAACAAQYGXg4AAakEAOgAAIABBgpeDgABqQQA6AAAgAEGDl4OAAGpBADoAACAAQYSXg4AAakEAOgAAIABBhZeDgABqQQA6AAAgAEGGl4OAAGpBADoAACAAQYeXg4AAakEAOgAAIABBCGoiAA0AC0GAeyEAA0AgAEGAjYGAAGpBADoAACAAQYGNgYAAakEAOgAAIABBgo2BgABqQQA6AAAgAEGDjYGAAGpBADoAACAAQYSNgYAAakEAOgAAIABBhY2BgABqQQA6AAAgAEGGjYGAAGpBADoAACAAQYeNgYAAakEAOgAAIABBCGoiAA0AC0GAgH4hAANAIABBgI2DgABqQQA6AAAgAEGBjYOAAGpBADoAACAAQYKNg4AAakEAOgAAIABBg42DgABqQQA6AAAgAEGEjYOAAGpBADoAACAAQYWNg4AAakEAOgAAIABBho2DgABqQQA6AAAgAEGHjYOAAGpBADoAACAAQQhqIgANAAsL";
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
    this.samplesPerPacket = sampleRate * 20 / 1000;
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
    this.port.postMessage({ type: "ready", backend: this.backend, packetMs: 20 });
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
