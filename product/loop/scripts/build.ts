import { realtimeMicWorkletSource } from '../../../contracts/ai-sdk/src/audio';
const root = new URL('../', import.meta.url).pathname;
const result = await Bun.build({
  entrypoints: [root + 'src/app.ts'],
  outdir: root + 'static', naming: 'app.js',
  target: 'browser', format: 'esm', minify: false, sourcemap: 'none',
});
if (!result.success) throw new AggregateError(result.logs, 'Loop build failed');

await Bun.write(root + 'static/mic-worklet.js', realtimeMicWorkletSource());
