import assert from "node:assert/strict";

import {
  RealtimeAudioPacketizer,
  supportsWasmSIMDAudio,
} from "../dist/index.js";

const packetizer = new RealtimeAudioPacketizer(48000);
const packets = [];
for (let offset = 0; offset < 960; offset += 128) {
  const count = Math.min(128, 960 - offset);
  packets.push(...packetizer.push(new Float32Array(count).fill(0.25)));
}

assert.equal(packets.length, 1, "20 ms of 48 kHz input must produce one packet");
assert.equal(packets[0].pcm.byteLength, 640, "uplink packet must contain 320 s16le samples");
assert.ok(Math.abs(packets[0].rms - 0.25) < 0.0001, "packet RMS must match the source");

const samples = new Int16Array(
  packets[0].pcm.buffer,
  packets[0].pcm.byteOffset,
  packets[0].pcm.byteLength / 2,
);
assert.ok(samples.every((sample) => sample >= 8190 && sample <= 8192), "PCM conversion must preserve amplitude");

if (supportsWasmSIMDAudio()) {
  assert.equal(packetizer.backend, "wasm-simd", "SIMD-capable runtimes must select the SIMD kernel");
}

const resetPacketizer = new RealtimeAudioPacketizer(44100);
resetPacketizer.push(new Float32Array(441));
resetPacketizer.reset();
assert.equal(
  resetPacketizer.push(new Float32Array(882)).length,
  1,
  "reset must discard a partial packet without changing duration",
);

console.log("audio tests passed");
