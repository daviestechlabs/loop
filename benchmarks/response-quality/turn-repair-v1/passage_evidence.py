"""Independent verification of passage boundaries and backend witnesses.

This offline verifier does not approve reading order, rights, or answer quality.
"""

import hashlib
import re
import struct

from source_span_eval import reconstruct
from verify_coupled_dialogue import require


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def admitted_passages(data):
    result = []
    for d in data.get("passages", []):
        pages, ranges, pieces = {}, {}, []
        require(0 <= d["document"] < len(data["documents"]), "Invalid passage document")
        require(
            1 <= d["first_page"] <= d["last_page"] < d["first_page"] + 3,
            "Invalid passage pages",
        )
        for page in range(d["first_page"], d["last_page"] + 1):
            pages[page] = reconstruct(
                [
                    r
                    for r in data["records"]
                    if r["document"] == d["document"] and r["page"] == page
                ]
            )
            begin = d["heading_begin"] if page == d["first_page"] else 0
            end = (
                d["next_heading_begin"] if page == d["last_page"] else len(pages[page])
            )
            require(0 <= begin <= end <= len(pages[page]), "Invalid passage boundary")
            while end > begin and pages[page][end - 1] == 32:
                end -= 1
            if begin == end and page != d["first_page"] and page == d["last_page"]:
                continue
            require(begin < end, "Empty passage page")
            ranges[page] = (begin, end)
            pieces.append(pages[page][begin:end])
        raw = b"\n".join(pieces)
        raw.decode()
        require(
            0 < len(raw) <= 4096 and digest(raw) == d["hash"],
            "Admitted passage assembly differs",
        )
        title = pages[d["first_page"]][d["heading_begin"] : d["heading_end"]].decode()
        name = "".join(re.findall("[A-Za-z]", title)).lower()
        result.append(
            {
                "definition": d,
                "document": data["documents"][d["document"]],
                "ranges": ranges,
                "raw": raw,
                "name": name,
            }
        )
    return result


def passage_text(citation, records, data, passages):
    require(
        citation.get("kind") == "complete_passage" and citation.get("record_id") == "",
        "Invalid passage identity",
    )
    require(
        not citation.get("excerpt_spans")
        and citation["chunk_index"] == 0
        and citation["score_metric"] == "none"
        and citation["score"] == 0,
        "Invalid passage record fields",
    )
    require(
        re.fullmatch("[a-f0-9]{64}", citation["passage_id"]) is not None,
        "Invalid passage ID",
    )
    candidates = [
        p
        for p in passages
        if p["document"]
        == [citation["document_id"], citation["book_slug"], citation["source_sha256"]]
        and p["definition"]["hash"] == citation["content_hash"]
        and p["name"] == citation["section"]
        and min(p["ranges"]) == citation["page_start"]
        and max(p["ranges"]) == citation["page_end"]
    ]
    require(len(candidates) == 1, "Passage is absent or ambiguous in admitted source")
    p = candidates[0]
    witnesses = citation["witnesses"]
    require(
        isinstance(witnesses, list) and 1 <= len(witnesses) <= 16,
        "Invalid witness count",
    )
    indexed = {r["id"]: r for r in data["records"]}
    seen, pieces = set(), []
    page, cursor, previous_chunk = (
        min(p["ranges"]),
        p["ranges"][min(p["ranges"])][0],
        -1,
    )
    identity = bytearray(
        b"dnd-complete-passage/v1\0" + citation["content_hash"].encode()
    )
    for w in witnesses:
        require(
            set(w)
            == {
                "record_id",
                "content_hash",
                "page",
                "chunk_index",
                "begin",
                "end",
                "record_length",
            },
            "Unexpected witness fields",
        )
        require(
            all(
                type(w[k]) is int
                for k in ("page", "chunk_index", "begin", "end", "record_length")
            ),
            "Noninteger witness range",
        )
        rid = w["record_id"]
        require(
            rid not in seen and rid in indexed and rid in records,
            "Duplicate, unfetched, or unindexed witness",
        )
        seen.add(rid)
        original = indexed[rid]
        record, meta = records[rid]
        raw = record["content"].encode()
        require(
            raw == original["content"].encode()
            and digest(raw)
            == original["hash"]
            == w["content_hash"]
            == meta["content_hash"],
            "Original witness bytes or hash differ",
        )
        require(
            len(raw) == w["record_length"] and 0 <= w["begin"] < w["end"] <= len(raw),
            "Invalid witness offsets",
        )
        require(
            original["document"] == p["definition"]["document"]
            and original["page"] == w["page"] == meta["page_start"] == meta["page_end"]
            and original["chunk"] == w["chunk_index"] == meta["chunk_index"],
            "Witness source position differs",
        )
        for key in (
            "document_id",
            "book_slug",
            "source_sha256",
            "corpus_version",
            "embedding_model",
        ):
            require(
                citation[key] == meta[key], "Witness citation scope differs: " + key
            )
        require(citation["source"] == record["source"], "Witness source URI differs")
        if w["page"] != page:
            require(
                cursor == p["ranges"][page][1]
                and w["page"] == page + 1
                and w["page"] in p["ranges"],
                "Passage has a page gap",
            )
            page, cursor, previous_chunk = w["page"], p["ranges"][w["page"]][0], -1
            pieces.append(b"\n")
        require(
            w["chunk_index"] > previous_chunk
            and original["begin"] + w["begin"] == cursor,
            "Passage witness overlap, gap, or order differs",
        )
        cursor = original["begin"] + w["end"]
        require(cursor <= p["ranges"][page][1], "Witness crosses admitted boundary")
        previous_chunk = w["chunk_index"]
        piece = raw[w["begin"] : w["end"]]
        piece.decode()
        pieces.append(piece)
        identity.extend(rid.encode())
        identity.extend(
            struct.pack("<4I", w["page"], w["chunk_index"], w["begin"], w["end"])
        )
    require(
        page == max(p["ranges"]) and cursor == p["ranges"][page][1],
        "Passage is incomplete",
    )
    require(
        b"".join(pieces) == p["raw"] and digest(identity) == citation["passage_id"],
        "Passage assembly or identity differs",
    )
    return p["raw"].decode()
