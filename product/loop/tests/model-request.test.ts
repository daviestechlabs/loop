import { expect, test } from 'bun:test';
import { ModelRequestCapture, MODEL_REQUEST_MAX, type ModelRequestChunk } from '../src/model-request';

async function chunks(length = 9000): Promise<ModelRequestChunk[]> {
  const bytes = Uint8Array.from({ length }, (_, index) => index % 251);
  const hash = Buffer.from(await crypto.subtle.digest('SHA-256', bytes)).toString('hex');
  const rows: ModelRequestChunk[] = [];
  for (let offset = 0; offset < length; offset += 8192) rows.push({
    type: 'model_request_chunk', protocol_version: 'turnstream.v1alpha1', request_id: 'synthetic-turn',
    sequence: rows.length, total_bytes: length, data: Buffer.from(bytes.subarray(offset, offset + 8192)).toString('base64'),
    final: offset + 8192 >= length, sha256: hash, captured_at: 1, nonce: 'a'.repeat(32), signature: 'b'.repeat(64),
  });
  return rows;
}

test('synthetic exact-capacity capture is complete; absent capture is allowed', async () => {
  new ModelRequestCapture('synthetic-turn').assertComplete();
  const capture = new ModelRequestCapture('synthetic-turn');
  for (const chunk of await chunks(MODEL_REQUEST_MAX)) expect(await capture.append(chunk)).toEqual(chunk);
  capture.assertComplete();
});

for (const [field, value] of Object.entries({ request_id: 'foreign', sequence: 1, total_bytes: MODEL_REQUEST_MAX + 1,
  protocol_version: 'other', type: 'other', final: true, captured_at: 0, nonce: 'bad', signature: 'bad',
  sha256: 'bad', data: '!!!!', extra: true })) test(`capture rejects invalid ${field}`, async () => {
  const capture = new ModelRequestCapture('synthetic-turn');
  const [chunk] = await chunks();
  await expect(capture.append({ ...chunk, [field]: value })).rejects.toThrow();
  expect(() => capture.assertComplete()).toThrow();
  await expect(capture.append(chunk)).rejects.toThrow();
});

for (const field of ['sha256', 'captured_at', 'nonce', 'signature', 'total_bytes'] as const)
  test(`capture rejects changed ${field} between chunks`, async () => {
    const [first, last] = await chunks();
    const capture = new ModelRequestCapture('synthetic-turn');
    await capture.append(first);
    const value = last![field];
    await expect(capture.append({ ...last, [field]: typeof value === 'number' ? value + 1 : 'c'.repeat(value.length) })).rejects.toThrow();
  });

test('capture rejects truncation, duplicate chunks, and incomplete final bytes', async () => {
  const [first, last] = await chunks();
  const truncated = new ModelRequestCapture('synthetic-turn');
  await truncated.append(first);
  expect(() => truncated.assertComplete()).toThrow();
  const duplicate = new ModelRequestCapture('synthetic-turn');
  await duplicate.append(first);
  await expect(duplicate.append(first)).rejects.toThrow();
  const short = new ModelRequestCapture('synthetic-turn');
  await short.append(first);
  await expect(short.append({ ...last, data: 'YQ==' })).rejects.toThrow();
});

test('capture rejects changed bytes, noncanonical base64, and post-completion data', async () => {
  const [one] = await chunks(1);
  await expect(new ModelRequestCapture('synthetic-turn').append({ ...one, data: 'AQ==' })).rejects.toThrow();
  await expect(new ModelRequestCapture('synthetic-turn').append({ ...one, data: 'AB==' })).rejects.toThrow();
  const capture = new ModelRequestCapture('synthetic-turn');
  await capture.append(one);
  capture.assertComplete();
  await expect(capture.append(one)).rejects.toThrow();
});

test('disposal during asynchronous hashing cannot resurrect the capture', async () => {
  const [one] = await chunks(1);
  const capture = new ModelRequestCapture('synthetic-turn');
  const pending = capture.append(one);
  capture.dispose();
  await expect(pending).rejects.toThrow();
  expect(() => capture.assertComplete()).toThrow();
});

test('concurrent append invalidates both results', async () => {
  const [one] = await chunks(1);
  const capture = new ModelRequestCapture('synthetic-turn');
  const pending = capture.append(one);
  await expect(capture.append(one)).rejects.toThrow();
  await expect(pending).rejects.toThrow();
  expect(() => capture.assertComplete()).toThrow();
});

test('short nonfinal chunks and missing binding fields fail closed', async () => {
  const [first] = await chunks();
  await expect(new ModelRequestCapture('synthetic-turn').append({ ...first, data: 'AA==' })).rejects.toThrow();
  const { signature: omitted, ...missing } = first!;
  expect(omitted).toHaveLength(64);
  await expect(new ModelRequestCapture('synthetic-turn').append(missing)).rejects.toThrow();
});
