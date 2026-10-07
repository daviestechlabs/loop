"""Measure C social shortcuts on mixed D&D intent. Offline development only."""
import argparse
import ctypes
import hashlib
import json
import platform
import shutil
import subprocess
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-no-swallowed-intent', action='store_true')
    parser.add_argument('--policy', choices=['legacy', 'whole-turn'], default='legacy')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    case_path = Path(__file__).with_name('shortcut-cases.json')
    cases = json.loads(case_path.read_text())
    assert cases['schema'] == 'turn-shortcut-cases/v1'
    assert cases['scope'] == 'development'
    assert len({row['id'] for row in cases['cases']}) == len(cases['cases'])
    for row in cases['cases']:
        assert isinstance(row['shortcut_allowed'], bool)
        assert isinstance(row['utterance'], str) and '\0' not in row['utterance']
    compiler = shutil.which('clang')
    if not compiler:
        raise SystemExit('Clang is required')
    args.output.mkdir(parents=True, exist_ok=False)
    sources = [
        'voice/cascade-router/internal/cascade/phatic_policy.c',
        'voice/cascade-router/internal/cascade/phatic_policy.h',
        'voice/cascade-router/internal/routeclassifier/route_classifier.c',
        'voice/cascade-router/internal/routeclassifier/route_classifier.h',
        'voice/cascade-router/internal/routeclassifier/route_classifier_model_v1.h',
        'voice/c-runtime/services/cascade_router_main.c',
    ]
    manifest = {}
    for name in sources + [str(case_path.relative_to(root)), str(Path(__file__).resolve().relative_to(root))]:
        data = (root / name).read_bytes()
        destination = args.output / 'source' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        manifest[name] = digest(data)
    library = args.output.resolve() / 'shortcut-kernels.so'
    source = args.output.resolve() / 'source'
    command = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-dynamiclib' if platform.system() == 'Darwin' else '-shared', '-fPIC',
               str(source / sources[0]), str(source / sources[2]), '-lm', '-o', str(library)]
    subprocess.run(command, check=True)
    kernel = ctypes.CDLL(str(library))
    kernel.phatic_policy_match_v1.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    kernel.phatic_policy_match_v1.restype = ctypes.c_uint32
    if args.policy == 'whole-turn':
        kernel.phatic_policy_match_v2.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        kernel.phatic_policy_match_v2.restype = ctypes.c_uint32
    kernel.phatic_policy_reply_v1.argtypes = [ctypes.c_uint32]
    kernel.phatic_policy_reply_v1.restype = ctypes.c_char_p
    kernel.route_classifier_packed_v1.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    kernel.route_classifier_packed_v1.restype = ctypes.c_uint64
    rows = []
    for case in cases['cases']:
        text = case['utterance'].encode()
        match = kernel.phatic_policy_match_v2 if args.policy == 'whole-turn' else kernel.phatic_policy_match_v1
        phatic = match(text, len(text)) & 255
        packed = kernel.route_classifier_packed_v1(text, len(text))
        assert packed != 2**64 - 1 and packed & 3 < 3
        shortcut = bool(phatic) or (args.policy == 'legacy' and packed & 3 == 0)
        rows.append({**case, 'phatic_reply': kernel.phatic_policy_reply_v1(phatic).decode(),
                     'classifier_route': ['answer', 'escalate', 'retrieve_then_escalate'][packed & 3],
                     'classifier_policy_flags': (packed >> 8) & 255,
                     'shortcut_without_context_overrides': shortcut,
                     'matches_development_label': shortcut == case['shortcut_allowed']})
    substantive = [r for r in rows if not r['shortcut_allowed']]
    controls = [r for r in rows if r['shortcut_allowed']]
    report = {
        'schema': 'turn-shortcut-probe/v1', 'scope': 'development', 'policy': args.policy,
        'source_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root)),
        'source_sha256': manifest, 'library_sha256': digest(library.read_bytes()),
        'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
        'build_command': command, 'rows': rows,
        'substantive_cases': len(substantive),
        'swallowed_intent_cases': sum(not r['matches_development_label'] for r in substantive),
        'social_controls': len(controls),
        'social_controls_passed': sum(r['matches_development_label'] for r in controls),
        'limitations': [
            'C kernels only. Cascade tool and governed retrieval overrides can bypass these kernels.',
            'Whole-turn mode models the cascade answer-label fallback; this probe does not execute the cascade process.',
            'No live route, audio transport, STT, generation, or game-state execution is tested.',
            'Cases expose development failures. They are not admitted training data or held-out quality evidence.',
        ],
    }
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ['substantive_cases', 'swallowed_intent_cases',
                                                 'social_controls', 'social_controls_passed']}))
    if args.require_no_swallowed_intent and any(not r['matches_development_label'] for r in rows):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
