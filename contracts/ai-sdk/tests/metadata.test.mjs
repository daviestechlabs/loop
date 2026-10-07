import assert from "node:assert/strict";
import { buildTurnMetadata, buildTurnRequestPayload, interactionModeConfig } from "../dist/index.js";

function test(name, fn) {
  fn();
  console.log(`ok - ${name}`);
}

test("buildTurnMetadata emits canonical product metadata", () => {
  const metadata = buildTurnMetadata({
    clientSurface: "companions-frontend",
    transport: "webtransport-turn-stream",
    interactionProfile: "realtime_voice",
    voiceMode: "realtime",
    turnProfile: "realtime",
    turnKind: "voice",
    traceId: "trace-1",
    retrievalSkip: true,
  });

  assert.equal(metadata.turn_source, "companions-frontend-webtransport-turn-stream");
  assert.equal(metadata.turn_kind, "voice");
  assert.equal(metadata.client_trace_id, "trace-1");
  assert.equal(metadata.client_transport, "webtransport-turn-stream");
  assert.equal(metadata.client_surface, "companions-frontend");
  assert.equal(metadata.interaction_profile, "realtime_voice");
  assert.equal(metadata.voice_mode, "realtime");
  assert.equal(metadata.turn_profile, "realtime");
  assert.equal(metadata.retrieval_skip, "true");
});

test("buildTurnRequestPayload applies mode policy", () => {
  const payload = buildTurnRequestPayload({
    text: "Review this plan",
    sessionId: "session-1",
    userId: "user-1",
    username: "billy",
    clientSurface: "companions-frontend",
    transport: "http-turn-stream",
    interactionProfile: "coder_agent",
    traceId: "trace-2",
    premium: true,
    enableRag: true,
    voiceId: "tara",
  });

  assert.equal(payload.enable_tts, false);
  assert.equal(payload.voice_id, "");
  assert.equal(payload.metadata.interaction_profile, "coder_agent");
  assert.equal(payload.metadata.agent_id, "waterdeep-coder");
  assert.equal(payload.metadata.task_intent, "coder_assist");
});

test("buildTurnRequestPayload carries a server-minted identity credential", () => {
  const payload = buildTurnRequestPayload({
    requestId: "turn-identity-1",
    identityToken: "signed.turn.identity",
    text: "hello",
    sessionId: "session-1",
    userId: "client-value-is-untrusted",
    username: "client-name-is-untrusted",
    clientSurface: "companions-frontend",
    transport: "next-webtransport",
    interactionProfile: "realtime_voice",
  });
  assert.equal(payload.identity_token, "signed.turn.identity");
  assert.equal(payload.request_id, "turn-identity-1");
});

test("interactionModeConfig returns a copy", () => {
  const first = interactionModeConfig("detail_voice");
  first.voiceMode = "mutated";
  assert.equal(interactionModeConfig("detail_voice").voiceMode, "detail");
});

test("Khelben media modes select governed task identities", () => {
  const image = interactionModeConfig("khelben_image");
  assert.equal(image.agentId, "khelben-image");
  assert.equal(image.taskIntent, "generate_dnd_map");
  assert.equal(image.enableTts, true);
  assert.equal(image.retrievalSkip, true);

  const video = interactionModeConfig("khelben_video");
  assert.equal(video.agentId, "khelben-video");
  assert.equal(video.taskIntent, "generate_dnd_cinematic");
  assert.equal(video.enableTts, true);
});
