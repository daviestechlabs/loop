"""Canonical protobuf interoperability and hostile request admission in both C codecs."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROTO = ROOT / "contracts/handler-base/proto"


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def field(number, value):
    return varint(number * 8 + 2) + varint(len(value)) + value


def encode(message, text):
    return subprocess.run(
        [
            "protoc",
            f"--proto_path={PROTO}",
            f"--encode=messages.v1.{message}",
            "messages/v1/messages.proto",
        ],
        input=text.encode(),
        capture_output=True,
        check=True,
    ).stdout


class InitiativeWire(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="c-initiative-wire-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.codecs = []
        common = [
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wshadow",
            "-Wconversion",
            "-Wsign-conversion",
            "-Wstrict-prototypes",
            "-Wmissing-prototypes",
            "-Wformat=2",
            "-Wundef",
            "-Wvla",
        ]
        if os.environ.get("SANITIZE") == "1":
            common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        fixture = "voice/c-runtime/tests/initiative_wire_fixture.c"
        for name, sources in [
            (
                "product",
                [
                    "-DPRODUCT_WIRE",
                    "contracts/handler-base/c-pb/pb_msg.c",
                    "contracts/handler-base/c-pb/pb_wire.c",
                ],
            ),
            ("runtime", ["voice/c-runtime/wire/pb_min.c"]),
        ]:
            binary = Path(cls.directory.name) / name
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc"))
                + common
                + [fixture]
                + sources
                + ["-lm", "-o", str(binary)],
                cwd=ROOT,
                check=True,
            )
            cls.codecs.append(binary)

    def check(self, wire, admitted):
        outputs = []
        for codec in self.codecs:
            with self.subTest(codec=codec.name):
                result = subprocess.run(
                    [str(codec)], input=wire, capture_output=True, timeout=5
                )
                self.assertEqual(result.returncode, 0 if admitted else 1, result.stderr)
                outputs.append(result.stdout)
        return outputs

    def request(self, count=1, **changes):
        values = {
            "campaign_version": 2,
            "expected_version": 0,
            "operation_id": "saved-roll",
            **changes,
        }
        text = "\n".join(f"{key}: {json.dumps(value)}" for key, value in values.items())
        for i in range(count):
            text += f'\nselections {{character_id: "pc-{i}" expression: "2d20kh1+5"}}'
        return encode("DndInitiativeRequest", text)

    def test_canonical_one_and_64_choices_preserve_operation_versions_and_audio_stages(
        self,
    ):
        base = encode(
            "TurnStartRequest",
            'request_id: "attempt-1" user_id: "owner" session_id: "table" '
            'text: "Roll initiative for the party." audio_committed_at_ms: 100 '
            "stt_request_received_at_ms: 101 stt_provider_request_started_at_ms: 102 "
            "stt_provider_ready_at_ms: 103 stt_transcript_published_at_ms: 110",
        )
        for count in (1, 64):
            for campaign, encounter in [(2, 0), (9007199254740991, 9007199254740990)]:
                wire = base + field(
                    18,
                    self.request(
                        count, campaign_version=campaign, expected_version=encounter
                    ),
                )
                for codec, output in zip(
                    self.codecs, self.check(wire, True), strict=True
                ):
                    text = subprocess.run(
                        [
                            "protoc",
                            f"--proto_path={PROTO}",
                            "--decode=messages.v1.TurnStartRequest",
                            "messages/v1/messages.proto",
                        ],
                        input=output,
                        capture_output=True,
                        check=True,
                    ).stdout.decode()
                    self.assertEqual(text.count("selections {"), count)
                    self.assertIn('operation_id: "saved-roll"', text)
                    if codec.name == "runtime":
                        # Prepared audio stages originate downstream of the product HTTP edge.
                        self.assertIn("audio_committed_at_ms: 100", text)
                        self.assertIn("stt_transcript_published_at_ms: 110", text)
                    self.assertIn(f"campaign_version: {campaign}", text)
                    if encounter:
                        self.assertIn(f"expected_version: {encounter}", text)

    def test_rejects_ambiguous_invalid_or_truncated_nested_requests(self):
        good = self.request()
        selection = encode(
            "DndInitiativeSelection", 'character_id: "pc-0" expression: "2d20kh1+5"'
        )
        invalid = [
            b"",
            self.request(0),
            self.request(65),
            good + field(3, selection),
            self.request(campaign_version=0),
            self.request(expected_version=-1),
            self.request(campaign_version=9007199254740992),
            self.request(expected_version=9007199254740991),
            self.request(operation_id=""),
            self.request(operation_id="x" * 65),
            self.request(operation_id="bad/id"),
            self.request(operation_id="nul\0id"),
            good + b"\x08\x02",
            good + b"\x10\x00\x10\x00",
            good + field(4, b"saved-roll"),
            good + b"\x28\x01",
            good.replace(b"\x08\x02", b"\x08\x82\x00", 1),
            good.replace(b"2d20kh1+5", b"2d20kh1+0"),
            good.replace(selection, selection[:-1]),
            b"\x08\x02"
            + field(3, selection + field(1, b"pc-0"))
            + field(4, b"saved-roll"),
            b"\x08\x02"
            + field(3, selection + field(9, b"unknown"))
            + field(4, b"saved-roll"),
        ]
        base = field(1, b"attempt-1")
        for candidate in invalid:
            self.check(base + field(18, candidate), False)
        for length in range(len(good)):
            self.check(base + field(18, good[:length]), False)
        self.check(base + field(18, good) + field(18, good), False)
        self.check(base + varint(18 * 8) + b"\x01", False)

    def test_encounter_actions_roundtrip_and_hostile_fields(self):
        base = field(1, b"attempt-action")
        for operation in range(1, 7):
            values = {
                "operation": operation,
                "operation_id": "action-1",
                "expected_version": 2,
            }
            if operation not in (1, 6):
                values["participant_id"] = "aria"
            if operation in (2, 3):
                values["amount"] = 7
            if operation in (4, 5):
                values["condition"] = 11

            def request(**changes):
                return encode(
                    "DndEncounterAction",
                    " ".join(
                        f"{key}: {json.dumps(value)}"
                        for key, value in {**values, **changes}.items()
                    ),
                )

            good = request()
            for output in self.check(base + field(20, good), True):
                self.assertTrue(output.startswith(base))
                self.assertTrue(output.endswith(field(20, good)))
            invalid = [
                request(operation=0),
                request(operation=7),
                request(expected_version=0),
                request(expected_version=-1),
                request(expected_version=9007199254740991),
                request(operation_id="bad/id"),
                request(operation_id="x" * 65),
                request(operation_id="nul\0id"),
                good + field(2, b"duplicate"),
                good + b"\x40\x01",
                good + b"\x08\x01",
                good.replace(bytes([8, operation]), bytes([8, operation | 128, 0]), 1),
            ]
            if operation in (1, 6):
                invalid += [
                    request(participant_id="aria"),
                    request(amount=1),
                    request(condition=1),
                    request(damage_type=1),
                ]
            elif operation in (2, 3):
                invalid += [
                    request(amount=0),
                    request(amount=100001),
                    request(condition=1),
                    request(participant_id=""),
                ]
                invalid += [request(damage_type=14 if operation == 2 else 1)]
            else:
                invalid += [
                    request(condition=0),
                    request(condition=15),
                    request(amount=1),
                    request(damage_type=1),
                ]
            for bad in invalid:
                self.check(base + field(20, bad), False)
            # Every prefix before the last required scalar is incomplete.
            for size in range(len(good)):
                self.check(base + field(20, good[:size]), False)
            self.check(base + varint(20 * 8) + b"\x01", False)
            for other in [
                field(18, self.request()),
                field(20, good),
                field(
                    19,
                    encode(
                        "DndCampaignRequest",
                        'operation: 1 operation_id: "create" campaign {name: "Table" ruleset: "5e"}',
                    ),
                ),
            ]:
                self.check(base + field(20, good) + other, False)
                self.check(base + other + field(20, good), False)

    def test_campaign_setup_roundtrip_and_hostile_fields(self):
        base = field(1, b"attempt-setup")
        create = encode(
            "DndCampaignRequest",
            'operation: 1 operation_id: "save-create" campaign {name: "Table" ruleset: "5e"}',
        )
        character = (
            'id: "pc" name: "Aria" kind: 1 species: "elf" class_name: "rogue" '
            "level: 3 armor_class: 14 max_hp: 23 ability_scores {strength: 8 dexterity: 16 "
            "constitution: 12 intelligence: 13 wisdom: 11 charisma: 14}"
        )
        add = encode(
            "DndCampaignRequest",
            'operation: 2 operation_id: "save-add" expected_version: 1 character {'
            + character
            + "}",
        )
        for request in (create, add):
            for output in self.check(base + field(19, request), True):
                decoded = subprocess.run(
                    [
                        "protoc",
                        f"--proto_path={PROTO}",
                        "--decode=messages.v1.TurnStartRequest",
                        "messages/v1/messages.proto",
                    ],
                    input=output,
                    capture_output=True,
                    check=True,
                ).stdout.decode()
                self.assertIn("dnd_campaign {", decoded)
                self.assertIn('operation_id: "save-', decoded)
                if request == add:
                    self.assertIn("dexterity: 16", decoded)
                    self.assertIn("max_hp: 23", decoded)
            for size in range(len(request)):
                self.check(base + field(19, request[:size]), False)
            self.check(base + field(19, request) + field(19, request), False)
            self.check(base + field(19, request) + field(18, self.request()), False)
            self.check(base + field(18, self.request()) + field(19, request), False)
            self.check(base + field(19, request + field(2, b"duplicate")), False)
            self.check(base + field(19, request + field(6, b"unknown")), False)
        invalid = [
            create + b"\x18\x01",
            add + b"\x08\x02",
            create.replace(b"Table", b"Ta\x00le"),
            create.replace(b"Table", b"Ta\xc0\x80e"),
            create.replace(b"Table", b"Tab\ned"),
            create.replace(b"\x08\x01", b"\x08\x81\x00", 1),
            encode(
                "DndCampaignRequest",
                'operation: 2 operation_id: "save-add" character {' + character + "}",
            ),
            encode(
                "DndCampaignRequest",
                'operation: 2 operation_id: "save-add" expected_version: 9007199254740991 character {'
                + character
                + "}",
            ),
            encode(
                "DndCampaignRequest",
                'operation: 2 operation_id: "save-add" expected_version: 1 character {'
                + character.replace("strength: 8", "strength: 0")
                + "}",
            ),
            encode(
                "DndCampaignRequest",
                'operation: 1 operation_id: "save-add" character {' + character + "}",
            ),
        ]
        for request in invalid:
            self.check(base + field(19, request), False)
        self.check(base + varint(19 * 8) + b"\x01", False)


if __name__ == "__main__":
    unittest.main()
