import assert from "node:assert/strict";
import { parseProductCitations, sceneSpeakerFromMetadata } from "../dist/index.js";

const citations = parseProductCitations({
  cascade_retrieval_citations: JSON.stringify([
    {
      source: "book://players-handbook",
      score: 0.97,
      book_slug: "players-handbook",
      title: "Player's Handbook",
      page_start: 195,
      page_end: 196,
      section: "Actions in Combat",
      collection: "dnd_text_chunks_v3",
      corpus_version: "dnd-corpus-v3",
      record_id: "record-1",
    },
    {
      source: "private://users/alice/campaigns/ravenloft/secret-note",
      score: 0.82,
      collection: "dnd_campaign_memory",
      record_id: "record-2",
    },
  ]),
});

assert.deepEqual(citations, [
  {
    id: "citation-1",
    kind: "rulebook",
    label: "Player's Handbook",
    location: "pp. 195–196 · Actions in Combat",
    score: 0.97,
    collection: "dnd_text_chunks_v3",
    corpusVersion: "dnd-corpus-v3",
  },
  {
    id: "citation-2",
    kind: "campaign",
    label: "Campaign memory",
    location: "Retrieved campaign context",
    score: 0.82,
    collection: "dnd_campaign_memory",
  },
]);
assert.equal(JSON.stringify(citations).includes("secret-note"), false);

assert.deepEqual(
  sceneSpeakerFromMetadata({
    scene_director_npc_id: "ireena",
    scene_director_npc_name: "Ireena Kolyana",
    scene_director_voice_id: "ireena-v1",
    scene_director_knowledge_scope: "campaign_canon",
  }),
  {
    id: "ireena",
    name: "Ireena Kolyana",
    voiceId: "ireena-v1",
    knowledgeScope: "campaign_canon",
  },
);

assert.deepEqual(parseProductCitations({ retrieval_citations: "{bad json" }), []);
assert.deepEqual(
  parseProductCitations({ retrieval_citations: JSON.stringify([{ source: "book://players-handbook" }]) }, 0),
  [],
);
assert.equal(sceneSpeakerFromMetadata({}), null);

console.log("product context tests passed");
