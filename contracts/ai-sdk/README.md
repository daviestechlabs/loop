# ai-sdk

Small TypeScript SDK for Davies Tech Labs interaction clients.

This repo starts narrow on purpose. It owns product/client protocol helpers that
should not keep drifting across product client surfaces (`companions-frontend` / Presence):

- canonical interaction profile metadata
- turn request payload construction
- WebTransport/turn-stream frame helpers
- protobuf-lite turn event decoding for browser streams
- a deterministic interaction runtime and bounded turn flight recorder shared
  by React and Alpine surfaces
- a shared 20 ms microphone packetizer and AudioWorklet used by every product
  surface; it selects a C WebAssembly SIMD audio kernel when supported and keeps
  a byte-compatible C scalar WebAssembly fallback

It is not a UI component kit and not a product template. Templates can consume
this package later once both active surfaces prove the SDK API.

## Build

```bash
bun install
bun run test
bun run bench:audio
```

The microphone worklet keeps render-quantum remainders across callbacks,
box-downsamples an exact 20 ms browser-rate window to 320 mono samples, and
emits 640-byte PCM16 WebTransport datagrams. The shared C kernel source is
`wasm/audio_kernel.c`; rebuild it with `scripts/build-audio-wasm.sh` when the
embedded kernel changes.

The SDK adds a `DTVP1` header and a big-endian sequence to each PCM datagram.
The final `DTVA1` control carries the exact packet count and PCM byte count.

The scalar fallback uses the same C source and fixed 128 KiB memory as SIMD.
Both modules have no host imports or heap allocation.
The protocol label `scalar` now means C WebAssembly.
Capture accepts normalized finite samples and exact 20 ms windows through 4096 samples.
The worklet pads its final partial window only for a manual finish.
Server endpoint feedback discards the partial window.
The playback decoder accepts bounded mono S16LE frames and clears its buffers after each conversion.
Legacy JavaScript math utilities remain exported for other clients.

The native oracle compares GCC, Clang, scalar WebAssembly, and SIMD WebAssembly output bytes.
It checks every signed PCM16 value and executes the generated worklet with fragmented input.
The script tests the checked-in browser bridge without a TypeScript build.

```bash
scripts/build-audio-wasm.sh
CC=clang SANITIZE=1 FUZZ_RUNS=20000 bash scripts/test-audio-kernel.sh
```

Rebuild the served bridge after changing an embedded module.
The normal SDK tests verify the audio source fingerprint.

The interaction runtime owns the framework-neutral lifecycle:
`connecting → listening → committing → thinking → speaking → interrupting`.
It records channel, first-text, first-voice, and cancel acknowledgement
milestones, rejects stale turn events, and can replay deterministic fixtures.
Product surfaces translate those states into their own visual language.

## C browser provenance

Typed tool results and retrieval citations use the runtime's C decoder and public response filter.
The browser runs those sources as WebAssembly, with no host imports or heap allocation.
The module uses one fixed 384 KiB memory.
JavaScript copies admitted fields into the existing product metadata shape.
It does not implement a second provenance policy.
Ranked citations retain their raw value and `score_metric` (`cosine` or `bm25`).
Exact record fetches use `score_metric: none` with a zero wire placeholder.
The C decoder rejects any nonzero placeholder.
The product view omits the placeholder score.
The product citation view exposes that metric as `scoreMetric`.
Citation metadata also preserves exact `excerpt_spans` from the C decoder.
Each range uses half-open byte offsets in the original record; its content hash still identifies that full record.
An absent range list means a record citation supplies the complete record.
Complete passage citations instead carry `kind: complete_passage`, `passage_id`, and original record `witnesses`.
Their assembly hash differs from the full original record hashes stored in those witnesses.
The decoder admits at most four passages with sixteen witnesses each.
It checks witness ranges, ordering, page bounds, and mutually exclusive record and passage identities.
The gateway verifies original bytes before assembly; browser metadata alone cannot repeat that verification.

Pass the active request ID as the second argument to `parseTurnEvent`.
Invalid or mismatched provenance throws before publication.
The adapter clears its C buffers after each projection.
Ordinary events retain their existing metadata.
Encounter snapshots require a matching typed tool receipt and the same C validation as HTTP.
The adapter copies participants only after that validation succeeds.
Owned campaign rosters and initiative receipts use the same C boundary.
Initiative receipts must match the saved participants and their dice arithmetic.
An omitted protobuf active index means zero, as the canonical contract requires.

Rebuild with Clang and a wasi-libc sysroot after any listed C source changes.
The source manifest also covers the wrapper, shared headers, and asset builder.

```bash
WASI_SYSROOT=/usr scripts/build-turn-provenance-wasm.sh
node scripts/build-provenance-fixtures.mjs
CC=clang SANITIZE=1 scripts/test-turn-provenance.sh
FUZZ_RUNS=20000 scripts/test-turn-provenance.sh
```

The focused tests need a C compiler, Node.js, and `protoc`.
They compare browser metadata with the native C HTTP writer.
Browser tests consume checked-in canonical bytes without installing `protoc`.
The normal SDK test checks the source fingerprint and wire fixtures.
Rebuild the served bridge with `product/companions-frontend/scripts/build-ai-sdk-bridge.mjs` from the Git root using Bun.
See the [local proof](evidence/browser-provenance-local-2026-09-06.md) for results and open gates.

## Package Boundary

The SDK should stay framework-neutral. Keep React, Next, Alpine, HTMX, and app
policy outside this repo unless the code is a pure protocol/client primitive.
