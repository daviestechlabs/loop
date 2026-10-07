import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import vm from 'node:vm';
import { readFileSync } from 'node:fs';
const browser = { window: {}, WebAssembly, Uint8Array, Float32Array, TextEncoder, TextDecoder, URL, Blob };
if (process.env.AUDIO_SDK_BUNDLE) vm.runInNewContext(readFileSync(process.env.AUDIO_SDK_BUNDLE, 'utf8'), browser);
const { RealtimeAudioPacketizer, PCM16Decoder, makeRealtimeMicWorkletUrl } = process.env.AUDIO_SDK_BUNDLE
  ? browser.window.DTLAI : await import('../dist/audio.js');
const asset = readFileSync(new URL('../src/audioWasm.ts', import.meta.url), 'utf8');
const encodedKernel = (kind) => asset.match(new RegExp(`export const ${kind}_AUDIO_KERNEL_BASE64 =([\\s\\S]*?);`))[1]
  .match(/"[^"]*"/g).map((chunk) => JSON.parse(chunk)).join('');
const SCALAR_AUDIO_KERNEL_BASE64 = encodedKernel('SCALAR');
const SIMD_AUDIO_KERNEL_BASE64 = encodedKernel('SIMD');

const kernels = [SCALAR_AUDIO_KERNEL_BASE64, SIMD_AUDIO_KERNEL_BASE64].map((encoded) => {
  const module = new WebAssembly.Module(Buffer.from(encoded, 'base64'));
  assert.deepEqual(WebAssembly.Module.imports(module), []);
  const kernel = new WebAssembly.Instance(module).exports;
  assert.equal(kernel.memory.buffer.byteLength, 131072);
  assert.throws(() => kernel.memory.grow(1));
  return kernel;
});
let seed = 0x8129;
const random = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 0x100000000 * 2 - 1; };
const vectors = [];
for (const length of [1, 3, 127, 128, 160, 320, 441, 882, 960, 1920, 4096]) {
  for (const sample of [-1, -0.25, 0, 0.25, 1, null]) {
    vectors.push(Float32Array.from({ length }, () => sample ?? random()));
  }
}
const nativeInput = [];
const expectedOutput = [];
for (const samples of vectors) {
  const outputs = kernels.map((kernel) => {
    new Float32Array(kernel.memory.buffer, kernel.input_ptr(), samples.length).set(samples);
    const energy = kernel.process(samples.length);
    assert.ok(energy >= 0 && energy <= 1);
    const result = Buffer.alloc(644);
    result.writeFloatLE(energy);
    result.set(new Uint8Array(kernel.memory.buffer, kernel.output_ptr(), 640), 4);
    kernel.clear_audio();
    assert.ok(new Uint8Array(kernel.memory.buffer, kernel.input_ptr(), 16384).every((byte) => byte === 0));
    assert.ok(new Uint8Array(kernel.memory.buffer, kernel.playback_ptr(), 32768).every((byte) => byte === 0));
    return result;
  });
  assert.deepEqual(outputs[0], outputs[1], `scalar/SIMD parity for ${samples.length} samples`);
  const header = Buffer.alloc(4);
  header.writeUInt32LE(samples.length);
  nativeInput.push(header, Buffer.from(samples.buffer));
  expectedOutput.push(outputs[0]);
}
if (process.env.AUDIO_NATIVE_ORACLE) {
  const result = spawnSync(process.env.AUDIO_NATIVE_ORACLE, { input: Buffer.concat(nativeInput), maxBuffer: 1024 * 1024 });
  assert.equal(result.status, 0, result.stderr.toString());
  assert.deepEqual(result.stdout, Buffer.concat(expectedOutput), 'native C and both WASM builds agree byte for byte');
}
for (const kernel of kernels) {
  for (const length of [0, 4097, 0xffffffff]) assert.equal(kernel.process(length), -1);
  for (const sample of [NaN, Infinity, -Infinity, 1.01, -1.01]) {
    new Float32Array(kernel.memory.buffer, kernel.input_ptr(), 1)[0] = sample;
    assert.equal(kernel.process(1), -1);
  }
  for (const length of [0, 1, 16385, 0xffffffff]) assert.equal(kernel.decode_pcm(length), 0);
  kernel.clear_audio();
}
for (const preferSIMD of [false, true]) {
  const decoder = new PCM16Decoder(preferSIMD);
  for (let start = -32768; start < 32768; start += 8192) {
    const pcm = Buffer.alloc(16384);
    for (let i = 0; i < 8192; i++) pcm.writeInt16LE(start + i, i * 2);
    const floats = decoder.decode(pcm);
    for (let i = 0; i < 8192; i++) assert.equal(floats[i], (start + i) / 32768);
  }
  for (const size of [0, 1, 16385]) assert.throws(() => decoder.decode(new Uint8Array(size)), { name: 'RangeError' });
}
for (const rate of [0, NaN, Infinity, 44101, 204850]) assert.throws(() => new RealtimeAudioPacketizer(rate), { name: 'RangeError' });
assert.throws(() => new RealtimeAudioPacketizer(48000, 10), { name: 'RangeError' });

// Execute the actual generated worklet, including cross-quantum carry and final padding.
const workletUrl = makeRealtimeMicWorkletUrl();
const workletSource = await (await fetch(workletUrl)).text();
URL.revokeObjectURL(workletUrl);
for (const rate of [16000, 44100, 48000, 96000]) {
  for (const preferSIMD of [false, true]) {
    let Processor;
    const messages = [];
    vm.runInNewContext(workletSource, {
      sampleRate: rate, WebAssembly, Float32Array, Uint8Array,
      AudioWorkletProcessor: class { port = { postMessage: (message) => messages.push(message) }; },
      registerProcessor: (_, value) => { Processor = value; },
    });
    const worklet = new Processor({ processorOptions: { preferSIMD } });
    const samples = new Float32Array(rate / 50 + 99).fill(0.25);
    for (let i = 0; i < samples.length; i += 128) worklet.process([[samples.subarray(i, i + 128)]], [[new Float32Array(128)]]);
    assert.equal(messages.filter((m) => m.type === 'packet').length, 1);
    worklet.port.onmessage({ data: { type: 'finish' } });
    const packets = messages.filter((m) => m.type === 'packet');
    assert.equal(packets.length, 2);
    assert.equal(messages.at(-1).type, 'finished');
    const padded = new Float32Array(rate / 50 * 2);
    padded.set(samples);
    const expected = new RealtimeAudioPacketizer(rate, 20, preferSIMD).push(padded);
    assert.deepEqual(Buffer.concat(packets.map((p) => p.pcm)), Buffer.concat(Array.from(expected, (p) => p.pcm)));
    assert.equal(worklet.process([[samples]], [[new Float32Array(128)]]), false);
    worklet.port.onmessage({ data: { type: 'finish' } });
    assert.equal(messages.filter((m) => m.type === 'finished').length, 1);
    const stopped = new Processor({ processorOptions: { preferSIMD } });
    stopped.process([[new Float32Array(128).fill(0.25)]], [[new Float32Array(128)]]);
    stopped.port.onmessage({ data: { type: 'stop' } });
    assert.equal(messages.filter((m) => m.type === 'packet').length, 2, 'server stop discards the unfinished packet');
    assert.equal(messages.filter((m) => m.type === 'finished').length, 2);
  }
}
console.log('PASS C audio: native/scalar/SIMD parity, all PCM16 values, bounds, clearing, worklet carry and final padding');
