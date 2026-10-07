"""Structural checks for authored cast-event development pairs, not admission."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

from verify_coupled_dialogue import require

FIELDS = {
    "actor": {"speaker", "other", "unknown"},
    "time": {"this_turn", "previous_turn", "next_turn", "unknown"},
    "status": {"occurred", "proposed", "negated", "uncertain"},
    "spell": {"fireball", "polymorph", "sanctuary", "shield_of_faith", "unresolved"},
    "modifier": {"none", "quickened", "ritual", "unknown"},
}


def validate(cases, conditions):
    expected = {c["id"]: c for c in conditions}
    require(len(expected) == len(conditions), "Duplicate condition ID")
    require(Counter(c["id"] for c in cases) == Counter(expected.keys()), "Case IDs differ")
    require(len({c["utterance"] for c in cases}) == len(cases), "Duplicate utterance")
    groups = defaultdict(list)
    for case in cases:
        require(set(case) == {"id", "family", "utterance", "expected", "quotes", "rationale"}, "Case fields differ")
        target = expected[case["id"]]
        require(case["family"] == target["family"], "Family differs")
        require(case["expected"] == target["expected"], "Authored target differs")
        require(set(case["expected"]) == set(FIELDS) == set(case["quotes"]), "Label fields differ")
        text = case["utterance"]
        require(isinstance(text, str) and text.isascii() and 20 <= len(text.split()) <= 65, "Utterance bounds differ")
        require(isinstance(case["rationale"], str) and bool(case["rationale"].strip()), "Missing rationale")
        for field, value in case["expected"].items():
            require(value in FIELDS[field], "Invalid label")
            quote = case["quotes"][field]
            require(isinstance(quote, str), "Invalid quote")
            require(bool(quote) or value in {"unknown", "unresolved"}, "Known label lacks quote")
            require(not quote or text.count(quote) == 1, "Quote is absent or ambiguous")
        groups[case["family"]].append(case)
    for family, pair in groups.items():
        require(len(pair) == 2, "Family is not a pair")
        fields = {expected[c["id"]]["contrast_field"] for c in pair}
        require(len(fields) == 1 and fields <= FIELDS.keys(), "Contrast field differs")
        changed = {key for key in FIELDS if pair[0]["expected"][key] != pair[1]["expected"][key]}
        require(changed == fields, "Pair changes unintended labels")
    return {"cases": len(cases), "families": len(groups), "literal_quote_checks": "passed",
            "semantic_entailment_proven": False, "independent_evaluation": False, "training_admitted": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    data = json.loads((root / "cases.json").read_text())
    require(data["scope"] == "development" and data["training_admitted"] is False, "Invalid dataset scope")
    print(json.dumps(validate(data["cases"], json.loads((root / "conditions.json").read_text()))))
