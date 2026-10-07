import { readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

export const requestId = "req-browser-dnd";
const root = fileURLToPath(new URL("../../../", import.meta.url));
export function fixtureText(kind) {
  if (kind === 'citation-spans') return fixtureText('citation').replace('score: 0.875',
    'score: 0.875 excerpt_spans { begin: 0 end: 12 } excerpt_spans { begin: 24 end: 40 }');
  if (kind === 'citation-none') return fixtureText('citation').replace('score: 0.875',
    'score: 0 score_metric: RETRIEVAL_SCORE_METRIC_NONE');
  if (kind === 'citation-bm25') return fixtureText('citation').replace('score: 0.875',
    'score: 42.25 score_metric: RETRIEVAL_SCORE_METRIC_BM25');
  return readFileSync(new URL(`./fixtures/${kind}-provenance.textproto`, import.meta.url), "utf8");
}
export function encodeFixture(text) {
  const result = spawnSync("protoc", [
    "--proto_path=" + path.join(root, "contracts/handler-base/proto"),
    "--encode=messages.v1.TurnEvent", "messages/v1/messages.proto",
  ], { input: text });
  if (result.status !== 0) throw new Error(`Canonical protobuf fixture failed: ${result.stderr}`);
  return new Uint8Array(result.stdout);
}
export function fixture(kind) {
  const encoded = JSON.parse(readFileSync(new URL('./fixtures/provenance-wire.json', import.meta.url), 'utf8'));
  return new Uint8Array(Buffer.from(encoded[kind], 'base64'));
}
