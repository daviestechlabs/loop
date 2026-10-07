"""Verify the prospective context intervention and export held development evidence."""

import argparse
from collections import Counter
import datetime
import json
import os
from pathlib import Path
import statistics
import uuid

import context_cast_eval as x


def summarize(rows):
    result = {}
    for policy in x.POLICIES:
        selected = [r for r in rows if r['policy'] == policy]
        counts = Counter(requests=len(selected), accepted=0, exact_all_events=0, all_joins_match=0,
                         accepted_nonempty_frames=0, exact_matched_events=0, missing_events=0,
                         unmatched_events=0, ambiguous_anchors=0, joins_hiding_wrong_fields=0,
                         explicit_context_fields=0)
        fields = Counter()
        for row in selected:
            a = row['assessment']
            if not a['accepted']: continue
            counts['accepted'] += 1
            counts['accepted_nonempty_frames'] += bool(a['extracted'])
            counts['exact_all_events'] += a['exact_all_events']
            counts['all_joins_match'] += a['all_joins_match']
            for key, source in (('missing_events', 'missing_events'), ('unmatched_events', 'unmatched_events'), ('ambiguous_anchors', 'ambiguous_event_anchors')):
                counts[key] += len(a[source])
            for check in a['checks']:
                fields.update(check['incorrect_fields'])
                counts['exact_matched_events'] += check['exact_event']
                counts['joins_hiding_wrong_fields'] += check['join_matches_authored_target'] and not check['exact_event']
            frame = json.loads(x.e.a.answer(row['response']), object_pairs_hook=x.e.a.unique_object)
            counts['explicit_context_fields'] += sum(bool(event[f]['context']) for event in frame['events'] for f in x.e.FIELDS)
        counts['rejected'] = len(selected) - counts['accepted']
        result[policy] = {'counts': dict(counts), 'incorrect_fields': dict(fields),
                          'median_request_ms': statistics.median(r['request_elapsed_ms'] for r in selected) if selected else None}
    return result


def analyze(experiment, facts, output):
    root = experiment/'results'
    receipt = x.e.a.read(experiment/'receipt.json')
    identity = x.e.a.read(experiment/'source-identity.json')
    x.e.a.require(receipt['complete'] and receipt['stable_model_pod_identity'], 'Incomplete operator')
    x.e.a.require(x.e.a.read(experiment/'identity-before.json') == x.e.a.read(experiment/'identity-after.json'), 'Pod identity changed')
    x.e.a.require(receipt['source_revision'] == identity['revision'], 'Source revision differs')
    x.e.a.require(receipt['results_manifest_sha256'] == x.e.a.digest((root/'manifest.json').read_bytes()), 'Manifest pin differs')
    for name in x.SOURCES:
        x.e.a.require(identity['files']['benchmarks/response-quality/turn-repair-v1/'+name] == x.e.a.digest((root/name).read_bytes()), 'Source snapshot differs')
    for name in x.KERNEL_FILES:
        x.e.a.require(identity['files']['voice/c-runtime/common/'+name] == x.e.a.digest((root/'kernel/common'/name).read_bytes()), 'Kernel snapshot differs')
    x.e.a.require(identity['files']['benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json'] == x.e.a.digest((root/'cases.json').read_bytes()), 'Case snapshot differs')
    x.e.a.require(identity['files']['voice/c-runtime/tests/test_dnd_source_compiler.py'] == x.e.a.digest((root/'source-decoder.py').read_bytes()), 'Decoder snapshot differs')
    verified = x.verify(root, experiment/'source', facts, receipt['facts_receipt_sha256'], experiment/'model-template.json')
    rows = [json.loads(line) for line in (root/'trials.jsonl').read_text().splitlines()]
    report = {'schema': 'waterdeep-context-cast-analysis/v1', 'source_revision': receipt['source_revision'],
              'verification': verified, 'conditions': summarize(rows),
              'reviews': [{k: r[k] for k in ('case_id', 'policy', 'seed', 'assessment', 'request_elapsed_ms')} for r in rows],
              'private_artifacts': {str(p): x.e.a.digest(p.read_bytes()) for p in
                                    (experiment/'receipt.json', experiment/'source-identity.json', root/'manifest.json')},
              'analyzer_sha256': x.e.a.digest(Path(__file__).read_bytes()),
              'scope': 'Sixteen visible Codex-authored cases in eight families; not independent evaluation',
              'timing_scope': 'Complete request duration through a loopback tunnel; not TTFA',
              'coverage_scope': 'Missing/extra event and field counts cover accepted frames only; rejections remain separate failures',
              'label_review': 'Codex self-review; independent human review absent',
              'evidence_scope': 'C and independent reference prove membership; wrong-event evidence can still bind',
              'entailment_proven': False, 'whole_history_proven': False, 'resource_state_authorized': False,
              'training_admitted': False, 'production_path': False}
    output.mkdir()
    x.e.a.write(output/'report.json', report)
    (output/'analyzer.py').write_bytes(Path(__file__).read_bytes())
    anvil = {'schemaVersion': 'anvil-learning-report/v1', 'goalId': 'cohesive-homelab-models-v1',
             'trackId': 'cohesion-speed', 'runId': str(uuid.uuid4()), 'sequence': 1,
             'recordedAt': datetime.datetime.now(datetime.UTC).isoformat().replace('+00:00', 'Z'),
             'title': 'Waterdeep: explicit cast-evidence contexts', 'status': 'held', 'scope': 'development',
             'sourceRevision': receipt['source_revision'],
             'summary': 'Paired real-model evidence localization with C and independent byte checks. Exact labels do not prove quote relevance, entailment, history completeness, or authority. No training or activation.',
             'progress': {'completed': len(rows), 'total': len(rows), 'unit': 'checks'},
             'metrics': [{'name': policy+' '+key, 'value': values['counts'][key], 'unit': 'count', 'direction': direction}
                         for policy, values in report['conditions'].items()
                         for key, direction in (('exact_all_events', 'higher'), ('accepted_nonempty_frames', 'higher'),
                                                ('missing_events', 'lower'), ('unmatched_events', 'lower'),
                                                ('joins_hiding_wrong_fields', 'lower'), ('rejected', 'lower'))],
             'blockers': ['Labels lack independent human review.', 'Wrong-event evidence can satisfy byte membership.',
                          'Claims do not establish complete history or authorized resources.', 'Human voice acceptance remains open.'],
             'evidence': [{'name': 'report.json', 'sha256': x.e.a.digest((output/'report.json').read_bytes())}]}
    x.e.a.write(output/'anvil-learning-report.json', anvil)
    x.e.a.write(output/'receipt.json', {'files': {p.name: x.e.a.digest(p.read_bytes()) for p in output.iterdir()
                                                if p.is_file() and not p.name.startswith('._')}})
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('experiment', 'facts', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(analyze(args.experiment, args.facts, args.output)['conditions'], indent=2))
