"""Offline capacity audit of candidate spell boundaries in pinned source bytes.

This does not admit passages, change runtime limits, or certify reading order.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "voice/c-runtime/tests"))
from test_dnd_source_compiler import decode
from source_span_eval import reconstruct

SCHOOLS = "abjuration|conjuration|divination|enchantment|evocation|illusion|necromancy|transmutation"
HEADER = re.compile(
    rf"(?:(?:1 *st|2 *nd|3 *rd|[4-9] *th) *- *level +(?:{SCHOOLS})|(?:{SCHOOLS}) cantrip) "
    r"(?:\(ritual\) )?Casting Time: ",
    re.IGNORECASE,
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def candidate_headings(raw):
    """Locate source-shaped headings; candidates still need source review."""
    text = raw.decode()
    result = []
    for match in HEADER.finditer(text):
        begin = max(text.rfind(p, 0, match.start()) for p in ".!?") + 1
        while begin < match.start() and text[begin] == " ":
            begin += 1
        title = text[begin : match.start()].rstrip(" ")
        if not title or any(
            not (c.isascii() and c.isalpha() or c in " ’'-/") for c in title
        ):
            continue
        words = title.split()
        letters = [c for c in title if c.isascii() and c.isalpha()]
        caps = sum(c.isupper() for c in letters)
        if not 4 <= len(letters) < 128:
            continue
        if caps * 4 < len(letters) * 3 and not (
            caps and sum(len(w) == 1 for w in words) * 4 >= len(words) * 3
        ):
            continue
        result.append(
            {
                "name": "".join(letters).lower(),
                "begin": len(text[:begin].encode()),
                "header_begin": len(text[: match.start()].encode()),
            }
        )
    return result


def minimum_cover(records, begin, end):
    """Minimum original records for one interval, with deterministic ties."""
    if not 0 <= begin < end:
        raise ValueError("Invalid requested interval")
    cursor, selected = begin, []
    while cursor < end:
        choices = [
            r
            for r in records
            if r["begin"] <= cursor < r["begin"] + len(r["content"].encode())
        ]
        if not choices:
            raise ValueError("Uncovered source bytes")
        record = min(
            choices, key=lambda r: (-(r["begin"] + len(r["content"].encode())), r["id"])
        )
        stop = min(end, record["begin"] + len(record["content"].encode()))
        selected.append(
            {
                "record_id": record["id"],
                "content_hash": record["hash"],
                "begin": cursor - record["begin"],
                "end": stop - record["begin"],
            }
        )
        cursor = stop
    return selected


def candidate_ranges(raw, pages):
    headings = [
        {**h, "page": page}
        for page in sorted(raw)
        for h in candidate_headings(raw[page])
    ]
    markers = [
        (p, len(raw[p].decode()[: m.start()].encode()))
        for p in sorted(raw)
        for m in HEADER.finditer(raw[p].decode())
    ]
    rows = []
    for i, current in enumerate(headings):
        row = dict(current, complete_candidate=False, admitted=False)
        rows.append(row)
        if i + 1 == len(headings):
            row["reason"] = "No following heading"
            continue
        following = headings[i + 1]
        page_numbers = range(current["page"], following["page"] + 1)
        if following["page"] - current["page"] > 2 or any(
            p not in raw for p in page_numbers
        ):
            row["reason"] = "Nonconsecutive or excessive page span"
            continue
        if any(
            (current["page"], current["header_begin"])
            < marker
            < (following["page"], following["begin"])
            for marker in markers
        ):
            row["reason"] = "Unresolved intervening spell header"
            continue
        pieces, size = [], 0
        for p in page_numbers:
            start = current["begin"] if p == current["page"] else 0
            stop = following["begin"] if p == following["page"] else len(raw[p])
            while stop > start and raw[p][stop - 1 : stop] == b" ":
                stop -= 1
            if start == stop:
                continue
            cut = raw[p][start:stop]
            if p == following["page"] and cut[-1:] not in (b".", b"!", b"?"):
                break
            pieces.append(
                {
                    "page": p,
                    "begin": start,
                    "end": stop,
                    "sha256": sha(cut),
                    "records": minimum_cover(pages[p], start, stop),
                }
            )
            size += len(cut)
        else:
            if pieces and raw[pieces[-1]["page"]][
                pieces[-1]["end"] - 1 : pieces[-1]["end"]
            ] in (b".", b"!", b"?"):
                row.update(
                    complete_candidate=True,
                    end_page=pieces[-1]["page"],
                    next_heading=following["name"],
                    bytes=size,
                    body_bytes=size - (current["header_begin"] - current["begin"]),
                    pieces=pieces,
                    minimum_records=sum(len(p["records"]) for p in pieces),
                    crosses_page=len(pieces) > 1,
                )
                continue
        row["reason"] = "No complete final sentence at candidate boundary"
    return rows


def audit(source, document_id):
    receipt = json.loads((source / "receipt.json").read_text())
    for name in ("index.dndsidx", "manifest.json"):
        if sha((source / name).read_bytes()) != receipt["files"][name]["sha256"]:
            raise ValueError("Source pin differs")
    data = decode((source / "index.dndsidx").read_bytes())
    ids = [i for i, doc in enumerate(data["documents"]) if doc[0] == document_id]
    if len(ids) != 1:
        raise ValueError("Document must identify exactly one retained printing")
    pages = {}
    for record in data["records"]:
        if record["document"] != ids[0]:
            continue
        if sha(record["content"].encode()) != record["hash"]:
            raise ValueError("Record hash differs")
        pages.setdefault(record["page"], []).append(record)
    raw = {page: reconstruct(records) for page, records in pages.items()}
    rows = candidate_ranges(raw, pages)
    complete = [r for r in rows if r["complete_candidate"]]
    names = [r["name"] for r in rows]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    return {
        "schema": "waterdeep-rule-span-audit/v1",
        "document_id": document_id,
        "source_index_sha256": sha((source / "index.dndsidx").read_bytes()),
        "source_manifest_sha256": sha((source / "manifest.json").read_bytes()),
        "page_count": len(pages),
        "candidate_headings": len(rows),
        "complete_candidates": len(complete),
        "duplicate_names": duplicates,
        "cross_page_candidates": sum(r["crosses_page"] for r in complete),
        "over_four_records": sum(r["minimum_records"] > 4 for r in complete),
        "over_1024_bytes": sum(r["body_bytes"] > 1024 for r in complete),
        "maximum_records": max((r["minimum_records"] for r in complete), default=0),
        "maximum_bytes": max((r["bytes"] for r in complete), default=0),
        "rows": rows,
        "limits": [
            "Candidate heading heuristics can miss boundaries; completeness is not source admission.",
            "Page offsets come from the pinned index. No PDF or reading-order revalidation occurs.",
            "No generated answer, backend call, training admission, or runtime limit change.",
            "Body byte counts exclude headings; cross-page whitespace joining remains unimplemented.",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--document-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.source, args.document_id)
    with args.output.open("x") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k != "rows"}, indent=2))
