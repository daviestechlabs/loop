import { test, expect, afterEach } from 'bun:test';
import { anvilComparisonURL, AnvilLinkSubmission, readAnvilEvidence, type AnvilReference } from '../src/anvil-evidence';
test('Anvil handoff uses the configured HTTPS authority without accepting redirect paths', () => {
  expect(anvilComparisonURL('https://anvil.example:9443').href).toBe('https://anvil.example:9443/app/experiments');
  expect(anvilComparisonURL('https://anvil.example:443').href).toBe('https://anvil.example/app/experiments');
  for (const value of ['http://anvil.example', 'https://user@anvil.example', 'https://anvil.example/other',
    'https://anvil.example?redirect=other', 'https://anvil.example#fragment', 'https://anvil.example\n'])
    expect(() => anvilComparisonURL(value)).toThrow();
});
const originalFetch = globalThis.fetch;
afterEach(() => { globalThis.fetch = originalFetch; });
const reference: AnvilReference = { id: 'report-link', kind: 'learning_report', reference: 'a'.repeat(64) };
const evidence = { schemaVersion: 'anvil-learning-evidence/v1', reportSha256: reference.reference, report: {}, answers: null, promotionDecision: null };
test('Anvil reads use the owned C route and same-origin session', async () => {
  globalThis.fetch = (async (url: string, options: RequestInit) => {
    expect(url).toBe('/api/turns/turn%2Fname/anvil-links/report-link/evidence');
    expect(options.method).toBe('GET'); expect(options.credentials).toBe('same-origin');
    return Response.json(evidence);
  }) as typeof fetch;
  expect(await readAnvilEvidence('turn/name', reference)).toEqual(evidence);
});
test('Anvil read rejects wrong bindings and promotion claims', async () => {
  for (const invalid of [{ ...evidence, reportSha256: 'b'.repeat(64) }, { ...evidence, schemaVersion: 'other' }, { ...evidence, promotionDecision: true }]) {
    globalThis.fetch = (async () => Response.json(invalid)) as typeof fetch;
    await expect(readAnvilEvidence('turn', reference)).rejects.toThrow('does not match');
  }
});
test('Anvil read preserves server failures for all reference kinds', async () => {
  let calls = 0;
  globalThis.fetch = (async () => { calls++; return Response.json({ error: 'anvil_evidence_unverified' }, { status: 502 }); }) as typeof fetch;
  await expect(readAnvilEvidence('turn', reference)).rejects.toThrow('anvil_evidence_unverified');
  await expect(readAnvilEvidence('turn', { ...reference, kind: 'experiment' })).rejects.toThrow('anvil_evidence_unverified');
  expect(calls).toBe(2);
});

const record = { id: 'turn', status: 'completed', manifest_sha256: 'b'.repeat(64), final_sha256: 'c'.repeat(64) } as import('../src/evidence').TurnRecord;
const report = { receiptSha256: 'd'.repeat(64), receipt: { turnId: record.id, manifestSha256: record.manifest_sha256, finalSha256: record.final_sha256 } };
test('reference submission binds the checked receipt and retries ambiguous writes unchanged', async () => {
  const submission = new AnvilLinkSubmission(record, 'learning_report', reference.reference);
  const posts: unknown[] = []; let reads = 0;
  globalThis.fetch = (async (url: string, options: RequestInit) => {
    expect(options.credentials).toBe('same-origin');
    if (url.endsWith('/report')) { reads++; return Response.json(report); }
    expect(url).toBe('/api/turns/turn/anvil-links');
    posts.push(JSON.parse(String(options.body)));
    if (posts.length === 1) throw new Error('Response lost after commit');
    return Response.json({ stored: true, remoteVerification: 'not_verified' });
  }) as typeof fetch;
  const first = submission.save(); expect(submission.save()).toBe(first);
  await expect(first).rejects.toThrow('Response lost');
  await submission.save(); await submission.save();
  expect(reads).toBe(1); expect(posts).toHaveLength(2); expect(posts[0]).toEqual(posts[1]);
  expect(posts[1]).toEqual({ ...submission.definition, receipt_sha256: report.receiptSha256 });
  expect(submission.saved).toBe(true);
});
test('reference submission rejects mismatched checked recordings before writing', async () => {
  for (const invalid of [{ ...report, receiptSha256: 'bad' }, { ...report, receipt: { ...report.receipt, turnId: 'other' } },
    { ...report, receipt: { ...report.receipt, finalSha256: 'e'.repeat(64) } }, { ...report, receipt: { ...report.receipt, manifestSha256: 'e'.repeat(64) } }]) {
    let calls = 0;
    globalThis.fetch = (async () => { calls++; return Response.json(invalid); }) as typeof fetch;
    await expect(new AnvilLinkSubmission(record, 'answer_review', reference.reference).save()).rejects.toThrow('does not match');
    expect(calls).toBe(1);
  }
});
test('reference submission validates input and refuses unconfirmed storage', async () => {
  expect(() => new AnvilLinkSubmission({ ...record, status: 'recording' }, 'learning_report', reference.reference)).toThrow('finalized');
  expect(() => new AnvilLinkSubmission(record, 'experiment', reference.reference)).toThrow('UUID');
  expect(() => new AnvilLinkSubmission(record, 'learning_report', 'https://anvil.example/report')).toThrow('SHA-256');
  globalThis.fetch = (async (url: string) => Response.json(url.endsWith('/report') ? report : { stored: false })) as typeof fetch;
  const submission = new AnvilLinkSubmission(record, 'learning_report', reference.reference);
  await expect(submission.save()).rejects.toThrow('did not confirm'); expect(submission.saved).toBe(false);
});

test('experiment reads require the referenced object and a snapshot hash without promotion', async () => {
  const reference: AnvilReference = { id: 'experiment-link', kind: 'experiment', reference: crypto.randomUUID() };
  const result = { schemaVersion: 'anvil-experiment-evidence/v1', experimentId: reference.reference,
    experiment: { id: reference.reference }, snapshotSha256: 'b'.repeat(64), promotionDecision: null };
  globalThis.fetch = (async () => Response.json(result)) as typeof fetch;
  expect(await readAnvilEvidence('turn', reference)).toEqual(result);
  for (const invalid of [{ ...result, experimentId: crypto.randomUUID() }, { ...result, experiment: { id: crypto.randomUUID() } },
    { ...result, snapshotSha256: 'bad' }, { ...result, promotionDecision: true }, evidence]) {
    globalThis.fetch = (async () => Response.json(invalid)) as typeof fetch;
    await expect(readAnvilEvidence('turn', reference)).rejects.toThrow('does not match');
  }
});

test('native admission remains an owned historical snapshot, never an execution claim', async () => {
  const reference: AnvilReference = { id: 'native-link', kind: 'native_evaluation', reference: crypto.randomUUID() };
  const result = { schemaVersion: 'anvil-native-evidence/v1', candidateId: reference.reference,
    admission: { candidateId: reference.reference, scope: 'historical_admission_not_execution_attestation' },
    snapshotSha256: 'b'.repeat(64), promotionDecision: null };
  const draft = new AnvilLinkSubmission(record, reference.kind, reference.reference);
  expect(draft.definition.kind).toBe('native_evaluation');
  globalThis.fetch = (async () => Response.json(result)) as typeof fetch;
  expect(await readAnvilEvidence('turn', reference)).toEqual(result);
  for (const invalid of [{ ...result, candidateId: crypto.randomUUID() },
    { ...result, admission: { ...result.admission, candidateId: crypto.randomUUID() } },
    { ...result, admission: { ...result.admission, scope: 'execution_verified' } },
    { ...result, snapshotSha256: 'bad' }, { ...result, promotionDecision: true }, evidence]) {
    globalThis.fetch = (async () => Response.json(invalid)) as typeof fetch;
    await expect(readAnvilEvidence('turn', reference)).rejects.toThrow('does not match');
  }
});
