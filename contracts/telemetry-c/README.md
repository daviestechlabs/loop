# telemetry-c

Process-local C reflex primitives used by the voice hot path.

## What it owns (the parts that must be fast and pure)

- Bounded process-local ring
- Energy VAD + low-latency audio decisions
- Honest monotonic stage timing and an explicit raw cycle counter

The ring is bounded process-local coordination state. Product telemetry belongs
to the typed turn owner and the existing OpenTelemetry/interaction analytics
path. The former Python → unary gRPC → Go → shared-memory C ring and idle C
consumer were deleted because their output had no product consumer.
Its writer critical section is process-local and allocation-free; PCM,
monitor, and lifecycle producers cannot reserve the same slot or regress
the published head. Audio initializes exactly one ring: Go-originated
lifecycle facts and C-originated frame/decision facts therefore share the
same bounded causal history.

Elapsed time and RTF use `telemetry_monotonic_ns()`: `CLOCK_MONOTONIC_RAW` on
Linux and `CLOCK_MONOTONIC` elsewhere. `get_tsc()` is a raw cycle counter on
x86_64 and must never be converted to time without calibration.

Run `scripts/test-timing.sh`.
That script compares the audio engine C timing with an independent outer clock.
It also stresses 16,000 events from eight producers without loss. The default timing gate allows at most 10%
relative error. `reflex_kernel.c` separately drives the public monitor aggregate
and snapshot APIs; it does not invent a lakehouse artifact or a synthetic
"Frontier" score.

See `voice/audio-processor/` for the current user and
`homelab-design/ROAST-ACTIONABLE-TODOS.md` for the reflex-kernel migration.

The design rule is simple: measured reflexes live in C; telemetry without a
human-used output is removed.
