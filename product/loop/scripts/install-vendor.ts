// Install the exact locked npm tarballs without a registry, resolver, or lifecycle scripts.
import { createHash } from 'node:crypto';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
const root = resolve(import.meta.dir, '..');
const lock = JSON.parse((await Bun.file(`${root}/bun.lock`).text()).replace(/,\s*([}\]])/g, '$1'));
const manifest = await Bun.file(`${root}/package.json`).json();
const vendor = await Bun.file(`${root}/vendor.lock.json`).json();
for (const [name, version] of Object.entries(manifest.devDependencies)) {
  if (lock.packages[name]?.[0] !== `${name}@${version}`) throw new Error(`Stale dependency lock: ${name}`);
}
for (const [name, entry] of Object.entries(lock.packages) as [string, [string, string, unknown, string]][]) {
  if (!/^(@[a-z0-9_.-]+\/)?[a-z0-9_.-]+$/.test(name)) throw new Error('Invalid package name');
  const matches = vendor.artifacts.filter((item: { npmPackage?: string }) => item.npmPackage === name);
  if (matches.length !== 1) throw new Error(`Missing or duplicate vendored package: ${name}`);
  const item = matches[0];
  if (`${name}@${item.npmVersion}` !== entry[0] || item.integrity !== entry[3]) throw new Error(`Stale vendored package: ${name}`);
  if (!/^vendor-artifacts\/[a-zA-Z0-9_.-]+\.tgz$/.test(item.path)) throw new Error('Invalid archive path');
  const archive = resolve(root, item.path), bytes = await Bun.file(archive).arrayBuffer();
  if (bytes.byteLength !== item.size || createHash('sha256').update(new Uint8Array(bytes)).digest('hex') !== item.sha256 ||
      `sha512-${createHash('sha512').update(new Uint8Array(bytes)).digest('base64')}` !== entry[3]) throw new Error(`Invalid archive: ${name}`);
  const target = resolve(root, 'node_modules', name); mkdirSync(target, { recursive: true });
  const result = Bun.spawnSync(['tar', '-xzf', archive, '--strip-components=1', '-C', target]);
  if (result.exitCode) throw new Error(`Cannot extract ${name}`);
}
console.log('Installed exact vendored npm dependencies without network or lifecycle scripts');
