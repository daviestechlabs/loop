import assert from "node:assert/strict";
import { FRAME_CONTROL_JSON, audioDatagram, controlDatagram, parseTurnEvent, readFrame, writeFrame } from "../dist/index.js";

function test(name, fn) {
  fn();
  console.log(`ok - ${name}`);
}

test("writeFrame emits type and big-endian payload length", () => {
  const frame = writeFrame(FRAME_CONTROL_JSON, "{}");
  const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
  assert.equal(view.getUint8(0), FRAME_CONTROL_JSON);
  assert.equal(view.getUint32(1, false), 2);
  assert.equal(new TextDecoder().decode(frame.slice(5)), "{}");
});

test("controlDatagram uses the DTVA1 prefix", () => {
  const datagram = controlDatagram({ type: "end" });
  assert.equal(new TextDecoder().decode(datagram.slice(0, 6)), "DTVA1:");
});

const framed = writeFrame(FRAME_CONTROL_JSON, '{}');
const streamReader = (chunks) => new ReadableStream({ start(controller) {
  chunks.forEach((chunk) => controller.enqueue(chunk)); controller.close();
} }).getReader();
for (let split = 0; split <= framed.length; split++) {
  const reader = streamReader([framed.slice(0, split), framed.slice(split), framed]);
  assert.equal(new TextDecoder().decode((await readFrame(reader)).payload), '{}');
  assert.equal(new TextDecoder().decode((await readFrame(reader)).payload), '{}');
  assert.equal(await readFrame(reader), null);
  reader.releaseLock();
}
for (const bytes of [framed.slice(0, 3), framed.slice(0, 5), framed.slice(0, 6),
  Uint8Array.from([1, 0, 0, 0, 0]), Uint8Array.from([1, 0, 16, 0, 1])]) {
  const reader = streamReader([bytes]);
  await assert.rejects(readFrame(reader), /[Tt]urn frame/);
  reader.releaseLock();
}
console.log('ok - framed streams preserve coalesced responses and reject truncation or oversized lengths');

test("audioDatagram binds a big-endian sequence to sample-aligned PCM", () => {
  const pcm = Uint8Array.from([0, 1, 2, 3]);
  const datagram = audioDatagram(0x01020304, pcm);
  assert.equal(new TextDecoder().decode(datagram.slice(0, 5)), "DTVP1");
  assert.equal(new DataView(datagram.buffer).getUint32(5, false), 0x01020304);
  assert.deepEqual(datagram.slice(9), pcm);
  assert.throws(() => audioDatagram(0, Uint8Array.from([1])), /sample-aligned/);
  assert.throws(() => audioDatagram(-1, pcm), /unsigned 32-bit/);
});

test("parseTurnEvent decodes text and metadata", () => {
  const bytes = Uint8Array.from([
    ...stringField(1, "req-1"),
    ...varintField(3, 4),
    ...stringField(5, "hello"),
    ...varintField(7, 16000),
    ...mapField(16, "interaction_profile", "realtime_voice"),
  ]);
  const event = parseTurnEvent(bytes);
  assert.equal(event.requestId, "req-1");
  assert.equal(event.name, "text_delta");
  assert.equal(event.text, "hello");
  assert.equal(event.sampleRate, 16000);
  assert.equal(event.metadata.interaction_profile, "realtime_voice");
});

test("parseTurnEvent decodes dual text and governed encounter state", () => {
  const participant = Uint8Array.from([
    ...stringField(1, "rogue"),
    ...stringField(2, "Rogue"),
    ...varintField(3, zigZag32(18)),
    ...varintField(4, 24),
    ...varintField(5, 17),
    ...stringField(6, "prone"),
  ]);
  const state = Uint8Array.from([
    ...stringField(1, "campaign-1"),
    ...stringField(2, "ambush-1"),
    ...varintField(3, 4),
    ...stringField(4, "active"),
    ...varintField(5, 2),
    ...varintField(6, zigZag32(0)),
    ...stringField(7, "rogue"),
    ...bytesField(8, participant),
    ...stringField(9, "encounter-" + "b".repeat(64)),
    ...stringField(10, "a".repeat(64)),
    ...stringField(11, "get"),
  ]);
  const bytes = Uint8Array.from([
    ...stringField(1, "encounter-turn"),
    ...varintField(3, 5),
    ...varintField(12, 1),
    ...stringField(5, "The door opens."),
    ...stringField(17, "The door <gasp> opens."),
    ...stringField(18, "The door opens."),
    ...bytesField(19, state),
    ...bytesField(20, Uint8Array.from([
      ...stringField(1, "dnd-encounter-state"),
      ...stringField(2, "encounter-" + "b".repeat(64)),
      ...stringField(3, "a".repeat(64)),
    ])),
  ]);
  const event = parseTurnEvent(bytes);
  assert.equal(event.speechText, "The door <gasp> opens.");
  assert.equal(event.displayText, "The door opens.");
  assert.equal(event.dndEncounterState?.campaignId, "campaign-1");
  assert.equal(event.dndEncounterState?.activeParticipantId, "rogue");
  assert.equal(event.dndEncounterState?.participants[0]?.currentHp, 17);
  assert.deepEqual(event.dndEncounterState?.participants[0]?.conditions, ["prone"]);
  assert.equal(event.dndEncounterState?.outputSha256, "a".repeat(64));
});

function varint(value) {
  const out = [];
  let cursor = value;
  while (cursor >= 0x80) {
    out.push((cursor & 0x7f) | 0x80);
    cursor = Math.floor(cursor / 128);
  }
  out.push(cursor);
  return out;
}

function varintField(field, value) {
  return [...varint(field << 3), ...varint(value)];
}

function stringField(field, value) {
  const encoded = new TextEncoder().encode(value);
  return [...varint((field << 3) | 2), ...varint(encoded.length), ...encoded];
}

function bytesField(field, value) {
  return [...varint((field << 3) | 2), ...varint(value.length), ...value];
}

function zigZag32(value) {
  return value >= 0 ? value * 2 : (-value * 2) - 1;
}

function mapField(field, key, value) {
  const entry = Uint8Array.from([...stringField(1, key), ...stringField(2, value)]);
  return [...varint((field << 3) | 2), ...varint(entry.length), ...entry];
}
