"""Portable experiment: actual C question -> owned C state -> spoken C projection.

Fixture-authored observations are not transcripts or proof of human turn timing.
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "agents/platform-tools/c-ptools"))
from test_campaign import character, state

VOICE = Path(os.environ.get("DND_SCENE_BIN", ROOT / "voice/c-runtime/c-dnd-scene-test"))
STATE = Path(
    os.environ.get(
        "CAMPAIGN_CONTRACT_BIN",
        ROOT / "agents/platform-tools/c-ptools/c-ptools-campaign-contract",
    )
)
PROMPT = "I cast fireball, no wait is Mira still in the room?"


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


class SceneContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="scene-voice-contract-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "campaign.json"

    def process(self, args, *, stdin=None, good=True):
        result = subprocess.run(
            args, input=stdin, capture_output=True, text=True, timeout=10, check=False
        )
        self.assertEqual(result.stderr, "")
        if good:
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout.strip()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        return None

    def retained(self, *, presence="present", visibility="table", **changes):
        return state(
            characters=[character(name="Mira")],
            version=3,
            scene_observations={
                "version": 3,
                "scene_id": "hall",
                "entries": [
                    {
                        "character_id": "npc",
                        "presence": presence,
                        "visibility": visibility,
                        "source_turn_id": "mira-enters-hall",
                    }
                ],
            },
            **changes,
        )

    def answer(self, before, *, prompt=PROMPT, room="hall"):
        command = self.process([str(VOICE), "input", prompt, "table", room])
        self.path.write_text(encoded(before))
        result = json.loads(
            self.process(
                [str(STATE), str(self.path), "user", "scene-call", "scene"],
                stdin=command,
            )
        )
        self.assertEqual(result["state"], before)
        self.assertTrue(result["read_only"])
        self.assertEqual(self.path.read_text(), encoded(before))
        reply = self.project(result["output"], prompt=prompt, room=room)
        return reply, result["output"]

    def project(
        self, output, *, prompt=PROMPT, room="hall", call_id="scene-call", good=True
    ):
        return self.process(
            [
                str(VOICE),
                "output",
                output if isinstance(output, str) else encoded(output),
                prompt,
                "table",
                room,
                call_id,
            ],
            good=good,
        )

    def test_actual_c_pipeline_distinguishes_presence_absence_and_unknown(self):
        expectations = {
            "present": "The current scene record places Mira in this room.",
            "absent": "The current scene record says Mira is not in this room.",
            "unknown": "I can't confirm whether Mira is in this room.",
        }
        for presence, expected in expectations.items():
            for prompt in (
                PROMPT,
                "I cast Fireball. No, wait. Is Mira still in the room?",
                "Is Mira here?",
                "Is Eli here, no wait is Mira still here?",
                "Can you tell me whether Mira is still here?",
                "Check whether Mira is in this room.",
                "Do we know if Mira is still here?",
                "Please can you check if Mira is in the room?",
                "Tell me whether Mira is here.",
                "Before I cast Fireball, is Mira here?",
                "I cast Fireball. Actually, wait—can you check if Mira is here?",
            ):
                with self.subTest(presence=presence, prompt=prompt):
                    answer, output = self.answer(
                        self.retained(presence=presence), prompt=prompt
                    )
                    self.assertEqual(answer, expected)
                    self.assertEqual(output["presence"], presence)
                    self.assertNotIn("fireball", answer.lower())

    def test_evidence_limits_do_not_become_guessed_answers(self):
        missing = self.retained()
        del missing["scene_observations"]
        stale = self.retained()
        stale["version"] = 4
        duplicate = self.retained()
        duplicate["characters"].append(character(id="other", name="mira"))
        absent_character = self.retained()
        absent_character["characters"][0]["name"] = "Eli"
        for before in (
            missing,
            stale,
            duplicate,
            absent_character,
            self.retained(visibility="dm"),
            self.retained(status="archived"),
        ):
            for prompt in (PROMPT, "Can you check if Mira is in this room?"):
                with self.subTest(state=before, prompt=prompt):
                    answer, output = self.answer(before, prompt=prompt)
                    self.assertEqual(answer, "I can't confirm whether Mira is in this room.")
                    self.assertEqual(
                        (output["presence"], output["source_turn_id"]), ("unknown", "")
                    )
        self.assertEqual(
            self.answer(self.retained(), room="courtyard")[1]["presence"], "unknown"
        )
        wrong_owner = self.retained(owner_user_id="other")
        self.path.write_text(encoded(wrong_owner))
        command = self.process([str(VOICE), "input", PROMPT, "table", "hall"])
        self.process(
            [str(STATE), str(self.path), "user", "scene-call", "scene"],
            stdin=command,
            good=False,
        )

    def test_output_binding_and_private_or_impossible_claims_reject(self):
        _, valid = self.answer(self.retained())
        variants = []
        for key, values in {
            "operation": ["set_scene", "get_scene_presence", None],
            "operation_id": ["other", "", None],
            "campaign_id": ["other", ""],
            "scene_id": ["other", ""],
            "character_name": ["Eli", "mira", ""],
            "version": [0, -1, 1.5, "3", 9007199254740992],
            "status": ["archived", "deleted", None],
            "presence": ["yes", "", None],
            "source_turn_id": ["", "../secret", "bad\nsource"],
        }.items():
            for value in values:
                variants.append({**valid, key: value})
        for key in valid:
            item = copy.deepcopy(valid)
            del item[key]
            variants.append(item)
        variants.extend(
            [
                {**valid, "private": "secret"},
                {**valid, "presence": "unknown"},
                encoded(valid).replace(
                    '"presence":"present"', '"presence":"present","presence":"absent"'
                ),
                encoded(valid) + "trailer",
                encoded(valid)[:-1],
                "[]",
            ]
        )
        for output in variants:
            with self.subTest(output=output):
                self.project(output, good=False)
        self.project(valid, call_id="another-call", good=False)

    def test_language_boundary_does_not_claim_general_scene_understanding(self):
        for prompt in (
            "Is Mira within twenty feet?",
            "Will Mira still be here after I cast?",
            "Was Mira still in the room?",
            "Did Mira leave?",
            "Where is she?",
            "I cast fireball",
            "Is Mira here? I cast fireball.",
            "Can you check if Mira was here?",
            "Tell me whether Mira will be here.",
            "Check whether Mira is safe here.",
            "Do we know if she is still here?",
            "Mira asked, can you check if Eli is here?",
            "If I cast Fireball, actually wait—can you check if Mira is here?",
        ):
            self.process([str(VOICE), "input", prompt, "table", "hall"], good=False)


if __name__ == "__main__":
    unittest.main()
