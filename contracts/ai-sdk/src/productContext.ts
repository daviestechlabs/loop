export type ProductCitationKind = "rulebook" | "campaign";

export type ProductCitation = {
  id: string;
  kind: ProductCitationKind;
  label: string;
  location: string;
  score?: number;
  scoreMetric?: "cosine" | "bm25" | "none";
  collection?: string;
  corpusVersion?: string;
};

export type SceneSpeaker = {
  id: string;
  name: string;
  voiceId: string;
  knowledgeScope: string;
};

type RawCitation = Record<string, unknown>;

const citationMetadataKeys = ["cascade_retrieval_citations", "retrieval_citations"] as const;
const maxCitationMetadataBytes = 128 * 1024;
const defaultCitationLimit = 8;

function text(value: unknown): string {
  return typeof value === "string" ? value.trim() : "";
}

function finiteNumber(value: unknown): number | undefined {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value !== "string" || value.trim() === "") return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : undefined;
}

function titleCaseSlug(value: string): string {
  return value
    .replace(/^book:\/\//, "")
    .replace(/[-_]+/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase())
    .trim();
}

function citationLocation(citation: RawCitation): string {
  const pageStart = finiteNumber(citation.page_start);
  const pageEnd = finiteNumber(citation.page_end);
  const section = text(citation.section);
  if (pageStart !== undefined) {
    const pages = pageEnd !== undefined && pageEnd !== pageStart ? `pp. ${pageStart}–${pageEnd}` : `p. ${pageStart}`;
    return section ? `${pages} · ${section}` : pages;
  }
  return section || "Retrieved campaign context";
}

function citationLabel(citation: RawCitation): { kind: ProductCitationKind; label: string } {
  const title = text(citation.title);
  const bookSlug = text(citation.book_slug);
  const source = text(citation.source);
  const collection = text(citation.collection).toLowerCase();
  const bookBacked = Boolean(bookSlug || source.startsWith("book://") || collection.startsWith("dnd_text_chunks"));
  if (bookBacked) {
    return {
      kind: "rulebook",
      label: title || titleCaseSlug(bookSlug || source) || "Rulebook",
    };
  }
  return { kind: "campaign", label: title || "Campaign memory" };
}

function rawCitations(metadata: Record<string, string>): RawCitation[] {
  for (const key of citationMetadataKeys) {
    const encoded = text(metadata[key]);
    if (!encoded || encoded.length > maxCitationMetadataBytes) continue;
    try {
      const decoded: unknown = JSON.parse(encoded);
      if (Array.isArray(decoded)) {
        return decoded.filter((value): value is RawCitation => Boolean(value) && typeof value === "object" && !Array.isArray(value));
      }
    } catch {
      // A malformed optional product field must not break the turn stream.
    }
  }
  return [];
}

export function parseProductCitations(
  metadata: Record<string, string> | null | undefined,
  limit = defaultCitationLimit,
): ProductCitation[] {
  if (!metadata) return [];
  const requestedLimit = Number.isFinite(limit) ? Math.floor(limit) : defaultCitationLimit;
  const boundedLimit = Math.max(0, Math.min(requestedLimit, 20));
  if (boundedLimit === 0) return [];
  const seen = new Set<string>();
  const citations: ProductCitation[] = [];
  for (const citation of rawCitations(metadata)) {
    const source = text(citation.source);
    if (!source) continue;
    const pageStart = finiteNumber(citation.page_start);
    const recordId = text(citation.record_id);
    const identity = text(citation.passage_id) || recordId || `${source}:${pageStart ?? ""}:${text(citation.section)}`;
    if (seen.has(identity)) continue;
    seen.add(identity);
    const presentation = citationLabel(citation);
    const score = citation.score_metric === "none" ? undefined : finiteNumber(citation.score);
    citations.push({
      id: `citation-${citations.length + 1}`,
      kind: presentation.kind,
      label: presentation.label,
      location: citationLocation(citation),
      ...(score !== undefined ? { score } : {}),
      ...(citation.score_metric === "cosine" || citation.score_metric === "bm25" || citation.score_metric === "none"
        ? { scoreMetric: citation.score_metric } : {}),
      ...(text(citation.collection) ? { collection: text(citation.collection) } : {}),
      ...(text(citation.corpus_version) ? { corpusVersion: text(citation.corpus_version) } : {}),
    });
    if (citations.length >= boundedLimit) break;
  }
  return citations;
}

export function sceneSpeakerFromMetadata(
  metadata: Record<string, string> | null | undefined,
): SceneSpeaker | null {
  if (!metadata) return null;
  const id = text(metadata.scene_director_npc_id);
  const name = text(metadata.scene_director_npc_name);
  if (!id && !name) return null;
  return {
    id: id || name.toLowerCase().replace(/[^a-z0-9]+/g, "-"),
    name: name || titleCaseSlug(id),
    voiceId: text(metadata.scene_director_voice_id),
    knowledgeScope: text(metadata.scene_director_knowledge_scope),
  };
}
