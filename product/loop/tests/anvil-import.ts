import { expect } from 'bun:test';
import { PGlite } from '@electric-sql/pglite';
import { mkdtempSync, readFileSync, readdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

// Run the production Anvil importer against its real migrations in an isolated database.
// No live credentials, model execution, or copied importer implementation.
export async function checkAnvilImport(report: Record<string, unknown>): Promise<void> {
  const anvil = join(import.meta.dir, '../../anvil');
  const build = await Bun.build({ entrypoints: [join(anvil, 'src/lib/anvil/learning-store.server.ts')], target: 'bun',
    plugins: [{ name: 'anvil-import-test-dependencies', setup(builder) {
      builder.onResolve({ filter: /^zod$/ }, () => ({ path: require.resolve('zod') }));
    } }],
  });
  expect(build.success).toBe(true);
  const directory = mkdtempSync(join(tmpdir(), 'loop-anvil-import-'));
  const db = new PGlite();
  try {
    const file = join(directory, 'importer.mjs'); await Bun.write(file, build.outputs[0]!);
    const { importLearningReport, listLearningReports } = await import(file);
    const migrations = join(anvil, 'migrations');
    for (const name of readdirSync(migrations).filter((name) => name.endsWith('.sql')).sort())
      await db.exec(readFileSync(join(migrations, name), 'utf8'));
    const sqlFor = (client: Pick<PGlite, 'query'>) => Object.assign(
      async (strings: TemplateStringsArray, ...values: unknown[]) => {
        const query = strings.reduce((text, fragment, i) => text + (i ? `$${i}` : '') + fragment, '');
        return (await client.query(query, values)).rows;
      },
      { query: async (query: string, values: unknown[] = []) => (await client.query(query, values)).rows },
    );
    const tx = (work: (sql: ReturnType<typeof sqlFor>) => Promise<unknown>) => db.transaction((client) => work(sqlFor(client)));
    const result = await importLearningReport(tx, 'loop-test-owner', report);
    expect(result.duplicate).toBe(false); expect(result.reportSha256).toMatch(/^[a-f0-9]{64}$/);
    expect(await importLearningReport(tx, 'loop-test-owner', report)).toEqual({ ...result, duplicate: true });
    const visible = await listLearningReports(sqlFor(db), 'loop-test-owner');
    expect(visible).toHaveLength(1); expect(visible[0].evidence).toEqual(report.evidence);
    expect(visible[0].status).toBe('held'); expect(visible[0].hasAnswerReview).toBe(false);
    expect(await listLearningReports(sqlFor(db), 'other-owner')).toEqual([]);
    await expect(importLearningReport(tx, 'loop-test-owner', { ...report, summary: 'Changed evidence' }))
      .rejects.toThrow('Report sequence already contains different evidence');
    await expect(importLearningReport(tx, 'loop-test-owner', { ...report, sequence: 2 }))
      .rejects.toThrow('A finished run needs a new run ID');
    const audit = await db.query<{ actor_id: string; detail: string }>(
      "select actor_id,detail from anvil_audit_events where action='learning.report-imported'",
    );
    expect(audit.rows).toHaveLength(1); expect(audit.rows[0]!.actor_id).toBe('loop-test-owner');
    expect(JSON.parse(audit.rows[0]!.detail).reportSha256).toBe(result.reportSha256);
  } finally { await db.close(); rmSync(directory, { recursive: true, force: true }); }
}
