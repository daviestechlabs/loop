#!/usr/bin/env python3
"""Compare two WebTransport parser shared libraries over deterministic corpora."""

from __future__ import annotations

import argparse
import ctypes
import itertools
import random
from pathlib import Path


class TurnRequest(ctypes.Structure):
    _fields_ = [
        ("request_id", ctypes.c_char * 128),
        ("session_id", ctypes.c_char * 128),
        ("text", ctypes.c_char * 2048),
        ("identity_token", ctypes.c_char * 1024),
        ("enable_rag", ctypes.c_int),
        ("enable_tts", ctypes.c_int),
        ("audio_first", ctypes.c_int),
    ]


class Control(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("request_id", ctypes.c_char * 128),
        ("reason", ctypes.c_char * 128),
        ("packet_count", ctypes.c_uint32),
        ("audio_bytes", ctypes.c_uint32),
    ]


class Datagram(ctypes.Structure):
    _fields_ = [
        ("session_stream_id", ctypes.c_uint64),
        ("payload", ctypes.POINTER(ctypes.c_uint8)),
        ("payload_len", ctypes.c_size_t),
        ("sequence", ctypes.c_uint32),
        ("is_audio", ctypes.c_int),
        ("is_control", ctypes.c_int),
        ("control", Control),
    ]


REQUEST_FIELDS = (
    b'"request_id":"req-diff"',
    b'"session_id":"session-diff"',
    b'"text":"Roll initiative"',
    b'"identity_token":"vat1.payload.signature"',
    b'"enable_rag":true',
    b'"enable_tts":true',
    b'"metadata":{"input_mode":"text","nested":[1,true,null]}',
)
REQUEST_BASE = b"{" + b",".join(REQUEST_FIELDS) + b"}"
REQUEST_WORD_PREFIX = b'{"ignored":"'
REQUEST_WORD_SUFFIX = (
    b'","request_id":"word-diff","text":"hello",'
    b'"identity_token":"vat1.payload.signature"}'
)
REQUEST_EDGES = (
    b"",
    b"{}",
    b"[]",
    b"[{}]",
    b'"object"',
    b"0",
    b"-1",
    b"true",
    b"false",
    b"null",
    b'{"":1,"":2,"request_id":"r","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b'{"a":1,"a":1,"request_id":"r","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b'{"a":1,"A":1,"request_id":"r","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b'{"a":1,"\\u0061":1,"request_id":"r","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b'{"ignored":{"":1,"":2},"request_id":"r","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b'{"ignored":[{"x":1,"x":2}],"request_id":"r","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b'{"request_id":"r","text":"x","identity_token":"vat1.p.s"} trailing',
    b' { "request_id" : "r", "text" : "x", '
    b'"identity_token" : "vat1.p.s" } \r\n',
    b'{"request_id":"r","text":"\\ud83d\\udc09",'
    b'"identity_token":"vat1.p.s"}',
    b'{"request_id":"r","text":"\\ud800","identity_token":"vat1.p.s"}',
    b'{"request_id":"raw-utf8","text":"Caf\xc3\xa9 \xf0\x9f\x90\x89",'
    b'"identity_token":"vat1.p.s"}',
    b'{"request_id":"invalid-utf8","text":"bad \xc3(",'
    b'"identity_token":"vat1.p.s"}',
    b'{"ignored":"bad \xe2(\xa1","request_id":"invalid-ignored",'
    b'"text":"x","identity_token":"vat1.p.s"}',
    b'{"\xc3(":1,"request_id":"invalid-key","text":"x",'
    b'"identity_token":"vat1.p.s"}',
    b"{" + b'"n":{' * 8 + b'"v":0' + b"}" * 8
    + b',"request_id":"r","text":"x","identity_token":"vat1.p.s"}',
)
CONTROL_PREFIX = b"\x01DTVA1:"
CONTROL_EDGES = (
    b'{"type":"cancel","request_id":"req","reason":"client_interrupt"}',
    b'{"type":"end","packet_count":500,"audio_bytes":320000}',
    b'{"type":"interrupt","request_id":"req"}',
    b'{"type":"cancel","type":"end","request_id":"req"}',
    b'{"ignored":{"same":1,"same":2},"type":"cancel",'
    b'"request_id":"req"}',
    b'{"":1,"":2,"type":"cancel","request_id":"req"}',
    b"[]",
    b"0",
    b"true",
    b"null",
    b"{}",
)
MUTATION_ALPHABET = (
    b'{}[],:"\\ abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
    b"0123456789\t\r\n\x00\x7f\x80\xff"
)


def load_library(path: Path):
    library = ctypes.CDLL(str(path))
    library.gw_wt_turn_request_parse.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(TurnRequest),
    ]
    library.gw_wt_turn_request_parse.restype = ctypes.c_int
    try:
        active_parser = library.gw_wt_turn_request_parse_active
    except AttributeError:
        active_parser = None
    if active_parser is not None:
        active_parser.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(TurnRequest),
        ]
        active_parser.restype = ctypes.c_int
    library.active_request_parser = active_parser
    library.gw_wt_datagram_parse.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(Datagram),
    ]
    library.gw_wt_datagram_parse.restype = ctypes.c_int
    return library


def input_buffer(data: bytes):
    buffer = (ctypes.c_uint8 * max(1, len(data)))()
    if data:
        ctypes.memmove(buffer, data, len(data))
    return buffer


def structure_bytes(value: ctypes.Structure) -> bytes:
    return ctypes.string_at(ctypes.byref(value), ctypes.sizeof(value))


def request_values(
    value: TurnRequest,
) -> tuple[bytes, bytes, bytes, bytes, int, int, int]:
    return (
        value.request_id,
        value.session_id,
        value.text,
        value.identity_token,
        value.enable_rag,
        value.enable_tts,
        value.audio_first,
    )


def compare_request(libraries, data: bytes, case: str) -> None:
    buffer = input_buffer(data)
    results = []
    for library in libraries:
        output = TurnRequest()
        ctypes.memset(ctypes.byref(output), 0xA5, ctypes.sizeof(output))
        status = library.gw_wt_turn_request_parse(
            buffer, len(data), ctypes.byref(output)
        )
        results.append((status, structure_bytes(output)))
    if results[0] != results[1]:
        raise AssertionError(
            f"request mismatch case={case} length={len(data)} data={data!r} "
            f"baseline={results[0][0]} candidate={results[1][0]}"
        )
    for index, library in enumerate(libraries):
        if library.active_request_parser is None:
            continue
        active_output = TurnRequest()
        ctypes.memset(
            ctypes.byref(active_output), 0xA5, ctypes.sizeof(active_output)
        )
        active_status = library.active_request_parser(
            buffer, len(data), ctypes.byref(active_output)
        )
        if active_status != results[index][0]:
            raise AssertionError(
                f"active request status mismatch case={case} length={len(data)} "
                f"checked={results[index][0]} active={active_status}"
            )
        if active_status == 0:
            checked_output = TurnRequest.from_buffer_copy(results[index][1])
            if request_values(active_output) != request_values(checked_output):
                raise AssertionError(
                    f"active request value mismatch case={case} "
                    f"length={len(data)}"
                )


def compare_datagram(libraries, data: bytes, case: str) -> None:
    buffer = input_buffer(data)
    results = []
    for library in libraries:
        output = Datagram()
        ctypes.memset(ctypes.byref(output), 0xA5, ctypes.sizeof(output))
        status = library.gw_wt_datagram_parse(
            buffer, len(data), ctypes.byref(output)
        )
        results.append((status, structure_bytes(output)))
    if results[0] != results[1]:
        raise AssertionError(
            f"datagram mismatch case={case} length={len(data)} data={data!r} "
            f"baseline={results[0][0]} candidate={results[1][0]}"
        )


def mutate(
    rng: random.Random,
    source: bytes,
    max_operations: int,
    max_span: int,
) -> bytes:
    value = bytearray(source)
    for _ in range(1 + rng.randrange(max_operations)):
        action = rng.randrange(4)
        if action == 0 and value:
            value[rng.randrange(len(value))] = MUTATION_ALPHABET[
                rng.randrange(len(MUTATION_ALPHABET))
            ]
        elif action == 1 and value:
            start = rng.randrange(len(value))
            width = 1 + rng.randrange(min(max_span, len(value) - start))
            del value[start:start + width]
        elif action == 2:
            position = rng.randrange(len(value) + 1)
            value[position:position] = bytes(
                [MUTATION_ALPHABET[rng.randrange(len(MUTATION_ALPHABET))]]
            )
        elif value:
            start = rng.randrange(len(value))
            width = 1 + rng.randrange(min(max_span, len(value) - start))
            position = rng.randrange(len(value) + 1)
            value[position:position] = value[start:start + width]
    return bytes(value)


def run(args: argparse.Namespace) -> tuple[int, int, int]:
    libraries = (load_library(args.baseline), load_library(args.candidate))
    rng = random.Random(args.seed)
    request_count = 0
    datagram_count = 0

    for permutation in itertools.permutations(REQUEST_FIELDS):
        compare_request(
            libraries,
            b"{" + b",".join(permutation) + b"}",
            f"permutation-{request_count}",
        )
        request_count += 1
    for index, data in enumerate(REQUEST_EDGES):
        compare_request(libraries, data, f"edge-{index}")
        request_count += 1
    for lane in range(8):
        for byte in range(256):
            content = bytearray(b"a" * 16)
            content[lane] = byte
            compare_request(
                libraries,
                REQUEST_WORD_PREFIX + bytes(content) + REQUEST_WORD_SUFFIX,
                f"word-byte-{lane}-{byte}",
            )
            request_count += 1
    for index in range(args.request_mutations):
        compare_request(
            libraries,
            mutate(rng, REQUEST_BASE, 7, 8),
            f"mutation-{index}",
        )
        request_count += 1
    for index in range(args.request_raw):
        length = rng.randrange(321)
        compare_request(
            libraries,
            bytes(rng.randrange(256) for _ in range(length)),
            f"raw-{index}",
        )
        request_count += 1
    for index, length in enumerate((16383, 16384, 16385)):
        compare_request(
            libraries,
            b"{" + b" " * (length - 2) + b"}",
            f"cap-{index}",
        )
        request_count += 1

    for index, data in enumerate(CONTROL_EDGES):
        compare_datagram(libraries, CONTROL_PREFIX + data, f"control-edge-{index}")
        datagram_count += 1
    for index in range(args.control_mutations):
        compare_datagram(
            libraries,
            CONTROL_PREFIX + mutate(rng, CONTROL_EDGES[0], 6, 6),
            f"control-mutation-{index}",
        )
        datagram_count += 1
    for index in range(args.datagram_raw):
        length = rng.randrange(1202)
        compare_datagram(
            libraries,
            bytes(rng.randrange(256) for _ in range(length)),
            f"datagram-raw-{index}",
        )
        datagram_count += 1
    active_count = request_count * sum(
        library.active_request_parser is not None for library in libraries
    )
    return request_count, datagram_count, active_count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--request-mutations", type=int, default=100_000)
    parser.add_argument("--request-raw", type=int, default=20_000)
    parser.add_argument("--control-mutations", type=int, default=60_000)
    parser.add_argument("--datagram-raw", type=int, default=30_000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0xC0DEC0DE)
    args = parser.parse_args()
    for field in (
        "request_mutations",
        "request_raw",
        "control_mutations",
        "datagram_raw",
    ):
        if getattr(args, field) < 0:
            parser.error(f"--{field.replace('_', '-')} must not be negative")
    requests, datagrams, active_requests = run(args)
    print(
        "PASS exact WebTransport parser differential "
        f"requests={requests} datagrams={datagrams} "
        f"active_logical={active_requests} total={requests + datagrams}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
