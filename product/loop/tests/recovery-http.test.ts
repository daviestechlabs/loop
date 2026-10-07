import { expect, test } from 'bun:test';
import { createServer } from 'node:net';
import { copyFileSync, mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const enabled = process.env.LOOP_RECOVERY_HTTP_TEST === '1';
const root = resolve(import.meta.dir, '..');

async function unusedPort(): Promise<number> {
  const socket = createServer();
  await new Promise<void>((resolve, reject) => {
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', resolve);
  });
  const address = socket.address();
  if (!address || typeof address === 'string') throw new Error('No fixture port');
  await new Promise<void>((resolve, reject) => socket.close((error) => error ? reject(error) : resolve()));
  return address.port;
}

test.skipIf(!enabled)('C HTTP recovery preserves receipts, PCM, snapshot boundaries, and session admission', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'loop-http-recovery-'));
  const children: ReturnType<typeof Bun.spawn>[] = [];
  async function start(database: string) {
    const port = await unusedPort();
    const base = `http://127.0.0.1:${port}`;
    const child = Bun.spawn([join(root, 'server/loop-test-http-asan'), database, join(root, 'static'), String(port)], {
      env: Object.fromEntries(Object.entries(process.env).filter(([name]) => !['LOOP_TEST_TLS_ORIGIN', 'LOOP_TEST_VOICE_SECRET'].includes(name))),
      stdout: 'inherit', stderr: 'inherit',
    });
    children.push(child);
    for (let attempt = 0; attempt < 100; attempt++) {
      if (child.exitCode !== null) throw new Error(`Recovery fixture exited: ${child.exitCode}`);
      try { if ((await fetch(`${base}/healthz`)).ok) return base; } catch { /* Wait for listener. */ }
      await Bun.sleep(25);
    }
    throw new Error('Recovery fixture did not become ready');
  }
  async function login(base: string) {
    const response = await fetch(`${base}/api/test/session`, { redirect: 'manual' });
    expect(response.status).toBe(302);
    return response.headers.get('set-cookie')!.split(';')[0]!;
  }
  async function create(base: string, cookie: string, id: string) {
    const response = await fetch(`${base}/api/turns`, { method: 'POST',
      headers: { Cookie: cookie, Origin: base, 'Content-Type': 'application/json' },
      body: JSON.stringify({ request_id: id, recording_consent: true, purpose: 'synthetic_recovery_test' }),
    });
    expect(response.status).toBe(201);
  }
  try {
    mkdirSync(join(dir, 'source'), { mode: 0o700 });
    mkdirSync(join(dir, 'restored'), { mode: 0o700 });
    const sourceDb = join(dir, 'source/loop.sqlite');
    const source = await start(sourceDb);
    const cookie = await login(source);
    const id = `recovery-${crypto.randomUUID()}`;
    const unfinished = `unfinished-${crypto.randomUUID()}`;
    const later = `later-${crypto.randomUUID()}`;
    const headers = { Cookie: cookie, Origin: source, 'Content-Type': 'application/json' };
    await create(source, cookie, id);
    await create(source, cookie, unfinished);
    for (const [direction, rate] of [['input', 16000], ['output', 24000]] as const) {
      const pcm = new Uint8Array(640); pcm[0] = direction === 'input' ? 41 : 73;
      expect((await fetch(`${source}/api/turns/${id}/audio/${direction}/${rate}`, {
        method: 'POST', headers: { ...headers, 'Content-Type': 'application/octet-stream' }, body: pcm,
      })).status).toBe(201);
    }
    expect((await fetch(`${source}/api/turns/${id}/finish`, { method: 'POST', headers,
      body: JSON.stringify({ status: 'failed', event_count: 0, transcript: '', answer: '',
        error: 'Synthetic recovery fixture', recording_error: null }),
    })).status).toBe(200);
    const paths = ['receipt', 'report', 'replay/input.s16le', 'replay/output.s16le', 'replay/input.wav', 'replay/output.wav'];
    const expected = new Map<string, Uint8Array>();
    for (const path of paths) {
      const response = await fetch(`${source}/api/turns/${id}/${path}`, { headers: { Cookie: cookie } });
      expect(response.status).toBe(200);
      expected.set(path, new Uint8Array(await response.arrayBuffer()));
    }
    // The source stays open: the production command must include committed WAL data.
    const snapshot = join(dir, 'snapshot.sqlite');
    const backup = Bun.spawn([join(root, 'server/loop-server'), '--backup-recordings', sourceDb, snapshot], {
      stdout: 'pipe', stderr: 'inherit',
    });
    children.push(backup);
    const output = await new Response(backup.stdout).text();
    expect(await backup.exited).toBe(0);
    expect(output.trim()).toBe('Published snapshot: 1 finalized recordings verified; 1 unfinished recordings not verified');
    await create(source, cookie, later);
    const restoredDb = join(dir, 'restored/loop.sqlite');
    copyFileSync(snapshot, restoredDb);
    const restored = await start(restoredDb);
    for (const path of paths) {
      const response = await fetch(`${restored}/api/turns/${id}/${path}`, { headers: { Cookie: cookie } });
      expect(response.status).toBe(200);
      expect(new Uint8Array(await response.arrayBuffer())).toEqual(expected.get(path)!);
      expect((await fetch(`${restored}/api/turns/${id}/${path}`)).status).toBe(401);
      expect((await fetch(`${restored}/api/turns/${id}/${path}`, {
        headers: { Cookie: `loop_session=${'0'.repeat(64)}` },
      })).status).toBe(401);
    }
    expect((await fetch(`${restored}/api/turns/${later}`, { headers: { Cookie: cookie } })).status).toBe(404);
    expect((await fetch(`${restored}/api/turns/${unfinished}/receipt`, { headers: { Cookie: cookie } })).status).toBe(409);
    expect((await fetch(`${restored}/api/logout`, {
      method: 'POST', headers: { Cookie: cookie, Origin: restored },
    })).status).toBe(200);
    expect((await fetch(`${restored}/api/turns/${id}/receipt`, { headers: { Cookie: cookie } })).status).toBe(401);
    const freshCookie = await login(restored);
    const freshReceipt = await fetch(`${restored}/api/turns/${id}/receipt`, { headers: { Cookie: freshCookie } });
    expect(freshReceipt.status).toBe(200);
    expect(new Uint8Array(await freshReceipt.arrayBuffer())).toEqual(expected.get('receipt')!);
    // Logging out the isolated restore must not mutate the source's session.
    expect((await fetch(`${source}/api/turns/${id}/receipt`, { headers: { Cookie: cookie } })).status).toBe(200);
  } finally {
    for (const child of children) if (child.exitCode === null) child.kill('SIGKILL');
    await Promise.all(children.map((child) => child.exited));
    rmSync(dir, { recursive: true, force: true });
  }
}, 30_000);
