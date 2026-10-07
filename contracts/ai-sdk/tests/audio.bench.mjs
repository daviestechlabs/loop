import { performance } from "node:perf_hooks";

import { RealtimeAudioPacketizer } from "../dist/index.js";

const iterations = Number(process.env.AUDIO_BENCH_ITERATIONS || 10000);
const input = new Float32Array(960);
for (let index = 0; index < input.length; index++) {
  input[index] = Math.sin((2 * Math.PI * 220 * index) / 48000) * 0.25;
}

function run(label, preferSIMD) {
  const packetizer = new RealtimeAudioPacketizer(48000, 20, preferSIMD);
  for (let index = 0; index < 100; index++) packetizer.push(input);
  const startedAt = performance.now();
  let bytes = 0;
  for (let index = 0; index < iterations; index++) {
    bytes += packetizer.push(input)[0]?.pcm.byteLength || 0;
  }
  const elapsedMs = performance.now() - startedAt;
  return {
    label,
    backend: packetizer.backend,
    iterations,
    elapsed_ms: Number(elapsedMs.toFixed(3)),
    us_per_20ms_packet: Number(((elapsedMs * 1000) / iterations).toFixed(3)),
    realtime_factor: Number((elapsedMs / (iterations * 20)).toFixed(6)),
    bytes,
  };
}

console.log(JSON.stringify({
  simd: run("simd", true),
  scalar: run("scalar", false),
}, null, 2));
