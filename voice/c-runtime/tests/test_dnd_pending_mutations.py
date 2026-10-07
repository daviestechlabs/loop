"""Require semantic assertions to detect deliberate held-proposal faults."""

import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "common/dnd_pending.c"
MUTATIONS = {
    "stale_ticket": ("slot->generation != ticket.generation ||", ""),
    "revive_canceled": (
        "if (slot->proposal_update == 2) return DND_PENDING_CONFLICT;",
        "/* Deliberately permit a canceled proposal to return. */",
    ),
    "foreign_owner": (
        "!strcmp(candidate->user_id, key.user_id) &&",
        "/* Deliberately omit owner binding. */",
    ),
    "ignore_scene_revision": (
        "if (slot->scene_revision && slot->view.held) slot->view.scene_changed = 1;",
        "/* Deliberately preserve old context validity. */",
    ),
}


def main():
    source = SOURCE.read_text()
    results = []
    with tempfile.TemporaryDirectory(prefix="dnd-pending-mutations-") as directory:
        root = Path(directory)
        for name in ("reference", *MUTATIONS):
            text = source
            if name != "reference":
                before, after = MUTATIONS[name]
                if text.count(before) != 1:
                    raise ValueError("Mutation no longer identifies one guard: " + name)
                text = text.replace(before, after)
            candidate = root / (name + ".c")
            candidate.write_text(text)
            binary = root / name
            subprocess.run(
                [
                    "clang",
                    "-std=c11",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "common"),
                    str(candidate),
                    str(ROOT / "common/utf8.c"),
                    str(ROOT / "tests/dnd_pending_test.c"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                check=True,
                timeout=60,
            )
            result = subprocess.run(
                [str(binary)],
                capture_output=True,
                text=True,
                timeout=15,
                env={
                    **os.environ,
                    "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
                    "UBSAN_OPTIONS": "halt_on_error=1",
                },
            )
            if name == "reference":
                if result.returncode != 0 or result.stderr:
                    raise ValueError("Reference failed: " + result.stderr)
            elif result.returncode != 1 or not result.stderr.startswith("FAIL "):
                raise ValueError("Mutation did not fail through an assertion: " + name)
            if "Sanitizer" in result.stderr or "runtime error:" in result.stderr:
                raise ValueError("A sanitizer failure cannot count as fault detection")
            results.append(
                {
                    "variant": name,
                    "exit_code": result.returncode,
                    "output": (result.stdout + result.stderr).strip(),
                }
            )
    print(json.dumps({"variants": results}, indent=2))


if __name__ == "__main__":
    main()
