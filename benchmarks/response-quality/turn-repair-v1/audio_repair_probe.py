"""Synthetic speech calibration for correction markers; no human timing claim."""

import argparse
import json
import os
import re
import subprocess
import time
import urllib.request
import wave
from pathlib import Path

from semantic_eval import Client, NoRedirect, sha, write

TEXT = {
    "prefix": "I cast Fireball.",
    "question": "No wait, is Mira still in the room?",
    "cancel": "No, don't cast it.",
}


def words(text):
    return re.findall(r"[a-z]+(?:'[a-z]+)?", text.lower())


def distance(reference, hypothesis):
    previous = list(range(len(hypothesis) + 1))
    for i, left in enumerate(reference, 1):
        current = [i]
        for j, right in enumerate(hypothesis, 1):
            current.append(
                min(current[-1] + 1, previous[j] + 1, previous[j - 1] + (left != right))
            )
        previous = current
    return previous[-1]


def pcm(path):
    with wave.open(str(path)) as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (
            1,
            2,
            16000,
        ):
            raise ValueError("Expected mono 16 kHz PCM")
        count = wav.getnframes()
        if not 0 < count <= 16000 * 20:
            raise ValueError("Empty or oversized synthetic speech")
        raw = wav.readframes(count)
        if len(raw) != count * 2:
            raise ValueError("Incomplete PCM")
        return raw


def repair_present(kind, text):
    tokens = words(text)
    if kind == "question":
        return any(tokens[i : i + 2] == ["no", "wait"] for i in range(len(tokens) - 1))
    normalized = " ".join(tokens).replace("don't", "do not")
    return "do not cast" in normalized


def run(endpoint, output):
    Client(endpoint)  # Same loopback-only validation as text experiments.
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).read_bytes()
    (output / "audio_repair_probe.py").write_bytes(source)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    rows = []
    with (output / "trials.jsonl").open("x") as journal:
        for voice in ("Samantha", "Daniel"):
            parts = {}
            for key, text in TEXT.items():
                path = output / f"{voice}-{key}.wav"
                subprocess.run(
                    [
                        "/usr/bin/say",
                        "-v",
                        voice,
                        "--data-format=LEI16@16000",
                        "-o",
                        str(path),
                        text,
                    ],
                    check=True,
                    capture_output=True,
                    timeout=30,
                )
                parts[key] = pcm(path)
            for gap_ms in (0, 300, 1000):
                for kind in ("question", "cancel"):
                    raw = parts["prefix"] + b"\x00\x00" * (gap_ms * 16) + parts[kind]
                    case_id = f"{voice}-{kind}-{gap_ms}"
                    path = output / f"{case_id}.wav"
                    with wave.open(str(path), "wb") as wav:
                        wav.setnchannels(1)
                        wav.setsampwidth(2)
                        wav.setframerate(16000)
                        wav.writeframes(raw)
                    request = urllib.request.Request(
                        endpoint.rstrip("/") + "/v1/internal/transcribe_pcm_s16le",
                        data=raw,
                        headers={
                            "Content-Type": "application/octet-stream",
                            "X-Sample-Rate": "16000",
                            "X-Language": "en",
                            "X-PCM-Format": "pcm_s16le",
                        },
                    )
                    start = time.monotonic()
                    with opener.open(request, timeout=60) as response:
                        body = response.read(200001)
                    elapsed = (time.monotonic() - start) * 1000
                    if len(body) > 200000:
                        raise ValueError("Oversized transcription")
                    result = json.loads(body)
                    text = result.get("text")
                    if (
                        result.get("status") != "ok"
                        or not isinstance(text, str)
                        or ("transcript" in result and result["transcript"] != text)
                    ):
                        raise ValueError("Invalid transcription response")
                    reference = TEXT["prefix"] + " " + TEXT[kind]
                    edits = distance(words(reference), words(text))
                    row = {
                        "case_id": case_id,
                        "voice": voice,
                        "kind": kind,
                        "inserted_gap_ms": gap_ms,
                        "reference": reference,
                        "transcript": text,
                        "response": result,
                        "word_edits": edits,
                        "reference_words": len(words(reference)),
                        "wer": edits / len(words(reference)),
                        "repair_marker_preserved": repair_present(kind, text),
                        "audio_sha256": sha(path.read_bytes()),
                        "pcm_sha256": sha(raw),
                        "audio_seconds": len(raw) / 32000,
                        "request_ms": elapsed,
                    }
                    journal.write(json.dumps(row) + "\n")
                    journal.flush()
                    rows.append(row)
                    print(
                        json.dumps(
                            {
                                "case_id": case_id,
                                "transcript": text,
                                "repair_marker_preserved": row[
                                    "repair_marker_preserved"
                                ],
                            }
                        ),
                        flush=True,
                    )
    summary = {
        "schema": "waterdeep-synthetic-repair-audio/v1",
        "trials": len(rows),
        "repair_markers_preserved": sum(r["repair_marker_preserved"] for r in rows),
        "word_edits": sum(r["word_edits"] for r in rows),
        "reference_words": sum(r["reference_words"] for r in rows),
        "production_gate": False,
        "human_audio": False,
        "training_performed": False,
        "limitations": [
            "Installed macOS TTS voices; manually concatenated utterance parts.",
            "Inserted silence supplements any silence already in synthesized parts.",
            "Complete utterances went directly to STT; browser endpoint timing and VAD were not measured.",
            "Repair-marker checks are lexical; semantic interpretation and overlap remain unmeasured.",
        ],
    }
    write(output / "summary.json", summary)
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-audio-probe-receipt/v1",
            "files": {
                p.name: sha(p.read_bytes()) for p in output.iterdir() if p.is_file()
            },
        },
    )
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(run(args.endpoint, args.output), indent=2))
