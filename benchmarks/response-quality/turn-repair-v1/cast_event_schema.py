"""Candidate vLLM structured-output request; independent semantic checks remain."""

import copy

from validate_cast_event_data import FIELDS


def response_format():
    return {
        "type": "json_schema",
        "json_schema": {
            "name": "waterdeep_cast_event_v1",
            "strict": True,
            "schema": {
                "type": "object",
                "additionalProperties": False,
                "required": list(FIELDS),
                "properties": {
                    field: {
                        "type": "object",
                        "additionalProperties": False,
                        "required": ["value", "quote"],
                        "properties": {
                            "value": {"type": "string", "enum": sorted(allowed)},
                            "quote": {"type": "string", "maxLength": 2048},
                        },
                    }
                    for field, allowed in FIELDS.items()
                },
            },
        },
    }


def constrained_request(baseline):
    if "response_format" in baseline or "structured_outputs" in baseline:
        raise ValueError("Baseline already constrains output")
    result = copy.deepcopy(baseline)
    result["response_format"] = response_format()
    return result
