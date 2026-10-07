import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { fixture, fixtureText, encodeFixture, requestId } from "./provenanceFixtures.mjs";

if (process.env.PROVENANCE_SDK_BUNDLE) {
  globalThis.window ??= {};
  await import(pathToFileURL(process.env.PROVENANCE_SDK_BUNDLE).href);
}
const { parseTurnEvent, parseProductCitations } = process.env.PROVENANCE_SDK_BUNDLE
  ? window.DTLAI : await import("../dist/index.js");
const asset = readFileSync(new URL("../src/turnProvenanceWasm.ts", import.meta.url), "utf8");
const encoded = asset.split("TURN_PROVENANCE_WASM_BASE64 =")[1].match(/"[A-Za-z0-9+/=]+"/g).map(JSON.parse).join("");

function normalized(metadata) {
  const result = { ...metadata };
  if (result.cascade_retrieval_citations) result.cascade_retrieval_citations = JSON.parse(result.cascade_retrieval_citations);
  return result;
}
function check(wire) {
  const event = parseTurnEvent(wire, requestId);
  if (process.env.PROVENANCE_NATIVE_ORACLE) {
    const reference = spawnSync(process.env.PROVENANCE_NATIVE_ORACLE, [requestId], { input: wire });
    assert.equal(reference.status, 0, String(reference.stderr));
    assert.deepEqual(normalized(event.metadata), normalized(JSON.parse(reference.stdout).metadata));
    for (const [key, name] of [['dndEncounterState', 'dnd_encounter_state'],
      ['dndCampaignRoster', 'dnd_campaign_roster'], ['dndInitiativeResult', 'dnd_initiative_result']]) {
      if (!event[key]) continue;
      const raw = JSON.parse(reference.stdout)[name];
      const camel = (value) => Array.isArray(value) ? value.map(camel) : value && typeof value === 'object'
        ? Object.fromEntries(Object.entries(value).map(([key, item]) => [key.replace(/_([a-z])/g, (_, c) => c.toUpperCase()), camel(item)])) : value;
      assert.deepEqual(event[key], camel(raw));
    }
  }
  return event;
}

const module = new WebAssembly.Module(Uint8Array.from(atob(encoded), (c) => c.charCodeAt(0)));
assert.deepEqual(WebAssembly.Module.imports(module), []);
const exports = new WebAssembly.Instance(module).exports;
assert.equal(exports.memory.buffer.byteLength, 393216);
assert.throws(() => exports.memory.grow(1), RangeError);
assert.equal(exports.project(131073, 1), 0);
assert.equal(exports.project(1, 128), 0);
assert.equal(exports.count(999), 0);
assert.equal(exports.string(999, 0, 0), 0);
assert.equal(exports.number(999, 0, 0), 0);

const tool = check(fixture("tool"));
assert.equal(tool.metadata.cascade_tool_id, "dnd-dice-roll");
assert.equal(tool.metadata.cascade_tool_call_id, "dice-" + "a".repeat(64));
assert.equal(tool.metadata.cascade_tool_output_hash, "b".repeat(64));
assert.equal(tool.metadata.cascade_tool_ms, "12");
const citation = check(fixture("citation"));
const sceneText = fixtureText("tool").replaceAll("dnd-dice-roll", "dnd-scene-presence").replaceAll("dice-", "scene-");
const scene = check(encodeFixture(sceneText));
assert.equal(scene.metadata.cascade_tool_id, "dnd-scene-presence");
assert.equal(scene.dndCampaignRoster, undefined);
assert.equal(scene.dndEncounterState, undefined);
assert.throws(() => parseTurnEvent(encodeFixture(sceneText.replaceAll("scene-", "dice-")), requestId), /provenance/);
// Campaign mutation receipts still require their snapshot; the read-only scene exception cannot widen them.
assert.throws(() => parseTurnEvent(encodeFixture(sceneText.replaceAll("dnd-scene-presence", "dnd-campaign-state")
  .replaceAll("scene-", "campaign-")), requestId), /provenance/);
const raw = JSON.parse(citation.metadata.cascade_retrieval_citations)[0];
assert.equal(raw.source_sha256, "b".repeat(64));
assert.equal(raw.content_hash, "d".repeat(64));
assert.equal(raw.chunk_index, 2);
assert.equal(raw.page_end, 97);
assert.equal(raw.score_metric, "cosine");
assert.equal(citation.metadata.cascade_grounding_prompt_version, "v3");
assert.equal(citation.metadata.cascade_grounding_prompt_sha256, "e".repeat(64));
assert.equal(parseProductCitations(citation.metadata)[0].location, "pp. 96–97 · Sneak Attack");
for (const score of [0, 42.25, 1e20]) {
  const event = check(encodeFixture(fixtureText("citation").replace("score: 0.875",
    `score: ${score} score_metric: RETRIEVAL_SCORE_METRIC_BM25`)));
  const admitted = JSON.parse(event.metadata.cascade_retrieval_citations)[0];
  assert.equal(admitted.score, score);
  assert.equal(admitted.score_metric, "bm25");
  assert.equal(parseProductCitations(event.metadata)[0].scoreMetric, "bm25");
}

const unscored = check(fixture("citation-none"));
const excerpted = check(fixture("citation-spans"));
assert.deepEqual(JSON.parse(excerpted.metadata.cascade_retrieval_citations)[0].excerpt_spans,
  [{ begin: 0, end: 12 }, { begin: 24, end: 40 }]);
assert.equal(JSON.parse(check(fixture('citation')).metadata.cascade_retrieval_citations)[0].excerpt_spans, undefined);
for (const spans of [
  'excerpt_spans {}', 'excerpt_spans { begin: 4 end: 4 }',
  'excerpt_spans { end: 12 } excerpt_spans { begin: 12 end: 15 }',
  'excerpt_spans { end: 12 } excerpt_spans { begin: 10 end: 15 }',
  'excerpt_spans { end: 8192 }',
  Array.from({ length: 9 }, (_, i) => `excerpt_spans { begin: ${i * 3} end: ${i * 3 + 1} }`).join(' '),
]) assert.throws(() => parseTurnEvent(encodeFixture(fixtureText('citation').replace('score: 0.875',
  'score: 0.875 ' + spans)), requestId), /provenance/);
const passage = check(fixture("passage"));
const passageRaw = JSON.parse(passage.metadata.cascade_retrieval_citations)[0];
assert.equal(passageRaw.kind, "complete_passage");
assert.equal(passageRaw.passage_id, "a".repeat(64));
assert.equal(passageRaw.record_id, "");
assert.deepEqual(passageRaw.witnesses, [
  { record_id: "1".repeat(64), content_hash: "2".repeat(64), page: 96, chunk_index: 2, begin: 2, end: 10, record_length: 30 },
  { record_id: "3".repeat(64), content_hash: "4".repeat(64), page: 97, chunk_index: 0, begin: 0, end: 12, record_length: 40 },
]);
assert.equal(parseProductCitations(passage.metadata).length, 1);
// Passage provenance must coexist with the provider metadata used by Loop.
const providerMetadata = {
  model_id: "fixture-model",
  model_identity_source: "provider_reported",
  usage_source: "provider_reported",
  prompt_tokens: "39",
  completion_tokens: "7",
  total_tokens: "46",
};
const measuredPassage = check(encodeFixture(fixtureText("passage") + "\n" +
  Object.entries(providerMetadata).map(([key, value]) =>
    `metadata { key: ${JSON.stringify(key)} value: ${JSON.stringify(value)} }`).join("\n")));
for (const [key, value] of Object.entries(providerMetadata))
  assert.equal(measuredPassage.metadata[key], value);
assert.deepEqual(JSON.parse(measuredPassage.metadata.cascade_retrieval_citations)[0], passageRaw);
for (const changed of [
  fixtureText("passage").replace('page: 97', 'page: 98'),
  fixtureText("passage").replace('end: 10', 'end: 31'),
  fixtureText("passage").replace('score_metric: RETRIEVAL_SCORE_METRIC_NONE', 'score: 0.5'),
  fixtureText("passage").replace('chunk_index: 0', 'chunk_index: 1'),
  fixtureText("passage").replace('passage_id:', 'record_id:'),
  fixtureText("passage").replace('page: 96', 'page: 0'),
]) assert.throws(() => parseTurnEvent(encodeFixture(changed), requestId), /provenance/);

const unscoredRaw = JSON.parse(unscored.metadata.cascade_retrieval_citations)[0];
assert.equal(unscoredRaw.score_metric, "none");
assert.equal(unscoredRaw.score, 0);
assert.equal(parseProductCitations(unscored.metadata)[0].scoreMetric, "none");
assert.equal("score" in parseProductCitations(unscored.metadata)[0], false);

for (const kind of ["tool", "citation", "citation-bm25", "citation-none", "citation-spans", "passage", "encounter", "roster", "initiative"]) {
  const wire = fixture(kind);
  check(wire);
  assert.deepEqual(wire, encodeFixture(fixtureText(kind)), 'Checked-in fixture matches canonical messages.proto');
  assert.throws(() => parseTurnEvent(wire, "req-other"), /mismatched/);
  assert.throws(() => parseTurnEvent(wire.subarray(0, wire.length - 1), requestId), /provenance/);
  assert.throws(() => parseTurnEvent(new Uint8Array([...wire, ...wire]), requestId), /provenance/);
}
for (const text of [
  fixtureText("tool").replace("dnd-dice-roll", "dnd\\000dice-roll"),
  fixtureText("tool").replace("elapsed_ms: 12", "elapsed_ms: -1"),
  fixtureText("tool").replace("TURN_EVENT_TEXT_COMPLETED", "TURN_EVENT_TEXT_DELTA"),
  fixtureText("citation").replace("score: 0.875", "score: nan"),
  fixtureText("citation").replace("score: 0.875", "score: 1e20"),
  fixtureText("citation").replace("score: 0.875", "score: -1 score_metric: RETRIEVAL_SCORE_METRIC_BM25"),
  fixtureText("citation").replace("score: 0.875", "score: nan score_metric: RETRIEVAL_SCORE_METRIC_BM25"),
  fixtureText("citation").replace("score: 0.875", "score: 42.25 score_metric: 2"),
  fixtureText("citation").replace("score: 0.875", "score: -1 score_metric: 2"),
  fixtureText("citation").replace("score: 0.875", "score: nan score_metric: 2"),
  fixtureText("citation").replace("score: 0.875", "score: 0 score_metric: 3"),
  fixtureText("citation").replace("source_sha256: \"b", "source_sha256: \"z"),
  fixtureText("citation").replace("is_final: true", "is_final: false"),
  fixtureText("citation").replace("rag.answer.grounded_response", "unreviewed.prompt"),
]) {
  const wire = encodeFixture(text);
  assert.throws(() => parseTurnEvent(wire, requestId), /provenance/);
  if (process.env.PROVENANCE_NATIVE_ORACLE) {
    const reference = spawnSync(process.env.PROVENANCE_NATIVE_ORACLE, [requestId], { input: wire });
    assert.equal(reference.status, 1, String(reference.stderr));
  }
}

const citationBlock = fixtureText('citation').match(/  citations \{[^}]*\}\n/s)[0];
const citations = (count) => Array.from({ length: count }, (_, index) =>
  citationBlock.replace('c'.repeat(64), String(index + 1).repeat(64))).join('');
check(encodeFixture(fixtureText('citation').replace(citationBlock, citations(4))));
assert.throws(() => parseTurnEvent(encodeFixture(
  fixtureText('citation').replace(citationBlock, citations(5))), requestId), /provenance/);
assert.throws(() => parseTurnEvent(encodeFixture(
  fixtureText('citation').replace(citationBlock, citationBlock.repeat(2))), requestId), /provenance/);
// Wrong wire types for typed provenance still invoke the C boundary.
assert.throws(() => parseTurnEvent(new Uint8Array([...fixture('tool'), 0xa0, 1, 1]), requestId), /provenance/);

for (const score of [0, 0.1, 1, -0.5, 1e-20, 1.000001]) {
  check(encodeFixture(fixtureText("citation").replace("score: 0.875", `score: ${score}`)));
}
check(encodeFixture(fixtureText("citation").replace("Sneak Attack\"", "Rogue \\\"rule\\\" — café\\nline\"")));
const withoutPrompt = fixtureText("citation").replace(/  grounding_prompt \{[^}]*\}\n/s, "");
assert.equal(check(encodeFixture(withoutPrompt)).metadata.cascade_grounding_prompt_id, undefined);
assert.equal(check(fixture("tool")).metadata.cascade_retrieval_citations, undefined);
assert.equal(check(fixture("citation")).metadata.cascade_tool_id, undefined);

const oldFields = encodeFixture(fixtureText("encounter") + `
metadata { key: "fixture" value: "preserved" }
timestamp: 12345
`);
const preserved = parseTurnEvent(oldFields, requestId);
assert.equal(preserved.metadata.fixture, "preserved");
assert.equal(preserved.dndEncounterState.version, 7);
assert.equal(preserved.timestamp, 12345);

const encounter = fixtureText('encounter');
for (const [from, to] of [
  ['current_hp: 4', 'current_hp: 8'], ['initiative: -2', 'initiative: 19'],
  ['id: "goblin"', 'id: "aria"'], ['name: "Goblin"', 'name: "Goblin\\000"'],
  ['name: "Goblin"', 'name: "Goblin\\n"'], ['name: "Goblin"', 'name: "Goblin\\377"'],
  ['conditions: "prone"', 'conditions: "prone" conditions: "prone"'],
  ['conditions: "prone"', 'conditions: "invented"'],
  ['version: 7', 'version: 9007199254740992'], ['version: 7', 'version: 0'],
  ['active_index: 0', 'active_index: 2'], ['status: "active"', 'status: "ended"'],
  ['active_participant_id: "aria"', 'active_participant_id: "goblin"'],
  ['operation: "get"', 'operation: "start"'],
  ['dnd-encounter-state', 'dnd-dice-roll'], ['is_final: true', 'is_final: false'],
  ['b'.repeat(64), 'c'.repeat(64)],
]) {
  const wire = encodeFixture(encounter.replace(from, to));
  assert.throws(() => parseTurnEvent(wire, requestId), /provenance/);
  if (process.env.PROVENANCE_NATIVE_ORACLE)
    assert.equal(spawnSync(process.env.PROVENANCE_NATIVE_ORACLE, [requestId], { input: wire }).status, 1);
}
assert.equal(check(fixture('encounter')).dndEncounterState.participants[0].currentHp, 0);
const participantBlocks = /  participants \{[^\n]*\}\n/g;
const baseEncounter = encounter.replace(participantBlocks, '').replace('active_participant_id: "aria"', 'active_participant_id: "p00"');
const withParticipants = (count, nameLength = 64) => baseEncounter.replace('  operation: "get"',
  Array.from({ length: count }, (_, i) => `  participants { id: "p${String(i).padStart(2, '0')}" name: "${'A'.repeat(nameLength)}" }`).join('\n') + '\n  operation: "get"');
assert.equal(check(encodeFixture(withParticipants(64))).dndEncounterState.participants.length, 64);
assert.throws(() => parseTurnEvent(encodeFixture(withParticipants(65)), requestId), /provenance/);
assert.throws(() => parseTurnEvent(encodeFixture(withParticipants(64, 120)), requestId), /provenance/);
const inactive = baseEncounter.replace('active_index: 0', 'active_index: -1').replace('active_participant_id: "p00"', 'active_participant_id: ""');
assert.equal(check(encodeFixture(inactive)).dndEncounterState.activeIndex, -1);
assert.equal(check(encodeFixture(inactive.replace('status: "active"', 'status: "ended"'))).dndEncounterState.status, 'ended');
// The C validator must run even when only a wrongly typed field 19 appears.
assert.throws(() => parseTurnEvent(new Uint8Array([...fixture('tool'), 0x98, 1, 1]), requestId), /provenance/);

const roster = fixtureText('roster');
const initiative = fixtureText('initiative');
assert.equal(check(fixture('roster')).dndCampaignRoster.characters[0].maxHp, 30);
assert.deepEqual(check(fixture('initiative')).dndInitiativeResult.rolls.map((roll) => roll.total), [18, -2]);
const rejectState = (text) => {
  const wire = encodeFixture(text);
  assert.throws(() => parseTurnEvent(wire, requestId), /provenance/);
  if (process.env.PROVENANCE_NATIVE_ORACLE)
    assert.equal(spawnSync(process.env.PROVENANCE_NATIVE_ORACLE, [requestId], { input: wire }).status, 1);
};
for (const [from, to] of [
  ['version: 4', 'version: 0'], ['version: 4', 'version: 9007199254740992'],
  ['kind: "npc"', 'kind: "account"'], ['id: "goblin"', 'id: "aria"'],
  ['max_hp: 7', 'max_hp: -1'], ['name: "Goblin"', 'name: "Goblin\\000"'],
  ['name: "Goblin"', 'name: "Goblin\\n"'], ['name: "Goblin"', 'name: "Goblin\\377"'],
  ['status: "active"', 'status: "unknown"'], ['is_final: true', 'is_final: false'],
  ['dnd-campaign-state', 'dnd-dice-roll'], ['b'.repeat(64), 'c'.repeat(64)],
]) rejectState(roster.replace(from, to));
for (const [from, to] of [
  ['campaign_version: 4', 'campaign_version: 0'], ['campaign_version: 4', 'campaign_version: 9007199254740992'],
  ['total: 18', 'total: 19'], ['rolls: [10, 15]', 'rolls: [10, 21]'],
  ['kept_indices: [1]', 'kept_indices: [0]'], ['kept_indices: [1]', 'kept_indices: [1, 1]'],
  ['expression: "2d20kh1+3"', 'expression: "2d20kh1+0"'],
  ['character_id: "aria"', 'character_id: "other"'],
  ['character_id: "goblin"', 'character_id: "aria"'],
  ['entropy_source: "getrandom"', 'entropy_source: "model"'],
  ['operation: "roll_initiative"', 'operation: "get"'],
  ['campaign_sha256: "c', 'campaign_sha256: "z'],
]) rejectState(initiative.replace(from, to));
rejectState(initiative.replace(/dnd_initiative_result \{[\s\S]*$/, ''));
rejectState(encounter.replace('operation: "get"', 'operation: "roll_initiative"'));
rejectState(roster.replace('dnd_campaign_roster {', 'dnd_encounter_state {} dnd_campaign_roster {'));
const rosterBase = roster.replace(/  characters \{[^\n]*\}\n/g, '');
const withCharacters = (count, name = 'é'.repeat(100)) => rosterBase.replace('  status: "active"',
  '  status: "active"\n' + Array.from({ length: count }, (_, i) =>
    `  characters { id: "pc-${i}" name: "${name}" kind: "player" max_hp: 30 }`).join('\n'));
const largeRoster = encodeFixture(withCharacters(200));
assert.ok(largeRoster.length > 32768, 'Exercise the extended HTTP response path');
assert.equal(check(largeRoster).dndCampaignRoster.characters.length, 200);
assert.equal(check(encodeFixture(withCharacters(0))).dndCampaignRoster.characters.length, 0);
rejectState(withCharacters(201));
rejectState(withCharacters(1, 'x'.repeat(201)));
for (const tag of [0xb0, 0xb8])
  assert.throws(() => parseTurnEvent(new Uint8Array([...fixture('tool'), tag, 1, 1]), requestId), /provenance/);

for (const invalidId of ['bad id', 'bad/id', 'bad\\nrequest', 'café']) {
  const wire = encodeFixture(fixtureText('tool').replace(requestId, invalidId));
  assert.throws(() => parseTurnEvent(wire, invalidId.replace('\\n', '\n')), /provenance/);
}

const wasm = globalThis.WebAssembly;
try {
  globalThis.WebAssembly = undefined;
  assert.throws(() => parseTurnEvent(fixture("tool"), requestId), /unavailable/);
} finally { globalThis.WebAssembly = wasm; }
console.log('PASS C browser provenance, canonical wire, rejection, and reuse' +
  (process.env.PROVENANCE_NATIVE_ORACLE ? '; native HTTP parity' : ''));
