#!/usr/bin/env python3
"""Compare two voice identity verifiers over deterministic token corpora."""

from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import hmac
import random
import struct
from pathlib import Path


IDENTITY_DOMAIN = b"voice-webtransport-identity-v1\n"
SECRET = b"0123456789abcdef0123456789abcdef"
NOW = 1_787_572_800
TOKEN_CAP = 512
BASE64URL_ALPHABET = frozenset(
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
)
MUTATION_BYTES = (
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    b".!+=/ \\:\t\r\n\x00\x7f\x80\xff"
)


class IdentityClaims(ctypes.Structure):
    _fields_ = [
        ("request_id", ctypes.c_char * 128),
        ("user_id", ctypes.c_char * 128),
        ("nonce", ctypes.c_char * 33),
        ("issued_at", ctypes.c_int64),
        ("expires_at", ctypes.c_int64),
        ("premium", ctypes.c_int),
    ]


class Verifier(ctypes.Structure):
    _fields_ = [("context", ctypes.c_void_p)]


def load_library(path: Path):
    library = ctypes.CDLL(str(path))
    library.voice_auth_verifier_init.argtypes = [
        ctypes.POINTER(Verifier),
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    library.voice_auth_verifier_init.restype = ctypes.c_int
    library.voice_auth_verifier_destroy.argtypes = [ctypes.POINTER(Verifier)]
    library.voice_auth_verifier_destroy.restype = None
    library.voice_auth_identity_verify.argtypes = [
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int64,
        ctypes.POINTER(IdentityClaims),
    ]
    library.voice_auth_identity_verify.restype = ctypes.c_int
    library.voice_auth_verifier_identity_verify.argtypes = [
        ctypes.POINTER(Verifier),
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int64,
        ctypes.POINTER(IdentityClaims),
    ]
    library.voice_auth_verifier_identity_verify.restype = ctypes.c_int
    return library


def claims_bytes(claims: IdentityClaims) -> bytes:
    return ctypes.string_at(ctypes.byref(claims), ctypes.sizeof(claims))


def identity_token(
    request_id: bytes,
    user_id: bytes,
    nonce: bytes,
    premium: int = 0,
) -> bytes:
    payload = (
        bytes((1, premium))
        + struct.pack(">QQ", NOW, NOW + 90)
        + nonce
        + struct.pack(">HH", len(request_id), len(user_id))
        + request_id
        + user_id
    )
    encoded = base64.urlsafe_b64encode(payload).rstrip(b"=")
    signature = hmac.new(SECRET, IDENTITY_DOMAIN + encoded, hashlib.sha256).hexdigest()
    return b"vat1." + encoded + b"." + signature.encode("ascii")


def valid_tokens() -> tuple[bytes, ...]:
    rng = random.Random(0x1D3A71A7)
    tokens = []
    symbols: set[int] = set()
    for index in range(256):
        nonce = bytes(rng.randrange(256) for _ in range(16))
        token = identity_token(
            f"request-{index}".encode(),
            f"user-{index}".encode(),
            nonce,
            index & 1,
        )
        tokens.append(token)
        encoded = token.split(b".", 2)[1]
        symbols.update(encoded)
    tokens.append(identity_token(b"r" * 127, b"u" * 127, bytes(range(16))))
    if symbols != BASE64URL_ALPHABET:
        missing = bytes(sorted(BASE64URL_ALPHABET - symbols))
        raise AssertionError(f"valid corpus misses base64url symbols: {missing!r}")
    return tuple(tokens)


def mutate(rng: random.Random, source: bytes) -> bytes:
    value = bytearray(source)
    for _ in range(1 + rng.randrange(7)):
        action = rng.randrange(4)
        if action == 0 and value:
            value[rng.randrange(len(value))] = MUTATION_BYTES[
                rng.randrange(len(MUTATION_BYTES))
            ]
        elif action == 1 and value:
            start = rng.randrange(len(value))
            width = 1 + rng.randrange(min(12, len(value) - start))
            del value[start : start + width]
        elif action == 2:
            position = rng.randrange(len(value) + 1)
            value[position:position] = bytes(
                (MUTATION_BYTES[rng.randrange(len(MUTATION_BYTES))],)
            )
        elif value:
            start = rng.randrange(len(value))
            width = 1 + rng.randrange(min(12, len(value) - start))
            position = rng.randrange(len(value) + 1)
            value[position:position] = value[start : start + width]
    return bytes(value)


def compare_case(
    libraries,
    verifiers,
    token: bytes,
    request_id: bytes,
    now: int,
    label: str,
) -> None:
    token_buffer = ctypes.create_string_buffer(token + b"\0")
    token_pointer = ctypes.cast(token_buffer, ctypes.c_char_p)
    results = []
    for library, verifier in zip(libraries, verifiers, strict=True):
        one_shot = IdentityClaims()
        prepared = IdentityClaims()
        ctypes.memset(ctypes.byref(one_shot), 0xA5, ctypes.sizeof(one_shot))
        ctypes.memset(ctypes.byref(prepared), 0xA5, ctypes.sizeof(prepared))
        one_shot_status = library.voice_auth_identity_verify(
            SECRET,
            len(SECRET),
            token_pointer,
            request_id,
            now,
            ctypes.byref(one_shot),
        )
        prepared_status = library.voice_auth_verifier_identity_verify(
            ctypes.byref(verifier),
            token_pointer,
            request_id,
            now,
            ctypes.byref(prepared),
        )
        result = (
            one_shot_status,
            claims_bytes(one_shot),
            prepared_status,
            claims_bytes(prepared),
        )
        if result[0:2] != result[2:4]:
            raise AssertionError(
                f"one-shot/prepared mismatch case={label} length={len(token)}"
            )
        results.append(result)
    if results[0] != results[1]:
        raise AssertionError(
            f"baseline/candidate mismatch case={label} length={len(token)} "
            f"baseline={results[0][0]} candidate={results[1][0]} token={token!r}"
        )


def run(args: argparse.Namespace) -> int:
    libraries = (load_library(args.baseline), load_library(args.candidate))
    verifiers = (Verifier(), Verifier())
    for library, verifier in zip(libraries, verifiers, strict=True):
        if library.voice_auth_verifier_init(
            ctypes.byref(verifier), SECRET, len(SECRET)
        ) != 0:
            raise RuntimeError("verifier initialization failed")
    rng = random.Random(args.seed)
    tokens = valid_tokens()
    cases = 0
    try:
        for index, token in enumerate(tokens):
            request_id = b"r" * 127 if index == len(tokens) - 1 else (
                f"request-{index}".encode()
            )
            for moment in (NOW - 1, NOW, NOW + 89, NOW + 90):
                compare_case(
                    libraries,
                    verifiers,
                    token,
                    request_id,
                    moment,
                    f"valid-{index}-time-{moment}",
                )
                cases += 1
            compare_case(
                libraries,
                verifiers,
                token,
                b"wrong-request",
                NOW,
                f"binding-{index}",
            )
            cases += 1
        edges = (
            b"",
            b"vat1.",
            b"vat1.a.0" * 40,
            b"vat1.A=." + b"0" * 64,
            b"vat1.A/." + b"0" * 64,
            b"vat1.A+." + b"0" * 64,
            b"vat1._-." + b"f" * 64,
            b"vat1." + b"A" * 401 + b"." + b"0" * 64,
            b"vat1." + b"A" * 400 + b"." + b"0" * 65,
            b"x" * (TOKEN_CAP - 1),
            b"x" * TOKEN_CAP,
        )
        for index, token in enumerate(edges):
            compare_case(
                libraries, verifiers, token, b"request-0", NOW, f"edge-{index}"
            )
            cases += 1
        for encoded_len in range(400):
            compare_case(
                libraries,
                verifiers,
                b"vat1." + b"A" * encoded_len + b"." + b"0" * 64,
                b"request-0",
                NOW,
                f"encoded-length-{encoded_len}",
            )
            cases += 1
        for index in range(args.mutations):
            compare_case(
                libraries,
                verifiers,
                mutate(rng, tokens[rng.randrange(len(tokens))]),
                f"request-{rng.randrange(256)}".encode(),
                NOW + rng.randrange(-1, 92),
                f"mutation-{index}",
            )
            cases += 1
        for index in range(args.raw):
            length = rng.randrange(601)
            token = bytes(rng.randrange(256) for _ in range(length))
            compare_case(
                libraries,
                verifiers,
                token,
                b"request-0",
                NOW,
                f"raw-{index}",
            )
            cases += 1
    finally:
        for library, verifier in zip(libraries, verifiers, strict=True):
            library.voice_auth_verifier_destroy(ctypes.byref(verifier))
    return cases


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--mutations", type=int, default=100_000)
    parser.add_argument("--raw", type=int, default=20_000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0xB64D1FF)
    args = parser.parse_args()
    if args.mutations < 0 or args.raw < 0:
        parser.error("case counts must not be negative")
    cases = run(args)
    print(f"PASS exact identity verifier differential cases={cases}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
