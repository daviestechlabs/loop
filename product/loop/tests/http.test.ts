import { test, expect } from 'bun:test';
import { checkAnvilImport } from './anvil-import';
import { createHash } from 'node:crypto';
import { connect } from 'node:net';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const base = process.env.LOOP_HTTP_TEST_URL;
const httpTest = test.skipIf(!base);
const faultTest = test.skipIf(!base || process.env.LOOP_HTTP_FAULT_TEST !== '1');
httpTest('C HTTP service stores evidence, rejects CSRF, and does not trust browser authority', async () => {
  const login = await fetch(`${base}/api/test/session`, { redirect: 'manual' }); expect(login.status).toBe(302);
  const policy = login.headers.get('content-security-policy') ?? '';
  expect(policy.split(';').find((part) => part.trim().startsWith('connect-src'))?.trim())
    .toBe("connect-src 'self' https://voice-session-gateway.lab.daviestechlabs.io:8443");
  const worklet = await fetch(`${base}/mic-worklet.js`);
  expect(worklet.status).toBe(200);
  expect(worklet.headers.get('content-type')).toContain('text/javascript');
  expect(await worklet.text()).toContain('registerProcessor("dtl-realtime-mic"');
  const cookie = login.headers.get('set-cookie')!.split(';')[0]!;
  const headers = { Cookie: cookie, Origin: base!, 'Content-Type': 'application/json' };
  const id = `http-${crypto.randomUUID()}`;
  const create = await fetch(`${base}/api/turns`, { method: 'POST', headers, body: JSON.stringify({ request_id: id, recording_consent: true, purpose: 'synthetic_http_test' }) });
  expect(create.status).toBe(201);
  const pcm = new Uint8Array([0, 0, 255, 127, 0, 128, 1, 0]);
  expect((await fetch(`${base}/api/turns/${id}/audio/output/24000`, {
    method: 'POST', headers: { ...headers, 'Content-Type': 'application/octet-stream' }, body: pcm,
  })).status).toBe(201);
  expect((await fetch(`${base}/api/turns/${id}/replay/output.wav`, { headers: { Cookie: cookie } })).status).toBe(409);
  expect((await fetch(`${base}/api/turns/${id}/replay/input.s16le`, { headers: { Cookie: cookie } })).status).toBe(409);
  const input = new Uint8Array(640); input[0] = 91;
  expect((await fetch(`${base}/api/turns/${id}/audio/input/16000`, {
    method: 'POST', headers: { ...headers, 'Content-Type': 'application/octet-stream' }, body: input,
  })).status).toBe(201);
  const event = { seq: 0, name: 'fixture_failure', source: 'browser', elapsed_ms: 0, payload: { synthetic: true, detail: '<script>not executable</script>' } };
  expect((await fetch(`${base}/api/turns/${id}/events`, { method: 'POST', headers, body: JSON.stringify(event) })).status).toBe(201);
  const final = { status: 'failed', event_count: 1, transcript: 'Synthetic fixture: test microphone delivery', answer: '', error: 'Synthetic network interruption', recording_error: null };
  expect((await fetch(`${base}/api/turns/${id}/finish`, { method: 'POST', headers, body: JSON.stringify(final) })).status).toBe(200);
  const read = await fetch(`${base}/api/turns/${id}`, { headers: { Cookie: cookie } }); const record = await read.json();
  expect(record.final.error).toBe('Synthetic network interruption'); expect(record.evidence_origin).toBe('browser_observed');
  expect(record.events[0].event.payload.detail).toBe('<script>not executable</script>');
  expect(record.final_sha256).toMatch(/^[0-9a-f]{64}$/);
  const reportResponse = await fetch(`${base}/api/turns/${id}/report`, { headers: { Cookie: cookie } });
  expect(reportResponse.status).toBe(200);
  const report = await reportResponse.json();
  expect(report.receiptSha256).toBe(createHash('sha256').update(JSON.stringify(report.receipt)).digest('hex'));
  expect(report.receipt.finalSha256).toBe(record.final_sha256);
  expect(report.measurements.every((metric: { value: unknown }) => metric.value === null)).toBe(true);
  expect(report.promotionDecision).toBeNull();
  expect(report.responseAudio).toEqual({
    status: 'not_established', unavailableReason: 'output_completion_not_observed',
    evidenceOrigin: 'browser_observed', serverAttested: false, acousticOutputVerified: false,
    recordedBytes: 8, sampleRate: 24000, pcmDurationMs: 0.167,
    browserReceivedBytes: null, browserReceivedPackets: null, browserReceivedSampleRate: null,
  });
  // Use Anvil's actual schema. Resolve its test dependency from this package.
  const contract = await Bun.build({
    entrypoints: [join(import.meta.dir, '../../anvil/src/lib/anvil/learning-contract.ts')], target: 'bun',
    plugins: [{ name: 'anvil-contract-test-dependencies', setup(build) {
      build.onResolve({ filter: /^zod$/ }, () => ({ path: require.resolve('zod') }));
    } }],
  });
  expect(contract.success).toBe(true);
  const contractDir = mkdtempSync(join(tmpdir(), 'loop-anvil-contract-'));
  let learningReportSchema;
  try {
    const file = join(contractDir, 'contract.mjs'); await Bun.write(file, contract.outputs[0]!);
    ({ learningReportSchema } = await import(file));
  } finally { rmSync(contractDir, { recursive: true, force: true }); }
  const binding = { run_id: '4b196128-9dc2-4d65-8bc5-8c839875afe1', sequence: 1,
    source_revision: 'a'.repeat(40), receipt_sha256: report.receiptSha256 };
  const exported = await fetch(`${base}/api/turns/${id}/anvil-report`, { method: 'POST', headers, body: JSON.stringify(binding) });
  expect(exported.status).toBe(200);
  const anvilReport = learningReportSchema.parse(await exported.json());
  expect(anvilReport.status).toBe('held'); expect(anvilReport.scope).toBe('development');
  expect(anvilReport.metrics).toEqual([]); expect(anvilReport.progress).toBeNull();
  const reportBytes = await (await fetch(`${base}/api/turns/${id}/report`, { headers: { Cookie: cookie } })).arrayBuffer();
  expect(anvilReport.evidence).toEqual([
    { name: `loop-${id}-receipt.json`, sha256: report.receiptSha256 },
    { name: `loop-${id}-report.json`, sha256: createHash('sha256').update(new Uint8Array(reportBytes)).digest('hex') },
  ]);
  for (const invalid of [{ ...binding, sequence: 0 }, { ...binding, run_id: 'not-a-uuid' },
    { ...binding, source_revision: 'unverified' }, { ...binding, status: 'succeeded' }]) {
    expect((await fetch(`${base}/api/turns/${id}/anvil-report`, { method: 'POST', headers, body: JSON.stringify(invalid) })).status).toBe(400);
  }

  const archiveResponse = await fetch(`${base}/api/turns/${id}/anvil-bundle`, { method: 'POST', headers, body: JSON.stringify(binding) });
  expect(archiveResponse.status).toBe(200); expect(archiveResponse.headers.get('content-type')).toBe('application/x-tar');
  const archive = new Uint8Array(await archiveResponse.arrayBuffer());
  expect(createHash('sha256').update(archive).digest('hex')).toBe(archiveResponse.headers.get('x-content-sha256'));
  // An independent archive reader checks checksums and reads members in memory only.
  const unpack = Bun.spawnSync(['python3', '-c', `
import hashlib, io, json, sys, tarfile
with tarfile.open(fileobj=io.BytesIO(sys.stdin.buffer.read()), mode='r:') as archive:
    members = archive.getmembers()
    assert len(members) == 3
    result = {}
    for member in members:
        assert member.isfile() and '/' not in member.name and member.mode == 0o600 and member.mtime == 0
        body = archive.extractfile(member).read()
        result[member.name] = {'sha256': hashlib.sha256(body).hexdigest(), 'body': body.decode('utf-8')}
    print(json.dumps(result))
`], { stdin: archive });
  expect(unpack.exitCode).toBe(0);
  const members = JSON.parse(unpack.stdout.toString());
  const bundledReport = learningReportSchema.parse(JSON.parse(members[`loop-${id}-anvil.json`].body));
  expect(bundledReport).toEqual(anvilReport);
  for (const evidence of bundledReport.evidence) expect(members[evidence.name].sha256).toBe(evidence.sha256);
  expect(members[`loop-${id}-report.json`].body).toBe(new TextDecoder().decode(reportBytes));
  const receiptText = await (await fetch(`${base}/api/turns/${id}/receipt`, { headers: { Cookie: cookie } })).text();
  expect(members[`loop-${id}-receipt.json`].body).toBe(receiptText);
  await checkAnvilImport(bundledReport);
  const capture = { id: `case-${id}`, source_turn: id, title: 'Synthetic transport failure', category: 'transport', expected: 'Complete the response' };
  expect((await fetch(`${base}/api/captures`, { method: 'POST', headers, body: JSON.stringify(capture) })).status).toBe(201);
  expect((await fetch(`${base}/api/captures`, { method: 'POST', headers, body: JSON.stringify(capture) })).status).toBe(200);
  expect((await fetch(`${base}/api/captures`, { method: 'POST', headers, body: JSON.stringify({ ...capture, expected: 'Changed expectation' }) })).status).toBe(409);
  const captures = await (await fetch(`${base}/api/captures`, { headers: { Cookie: cookie } })).json();
  expect(captures.captures.find((entry: { definition: { id: string } }) => entry.definition.id === capture.id).definition).toEqual(capture);
  expect(report.inputDelivery).toEqual({
    status: 'not_established', unavailableReason: 'input_end_not_observed',
    evidenceOrigin: 'browser_observed', serverAttested: false,
    recordedBytes: 640, sampleRate: 16000,
    browserSentBytes: null, browserSentPackets: null,
    gatewayReportedBytes: null, gatewayReportedPackets: null, gatewayReportedLostDatagrams: null,
  });
  const reference = { id: 'experiment-link', kind: 'experiment', reference: crypto.randomUUID(), receipt_sha256: report.receiptSha256 };
  expect((await fetch(`${base}/api/turns/${id}/anvil-links`, { method: 'POST', headers, body: JSON.stringify(reference) })).status).toBe(201);
  expect((await fetch(`${base}/api/turns/${id}/anvil-links`, { method: 'POST', headers, body: JSON.stringify(reference) })).status).toBe(200);
  expect((await fetch(`${base}/api/turns/${id}/anvil-links`, { method: 'POST', headers, body: JSON.stringify({ ...reference, verified: true }) })).status).toBe(400);
  expect((await fetch(`${base}/api/turns/${id}/anvil-links`, { method: 'POST', headers: { ...headers, Origin: 'https://attacker.test' }, body: JSON.stringify(reference) })).status).toBe(403);
  const links = await (await fetch(`${base}/api/turns/${id}/anvil-links`, { headers: { Cookie: cookie } })).json();
  expect(links.origin).toBe('operator_annotation');
  expect(links.remoteVerification).toBe('not_verified');
  expect(links.promotionDecision).toBeNull();
  expect(links.references[0].reference).toEqual(reference);
  expect(links.references[0].sha256).toBe(createHash('sha256').update(JSON.stringify(reference)).digest('hex'));
  const receiptResponse = await fetch(`${base}/api/turns/${id}/receipt`, { headers: { Cookie: cookie } });
  expect(receiptResponse.status).toBe(200);
  const receiptBytes = await receiptResponse.arrayBuffer();
  expect(createHash('sha256').update(new Uint8Array(receiptBytes)).digest('hex')).toBe(report.anvilEvidence.sha256);
  expect(report.anvilEvidence).toEqual({ name: `loop-${id}-receipt.json`, sha256: report.receiptSha256 });
  expect(receiptResponse.headers.get('content-disposition')).toContain(report.anvilEvidence.name);
  expect((await fetch(`${base}/api/turns/${id}/receipt`)).status).toBe(401);
  const replayResponse = await fetch(`${base}/api/turns/${id}/replay/output.wav`, { headers: { Cookie: cookie } });
  expect(replayResponse.status).toBe(200);
  expect(replayResponse.headers.get('content-type')).toBe('audio/wav');
  const wavBytes = await replayResponse.arrayBuffer();
  const wav = new DataView(wavBytes);
  expect(wav.getUint32(24, true)).toBe(24000);
  expect(wav.getUint32(40, true)).toBe(pcm.byteLength);
  expect(new Uint8Array(wavBytes.slice(44))).toEqual(pcm);
  expect(createHash('sha256').update(new Uint8Array(wavBytes)).digest('hex')).toBe(replayResponse.headers.get('x-content-sha256'));
  expect(replayResponse.headers.get('x-pcm-sha256')).toBe(record.audio.find((audio: { kind: string }) => audio.kind === 'output').sha256);
  expect(replayResponse.headers.get('x-loop-receipt-sha256')).toBe(report.receiptSha256);
  expect((await fetch(`${base}/api/turns/${id}/replay/input.wav`, { headers: { Cookie: cookie } })).status).toBe(200);
  expect((await fetch(`${base}/api/turns/${id}/replay/output.wav`)).status).toBe(401);
  for (const [kind, bytes, rate] of [['input', input, 16000], ['output', pcm, 24000]] as const) {
    const raw = await fetch(`${base}/api/turns/${id}/replay/${kind}.s16le`, { headers: { Cookie: cookie } });
    expect(raw.status).toBe(200);
    const exported = new Uint8Array(await raw.arrayBuffer());
    expect(exported).toEqual(bytes);
    expect(raw.headers.get('content-type')).toBe('application/octet-stream');
    expect(raw.headers.get('content-disposition')).toContain(`loop-${id}-${kind}.s16le`);
    expect(raw.headers.get('x-pcm-sample-rate')).toBe(String(rate));
    expect(raw.headers.get('x-pcm-encoding')).toBe('pcm_s16le');
    expect(raw.headers.get('x-pcm-channels')).toBe('1');
    expect(raw.headers.get('x-content-sha256')).toBe(createHash('sha256').update(exported).digest('hex'));
    expect(raw.headers.get('x-content-sha256')).toBe(raw.headers.get('x-pcm-sha256'));
    expect(raw.headers.get('x-loop-receipt-sha256')).toBe(report.receiptSha256);
    if (kind === 'input') verifyBenchmarkInput(exported, raw.headers.get('x-pcm-sha256')!);
  }
  expect((await fetch(`${base}/api/turns/${id}/replay/input.s16le`)).status).toBe(401);
  const rerun = { request_id: `${id}-rerun`, mode: 'full_system_active_model' };
  const rerunResponse = await fetch(`${base}/api/turns/${id}/reruns`, { method: 'POST', headers, body: JSON.stringify(rerun) });
  expect(rerunResponse.status).toBe(201);
  const plan = await rerunResponse.json();
  expect(plan.execution).toBe('pending_browser_transport');
  expect(plan.manifest.rerun.source_receipt_sha256).toBe(report.receiptSha256);
  expect(plan.manifest.rerun.model_revision).toBeNull();
  expect(plan.source_audio.sha256).toBe(createHash('sha256').update(input).digest('hex'));
  const child = await (await fetch(`${base}/api/turns/${rerun.request_id}`, { headers: { Cookie: cookie } })).json();
  expect(child.status).toBe('recording'); expect(child.purpose).toBe('saved_input_rerun');
  const finishRerun = () => fetch(`${base}/api/turns/${rerun.request_id}/finish`, {
    method: 'POST', headers, body: JSON.stringify({ status: 'completed', event_count: 0 }),
  });
  expect((await finishRerun()).status).toBe(409);
  const altered = input.slice(); altered[0] ^= 1;
  const uploadRerun = (bytes: Uint8Array) => fetch(`${base}/api/turns/${rerun.request_id}/audio/input/16000`, {
    method: 'POST', headers: { ...headers, 'Content-Type': 'application/octet-stream' }, body: new Blob([bytes]),
  });
  expect((await uploadRerun(altered)).status).toBe(409);
  expect((await uploadRerun(input)).status).toBe(201);
  expect((await finishRerun()).status).toBe(200);
  const replayed = await fetch(`${base}/api/turns/${rerun.request_id}/replay/input.s16le`, { headers: { Cookie: cookie } });
  expect(replayed.status).toBe(200); expect(new Uint8Array(await replayed.arrayBuffer())).toEqual(input);

  expect(child.events).toEqual([]); expect(child.audio).toEqual([]);
  expect((await fetch(`${base}/api/turns/${id}/reruns`, { method: 'POST', headers: { ...headers, Origin: 'https://attacker.test' }, body: JSON.stringify(rerun) })).status).toBe(403);
  const bad = await fetch(`${base}/api/turns`, { method: 'POST', headers: { ...headers, Origin: 'https://attacker.test' }, body: '{}' }); expect(bad.status).toBe(403);
  expect((await fetch(`${base}/api/turns`, { headers: { 'X-User': 'fixture-operator' } })).status).toBe(401);
  expect((await fetch(`${base}/api/turn-identity`, { method: 'POST', headers, body: JSON.stringify({ request_id: id }) })).status).toBe(503);
  const logout = await fetch(`${base}/api/logout`, { method: 'POST', headers, body: '{}' }); expect(logout.status).toBe(200);
  expect((await fetch(`${base}/api/turns`, { headers: { Cookie: cookie } })).status).toBe(401);
}, 30000);

function verifyBenchmarkInput(pcm: Uint8Array, sha256: string): void {
  const directory = mkdtempSync(join(tmpdir(), 'loop-benchmark-contract-'));
  try {
    writeFileSync(join(directory, 'input.s16le'), pcm);
    const manifest = join(directory, 'capture.json');
    writeFileSync(manifest, JSON.stringify({
      schema_version: 'r-sim-4-captured-pcm/v1', fixture_id: 'loop-http-synthetic',
      profile: 'synthetic_test_fixture', captured_at: '2026-09-20T00:00:00Z',
      capture_source: 'loop-http-test', encoding: 'pcm_s16le', sample_rate_hz: 16000,
      channels: 1, bits_per_sample: 16, frame_duration_ms: 20, pcm_path: 'input.s16le',
      pcm_sha256: sha256, assistant_speaking: false, cancel_frame: null,
      approval: { status: 'synthetic' },
    }));
    const validator = new URL('../../../voice/voice-session-gateway/reflex-c/captured_pcm_replay.py', import.meta.url).pathname;
    // Exercise the existing suite's validator; do not execute a model or approve a capture.
    const result = Bun.spawnSync(['python3', '-B', '-c', `
import importlib.util, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location('capture_validator', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
manifest = Path(sys.argv[2])
normalized, pcm = module.validate_manifest(manifest, require_real_approved=False)
assert normalized['pcm_sha256'] == sys.argv[3]
try:
    module.validate_manifest(manifest, require_real_approved=True)
except module.ManifestError:
    pass
else:
    raise AssertionError('synthetic Loop evidence passed the human gate')
pcm.write_bytes(bytes([pcm.read_bytes()[0] ^ 1]) + pcm.read_bytes()[1:])
try:
    module.validate_manifest(manifest, require_real_approved=False)
except module.ManifestError:
    pass
else:
    raise AssertionError('modified PCM passed the integrity gate')
`, validator, manifest, sha256]);
    expect(result.stderr.toString()).toBe('');
    expect(result.exitCode).toBe(0);
  } finally { rmSync(directory, { recursive: true, force: true }); }
}
httpTest('C HTTP boundary rejects ambiguous request framing and unsupported transfer coding', async () => {
  async function request(raw: string): Promise<string> {
    const url = new URL(base!);
    return new Promise((resolve, reject) => {
      let result = ''; const socket = connect(Number(url.port), url.hostname, () => socket.write(raw));
      socket.setTimeout(3000, () => { socket.destroy(); reject(new Error('HTTP parser timed out')); });
      socket.on('data', (chunk) => { result += chunk.toString(); }); socket.on('end', () => resolve(result)); socket.on('error', reject);
    });
  }
  for (const header of ['Content-Length: 2\r\nContent-Length: 2', 'Content-Length: 2\r\nTransfer-Encoding: chunked', 'Content-Length: +2', 'Content-Length: 2\r\nOrigin: a\r\nOrigin: b', 'Content-Length: 2\r\nX-Envoy-Oidc-Id-Token: a\r\nx-envoy-oidc-id-token: b', 'Content-Length: 2\r\nX-Envoy-Oidc-Id-Token: ']) {
    const result = await request(`POST /api/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n${header}\r\n\r\n{}`);
    expect(result.startsWith('HTTP/1.1 400')).toBe(true);
  }
});

faultTest('slow headers and bodies cannot retain all HTTP workers by dribbling bytes', async () => {
  const url = new URL(base!);
  if (!['127.0.0.1', 'localhost'].includes(url.hostname)) throw new Error('Fault tests require the local fixture');
  const sockets: ReturnType<typeof connect>[] = [];
  try {
    const results = await Promise.all(Array.from({ length: 4 }, (_, index) => new Promise<number>((resolve, reject) => {
      const started = performance.now();
      const socket = connect(Number(url.port), url.hostname, () => {
        socket.write(index % 2 === 0
          ? 'GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Slow: '
          : 'POST /api/turns HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4096\r\nContent-Type: application/json\r\n\r\n{');
      });
      sockets.push(socket);
      const drip = setInterval(() => { if (!socket.destroyed) socket.write('x'); }, 50);
      const limit = setTimeout(() => { reject(new Error('Slow request outlived the fixture deadline')); socket.destroy(); }, 2500);
      socket.on('data', () => {});
      socket.on('error', (error: NodeJS.ErrnoException) => { if (error.code !== 'ECONNRESET' && error.code !== 'EPIPE') reject(error); });
      socket.on('close', () => { clearInterval(drip); clearTimeout(limit); resolve(performance.now() - started); });
    })));
    for (const elapsed of results) { expect(elapsed).toBeGreaterThan(500); expect(elapsed).toBeLessThan(2500); }
    const recovered = await fetch(`${base}/healthz`, { signal: AbortSignal.timeout(1500) });
    expect(recovered.status).toBe(200);
  } finally { for (const socket of sockets) socket.destroy(); }
});
