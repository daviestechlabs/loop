"""Check that matched-context scoring cannot reward a fixed decision."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from context_pair_audit import analyze, audit
from semantic_eval import sha


def fixture():
    targets = [
        dict(action_state="eligible_for_governed_commit", presence="unknown",
             commit_requested=True, dialogue_act="confirm_prepared_action"),
        dict(action_state="held", presence="unknown", commit_requested=False,
             dialogue_act="reject_foreign_confirmation"),
    ]
    cases = [dict(case_id=str(i), group="new-turn-evaluation", target=t, system="same policy",
                  prompt=json.dumps(dict(utterance="Do it.", evidence=dict(owner="a", speaker=s))))
             for i, (s, t) in enumerate(zip(["a", "b"], targets))]
    trials = []
    for case in cases:
        response = json.dumps({**case["target"], "answer": "A proposal only."})
        trials.append(dict(case_id=case["case_id"], group=case["group"], condition="base",
                           response=response, response_sha256=sha(response.encode()), finish_reason="stop"))
    return dict(schema="waterdeep-mlx-ablation-response-review/v1", source_receipt_sha256="a" * 64,
                cases=cases, trials=trials)


class PairAuditTests(unittest.TestCase):
    def test_context_conditioned_decisions_pass(self):
        result = analyze(fixture())["conditions"]["base"]
        self.assertEqual(result["primary_members_correct"], 2)
        self.assertEqual(result["primary_pairs_correct"], 1)
        self.assertEqual(result["exact_groups_correct"], 1)

    def test_constant_action_gets_no_pair_credit(self):
        doc = fixture()
        doc["trials"][1].update({k: doc["trials"][0][k] for k in ["response", "response_sha256"]})
        result = analyze(doc)["conditions"]["base"]
        self.assertEqual(result["primary_members_correct"], 1)
        self.assertEqual(result["primary_pairs_correct"], 0)
        self.assertEqual(result["primary_groups_correct"], 0)

    def test_truncated_generation_is_not_a_pass(self):
        doc = fixture(); doc["trials"][1]["finish_reason"] = "length"
        result = analyze(doc)["conditions"]["base"]
        self.assertEqual(result["valid_members"], 1)
        self.assertEqual(result["primary_pairs_correct"], 0)

    def test_archive_damage_and_incomplete_matrix_fail_closed(self):
        for mutation in ["hash", "missing", "duplicate", "foreign", "partition"]:
            with self.subTest(mutation=mutation):
                doc = fixture()
                if mutation == "hash": doc["trials"][0]["response"] += " "
                if mutation == "missing": doc["trials"].pop()
                if mutation == "duplicate": doc["trials"].append(copy.deepcopy(doc["trials"][0]))
                if mutation == "foreign": doc["trials"][0]["case_id"] = "unknown"
                if mutation == "partition": doc["trials"][0]["group"] = "original-turn-development"
                with self.assertRaises(ValueError): analyze(doc)

    def test_different_words_systems_or_partitions_do_not_pair(self):
        for mutation in ["utterance", "system", "partition"]:
            with self.subTest(mutation=mutation):
                doc = fixture()
                if mutation == "utterance":
                    p=json.loads(doc["cases"][1]["prompt"]);p["utterance"]="Do another thing."
                    doc["cases"][1]["prompt"]=json.dumps(p)
                if mutation == "system": doc["cases"][1]["system"]="different policy"
                if mutation == "partition":
                    doc["cases"][1]["group"]="original-turn-development"
                    doc["trials"][1]["group"]="original-turn-development"
                with self.assertRaisesRegex(ValueError, "No matched"): analyze(doc)

    def test_contradictory_labels_without_context_change_rejected(self):
        doc=fixture();doc["cases"][1]["prompt"]=doc["cases"][0]["prompt"]
        with self.assertRaisesRegex(ValueError, "identical context"): analyze(doc)

    def test_evidence_binds_archive_receipt_and_counts(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);raw=json.dumps(fixture()).encode();r=root/"review.json";r.write_bytes(raw)
            meta=dict(schema="waterdeep-mlx-ablation-evidence/v1", response_review_sha256=sha(raw),
                      run=dict(receipt_sha256="a"*64), result=dict(cases=2,conditions=1,generations=2),
                      recipe=dict(model="test-only",revision="b"*40))
            e=root/"evidence.json";e.write_text(json.dumps(meta))
            self.assertEqual(audit(r,e)["new_generations"],0)
            for key in ["hash", "receipt", "counts"]:
                bad=copy.deepcopy(meta)
                if key=="hash":bad["response_review_sha256"]="0"*64
                if key=="receipt":bad["run"]["receipt_sha256"]="0"*64
                if key=="counts":bad["result"]["generations"]=3
                e.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):audit(r,e)


if __name__ == "__main__":
    unittest.main()
