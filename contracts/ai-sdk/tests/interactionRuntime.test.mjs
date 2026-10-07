import assert from "node:assert/strict";
import {
  createInteractionRuntime,
  interactionPhasePresentation,
  replayInteractionTrace,
} from "../dist/index.js";

const events = [
  { type: "start", at: 100, turnId: "turn-1", inputMode: "audio", transport: "webtransport", audioKernel: "wasm-simd" },
  { type: "connected", at: 135, inputMode: "audio" },
  { type: "voice_activity", at: 200 },
  { type: "voice_activity", at: 220 },
  { type: "commit", at: 900 },
  { type: "thinking", at: 940, metadata: { llm_route: "cascade-local" } },
  { type: "response_text", at: 1110 },
  { type: "speaking", at: 1260 },
];

const runtime = createInteractionRuntime({ now: () => 0 });
for (const event of events) runtime.dispatch(event);
let snapshot = runtime.getSnapshot();

assert.equal(snapshot.phase, "speaking");
assert.equal(snapshot.turnId, "turn-1");
assert.equal(snapshot.transport, "webtransport");
assert.equal(snapshot.audioKernel, "wasm-simd");
assert.equal(snapshot.context.llm_route, "cascade-local");
assert.equal(snapshot.latency.connectMs, 35);
assert.equal(snapshot.latency.utteranceMs, 700);
assert.equal(snapshot.latency.firstTextMs, 210);
assert.equal(snapshot.latency.firstAudioMs, 360);
assert.equal(snapshot.history.filter((entry) => entry.event === "voice_activity").length, 1);

runtime.dispatch({ type: "response_text", at: 1280, turnId: "turn-1" });
assert.equal(runtime.getSnapshot().phase, "speaking");
runtime.dispatch({ type: "interrupt", at: 1300, reason: "voice_barge_in" });
runtime.dispatch({ type: "complete", at: 1310, turnId: "stale-turn" });
runtime.dispatch({ type: "canceled", at: 1342 });
snapshot = runtime.getSnapshot();
assert.equal(snapshot.phase, "recovering");
assert.equal(snapshot.latency.interruptMs, 42);

const replayed = replayInteractionTrace([...events, { type: "complete", at: 1500 }]);
assert.equal(replayed.phase, "completed");
assert.equal(replayed.latency.totalMs, 1400);
assert.equal(interactionPhasePresentation("speaking").label, "In the scene");

console.log("interaction runtime tests passed");
