# Embedded audio reflex session

`reflex_session_v1` is the pure-C session reflex for WebTransport PCM.
It validates mono 16 kHz PCM S16LE and conditions one frame in place.
It runs energy VAD and decides endpoint and interrupt state.
A silence endpoint requires both elapsed time and enough received silence PCM.
A delivery stall does not count as silence audio.
Speech resets both silence measures.
Callers can retain a fixed-capacity conditioned utterance for replay tests.

The ABI allocates only during `reflex_session_init_v1`.
Frame processing performs no heap operations.
The pure-C gateway disables utterance retention for its decision-only session.
It streams original PCM to the standalone C audio processor immediately.
The audio processor conditions each frame once and owns the STT payload.

Run strict replay, sanitizer, latency and valgrind gates:

```sh
./voice/voice-session-gateway/reflex-c/run-gates.sh
```

## Captured browser PCM

R-SIM-4.2 requires a real browser/table utterance, not the generated sine-wave
test. Store the private PCM outside Git as mono 16 kHz S16LE, concatenated in
complete 20 ms (640-byte) WebTransport frames. Put a manifest beside it using
[`captured_pcm_manifest.schema.json`](captured_pcm_manifest.schema.json):

```sh
python3 benchmarks/voice-validation-suite/scripts/r-sim-prepare-capture.py \
  --pcm /secure/captures/table-turn-20260726-01.s16le \
  --output /secure/captures/table-turn-20260726-01.json \
  --fixture-id table-turn-20260726-01 \
  --captured-at 2026-07-26T20:00:00-04:00 \
  --assistant-silent
```

The preparer reads an existing user-consented capture; it does not access a
microphone. It binds the actual hash and framing but deliberately emits only a
pending manifest. Review the private PCM and record approval separately.
When no private capture exists yet, the validation suite includes a dormant
operator-only DevTools Snippet; the consent-driven workflow and commands are
documented in
[`benchmarks/voice-validation-suite/README.md`](../../../benchmarks/voice-validation-suite/README.md).
The helper is absent from the product image and records only after an operator
runs and arms it. It captures only
successfully written WebTransport PCM datagrams, keeps at most 30 seconds in
browser memory, and can emit only a pending manifest.

```json
{
  "schema_version": "r-sim-4-captured-pcm/v1",
  "fixture_id": "table-turn-20260726-01",
  "profile": "real_browser_capture",
  "captured_at": "2026-07-26T20:00:00-04:00",
  "capture_source": "voice-session-gateway/webtransport",
  "encoding": "pcm_s16le",
  "sample_rate_hz": 16000,
  "channels": 1,
  "bits_per_sample": 16,
  "frame_duration_ms": 20,
  "pcm_path": "table-turn-20260726-01.s16le",
  "pcm_sha256": "<64 lowercase hex characters>",
  "assistant_speaking": true,
  "cancel_frame": null,
  "approval": {
    "status": "approved",
    "reviewer": "<reviewer>",
    "approved_at": "2026-07-26T20:10:00-04:00"
  }
}
```

Replay it through the standalone audio engine and embedded session:

```sh
python3 voice/voice-session-gateway/reflex-c/captured_pcm_replay.py \
  --manifest /secure/captures/table-turn-20260726-01.json \
  --report homelab-design/evidence/r-sim-4-table-turn-20260726-01.json \
  --require-real-approved
```

The report records the fixture hash and provenance, per-frame speech/feature
mismatches, legacy and embedded endpoint/interrupt frames, cancellation state,
conditioned-PCM byte parity, captured-replay p50/p95/p99, strict C and
ASAN/UBSAN results, and valgrind. A real closure run fails if p99 is at least
50 µs or valgrind is unavailable. Synthetic or pending fixtures can exercise
the tool but are ineligible for live R-SIM-4.2 evidence.
