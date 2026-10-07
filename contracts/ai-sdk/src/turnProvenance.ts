import { TURN_PROVENANCE_WASM_BASE64 } from "./turnProvenanceWasm.js";

type ProvenanceKernel = {
  memory: WebAssembly.Memory;
  input(): number;
  request(): number;
  project(length: number, requestLength: number): number;
  count(kind: number): number;
  string(kind: number, index: number, field: number): number;
  number(kind: number, index: number, field: number): number;
  clear(): void;
};

let kernel: ProvenanceKernel | undefined;
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

function getKernel(): ProvenanceKernel {
  if (typeof WebAssembly === "undefined") throw new Error("C turn provenance decoder unavailable");
  if (!kernel) {
    const bytes = Uint8Array.from(atob(TURN_PROVENANCE_WASM_BASE64), (byte) => byte.charCodeAt(0));
    const module = new WebAssembly.Module(bytes);
    if (WebAssembly.Module.imports(module).length !== 0) throw new Error("C turn provenance decoder has host imports");
    kernel = new WebAssembly.Instance(module).exports as unknown as ProvenanceKernel;
  }
  return kernel;
}

const citationStrings = ["source", "book_slug", "collection", "corpus_version", "embedding_model",
  "record_id", "document_id", "content_hash", "source_sha256", "section", "score_metric"] as const;
const citationNumbers = ["page_start", "page_end", "chunk_index", "score"] as const;

// C owns validation. This adapter copies admitted fields to the existing HTTP
// metadata shape; parity tests compare it with the native C HTTP writer.
export function projectTurnProvenance(bytes: Uint8Array, requestId: string): Record<string, string> {
  const current = getKernel();
  const request = encoder.encode(requestId);
  try {
    if (bytes.byteLength > 131072 || request.byteLength >= 128) throw new Error("Turn provenance exceeds buffer capacity");
    const memory = new Uint8Array(current.memory.buffer);
    memory.set(bytes, current.input());
    memory.set(request, current.request());
    memory[current.request() + request.byteLength] = 0;
    if (!current.project(bytes.byteLength, request.byteLength)) throw new Error("Invalid or mismatched turn provenance");
    const string = (kind: number, index: number, field: number): string => {
      const start = current.string(kind, index, field);
      const end = memory.indexOf(0, start);
      if (!start || end < start) throw new Error("Invalid C provenance field");
      return decoder.decode(memory.subarray(start, end));
    };
    const metadata: Record<string, string> = {};
    if (current.count(0)) {
      metadata.cascade_fallback_reason = "tool_executed";
      metadata.cascade_tool_id = string(0, 0, 0);
      metadata.cascade_tool_call_id = string(0, 0, 1);
      metadata.cascade_tool_output_hash = string(0, 0, 2);
      metadata.cascade_tool_ms = String(current.number(0, 0, 0));
    }
    const count = current.count(1);
    if (count) {
      const citations = [];
      for (let index = 0; index < count; index++) {
        const citation: Record<string, unknown> = Object.fromEntries([
          ...citationStrings.map((name, field) => [name, string(1, index, field)]),
          ...citationNumbers.map((name, field) => [name, current.number(1, index, field)]),
        ]);
        const spanCount = current.number(1, index, 4);
        if (spanCount) citation.excerpt_spans = Array.from({ length: spanCount }, (_, span) => ({
          begin: current.number(1, index, 5 + span * 2),
          end: current.number(1, index, 6 + span * 2),
        }));
        const witnessCount = current.number(1, index, 21);
        if (witnessCount) {
          citation.kind = "complete_passage";
          citation.passage_id = string(1, index, 11);
          citation.witnesses = Array.from({ length: witnessCount }, (_, witness) => {
            const at = index * 16 + witness;
            return {
              record_id: string(3, at, 0), content_hash: string(3, at, 1),
              page: current.number(3, at, 0), chunk_index: current.number(3, at, 1),
              begin: current.number(3, at, 2), end: current.number(3, at, 3),
              record_length: current.number(3, at, 4),
            };
          });
        }
        citations.push(citation);
      }
      metadata.cascade_route = "retrieve_then_escalate";
      metadata.cascade_retrieval_used = "true";
      metadata.cascade_retrieved_documents = String(count);
      metadata.cascade_retrieval_citations = JSON.stringify(citations);
    }
    if (current.count(2)) {
      metadata.cascade_grounding_prompt_id = string(2, 0, 0);
      metadata.cascade_grounding_prompt_version = string(2, 0, 1);
      metadata.cascade_grounding_prompt_sha256 = string(2, 0, 2);
    }
    return metadata;
  } finally {
    current.clear();
  }
}
