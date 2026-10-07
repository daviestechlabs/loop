import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import context_cast_eval as x
import review_context_evidence as r
from test_context_cast_eval import events
from test_multi_cast_eval import FACTS, reply


class EvidenceReviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary, _ = x.c.compile_probe(Path(cls.tmp.name))
        cls.case = x.m.cases()[0]

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def row(self, rows, case=None):
        case = case or self.case
        response = reply(rows)
        return {'case_id': case['id'], 'policy': 'explicit_context', 'seed': 0, 'response': response,
                'assessment': x.assess(response, case, FACTS, 'explicit_context', self.binary)}

    def test_wrong_event_quote_is_disjoint_despite_matching_label_and_large_context(self):
        rows = events(self.case)
        rows[0]['status'].update(quote='completed', context=rows[1]['anchor'])
        row = self.row(rows)
        self.assertTrue(row['assessment']['exact_all_events'])
        before = copy.deepcopy(row)
        report = r.ledger([row])
        status = next(e for e in report['fields'] if e['target_event']==0 and e['field']=='status')
        self.assertEqual(status['location_relation'], 'disjoint')
        self.assertTrue(status['value_matches'])
        self.assertFalse(status['semantic_support_proven'])
        self.assertEqual(row, before)
        # A context covering both events must not substitute its range for the quoted range.
        rows[0]['spell'].update(quote='Mira completed Polymorph', context=self.case['utterance'])
        report = r.ledger([self.row(rows)])
        spell = next(e for e in report['fields'] if e['target_event']==0 and e['field']=='spell')
        self.assertEqual(spell['location_relation'], 'disjoint')

    def test_correct_substring_is_inside_but_not_semantically_approved(self):
        rows = events(self.case)
        rows[0]['status'].update(quote='completed', context=rows[0]['anchor'])
        report = r.ledger([self.row(rows)])
        status = next(e for e in report['fields'] if e['target_event']==0 and e['field']=='status')
        self.assertEqual(status['location_relation'], 'inside_authored')
        self.assertEqual(status['human_semantic_review'], 'unreviewed')
        self.assertFalse(report['human_review_complete'])

    def test_rejections_omissions_and_empty_frames_cannot_look_like_full_coverage(self):
        rows = events(self.case)
        rows[0]['status'].update(quote='completed', context='')
        rejected = self.row(rows)
        omitted = self.row(events(self.case)[1:])
        empty_case = next(c for c in x.m.cases() if c['id']=='scene_only_a')
        report = r.ledger([rejected, omitted, self.row([], empty_case)])
        self.assertEqual(len(report['fields']), 5)
        self.assertIn('rejection', report['reviews'][0])
        self.assertEqual(report['reviews'][1]['missing_events'], [0])
        self.assertFalse(report['semantic_support_proven'])

    def test_ranges_bind_hashes_and_identity_and_utf8_byte_boundaries(self):
        raw='Míra dit feu'.encode(); identity=x.e.a.digest(raw)
        begin=len('Míra dit '.encode()); end=len(raw)
        span={'begin':begin,'end':end,'sha256':x.e.a.digest(b'feu'),'utterance_sha256':identity}
        self.assertEqual(r.bound_text(span,raw,identity),'feu')
        for changed in (dict(span,begin=begin-1),dict(span,utterance_sha256='0'*64),dict(span,end=999)):
            with self.assertRaises(ValueError):r.bound_text(changed,raw,identity)
        row=self.row(events(self.case))
        row['assessment']['extracted'][0]['quote_spans']['actor']['sha256']='0'*64
        with self.assertRaises(ValueError):r.ledger([row])

    def test_relation_classes_and_markdown_escape_do_not_grant_semantic_credit(self):
        def span(a,b):return {'begin':a,'end':b}
        for actual,expected,label in [(None,None,'both_empty'),(None,span(1,3),'missing_quote'),
                                     (span(1,3),None,'authored_evidence_empty'),(span(1,3),span(1,3),'exact'),
                                     (span(2,3),span(1,4),'inside_authored'),(span(0,4),span(1,3),'contains_authored'),
                                     (span(0,2),span(1,3),'partial_overlap'),(span(3,4),span(1,3),'disjoint')]:
            self.assertEqual(r.relation(actual,expected),label)
        report=r.ledger([self.row(events(self.case))])
        report['fields'][0]['quote']='<script>alert(1)</script>|`x`\nnext'
        rendered=r.markdown(report)
        self.assertNotIn('<script>',rendered)
        self.assertIn('&lt;script&gt;',rendered)
        self.assertIn('\\|',rendered)

    def test_collector_requires_terminal_verified_archive(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(r.analysis,'analyze',side_effect=ValueError('Incomplete operator')):
            out=Path(tmp)/'review'
            with self.assertRaisesRegex(ValueError,'Incomplete'):
                r.collect(Path(tmp),Path(tmp),out)
            self.assertFalse((out/'ledger.json').exists())
            self.assertFalse((out/'receipt.json').exists())


if __name__=='__main__':unittest.main()
