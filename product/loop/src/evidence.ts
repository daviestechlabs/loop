import type { Observation } from './voice';
export type RecordedEvent = Observation & { seq: number; elapsed_ms: number };
export type FinalRecord = {
  status: 'completed' | 'failed' | 'interrupted'; event_count: number;
  transcript: string; answer: string; error: string | null;
  recording_error: string | null;
};
export type TurnSummary = {
  purpose?: string | null;
  id: string; status: 'recording' | FinalRecord['status']; created: number;
  manifest_sha256: string; final: FinalRecord | null; final_sha256: string | null;
};
export type TurnRecord = TurnSummary & {
  evidence_origin: 'browser_observed'; manifest: Record<string, unknown>;
  events: Array<{ event: RecordedEvent; sha256: string; received: number }>;
  audio: Array<{ kind: 'input' | 'output'; sample_rate: number; bytes: number; sha256: string }>;
};
// These are browser-captured gateway observations, never independent server attestations.
export function gatewayMetadata(events: TurnRecord['events']): Record<string, unknown> {
  const result: Record<string, unknown> = Object.create(null);
  const conflicts = new Set<string>();
  for (const { event } of events) {
    if (event.source !== 'gateway_observed') continue;
    const metadata = event.payload.metadata;
    if (!metadata || typeof metadata !== 'object' || Array.isArray(metadata)) continue;
    for (const key of ['stt_ms', 'planner_ms', 'tool_ms', 'llm_ms', 'tts_ms', 'model_id', 'model_identity_source', 'prompt_tokens', 'completion_tokens', 'total_tokens', 'usage_source']) {
      if (!Object.hasOwn(metadata, key)) continue;
      const value = (metadata as Record<string, unknown>)[key];
      if (Object.hasOwn(result, key) && result[key] !== value) conflicts.add(key);
      result[key] = value;
    }
  }
  for (const key of conflicts) result[key] = null;
  return result;
}
export function gatewayTokenUsage(events: TurnRecord['events']): { input: number; output: number; total: number } | null {
  const meta = gatewayMetadata(events);
  if (meta.usage_source !== 'provider_reported') return null;
  const counts = ['prompt_tokens', 'completion_tokens', 'total_tokens'].map((key) => {
    const value = meta[key];
    if (typeof value !== 'number' && !(typeof value === 'string' && /^(?:0|[1-9][0-9]*)$/.test(value))) return null;
    const n = Number(value);
    return Number.isSafeInteger(n) && n >= 0 ? n : null;
  });
  const [input, output, total] = counts;
  if (input == null || output == null || total == null || input + output !== total) return null;
  return { input, output, total };
}
export function reportedMilliseconds(value: unknown): number | null {
  if (typeof value !== 'number' && !(typeof value === 'string' && /^(?:0|[1-9][0-9]*)(?:\.[0-9]+)?$/.test(value))) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
}
// These timestamps describe the STT service's receipt-to-transcript interval.
// Do not combine endpoints from separate events or sum repeated TTS segments.
export function gatewaySTTInterval(events: TurnRecord['events']): number | null {
  const startKey = 'stage_stt_request_received_at_ms';
  const endKey = 'stage_stt_transcript_published_at_ms';
  let interval: { start: number; end: number } | null = null;
  for (const { event } of events) {
    if (event.source !== 'gateway_observed') continue;
    const metadata = event.payload.metadata;
    if (!metadata || typeof metadata !== 'object' || Array.isArray(metadata)) continue;
    const fields = metadata as Record<string, unknown>;
    if (!Object.hasOwn(fields, startKey) && !Object.hasOwn(fields, endKey)) continue;
    const start = reportedMilliseconds(fields[startKey]);
    const end = reportedMilliseconds(fields[endKey]);
    if (start === null || end === null || !Number.isSafeInteger(start) || !Number.isSafeInteger(end) ||
        start <= 0 || end < start || (interval && (interval.start !== start || interval.end !== end))) return null;
    interval = { start, end };
  }
  return interval ? interval.end - interval.start : null;
}
export type FailureCapture = { id: string; source_turn: string; title: string; category: string; expected: string };
export class CaptureSubmission {
  readonly definition: Readonly<FailureCapture>;
  saved = false;
  private pending: Promise<void> | null = null;
  constructor(record: TurnRecord, fields: Pick<FailureCapture, 'title' | 'category' | 'expected'>) {
    if (record.status === 'recording' || !/^[0-9a-f]{64}$/.test(record.manifest_sha256) || !/^[0-9a-f]{64}$/.test(record.final_sha256 ?? ''))
      throw new Error('Save the finalized recording before capturing a failure case.');
    const title = fields.title.trim(), category = fields.category.trim(), expected = fields.expected.trim();
    if (!title || !category || !expected) throw new Error('Enter a title, category, and expected behavior.');
    const encoder = new TextEncoder();
    if (encoder.encode(title).length > 160 || encoder.encode(category).length > 80 || encoder.encode(expected).length > 8192)
      throw new Error('Shorten the title, category, or expected behavior to fit the case limits.');
    this.definition = Object.freeze({ id: crypto.randomUUID(), source_turn: record.id, title, category, expected });
  }
  save(): Promise<void> {
    if (this.saved) return Promise.resolve();
    if (this.pending) return this.pending;
    this.pending = (async () => {
      const response = await api<{ stored: boolean }>('/api/captures', this.definition);
      if (response.stored !== true) throw new Error('The service did not confirm the failure case was saved.');
      this.saved = true;
    })().finally(() => { this.pending = null; });
    return this.pending;
  }
}
export class APIError extends Error { constructor(public status: number, message: string) { super(message); } }
export async function api<T>(path: string, body?: unknown, signal?: AbortSignal): Promise<T> {
  const timeout = AbortSignal.timeout(15000);
  const options: RequestInit = {
    method: body === undefined ? 'GET' : 'POST', credentials: 'same-origin',
    signal: signal ? AbortSignal.any([signal, timeout]) : timeout,
  };
  if (body !== undefined) { options.body = JSON.stringify(body); options.headers = { 'Content-Type': 'application/json' }; }
  const response = await fetch(path, options);
  const payload: unknown = await response.json();
  if (!response.ok) throw new APIError(response.status, typeof payload === 'object' && payload && 'error' in payload ? String(payload.error) : `HTTP ${response.status}`);
  return payload as T;
}
export type AnvilExportBinding = { runId: string; sourceRevision: string; sequence: number };
export async function anvilBundle(record: TurnRecord, binding: AnvilExportBinding, signal?: AbortSignal): Promise<Blob> {
  const { id, status, manifest_sha256: manifest, final_sha256: final } = record;
  const request = { run_id: binding.runId, source_revision: binding.sourceRevision, sequence: binding.sequence };
  if (status === 'recording' || !/^[A-Za-z0-9_-]{1,80}$/.test(id) ||
      !/^[a-f0-9]{64}$/.test(manifest) || !/^[a-f0-9]{64}$/.test(final ?? ''))
    throw new Error('Save the finalized recording before exporting for Anvil.');
  if (!/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/.test(request.run_id) ||
      !/^[a-f0-9]{40}$/.test(request.source_revision) || !Number.isInteger(request.sequence) || request.sequence < 1 || request.sequence > 1000000)
    throw new Error('Enter a version 4 run UUID, full source commit, and valid report sequence.');
  const report = await api<{ receiptSha256: string; receipt: { turnId: string; manifestSha256: string; finalSha256: string } }>(`/api/turns/${id}/report`, undefined, signal);
  if (!/^[a-f0-9]{64}$/.test(report.receiptSha256) || report.receipt?.turnId !== id ||
      report.receipt.manifestSha256 !== manifest || report.receipt.finalSha256 !== final)
    throw new Error('The checked report does not match the opened recording.');
  const response = await fetch(`/api/turns/${id}/anvil-bundle`, {
    method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...request, receipt_sha256: report.receiptSha256 }),
    signal: signal ? AbortSignal.any([signal, AbortSignal.timeout(15000)]) : AbortSignal.timeout(15000),
  });
  if (!response.ok) throw new APIError(response.status, `Anvil export failed (${response.status}).`);
  const size = Number(response.headers.get('content-length'));
  const expected = response.headers.get('x-content-sha256') ?? '';
  if (response.headers.get('content-type') !== 'application/x-tar' || !/^[a-f0-9]{64}$/.test(expected) ||
      !Number.isSafeInteger(size) || size < 2560 || size > 6 * 1024 * 1024 || size % 512)
    throw new Error('Invalid evidence bundle response.');
  const bytes = await response.arrayBuffer();
  const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), (v) => v.toString(16).padStart(2, '0')).join('');
  if (bytes.byteLength !== size || digest !== expected) throw new Error('Evidence bundle digest mismatch.');
  signal?.throwIfAborted();
  return new Blob([bytes], { type: 'application/x-tar' });
}
export class Recorder {
  readonly events: RecordedEvent[] = [];
  private started = performance.now();
  private acknowledged = 0;
  private flushing: Promise<void> | null = null;
  private audio = new Map<'input' | 'output', { chunks: Uint8Array<ArrayBuffer>[]; bytes: number; rate: number }>();
  constructor(readonly id: string) {}
  record(observation: Observation): void {
    if (this.events.length >= 2048) throw new Error('Recording event capacity exceeded');
    this.events.push({ ...observation, seq: this.events.length, elapsed_ms: Math.round((performance.now() - this.started) * 1000) / 1000 });
  }
  addAudio(kind: 'input' | 'output', pcm: Uint8Array, rate: number): void {
    const data = this.audio.get(kind) ?? { chunks: [], bytes: 0, rate };
    if (data.rate !== rate || !Number.isInteger(rate) || rate < 8000 || rate > 48000) throw new Error('Recording audio format changed');
    if (data.bytes + pcm.byteLength > 4 * 1024 * 1024) throw new Error('Recording audio capacity exceeded');
    data.chunks.push(new Uint8Array(pcm)); data.bytes += pcm.byteLength; this.audio.set(kind, data);
  }
  flush(): Promise<void> {
    if (this.flushing) return this.flushing;
    this.flushing = (async () => {
      while (this.acknowledged < this.events.length) {
        await api(`/api/turns/${this.id}/events`, this.events[this.acknowledged]);
        this.acknowledged++;
      }
    })().finally(() => { this.flushing = null; });
    return this.flushing;
  }
  async finish(final: FinalRecord): Promise<void> {
    await this.flush();
    for (const [kind, data] of this.audio) {
      if (!data.bytes) continue;
      const response = await fetch(`/api/turns/${this.id}/audio/${kind}/${data.rate}`, {
        method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/octet-stream' },
        body: new Blob(data.chunks), signal: AbortSignal.timeout(30000),
      });
      if (!response.ok) throw new APIError(response.status, `Could not save ${kind} audio (${response.status})`);
    }
    await api(`/api/turns/${this.id}/finish`, final);
  }
}
