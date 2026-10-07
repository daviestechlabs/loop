import { writeFileSync } from 'node:fs';
import { fixtureText, encodeFixture } from '../tests/provenanceFixtures.mjs';

// Browser runners consume canonical bytes without installing protoc.
const fixtures = Object.fromEntries(['tool', 'citation', 'citation-bm25', 'citation-none', 'citation-spans', 'passage', 'encounter', 'roster', 'initiative'].map((kind) =>
  [kind, Buffer.from(encodeFixture(fixtureText(kind))).toString('base64')]));
writeFileSync(new URL('../tests/fixtures/provenance-wire.json', import.meta.url), JSON.stringify(fixtures, null, 2) + '\n');
