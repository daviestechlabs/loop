"""Exercise serialized arrival orders and require semantic mutant detection."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ScheduleReplayTest(unittest.TestCase):
    def test_arrival_orders_and_fault_detection(self):
        source = Path(__file__).with_name('schedule_replay.c')
        with tempfile.TemporaryDirectory() as directory:
            for name, define in (
                ('reference', None), ('eager', 'REPLAY_MUTANT_EAGER'),
                ('stale', 'REPLAY_MUTANT_STALE'), ('duplicate', 'REPLAY_MUTANT_DUPLICATE'),
            ):
                with self.subTest(variant=name):
                    binary = Path(directory) / name
                    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                               '-O1', '-g', '-fsanitize=address,undefined']
                    if define:
                        command.append('-D' + define)
                    subprocess.run(command + [str(source), '-o', str(binary)],
                                   check=True, capture_output=True, timeout=60)
                    run = subprocess.run(
                        [str(binary)], capture_output=True, text=True, timeout=30,
                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'},
                    )
                    self.assertEqual(run.stderr, '')
                    rows = [json.loads(line) for line in run.stdout.splitlines()]
                    self.assertEqual(len(rows), 721)
                    summary = rows.pop()
                    self.assertEqual(len({tuple(row['order']) for row in rows}), 720)
                    self.assertEqual(summary['schedules'], 720)
                    self.assertEqual(summary['positive_schedules'], 360)
                    self.assertEqual(summary['failures'], sum(not r['passed'] for r in rows))
                    self.assertEqual(run.returncode, 1 if define else 0)
                    if define:
                        self.assertGreater(summary['failures'], 0)
                    else:
                        self.assertEqual(summary['failures'], 0)
                        self.assertTrue(all(r['prefix_failures'] == 0 for r in rows))
                        self.assertTrue(all(r['expected_commits'] == r['observed_commits'] for r in rows))


if __name__ == '__main__':
    unittest.main()
