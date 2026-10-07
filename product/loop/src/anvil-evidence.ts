import { api, type TurnRecord } from './evidence';

export function anvilComparisonURL(configuredOrigin: string): URL {
  const origin = new URL(configuredOrigin);
  if (origin.protocol !== 'https:' || origin.pathname !== '/' || origin.search || origin.hash ||
      origin.username || origin.password || /[\s\\]/.test(configuredOrigin))
    throw new Error('Invalid configured Anvil origin.');
  return new URL('/app/experiments', origin);
}

export type AnvilReference = { id: string; kind: 'learning_report' | 'answer_review' | 'experiment' | 'native_evaluation'; reference: string };
type ReferenceList = { references: Array<{ reference: AnvilReference }> };
export type AnvilEvidence =
  | { schemaVersion: 'anvil-learning-evidence/v1'; reportSha256: string; report: unknown; answers: unknown; promotionDecision: null }
  | { schemaVersion: 'anvil-experiment-evidence/v1'; experimentId: string; snapshotSha256: string; experiment: { id: string }; promotionDecision: null }
  | { schemaVersion: 'anvil-native-evidence/v1'; candidateId: string; snapshotSha256: string; admission: { candidateId: string; scope: string }; promotionDecision: null };

export async function readAnvilEvidence(turn: string, reference: AnvilReference, signal?: AbortSignal): Promise<AnvilEvidence> {
  const result = await api<AnvilEvidence>(`/api/turns/${encodeURIComponent(turn)}/anvil-links/${encodeURIComponent(reference.id)}/evidence`, undefined, signal);
  const matches = reference.kind === 'native_evaluation'
    ? result.schemaVersion === 'anvil-native-evidence/v1' && result.candidateId === reference.reference &&
      result.admission?.candidateId === reference.reference && result.admission.scope === 'historical_admission_not_execution_attestation' &&
      /^[a-f0-9]{64}$/.test(result.snapshotSha256)
    : reference.kind === 'experiment'
    ? result.schemaVersion === 'anvil-experiment-evidence/v1' && result.experimentId === reference.reference &&
      result.experiment?.id === reference.reference && /^[a-f0-9]{64}$/.test(result.snapshotSha256)
    : result.schemaVersion === 'anvil-learning-evidence/v1' && result.reportSha256 === reference.reference;
  if (!matches || result.promotionDecision !== null)
    throw new Error('Anvil returned evidence that does not match this reference.');
  return result;
}

/** Keep one immutable definition across failed or ambiguous POST retries. */
export class AnvilLinkSubmission {
  readonly definition: Readonly<AnvilReference>;
  private readonly record: TurnRecord;
  private receipt: string | null = null;
  private pending: Promise<void> | null = null;
  saved = false;
  constructor(record: TurnRecord, kind: AnvilReference['kind'], reference: string) {
    reference = reference.trim();
    if (record.status === 'recording' || !/^[A-Za-z0-9_-]{1,80}$/.test(record.id) ||
        !/^[a-f0-9]{64}$/.test(record.manifest_sha256) || !/^[a-f0-9]{64}$/.test(record.final_sha256 ?? ''))
      throw new Error('Save the finalized recording before linking Anvil evidence.');
    const valid = (kind === 'experiment' || kind === 'native_evaluation') ? /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/.test(reference)
      : (kind === 'learning_report' || kind === 'answer_review') && /^[a-f0-9]{64}$/.test(reference);
    if (!valid) throw new Error('Enter an Anvil report SHA-256 or a version 4 experiment or native evaluation UUID for the selected kind.');
    this.record = structuredClone(record);
    this.definition = Object.freeze({ id: crypto.randomUUID(), kind, reference });
  }
  save(signal?: AbortSignal): Promise<void> {
    if (this.saved) return Promise.resolve();
    if (this.pending) return this.pending;
    this.pending = (async () => {
      const record = this.record;
      if (!this.receipt) {
        const report = await api<{ receiptSha256: string; receipt: { turnId: string; manifestSha256: string; finalSha256: string } }>(`/api/turns/${record.id}/report`, undefined, signal);
        if (!/^[a-f0-9]{64}$/.test(report.receiptSha256) || report.receipt?.turnId !== record.id ||
            report.receipt.manifestSha256 !== record.manifest_sha256 || report.receipt.finalSha256 !== record.final_sha256)
          throw new Error('The checked report does not match the opened recording.');
        this.receipt = report.receiptSha256;
      }
      signal?.throwIfAborted();
      const result = await api<{ stored: boolean; remoteVerification: string }>(`/api/turns/${record.id}/anvil-links`,
        { ...this.definition, receipt_sha256: this.receipt }, signal);
      if (result.stored !== true || result.remoteVerification !== 'not_verified')
        throw new Error('The service did not confirm the reference annotation was saved.');
      this.saved = true;
    })().finally(() => { this.pending = null; });
    return this.pending;
  }
}

// Preserve uncertain submissions when the operator closes and reopens a dialog.
const drafts = new Map<string, AnvilLinkSubmission>();

/** Scope requests and rendered evidence to one open recording dialog. */
export function showAnvilReferences(root: HTMLElement, record: TurnRecord): () => void {
  const turn = record.id;
  const controller = new AbortController();
  const button = document.createElement('button');
  button.type = 'button'; button.textContent = 'load linked Anvil evidence';
  const status = document.createElement('p'); status.setAttribute('role', 'status');
  const list = document.createElement('ul');
  const form = document.createElement('form');
  const fields = document.createElement('fieldset');
  const legend = document.createElement('legend'); legend.textContent = 'Link existing Anvil evidence';
  const kindLabel = document.createElement('label'); kindLabel.textContent = 'Reference kind';
  const kind = document.createElement('select'); kind.id = 'anvil-reference-kind'; kindLabel.htmlFor = kind.id;
  for (const value of ['learning_report', 'answer_review', 'experiment', 'native_evaluation'] as const) {
    const option = document.createElement('option'); option.value = value; option.textContent = value.replaceAll('_', ' '); kind.append(option);
  }

  const referenceLabel = document.createElement('label'); referenceLabel.textContent = 'Anvil reference';
  const reference = document.createElement('input'); reference.required = true; reference.maxLength = 64;
  reference.autocomplete = 'off'; reference.spellcheck = false; referenceLabel.append(reference);
  const help = document.createElement('p');
  help.textContent = 'Copy the report hash, experiment UUID, or native evaluation UUID from Anvil. This saves an immutable annotation, not model provenance or a review verdict.';
  fields.append(legend, kindLabel, kind, referenceLabel, help);
  const save = document.createElement('button'); save.type = 'submit'; save.textContent = 'save Anvil reference';
  const savedStatus = document.createElement('p'); savedStatus.setAttribute('role', 'status');
  const another = document.createElement('button'); another.type = 'button'; another.textContent = 'link another reference';
  const renderDraft = () => {
    const draft = drafts.get(turn);
    fields.disabled = Boolean(draft); save.disabled = Boolean(draft?.saved); another.hidden = !draft?.saved;
    if (draft) { kind.value = draft.definition.kind; reference.value = draft.definition.reference; }
    savedStatus.textContent = draft?.saved ? `Saved annotation ${draft.definition.id}. Read it from Anvil to verify the remote object.`
      : draft ? 'Retry uses the same reference ID and fields.' : '';
  };
  another.addEventListener('click', () => { drafts.delete(turn); form.reset(); renderDraft(); });
  form.addEventListener('submit', (event) => { event.preventDefault(); void (async () => {
    try {
      let draft = drafts.get(turn);
      if (!draft) { draft = new AnvilLinkSubmission(record, kind.value as AnvilReference['kind'], reference.value); drafts.set(turn, draft); }
      fields.disabled = true; save.disabled = true; savedStatus.textContent = 'Saving reference annotation…';
      await draft.save(controller.signal);
      if (!controller.signal.aborted) { renderDraft(); button.click(); }
    } catch (error) {
      if (!controller.signal.aborted) {
        save.disabled = false;
        savedStatus.textContent = `Could not save reference: ${error instanceof Error ? error.message : String(error)}`;
      }
    }
  })(); });
  form.append(fields, save, another, savedStatus); renderDraft();
  const prepared = document.createElement('button'); prepared.type = 'button';
  prepared.textContent = 'prepare captured request for Anvil';
  const preparedStatus = document.createElement('p'); preparedStatus.setAttribute('role', 'status');
  const handoff = document.createElement('a'); handoff.hidden = true;
  handoff.textContent = 'open Anvil comparison'; handoff.target = '_blank'; handoff.rel = 'noopener noreferrer';
  prepared.addEventListener('click', () => { void (async () => {
    prepared.disabled = true; handoff.hidden = true; preparedStatus.textContent = 'Verifying the saved model request…';
    try {
      const binding = await api<{ schemaVersion: string; requestId: string; receiptSha256: string; requestSha256: string; scope: string }>(
        `/api/turns/${encodeURIComponent(turn)}/model-request`, undefined, controller.signal);
      if (binding.schemaVersion !== 'loop-prepared-model-request/v1' || binding.requestId !== turn ||
          binding.scope !== 'prepared_request_not_model_attestation' ||
          !/^[a-f0-9]{64}$/.test(binding.receiptSha256) || !/^[a-f0-9]{64}$/.test(binding.requestSha256))
        throw new Error('Prepared request binding failed.');
      const configuration = await api<{ anvil_origin: string }>('/api/config', undefined, controller.signal);
      const url = anvilComparisonURL(configuration.anvil_origin);
      url.searchParams.set('loopTurn', turn); url.searchParams.set('loopReceipt', binding.receiptSha256);
      url.searchParams.set('loopHash', binding.requestSha256); handoff.href = url.href; handoff.hidden = false;
      preparedStatus.textContent = 'Anvil will verify this recording again. Choose an admitted challenger and consent to storing its complete context there.';
    } catch (error) {
      if (!controller.signal.aborted) preparedStatus.textContent = `Could not prepare the captured request: ${error instanceof Error ? error.message : String(error)}. A saved-input rerun needs fresh model-context recording consent.`;
    } finally { prepared.disabled = false; }
  })(); });
  root.replaceChildren(prepared, preparedStatus, handoff, button, status, list, form);
  button.addEventListener('click', () => { void (async () => {
    button.disabled = true; status.textContent = 'Loading references…'; list.replaceChildren();
    try {
      const result = await api<ReferenceList>(`/api/turns/${encodeURIComponent(turn)}/anvil-links`, undefined, controller.signal);
      if (controller.signal.aborted) return;
      status.textContent = result.references.length ? 'Stored references are annotations. Read a report to check it with Anvil.' : 'No Anvil references are linked to this turn.';
      for (const { reference } of result.references) {
        const item = document.createElement('li');
        const label = document.createElement('p'); label.textContent = `${reference.kind} · ${reference.reference}`;
        const read = document.createElement('button'); read.type = 'button'; read.textContent = 'read from Anvil';
        const notice = document.createElement('p'); notice.setAttribute('role', 'status');
        const output = document.createElement('pre');
        read.addEventListener('click', () => { void (async () => {
          read.disabled = true; output.textContent = ''; notice.textContent = 'Checking with Anvil…';
          try {
            const evidence = await readAnvilEvidence(turn, reference, controller.signal);
            if (controller.signal.aborted) return;
            notice.textContent = evidence.schemaVersion === 'anvil-native-evidence/v1'
              ? 'Historical Anvil admission. This does not prove this voice turn used that model or grant a new execution window.'
              : 'Read from Anvil. This does not establish model provenance or promotion.';
            output.textContent = JSON.stringify(evidence.schemaVersion === 'anvil-native-evidence/v1'
              ? { snapshotSha256: evidence.snapshotSha256, admission: evidence.admission }
              : evidence.schemaVersion === 'anvil-experiment-evidence/v1'
              ? { snapshotSha256: evidence.snapshotSha256, experiment: evidence.experiment }
              : { report: evidence.report, answers: evidence.answers }, null, 2);
          } catch (error) {
            if (!controller.signal.aborted) notice.textContent = `Could not read Anvil evidence: ${error instanceof Error ? error.message : String(error)}`;
          } finally { if (!controller.signal.aborted) read.disabled = false; }
        })(); });
        item.append(label, read, notice, output); list.append(item);
      }
    } catch (error) {
      if (!controller.signal.aborted) status.textContent = `Could not load references: ${error instanceof Error ? error.message : String(error)}`;
    } finally { if (!controller.signal.aborted) button.disabled = false; }
  })(); });
  return () => { controller.abort(); root.replaceChildren(); };
}
