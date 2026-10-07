"""Post hoc evidence-location ledger; no model calls or semantic admission."""

import argparse
from collections import Counter
import html
import json
import os
from pathlib import Path

import analyze_context_cast as analysis
import context_cast_eval as x


def relation(actual, authored):
    if actual is None:
        return 'both_empty' if authored is None else 'missing_quote'
    if authored is None:
        return 'authored_evidence_empty'
    a, b = (actual[k] for k in ('begin', 'end'))
    c, d = (authored[k] for k in ('begin', 'end'))
    if (a, b) == (c, d): return 'exact'
    if c <= a < b <= d: return 'inside_authored'
    if a <= c < d <= b: return 'contains_authored'
    if a < d and c < b: return 'partial_overlap'
    return 'disjoint'


def bound_text(span, raw, identity):
    if span is None: return ''
    begin, end = span['begin'], span['end']
    x.e.a.require(type(begin) is int and type(end) is int and 0 <= begin < end <= len(raw), 'Invalid evidence range')
    x.e.a.require(span.get('utterance_sha256', identity) == identity, 'Foreign utterance range')
    value = raw[begin:end]
    x.e.a.require(x.e.a.digest(value) == span['sha256'], 'Evidence range hash differs')
    return value.decode('utf-8')


def ledger(rows):
    """Caller verifies the completed archive; this function never changes a score."""
    cases = {case['id']: case for case in x.m.cases()}
    entries, reviews = [], []
    for row in rows:
        case = cases[row['case_id']]
        a = row['assessment']
        review = {k: row[k] for k in ('case_id', 'policy', 'seed')}
        review.update(accepted=a['accepted'], human_semantic_review='unreviewed', semantic_support_proven=False)
        if not a['accepted']:
            review['rejection'] = a['error']
            reviews.append(review)
            continue
        raw = case['utterance'].encode()
        identity = x.e.a.digest(raw)
        x.e.a.require(a['utterance_sha256'] == identity, 'Assessment utterance differs')
        expected = x.m.parse(json.dumps({'events': case['events']}), case['utterance'])
        for check in a['checks']:
            index, target = check['extracted_event'], check['target_event']
            event, authored = a['extracted'][index], expected[target]
            for field in x.e.FIELDS:
                actual_span, expected_span = event['quote_spans'][field], authored['quote_spans'][field]
                entry = {k: row[k] for k in ('case_id', 'policy', 'seed')}
                entry.update(extracted_event=index, target_event=target, field=field, utterance_sha256=identity,
                             utterance=case['utterance'], value=event['values'][field],
                             authored_value=authored['values'][field],
                             value_matches=event['values'][field] == authored['values'][field],
                             quote=bound_text(actual_span, raw, identity),
                             context=bound_text(event['context_spans'][field], raw, identity),
                             authored_evidence=bound_text(expected_span, raw, identity),
                             quote_range=actual_span, authored_range=expected_span,
                             location_relation=relation(actual_span, expected_span),
                             human_semantic_review='unreviewed', semantic_support_proven=False)
                entries.append(entry)
        review.update(missing_events=a['missing_events'], unmatched_events=a['unmatched_events'],
                      ambiguous_event_anchors=a['ambiguous_event_anchors'])
        reviews.append(review)
    counts = {policy: dict(Counter(e['location_relation'] for e in entries if e['policy'] == policy)) for policy in x.POLICIES}
    return {'scope': 'Post hoc diagnostic introduced after the first paired responses; original scores remain unchanged',
            'location_scope': 'Relations compare quote bytes with authored field evidence, not the larger supplied context',
            'coverage_scope': 'Matched events in accepted frames only; rejected frames and unmatched events remain explicit',
            'semantic_limit': 'A location match is not entailment; a disjoint quote can be valid alternative evidence',
            'training_admitted': False, 'human_review_complete': False, 'semantic_support_proven': False,
            'counts': counts, 'reviews': reviews, 'fields': entries}


def markdown(report):
    def cell(value):
        text = str(value)
        if len(text) > 180: text = text[:177] + '...'
        return html.escape(text).replace('|', '\\|').replace('\n', ' ').replace('\r', ' ').replace('`', '&#96;')
    lines = ['# Cast evidence review ledger', '',
             'This ledger supports manual review. No field has independent semantic approval.',
             'Location relations do not prove or disprove semantic support. Original model scores stay unchanged.',
             'Long cells are abbreviated. The JSON ledger preserves full text, byte ranges, and hashes.', '',
             '| Case | Condition | Event | Field | Value | Quote | Context | Authored evidence | Location |',
             '|---|---|---|---|---|---|---|---|---|']
    for e in report['fields']:
        values = [e['case_id'], e['policy'], e['target_event'], e['field'], e['value'], e['quote'], e['context'], e['authored_evidence'], e['location_relation']]
        lines.append('| ' + ' | '.join(cell(v) for v in values) + ' |')
    lines += ['', '## Rejected frames and coverage', '']
    for r in report['reviews']:
        if not r['accepted']:
            lines.append('- ' + cell(r['case_id']) + ' / ' + cell(r['policy']) + ': ' + cell(r['rejection']))
        elif r['missing_events'] or r['unmatched_events'] or r['ambiguous_event_anchors']:
            lines.append('- ' + cell(r['case_id']) + ' / ' + cell(r['policy']) + ': event coverage needs review.')
    return '\n'.join(lines)+'\n'


def collect(experiment, facts, output):
    output.mkdir()
    # The existing analyzer verifies completion, source pins, all requests and the C/reference results.
    verified = analysis.analyze(experiment, facts, output/'verified-analysis')
    rows = [json.loads(line) for line in (experiment/'results/trials.jsonl').read_text().splitlines()]
    report = ledger(rows)
    report.update(inference_source_revision=verified['source_revision'],
                  trials_manifest_sha256=x.e.a.digest((experiment/'results/manifest.json').read_bytes()),
                  review_tool_sha256=x.e.a.digest(Path(__file__).read_bytes()))
    x.e.a.write(output/'ledger.json', report)
    (output/'ledger.md').write_text(markdown(report))
    (output/'review-tool.py').write_bytes(Path(__file__).read_bytes())
    x.e.a.write(output/'receipt.json', {'files': {str(p.relative_to(output)): x.e.a.digest(p.read_bytes())
                                                for p in output.rglob('*') if p.is_file() and not p.name.startswith('._')}})
    return {'fields': len(report['fields']), 'counts': report['counts'], 'semantic_support_proven': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('experiment', 'facts', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(collect(args.experiment, args.facts, args.output), indent=2))
