"""Bounded authored case packs; expected labels never enter model requests."""

import json
import re


def load_pack(path):
    pack = json.loads(path.read_text())
    if pack.get("schema") != "dnd-dialogue-challenge/v1":
        raise ValueError("Unknown challenge schema")
    cases = pack.get("cases")
    if not isinstance(cases, list) or not 1 <= len(cases) <= 16:
        raise ValueError("Challenge needs one through sixteen cases")
    seen = set()
    for case in cases:
        if set(case) != {
            "case_id",
            "message",
            "scene",
            "expected_proposal",
            "review_criteria",
        }:
            raise ValueError("Unexpected case fields")
        name = case["case_id"]
        if (
            not isinstance(name, str)
            or not re.fullmatch(r"[a-z][a-z0-9_]{0,63}", name)
            or name in seen
        ):
            raise ValueError("Unsafe or duplicate case identity")
        seen.add(name)
        for key, maximum in (("message", 2048), ("review_criteria", 1024)):
            value = case[key]
            if (
                not isinstance(value, str)
                or not value.strip()
                or len(value.encode()) > maximum
                or "\x00" in value
            ):
                raise ValueError("Invalid case text")
        if case["scene"] not in ("hall", "courtyard") or case[
            "expected_proposal"
        ] not in ("held", "cleared"):
            raise ValueError("Invalid case scope or expected effect")
    return pack
