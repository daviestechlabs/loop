#!/usr/bin/env python3
"""Validate and replay one captured WebTransport PCM utterance."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from typing import Any


SCHEMA_VERSION = "r-sim-4-captured-pcm/v1"
REPORT_VERSION = "r-sim-4-captured-pcm-replay/v1"
FRAME_BYTES = 640
MAX_PCM_BYTES = 16000 * 2 * 30
MAX_PROCESS_P99_NS = 50_000
TIMING_FIELDS = {"process_ns_p50", "process_ns_p95", "process_ns_p99"}
MANIFEST_FIELDS = {
    "schema_version",
    "fixture_id",
    "profile",
    "captured_at",
    "capture_source",
    "encoding",
    "sample_rate_hz",
    "channels",
    "bits_per_sample",
    "frame_duration_ms",
    "pcm_path",
    "pcm_sha256",
    "assistant_speaking",
    "cancel_frame",
    "approval",
}
APPROVAL_FIELDS = {"status", "reviewer", "approved_at"}


class ManifestError(ValueError):
    pass


def _require_exact(value: Any, expected: Any, field: str) -> None:
    if value != expected:
        raise ManifestError(f"{field} must be {expected!r}")


def _require_text(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ManifestError(f"{field} must be a non-empty string")
    return value.strip()


def _parse_time(value: Any, field: str) -> str:
    text = _require_text(value, field)
    normalized = text[:-1] + "+00:00" if text.endswith("Z") else text
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError as exc:
        raise ManifestError(f"{field} must be an ISO-8601 timestamp") from exc
    if parsed.tzinfo is None:
        raise ManifestError(f"{field} must include a timezone")
    return text


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_manifest(
    manifest_path: Path,
    *,
    require_real_approved: bool,
) -> tuple[dict[str, Any], Path]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ManifestError("manifest root must be an object")
    unknown_fields = sorted(set(manifest) - MANIFEST_FIELDS)
    if unknown_fields:
        raise ManifestError(f"unknown manifest fields: {', '.join(unknown_fields)}")

    _require_exact(manifest.get("schema_version"), SCHEMA_VERSION, "schema_version")
    fixture_id = _require_text(manifest.get("fixture_id"), "fixture_id")
    profile = manifest.get("profile")
    if profile not in {"real_browser_capture", "synthetic_test_fixture"}:
        raise ManifestError(
            "profile must be real_browser_capture or synthetic_test_fixture"
        )
    captured_at = _parse_time(manifest.get("captured_at"), "captured_at")
    capture_source = _require_text(manifest.get("capture_source"), "capture_source")
    _require_exact(manifest.get("encoding"), "pcm_s16le", "encoding")
    _require_exact(manifest.get("sample_rate_hz"), 16000, "sample_rate_hz")
    _require_exact(manifest.get("channels"), 1, "channels")
    _require_exact(manifest.get("bits_per_sample"), 16, "bits_per_sample")
    _require_exact(manifest.get("frame_duration_ms"), 20, "frame_duration_ms")

    pcm_name = _require_text(manifest.get("pcm_path"), "pcm_path")
    pcm_relative = Path(pcm_name)
    if pcm_relative.is_absolute() or len(pcm_relative.parts) != 1:
        raise ManifestError("pcm_path must name a file beside the manifest")
    pcm_path = (manifest_path.parent / pcm_name).resolve()
    if pcm_path.parent != manifest_path.parent.resolve():
        raise ManifestError("pcm_path must resolve beside the manifest")
    if not pcm_path.is_file():
        raise ManifestError(f"PCM fixture does not exist: {pcm_path}")
    pcm_size = pcm_path.stat().st_size
    if pcm_size == 0 or pcm_size % FRAME_BYTES != 0:
        raise ManifestError(
            f"PCM fixture must contain complete 20 ms frames ({FRAME_BYTES} bytes each)"
        )
    if pcm_size > MAX_PCM_BYTES:
        raise ManifestError("PCM fixture exceeds the 30 second session capacity")

    expected_hash = _require_text(manifest.get("pcm_sha256"), "pcm_sha256").lower()
    if len(expected_hash) != 64 or any(
        char not in "0123456789abcdef" for char in expected_hash
    ):
        raise ManifestError("pcm_sha256 must be 64 lowercase hexadecimal characters")
    actual_hash = _sha256(pcm_path)
    if actual_hash != expected_hash:
        raise ManifestError(
            f"PCM SHA-256 mismatch: manifest={expected_hash} actual={actual_hash}"
        )

    assistant_speaking = manifest.get("assistant_speaking")
    if not isinstance(assistant_speaking, bool):
        raise ManifestError("assistant_speaking must be true or false")
    cancel_frame = manifest.get("cancel_frame")
    if cancel_frame is not None:
        if isinstance(cancel_frame, bool) or not isinstance(cancel_frame, int):
            raise ManifestError("cancel_frame must be null or an integer")
        total_frames = pcm_size // FRAME_BYTES
        if cancel_frame < 0 or cancel_frame > total_frames:
            raise ManifestError(
                f"cancel_frame must be between 0 and {total_frames}, inclusive"
            )

    approval = manifest.get("approval")
    if not isinstance(approval, dict):
        raise ManifestError("approval must be an object")
    unknown_approval_fields = sorted(set(approval) - APPROVAL_FIELDS)
    if unknown_approval_fields:
        raise ManifestError(
            f"unknown approval fields: {', '.join(unknown_approval_fields)}"
        )
    approval_status = approval.get("status")
    if approval_status not in {"approved", "pending", "synthetic"}:
        raise ManifestError("approval.status must be approved, pending, or synthetic")
    if profile == "synthetic_test_fixture" and approval_status != "synthetic":
        raise ManifestError("synthetic fixtures must use approval.status=synthetic")
    if profile == "real_browser_capture" and approval_status == "synthetic":
        raise ManifestError("real captures cannot use approval.status=synthetic")
    if approval_status == "approved":
        _require_text(approval.get("reviewer"), "approval.reviewer")
        _parse_time(approval.get("approved_at"), "approval.approved_at")
    if require_real_approved and (
        profile != "real_browser_capture" or approval_status != "approved"
    ):
        raise ManifestError(
            "live closure requires profile=real_browser_capture and approval.status=approved"
        )

    normalized = {
        "fixture_id": fixture_id,
        "profile": profile,
        "captured_at": captured_at,
        "capture_source": capture_source,
        "pcm_sha256": actual_hash,
        "pcm_bytes": pcm_size,
        "sample_rate_hz": 16000,
        "channels": 1,
        "bits_per_sample": 16,
        "frame_duration_ms": 20,
        "assistant_speaking": assistant_speaking,
        "cancel_frame": cancel_frame,
        "approval": approval,
    }
    return normalized, pcm_path


def compile_runner(output: Path, *, sanitize: bool) -> None:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parents[2]
    dsp_dir = repo_root / "voice/audio-processor/dsp"
    telemetry_dir = repo_root / "contracts/telemetry-c"
    compiler = os.environ.get("CC", "cc")
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-D_POSIX_C_SOURCE=200809L",
        f"-I{script_dir}",
        f"-I{dsp_dir}",
        f"-I{telemetry_dir}",
    ]
    if sanitize:
        command.extend(
            [
                "-O1",
                "-g",
                "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined",
            ]
        )
    else:
        command.append("-O3")
    command.extend(
        [
            str(script_dir / "reflex_session_test.c"),
            str(script_dir / "reflex_session.c"),
            str(dsp_dir / "audio_engine.c"),
            str(dsp_dir / "pcm_condition.c"),
            str(telemetry_dir / "telemetry.c"),
            "-lm",
            "-pthread",
            "-o",
            str(output),
        ]
    )
    subprocess.run(command, check=True)


def run_runner(
    runner: Path,
    pcm_path: Path,
    manifest: dict[str, Any],
    *,
    prefix: list[str] | None = None,
    sanitize: bool = False,
) -> tuple[dict[str, Any], int]:
    cancel_frame = manifest["cancel_frame"]
    command = list(prefix or [])
    command.extend(
        [
            str(runner),
            "--captured",
            str(pcm_path),
            "1" if manifest["assistant_speaking"] else "0",
            str(cancel_frame if cancel_frame is not None else -1),
        ]
    )
    env = os.environ.copy()
    if sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
    completed = subprocess.run(command, text=True, capture_output=True, env=env)
    if completed.returncode not in {0, 1}:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"native replay failed ({completed.returncode}): {detail}")
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"native replay returned invalid JSON: {completed.stdout!r}"
        ) from exc
    if not isinstance(result, dict):
        raise RuntimeError("native replay result must be an object")
    return result, completed.returncode


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def _without_timings(result: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in result.items() if key not in TIMING_FIELDS}


def execute_manifest(
    manifest_path: Path,
    report_path: Path,
    *,
    require_real_approved: bool,
) -> int:
    fixture, pcm_path = validate_manifest(
        manifest_path, require_real_approved=require_real_approved
    )
    with tempfile.TemporaryDirectory(prefix="r-sim-reflex-replay-") as temp:
        build_dir = Path(temp)
        strict_runner = build_dir / "reflex-replay"
        sanitized_runner = build_dir / "reflex-replay-sanitize"
        compile_runner(strict_runner, sanitize=False)
        compile_runner(sanitized_runner, sanitize=True)

        strict_result, strict_code = run_runner(strict_runner, pcm_path, fixture)
        sanitized_result, sanitized_code = run_runner(
            sanitized_runner, pcm_path, fixture, sanitize=True
        )
        deterministic = _without_timings(strict_result) == _without_timings(
            sanitized_result
        )

        valgrind_status = "unavailable"
        valgrind_result: dict[str, Any] | None = None
        if shutil.which("valgrind") is not None:
            valgrind_result, valgrind_code = run_runner(
                strict_runner,
                pcm_path,
                fixture,
                prefix=[
                    "valgrind",
                    "--quiet",
                    "--error-exitcode=86",
                    "--leak-check=full",
                    "--show-leak-kinds=definite,indirect,possible",
                ],
            )
            if valgrind_code not in {0, 1}:
                raise RuntimeError("valgrind replay failed")
            valgrind_status = "pass"
            deterministic = deterministic and (
                _without_timings(valgrind_result)
                == _without_timings(strict_result)
            )

    approved_real = (
        fixture["profile"] == "real_browser_capture"
        and fixture["approval"]["status"] == "approved"
    )
    gates_pass = (
        strict_code == 0
        and sanitized_code == 0
        and strict_result.get("overall_match") is True
        and sanitized_result.get("overall_match") is True
        and isinstance(strict_result.get("process_ns_p99"), int)
        and strict_result["process_ns_p99"] < MAX_PROCESS_P99_NS
        and deterministic
        and (not require_real_approved or valgrind_status == "pass")
    )
    report = {
        "schema_version": REPORT_VERSION,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "fixture": fixture,
        "replay": strict_result,
        "gates": {
            "strict_c": strict_code == 0,
            "asan_ubsan": sanitized_code == 0,
            "valgrind": valgrind_status,
            "deterministic_across_instrumentation": deterministic,
            "process_p99_below_50us": (
                isinstance(strict_result.get("process_ns_p99"), int)
                and strict_result["process_ns_p99"] < MAX_PROCESS_P99_NS
            ),
        },
        "eligible_for_r_sim_4_2_live_evidence": approved_real and gates_pass,
    }
    write_report(report_path, report)
    return 0 if gates_pass else 1


def _synthetic_pcm() -> bytes:
    frames: list[bytes] = []
    for frame_index in range(56):
        values = []
        for sample in range(320):
            if frame_index < 12:
                absolute = frame_index * 320 + sample
                value = int(6000.0 * math.sin(2.0 * math.pi * 440.0 * absolute / 16000.0))
            else:
                value = 0
            values.append(value)
        frames.append(struct.pack("<320h", *values))
    return b"".join(frames)


def _write_synthetic_fixture(root: Path) -> tuple[Path, dict[str, Any]]:
    pcm_path = root / "synthetic.s16le"
    pcm_path.write_bytes(_synthetic_pcm())
    manifest_path = root / "manifest.json"
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "fixture_id": "synthetic-self-test",
        "profile": "synthetic_test_fixture",
        "captured_at": "2026-07-26T00:00:00Z",
        "capture_source": "generated-sine-self-test",
        "encoding": "pcm_s16le",
        "sample_rate_hz": 16000,
        "channels": 1,
        "bits_per_sample": 16,
        "frame_duration_ms": 20,
        "pcm_path": pcm_path.name,
        "pcm_sha256": _sha256(pcm_path),
        "assistant_speaking": True,
        "cancel_frame": None,
        "approval": {"status": "synthetic"},
    }
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path, manifest


def self_test(runner: Path, *, prefix: list[str] | None = None) -> None:
    with tempfile.TemporaryDirectory(prefix="r-sim-replay-self-test-") as temp:
        root = Path(temp)
        manifest_path, manifest = _write_synthetic_fixture(root)
        normalized, resolved_pcm = validate_manifest(
            manifest_path, require_real_approved=False
        )
        result, code = run_runner(
            runner, resolved_pcm, normalized, prefix=prefix
        )
        if code != 0 or result.get("overall_match") is not True:
            raise RuntimeError(f"synthetic replay mismatch: {result}")

        manifest["assistant_speaking"] = False
        manifest["cancel_frame"] = 6
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        cancel_fixture, cancel_pcm = validate_manifest(
            manifest_path, require_real_approved=False
        )
        cancel_result, cancel_code = run_runner(
            runner, cancel_pcm, cancel_fixture, prefix=prefix
        )
        if (
            cancel_code != 0
            or cancel_result.get("overall_match") is not True
            or cancel_result.get("cancel_match") is not True
            or cancel_result.get("legacy_audio_bytes") != 0
            or cancel_result.get("embedded_audio_bytes") != 0
        ):
            raise RuntimeError(f"synthetic cancel replay mismatch: {cancel_result}")

        try:
            validate_manifest(manifest_path, require_real_approved=True)
        except ManifestError:
            pass
        else:
            raise RuntimeError("synthetic fixture was accepted as live evidence")

        manifest["pcm_sha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            validate_manifest(manifest_path, require_real_approved=False)
        except ManifestError:
            pass
        else:
            raise RuntimeError("bad PCM hash was accepted")

        partial_path = root / "partial-frame.s16le"
        partial_path.write_bytes(b"\x00\x00")
        manifest["pcm_path"] = partial_path.name
        manifest["pcm_sha256"] = _sha256(partial_path)
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            validate_manifest(manifest_path, require_real_approved=False)
        except ManifestError:
            pass
        else:
            raise RuntimeError("partial 20 ms PCM frame was accepted")


def full_self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="r-sim-replay-full-self-test-") as temp:
        root = Path(temp)
        manifest_path, _ = _write_synthetic_fixture(root)
        report_path = root / "report.json"
        if execute_manifest(
            manifest_path, report_path, require_real_approved=False
        ) != 0:
            raise RuntimeError("full synthetic replay gate failed")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if report.get("eligible_for_r_sim_4_2_live_evidence") is not False:
            raise RuntimeError("synthetic report was marked eligible for live evidence")
        gates = report.get("gates", {})
        if (
            gates.get("strict_c") is not True
            or gates.get("asan_ubsan") is not True
            or gates.get("deterministic_across_instrumentation") is not True
            or gates.get("process_p99_below_50us") is not True
        ):
            raise RuntimeError(f"full synthetic replay gates are incomplete: {gates}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate a provenance-bearing WebTransport PCM capture and replay "
            "it through the legacy audio engine and embedded reflex session."
        )
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--manifest", type=Path)
    group.add_argument("--self-test", action="store_true")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--runner", type=Path)
    parser.add_argument(
        "--under-valgrind",
        action="store_true",
        help="run the self-test native replay under valgrind",
    )
    parser.add_argument(
        "--require-real-approved",
        action="store_true",
        help="reject synthetic, pending, or unreviewed fixtures",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.self_test:
            if args.runner is None:
                if args.under_valgrind:
                    raise ManifestError("--under-valgrind requires --runner")
                full_self_test()
                print("captured PCM full replay self-test: pass")
                return 0
            prefix = None
            if args.under_valgrind:
                if shutil.which("valgrind") is None:
                    raise ManifestError("valgrind is unavailable")
                prefix = [
                    "valgrind",
                    "--quiet",
                    "--error-exitcode=86",
                    "--leak-check=full",
                    "--show-leak-kinds=definite,indirect,possible",
                ]
            self_test(args.runner.resolve(), prefix=prefix)
            print("captured PCM replay self-test: pass")
            return 0
        if args.report is None:
            raise ManifestError("--manifest requires --report")
        return execute_manifest(
            args.manifest.resolve(),
            args.report.resolve(),
            require_real_approved=args.require_real_approved,
        )
    except (ManifestError, OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"captured PCM replay: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
