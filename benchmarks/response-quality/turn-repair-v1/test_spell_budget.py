"""Independent resource matching oracle for the bounded C research kernel."""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

from spell_budget_cases import controls, encoded

HERE = Path(__file__).resolve().parent


def oracle(raw):
    edition, count, rules, amin, amax, bmin, bmax, rmin, rmax, past, c1, l1, c2, l2 = (
        raw
    )
    if (
        count not in (1, 2)
        or not 0 <= amin <= amax <= 2
        or not 0 <= bmin <= bmax <= 1
        or not 0 <= rmin <= rmax <= 1
        or past > 4
        or rules > 3
        or c1 > 3
        or c2 > 3
        or l1 > 2
        or l2 > 2
        or (count == 1 and (c2 or l2))
        or (past == 2 and bmin)
    ):
        return 0, 0, 0
    if edition != 1:
        return 1, 0, 0
    if rules != 3:
        return 2, 0, 0
    spell_options = []
    for cost, level in ((c1, l1), (c2, l2))[:count]:
        spell_options.append(
            list(
                itertools.product(
                    (cost,) if cost else (1, 2, 3), (level,) if level != 2 else (0, 1)
                )
            )
        )
    worlds = fit = 0
    for spells, a, b, r, history in itertools.product(
        itertools.product(*spell_options),
        range(amin, amax + 1),
        range(bmin, bmax + 1),
        range(rmin, rmax + 1),
        range(4) if past == 4 else (past,),
    ):
        if history == 2 and b:
            continue
        worlds += 1
        # Match individual spells to distinct available resources, independently
        # of the kernel's per-kind arithmetic.
        resources = [1] * a + [2] * b + [3] * r
        can_allocate = any(
            all(slot == spell[0] for slot, spell in zip(slots, spells))
            for slots in itertools.permutations(resources, len(spells))
        )
        # Represent earlier spells and evaluate pairwise bonus-spell compatibility.
        prior = {0: (), 1: ((1, 1),), 2: ((2, 0),), 3: ((1, 0),)}[history]
        all_spells = prior + spells
        compatible = all(
            other == (1, 1)
            for i, s in enumerate(all_spells)
            if s[0] == 2
            for j, other in enumerate(all_spells)
            if i != j
        )
        fit += can_allocate and compatible
    if not worlds:
        return 0, 0, 0
    return (3 if fit == worlds else 4 if not fit else 2), worlds, fit


def corpus():
    rows = [row for _, row, _ in controls()]
    # Exhaust all concrete single/pair plans, capacities, and history states.
    for count in (1, 2):
        for spells, a, b, r, history in itertools.product(
            itertools.product(itertools.product((1, 2, 3), (0, 1)), repeat=count),
            range(3),
            range(2),
            range(2),
            range(4),
        ):
            rows.append(
                encoded(
                    actions=(a, a),
                    bonus=(b, b),
                    reaction=(r, r),
                    history=history,
                    spells=spells,
                )
            )
    rng = random.Random(20260923)
    for _ in range(1024):
        action = tuple(sorted((rng.randrange(3), rng.randrange(3))))
        bonus = tuple(sorted((rng.randrange(2), rng.randrange(2))))
        reaction = tuple(sorted((rng.randrange(2), rng.randrange(2))))
        spells = tuple(
            (rng.randrange(4), rng.randrange(3)) for _ in range(rng.randrange(1, 3))
        )
        rows.append(
            encoded(
                actions=action,
                bonus=bonus,
                reaction=reaction,
                history=rng.randrange(5),
                spells=spells,
            )
        )
    # Probe every byte value at every input position around one admitted case.
    base = encoded()
    for position in range(len(base)):
        for value in range(256):
            row = bytearray(base)
            row[position] = value
            rows.append(bytes(row))
    return list(dict.fromkeys(rows))


def compile_probe(directory, source=None, sanitize=False):
    binary = directory / "probe"
    source = source or HERE / "spell_budget.c"
    flags = [
        "-std=c11",
        "-O1" if sanitize else "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wsign-conversion",
        "-Wshadow",
        "-Wstrict-prototypes",
        "-Wmissing-prototypes",
    ]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command = [
        os.environ.get("CC", "cc"),
        *flags,
        "-I" + str(HERE),
        str(source),
        str(HERE / "spell_budget_probe.c"),
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True, capture_output=True)
    return binary, command


def execute(binary, rows):
    result = subprocess.run(
        [str(binary)],
        input=b"".join(rows),
        capture_output=True,
        check=True,
        env=os.environ
        | {
            "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1",
        },
    )
    values = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    if len(values) != len(rows) or any(len(v) != 5 for v in values):
        raise ValueError("Missing or malformed probe results")
    return values


class SpellBudgetTests(unittest.TestCase):
    def test_typed_controls_and_matching_oracle(self):
        rows = corpus()
        expected = [oracle(row) for row in rows]
        with tempfile.TemporaryDirectory() as tmp:
            binary, _ = compile_probe(Path(tmp), sanitize=True)
            actual = execute(binary, rows)
            for row, want, got in zip(rows, expected, actual, strict=True):
                self.assertEqual(got[:3], want, list(row))
                self.assertEqual(
                    got[4] & ~got[3],
                    0,
                    "Universal reasons must occur in the diagnostic union",
                )
                if got[0] in (2, 3):
                    self.assertEqual(
                        got[4], 0, "A fitting world excludes universal rejection"
                    )
            for name, row, status in controls():
                self.assertEqual(oracle(row)[0], status, name)
                self.assertEqual(actual[rows.index(row)][0], status, name)


def proof(output):
    output.mkdir()
    rows = corpus()
    expected = [oracle(row) for row in rows]
    for name in (
        "spell_budget.c",
        "spell_budget.h",
        "spell_budget_probe.c",
        "spell_budget_cases.py",
        "test_spell_budget.py",
    ):
        (output / name).write_bytes((HERE / name).read_bytes())
    binary, command = compile_probe(output, sanitize=True)
    actual = execute(binary, rows)
    mismatches = [
        {"input": list(row), "expected": want, "actual": got}
        for row, want, got in zip(rows, expected, actual, strict=True)
        if want != got[:3]
    ]
    (output / "inputs.bin").write_bytes(b"".join(rows))
    (output / "results.json").write_text(json.dumps(actual) + "\n")
    original = (HERE / "spell_budget.c").read_text()
    mutations = {
        "ignore_bonus_restriction": (
            "if (needed[COST_BONUS] || history == HISTORY_BONUS)",
            "if (0)",
        ),
        "blanket_levelled_limit": (
            "return reasons;",
            "if (in->spell_count == 2 && !spells[0].cantrip && !spells[1].cantrip) reasons |= REJECT_BONUS_SPELL; return reasons;",
        ),
        "optimistic_unknown": (
            "? BUDGET_FITS : BUDGET_UNKNOWN;",
            "? BUDGET_FITS : BUDGET_FITS;",
        ),
        "pessimistic_unknown": (
            "? BUDGET_FITS : BUDGET_UNKNOWN;",
            "? BUDGET_FITS : BUDGET_EXCEEDS;",
        ),
        "ignore_earlier_spell": ("history == HISTORY_INCOMPATIBLE", "history == 99"),
    }
    mutation_results = []
    for name, (old, new) in mutations.items():
        if original.count(old) != 1:
            raise ValueError("Mutation no longer matches: " + name)
        root = output / name
        root.mkdir()
        source = root / "spell_budget.c"
        source.write_text(original.replace(old, new))
        mutated, _ = compile_probe(root, source=source)
        observed = execute(mutated, rows)
        failed = [
            i
            for i, (want, got) in enumerate(zip(expected, observed, strict=True))
            if want != got[:3]
        ]
        if not failed:
            raise ValueError("Fault survived: " + name)
        mutation_results.append(
            {
                "name": name,
                "mismatches": len(failed),
                "first_input": list(rows[failed[0]]),
                "expected": expected[failed[0]],
                "actual": observed[failed[0]],
            }
        )
    receipt = {
        "cases": len(rows),
        "authored_controls": len(controls()),
        "mismatches": mismatches,
        "mutations": mutation_results,
        "compile_command": command,
        "compiler": subprocess.check_output(
            [command[0], "--version"], text=True
        ).splitlines()[0],
        "files": {
            str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in output.rglob("*")
            if p.is_file()
        },
        "scope": "Typed, manually annotated, bounded action-budget research; no natural-language extraction or execution authorization",
        "training_admitted": False,
        "live_activation": False,
    }
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if mismatches:
        raise ValueError("Kernel disagrees with resource-matching oracle")
    print(
        json.dumps(
            {k: v for k, v in receipt.items() if k not in ("files", "compile_command")},
            indent=2,
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        os.umask(0o077)
        proof(args.output)
    else:
        unittest.main(argv=[__file__])
