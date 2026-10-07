import copy
from dataclasses import FrozenInstanceError
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import context_cast_eval as x
from analyze_context_cast import analyze, summarize
from test_multi_cast_eval import FACTS, reply


def events(case):
    result = copy.deepcopy(case["events"])
    for event in result:
        for field in x.e.FIELDS:
            event[field]["context"] = ""
    return result


class ContextCastTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary, _ = x.c.compile_probe(Path(cls.tmp.name))
        cls.case = x.m.cases()[0]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def assess(self, rows, case=None, policy="explicit_context"):
        return x.assess(reply(rows), case or self.case, FACTS, policy, self.binary)

    def test_all_authored_controls_match_both_conditions_without_losing_unknowns(self):
        for case in x.m.cases():
            for policy in x.POLICIES:
                with self.subTest(case=case["id"], policy=policy):
                    got = self.assess(events(case), case, policy)
                    self.assertTrue(got["exact_all_events"])
                    self.assertTrue(got["all_joins_match"])
                    self.assertFalse(got["entailment_proven"])
                    self.assertFalse(got["whole_history_proven"])
                    self.assertFalse(got["resource_state_authorized"])

    def test_explicit_context_resolves_ambiguity_and_carries_utterance_identity(self):
        rows = events(self.case)
        rows[0]["status"]["quote"] = "completed"
        self.assertFalse(self.assess(rows)["accepted"])
        rows[0]["status"]["context"] = rows[0]["anchor"]
        got = self.assess(rows)
        self.assertTrue(got["exact_all_events"])
        span = got["extracted"][0]["quote_spans"]["status"]
        self.assertEqual(span["utterance_sha256"], x.binding(self.case).sha256)
        self.assertEqual(self.case["utterance"].encode()[span["begin"]:span["end"]], b"completed")
        self.assertFalse(self.assess(rows, policy="global_quote")["accepted"])
        with self.assertRaisesRegex(ValueError, "identity"):
            x.Utterance(self.case["utterance"] + " changed", span["utterance_sha256"])
        with self.assertRaises(FrozenInstanceError):
            x.binding(self.case).text = "changed"

    def test_missing_repeated_wrong_scope_context_and_nul_reject(self):
        for context, quote in (("absent", "completed"), ("completed", "completed"),
                               (self.case["events"][0]["anchor"], "Polymorph"),
                               ("", "completed"), ("x\0", "x"), ("x" * 4097, "x")):
            with self.subTest(context=context[:30]):
                rows = events(self.case)
                rows[0]["status"].update(context=context, quote=quote)
                self.assertFalse(self.assess(rows)["accepted"])
        rows = events(self.case)
        rows[0]["status"].update(context=rows[0]["anchor"], quote="")
        self.assertFalse(self.assess(rows)["accepted"])
        rows[0]["modifier"].update(value="unknown", context=rows[0]["anchor"], quote="")
        self.assertFalse(self.assess(rows)["accepted"])

    def test_membership_does_not_prove_semantic_support_and_wrong_fields_stay_visible(self):
        rows = events(self.case)
        # Borrow the other event's words: the kernel can bind them but cannot prove their relevance.
        rows[0]["status"].update(quote="completed", context=rows[1]["anchor"])
        got = self.assess(rows)
        self.assertTrue(got["exact_all_events"])
        self.assertFalse(got["entailment_proven"])
        rows[0]["actor"]["value"] = "other"
        got = self.assess(rows)
        self.assertTrue(got["accepted"])
        self.assertTrue(got["all_joins_match"])
        self.assertFalse(got["exact_all_events"])
        self.assertEqual(got["checks"][0]["incorrect_fields"], ["actor"])

    def test_coverage_duplicate_anchors_and_merge_are_independent_of_membership(self):
        rows = events(self.case)
        omitted = self.assess(rows[1:])
        self.assertEqual(omitted["missing_events"], [0])
        self.assertFalse(omitted["exact_all_events"])
        self.assertFalse(self.assess(rows[::-1])["accepted"])
        self.assertFalse(self.assess([rows[0]] * 2)["accepted"])
        ghost = copy.deepcopy(rows[-1])
        ghost["anchor"] = "Both casts used no casting-time modifiers."
        extra = self.assess([*rows, ghost])
        self.assertEqual(extra["unmatched_events"], [2])
        merged = rows[:1]
        merged[0]["anchor"] = self.case["utterance"]
        self.assertEqual(self.assess(merged)["ambiguous_event_anchors"], [0])

    def test_utf8_ranges_use_bytes_and_duplicate_keys_and_overlaps_reject(self):
        utterance = x.Utterance("Míra dit feu. Lynn dit feu.", x.e.a.digest("Míra dit feu. Lynn dit feu.".encode()))
        event = {"anchor": "Míra dit feu.", **{k: {"value": next(iter(sorted(v))), "quote": "feu", "context": "Míra dit feu."} for k, v in x.e.FIELDS.items()}}
        got = x.parse(json.dumps({"events": [event]}), utterance, "explicit_context", self.binary)
        self.assertEqual(got[0]["quote_spans"]["actor"]["begin"], len("Míra dit ".encode()))
        for payload in ('{"events":[],"events":[]}', '{"events":null}', '{"events":[],"execute":true}', '```json\n{"events":[]}\n```'):
            with self.assertRaises(ValueError):
                x.parse(payload, utterance, "explicit_context", self.binary)
        repeated = x.Utterance("banana", x.e.a.digest(b"banana"))
        event["anchor"] = "ana"
        for field in x.e.FIELDS:
            event[field].update(context="", quote="banana")
        with self.assertRaisesRegex(ValueError, "status=-4"):
            x.parse(json.dumps({"events": [event]}), repeated, "explicit_context", self.binary)

    def test_kernel_disagreement_aborts_instead_of_becoming_model_rejection(self):
        with patch.object(x.c, "oracle", return_value=(0, 0, 0, 0, 0)):
            with self.assertRaisesRegex(RuntimeError, "disagrees"):
                self.assess(events(self.case))
        with patch.object(x.c, "execute", side_effect=ValueError("Malformed probe output")):
            with self.assertRaisesRegex(RuntimeError, "invalid output"):
                self.assess(events(self.case))

    def test_conditions_change_only_evidence_instruction_not_schema_or_sampling(self):
        template = {"model": "fixture", "temperature": 0.2}
        case = self.case
        poisoned = dict(case, events=[], id="secret", rationale="secret", expected_joins=[])
        a = x.request(template, case, "global_quote", 0)
        b = x.request(template, case, "explicit_context", 0)
        self.assertEqual(a, x.request(template, poisoned, "global_quote", 0))
        self.assertEqual(a["response_format"], b["response_format"])
        self.assertEqual(a["max_completion_tokens"], 3072)
        self.assertEqual(b["messages"][0]["content"], x.INSTRUCTION + x.EVIDENCE['explicit_context'])
        b['messages'] = a['messages']
        self.assertEqual(a, b)
        self.assertEqual([r[2] for r in list(x.matrix())[:4]], ["global_quote", "explicit_context", "explicit_context", "global_quote"])
        self.assertEqual(len(list(x.matrix())), 32)
        self.assertEqual(template, {"model": "fixture", "temperature": 0.2})

    def test_archive_rebuild_checks_request_assessment_and_rehashed_mutations(self):
        template = {"model": "fixture", "temperature": 0.2}
        by_text = {case['utterance']: case for case in x.m.cases()}
        class FakeClient:
            def __init__(self, endpoint, timeout_seconds):
                if timeout_seconds != 240: raise ValueError('Wrong timeout')
            def request(self, path, body=None):
                if path == '/v1/models': return {'data': [{'id': 'fixture'}]}
                return reply(events(by_text[body['messages'][1]['content']]))
        with tempfile.TemporaryDirectory() as tmp, patch.object(x.e, 'inputs', return_value=(FACTS, template)), patch.object(x.e.a, 'Client', FakeClient), patch('builtins.print'):
            root = Path(tmp)
            template_path = root/'model-template.json'
            template_path.write_text(json.dumps(template))
            output = root/'results'
            x.run('unused', root, root, 'pinned', template_path, output)
            self.assertEqual(x.verify(output, root, root, 'pinned', template_path)['exact_all_events'], 32)
            files = {'benchmarks/response-quality/turn-repair-v1/'+name: x.e.a.digest((output/name).read_bytes()) for name in x.SOURCES}
            files.update({'voice/c-runtime/common/'+name: x.e.a.digest((output/'kernel/common'/name).read_bytes()) for name in x.KERNEL_FILES})
            files['benchmarks/response-quality/turn-repair-v1/synthetic/multi-cast-20260928.json'] = x.e.a.digest((output/'cases.json').read_bytes())
            files['voice/c-runtime/tests/test_dnd_source_compiler.py'] = x.e.a.digest((output/'source-decoder.py').read_bytes())
            x.e.a.write(root/'source-identity.json', {'revision': 'a'*40, 'files': files})
            for phase in ('before', 'after'):
                x.e.a.write(root/('identity-'+phase+'.json'), {'uid': 'fixture'})
            x.e.a.write(root/'receipt.json', {'complete': True, 'stable_model_pod_identity': True,
                                              'source_revision': 'a'*40, 'facts_receipt_sha256': 'pinned',
                                              'results_manifest_sha256': x.e.a.digest((output/'manifest.json').read_bytes())})
            report = analyze(root, root, root/'analysis')
            self.assertEqual(report['conditions']['global_quote']['counts']['exact_all_events'], 16)
            self.assertEqual(report['conditions']['explicit_context']['counts']['exact_matched_events'], 29)
            self.assertFalse(report['entailment_proven'])
            exported = x.e.a.read(root/'analysis/anvil-learning-report.json')
            self.assertEqual((exported['status'], exported['scope'], len(exported['metrics'])), ('held', 'development', 12))
            self.assertEqual(exported['progress']['unit'], 'checks')
            original = (output/'trials.jsonl').read_bytes()
            manifest = x.e.a.read(output/'manifest.json')
            for kind in ('request', 'assessment'):
                rows = [json.loads(line) for line in original.splitlines()]
                if kind == 'request': rows[0]['request']['messages'][1]['content'] += ' changed'
                else: rows[0]['assessment']['exact_all_events'] = False
                (output/'trials.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
                manifest['files']['trials.jsonl'] = x.e.a.digest((output/'trials.jsonl').read_bytes())
                (output/'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, 'Request differs|Assessment differs'):
                    x.verify(output, root, root, 'pinned', template_path)


    def test_summary_retains_rejections_and_joins_that_hide_wrong_fields(self):
        rows = events(self.case)
        rows[0]['actor']['value'] = 'other'
        wrong = self.assess(rows)
        rows[0]['status']['quote'] = 'completed'
        rejected = self.assess(rows)
        summarized = summarize([{'policy': 'explicit_context', 'response': reply(events(self.case)), 'assessment': wrong, 'request_elapsed_ms': 5},
                                {'policy': 'explicit_context', 'response': reply(rows), 'assessment': rejected, 'request_elapsed_ms': 7}])
        self.assertEqual(summarized['explicit_context']['counts']['rejected'], 1)
        self.assertEqual(summarized['explicit_context']['counts']['exact_all_events'], 0)
        self.assertEqual(summarized['explicit_context']['counts']['joins_hiding_wrong_fields'], 1)


if __name__ == '__main__':
    unittest.main()
