// Transport validation for explicitly consented recordings.
// The C store verifies the edge signature. This class verifies framing and hash.
export const MODEL_REQUEST_MAX = (40960 + 4096) * 6 + 1023;
export const MODEL_REQUEST_CHUNK_MAX = 8192;

export interface ModelRequestChunk {
  type: 'model_request_chunk';
  protocol_version: 'turnstream.v1alpha1';
  request_id: string;
  sequence: number;
  total_bytes: number;
  data: string;
  final: boolean;
  sha256: string;
  captured_at: number;
  nonce: string;
  signature: string;
}

const fields = ['type', 'protocol_version', 'request_id', 'sequence', 'total_bytes',
  'data', 'final', 'sha256', 'captured_at', 'nonce', 'signature'];
const hex = (value: unknown, length: number): value is string =>
  typeof value === 'string' && value.length === length && /^[a-f0-9]+$/.test(value);

export class ModelRequestCapture {
  private bytes: Uint8Array<ArrayBuffer> | undefined;
  private binding: Pick<ModelRequestChunk, 'total_bytes' | 'sha256' | 'captured_at' | 'nonce' | 'signature'> | undefined;
  private received = 0;
  private sequence = 0;
  private state: 'empty' | 'collecting' | 'checking' | 'complete' | 'failed' = 'empty';

  constructor(private readonly requestId: string) {
    if (!/^[a-zA-Z0-9_-]{1,127}$/.test(requestId)) throw new Error('Invalid capture request ID');
  }

  private fail(): never {
    this.dispose();
    throw new Error('Invalid prepared model request capture');
  }

  dispose(): void {
    this.bytes?.fill(0);
    this.bytes = undefined;
    this.binding = undefined;
    this.state = 'failed';
  }

  // Call serially. A concurrent append fails the entire capture.
  async append(value: unknown): Promise<ModelRequestChunk> {
    let decoded: Uint8Array | undefined;
    try {
      if (this.state !== 'empty' && this.state !== 'collecting') return this.fail();
      if (!value || typeof value !== 'object' || Array.isArray(value)) return this.fail();
      const row = value as Record<string, unknown>;
      if (Object.keys(row).length !== fields.length || !fields.every((key) => Object.hasOwn(row, key)) ||
          row.type !== 'model_request_chunk' || row.protocol_version !== 'turnstream.v1alpha1' ||
          row.request_id !== this.requestId || row.sequence !== this.sequence ||
          !Number.isSafeInteger(row.total_bytes) || typeof row.total_bytes !== 'number' ||
          row.total_bytes < 1 || row.total_bytes > MODEL_REQUEST_MAX ||
          typeof row.final !== 'boolean' || typeof row.data !== 'string' ||
          !row.data.length || row.data.length > 10924 || row.data.length % 4 !== 0 ||
          !hex(row.sha256, 64) || !hex(row.nonce, 32) || !hex(row.signature, 64) ||
          !Number.isSafeInteger(row.captured_at) || typeof row.captured_at !== 'number' || row.captured_at <= 0)
        return this.fail();
      const chunk = { ...row } as unknown as ModelRequestChunk;
      if (this.binding && (chunk.total_bytes !== this.binding.total_bytes || chunk.sha256 !== this.binding.sha256 ||
          chunk.captured_at !== this.binding.captured_at || chunk.nonce !== this.binding.nonce ||
          chunk.signature !== this.binding.signature)) return this.fail();
      const binary = atob(chunk.data);
      if (btoa(binary) !== chunk.data) return this.fail();
      decoded = Uint8Array.from(binary, (character) => character.charCodeAt(0));
      const remaining = chunk.total_bytes - this.received;
      if (decoded.length !== Math.min(remaining, MODEL_REQUEST_CHUNK_MAX) ||
          chunk.final !== (decoded.length === remaining)) return this.fail();
      if (!this.binding) {
        this.binding = { total_bytes: chunk.total_bytes, sha256: chunk.sha256,
          captured_at: chunk.captured_at, nonce: chunk.nonce, signature: chunk.signature };
        this.bytes = new Uint8Array(chunk.total_bytes);
      }
      if (!this.bytes) return this.fail();
      this.bytes.set(decoded, this.received);
      this.received += decoded.length;
      this.sequence++;
      this.state = 'collecting';
      if (chunk.final) {
        this.state = 'checking';
        const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', this.bytes));
        const hash = Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
        // Disposal or another append while hashing invalidates this result.
        if (this.state !== 'checking' || hash !== chunk.sha256) return this.fail();
        this.bytes.fill(0);
        this.bytes = undefined;
        this.binding = undefined;
        this.state = 'complete';
      }
      return chunk;
    } catch {
      return this.fail();
    } finally {
      decoded?.fill(0);
    }
  }

  assertComplete(): void {
    // Tool or canned turns need not prepare a model request.
    if (this.state !== 'empty' && this.state !== 'complete') this.fail();
  }
}
