import copy
import json
import unittest

import cast_event_eval as e


def fact(name="fireball", level=3, cost=1):
    return {"name": name, "document": [e.PRINTING, "fixture", "a" * 64],
            "valid_header": True, "assembly_sha256": "b" * 64, "level": level,
            "standard_cost": cost, "classification": {}, "casting_time": {}}


class EventExtractionTests(unittest.TestCase):
    def test_authored_frames_preserve_labels_and_independent_join_targets(self):
        for case in e.cases():
            frame = {key: {"value": value, "quote": case["quotes"][key]} for key, value in case["expected"].items()}
            values, spans = e.parse(json.dumps(frame), case["utterance"])
            self.assertEqual(values, case["expected"])
            self.assertEqual(e.join(values, [fact()])["kind"], e.EXPECTED_JOIN[case["id"]])
            for key, span in spans.items():
                if span:
                    self.assertEqual(case["utterance"].encode()[span["begin"]:span["end"]].decode(), case["quotes"][key])

    def test_missing_duplicate_unknown_value_or_unquoted_fact_rejects(self):
        case = e.cases()[0]
        frame = {key: {"value": value, "quote": case["quotes"][key]} for key, value in case["expected"].items()}
        bad = ["```json\n" + json.dumps(frame) + "\n```", json.dumps(frame)[:-1] + ', "actor":{}}']
        for key, value in (("value", "invalid"), ("quote", "absent"), ("quote", "")):
            changed = copy.deepcopy(frame)
            changed["actor"][key] = value
            bad.append(json.dumps(changed))
        for text in bad:
            with self.assertRaises(ValueError):
                e.parse(text, case["utterance"])

    def test_join_never_treats_missing_source_or_unknown_modifier_as_standard_cast(self):
        values = e.cases()[0]["expected"]
        self.assertEqual(e.join(values, [fact(level=0)])["kind"], "action_cantrip_claim")
        self.assertEqual(e.join(values, [fact(cost=0)])["kind"], "unsupported_casting_cost")
        for field in ("actor", "time"):
            self.assertEqual(e.join(values | {field: "unknown"}, [fact()])["kind"], "unresolved_event")
        for modifier in ("unknown", "quickened", "ritual"):
            self.assertEqual(e.join(values | {"modifier": modifier}, [fact()])["kind"], "modified_cast_unresolved")
        wrong = fact()
        wrong["document"][0] = "wrong-printing"
        for source in ([], [wrong], [fact(), fact()]):
            self.assertEqual(e.join(values, source)["kind"], "unresolved_spell")

    def test_request_excludes_labels_and_balances_prompt_order(self):
        template = {"model": "fixture", "messages": [], "temperature": 0.2}
        case = e.cases()[0]
        changed = copy.deepcopy(case)
        changed.update(expected={}, quotes={}, rationale="forbidden-gold", family="forbidden-family", id="forbidden-id")
        self.assertEqual(e.request(template, case, "basic", 0), e.request(template, changed, "basic", 0))
        self.assertEqual(e.request(template, case, "basic", 0)["messages"][1]["content"], case["utterance"])
        matrix = list(e.matrix())
        self.assertEqual(len(matrix), 32)
        for case in e.cases():
            for seed in (0, 1):
                self.assertEqual({p for s, c, p in matrix if s == seed and c["id"] == case["id"]}, set(e.POLICIES))

    def test_semantic_failure_is_separate_from_literal_quote_acceptance(self):
        case = e.cases()[0]
        frame = {key: {"value": value, "quote": case["quotes"][key]} for key, value in case["expected"].items()}
        frame["actor"]["value"] = "other"
        response = {"choices": [{"finish_reason": "stop", "message": {"content": json.dumps(frame)}}]}
        result = e.assess(response, case, [fact()])
        self.assertTrue(result["accepted"])
        self.assertFalse(result["exact_event"])
        self.assertEqual(result["incorrect_fields"], ["actor"])
        self.assertFalse(result["join_matches_authored_target"])


if __name__ == "__main__":
    unittest.main()
