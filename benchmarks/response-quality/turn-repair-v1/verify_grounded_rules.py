"""Verify C events, model output, scoped backend records and public citations.

This verifies evidence integrity, not the semantic quality of the answers.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile

from grounded_rules_eval import CASES, CONDITIONS, load_cases
from passage_evidence import admitted_passages, passage_text
from verify_coupled_dialogue import model_catalog, read, require


def digest(data):
    return hashlib.sha256(data).hexdigest()


def admitted_spell_ranges(data):
    """Reconstruct possible source cuts independently from admitted entries."""
    allowed = {}
    for entry in data["entries"]:
        if not entry["flags"] & 4 or not entry["body"]["count"]:
            continue
        for group in (entry["heading"], entry["body"]):
            for span in group["spans"][: group["count"]]:
                record = data["records"][span["record"]]
                begin = (
                    max(entry["heading"]["begin"], record["begin"]) - record["begin"]
                )
                end = min(
                    entry["body"]["end"] - record["begin"],
                    len(record["content"].encode()),
                )
                require(0 <= begin < end, "Invalid admitted spell interval")
                allowed.setdefault(record["id"], set()).add((begin, end))
    return allowed


def excerpt_text(content, citation, allowed):
    """Check byte ranges and their source boundaries, not query relevance."""
    if "excerpt_spans" not in citation:
        return content
    spans = citation["excerpt_spans"]
    require(isinstance(spans, list) and 0 < len(spans) <= 8, "Invalid excerpt count")
    raw, pieces, previous = content.encode(), [], -1
    for span in spans:
        require(
            isinstance(span, dict) and set(span) == {"begin", "end"},
            "Invalid excerpt fields",
        )
        begin, end = span["begin"], span["end"]
        require(
            type(begin) is int
            and type(end) is int
            and previous < begin < end <= len(raw),
            "Invalid excerpt bounds",
        )
        # A union can include touching or overlapping admitted spell intervals,
        # but cannot bridge unselected text or move either source boundary.
        cursor = begin
        candidates = [
            (a, b)
            for a, b in allowed.get(citation["record_id"], ())
            if begin <= a < b <= end
        ]
        while cursor < end:
            progress = max((b for a, b in candidates if a <= cursor), default=cursor)
            require(progress > cursor, "Excerpt is not an admitted source interval")
            cursor = progress
        pieces.append(raw[begin:end].decode())
        previous = end
    return "\n[Omitted source text]\n".join(pieces)


def build_renderer(repo, output):
    speech = repo / "contracts/handler-base/speechsegment"
    common = repo / "voice/c-runtime/common"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(speech),
            "-I" + str(common),
            str(
                repo
                / "benchmarks/response-quality/turn-repair-v1/canonical_model_reply.c"
            ),
            str(speech / "speech_markup.c"),
            str(speech / "speech_sanitize.c"),
            str(common / "utf8.c"),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
    )


def verify(root, source, repo, case_pack=None):
    with tempfile.TemporaryDirectory(prefix="waterdeep-reply-verify-") as temporary:
        renderer = Path(temporary) / "render"
        build_renderer(repo, renderer)
        return verify_with_renderer(root, source, repo, case_pack, renderer)


def verify_with_renderer(root, source, repo, case_pack, renderer):
    protocol = read(root / "design.json")
    cases = CASES
    if protocol.get("case_pack_sha256"):
        require(case_pack is not None, "An explicit authored case pack is required")
        require(
            digest(case_pack.read_bytes()) == protocol["case_pack_sha256"],
            "Case pack pin differs",
        )
        require(
            (root / "case-pack.json").read_bytes() == case_pack.read_bytes(),
            "Retained case pack differs",
        )
        cases = load_cases(case_pack)
    else:
        require(case_pack is None, "Unexpected case pack")
    require(protocol["cases"] == [list(c) for c in cases], "Case design changed")
    require(protocol["conditions"] == list(CONDITIONS), "Conditions changed")
    require(protocol["repetitions"] == 1, "Repetition count changed")
    for filename in ("index.dndsidx", "manifest.json"):
        require(
            digest((source / filename).read_bytes())
            == protocol["source"]["files"][filename]["sha256"],
            "Source pin differs",
        )
    config = protocol["source"]["config"]
    sys.path.insert(0, str(repo / "voice/c-runtime/tests"))
    from test_dnd_source_compiler import decode

    source_data = decode((source / "index.dndsidx").read_bytes())
    allowed_spans = admitted_spell_ranges(source_data)
    passages = admitted_passages(source_data)
    corpus = config["RAG_SHARED_RULEBOOK_CORPUS_VERSION"]
    collection = config["RAG_SHARED_RULEBOOK_COLLECTION"]
    manifest = read(source / "manifest.json")
    documents = {
        digest((corpus + ":" + (s["etag"] or s["source_key"])).encode())[:24]: s
        for s in manifest["sources"]
        if s["enabled"]
    }
    require(
        model_catalog(read(root / "model-before.json"))
        == model_catalog(read(root / "model-after.json")),
        "Model catalog changed",
    )
    prompts = repo / "contracts/prompt-library/prompts"
    direct = (
        (prompts / "voice/product-response-style.system.txt").read_text().strip()
        + "\n\n"
        + (prompts / "voice/dnd-dialogue.system.txt").read_text().strip()
    )
    grounding = (prompts / "rag/grounded-response.system.txt").read_text().strip()
    expected = [
        (case_id, message, condition)
        for i, (case_id, message, _) in enumerate(cases)
        for condition in CONDITIONS[:: 1 if i % 2 == 0 else -1]
    ]
    results = read(root / "results.json")
    require(len(results) == len(expected), "Missing or extra trials")
    settings = None
    citations_total = 0
    passages_total = witnesses_total = 0
    for row, (case_id, message, condition) in zip(results, expected, strict=True):
        require(
            (row["case_id"], row["message"], row["condition"])
            == (case_id, message, condition),
            "Trial order or input differs",
        )
        path = root / f"{case_id}-{condition}"
        require(row == read(path / "trial.json"), "Trial and aggregate disagree")
        require(
            row["completed"] and row["cleanup_ok"] and "error" not in row,
            "Incomplete trial",
        )
        request = read(path / "request.json")
        require(
            request["message"] == message and request["enable_tts"] is False,
            "Product input changed",
        )
        meta = {"interaction_profile": "dnd_app", "campaign_id": "table"}
        if condition == "live_retrieval":
            meta.update(knowledge_scope="shared_rulebook", retrieval_force="true")
        require(request["metadata"] == meta, "Product metadata changed")
        events = [
            json.loads(line)
            for line in (path / "response.ndjson").read_bytes().splitlines()
            if line
        ]
        require(
            events == row["events"] and events[-1]["type"] == "completed",
            "Product event stream differs",
        )
        finals = [e for e in events if e["type"] == "text_completed"]
        require(
            len(finals) == 1 and finals[0]["text"] == row["answer"],
            "Final answer differs",
        )
        exchanges = read(path / "model-exchanges.json")
        require(
            len(exchanges) == row["model_calls"] == 1, "Unexpected model call count"
        )
        exchange = exchanges[0]
        require(
            exchange["status"] == 200 and "error" not in exchange,
            "Model transport failed",
        )
        chunks = [
            json.loads(line[6:])
            for line in exchange["response_sse"].splitlines()
            if line.startswith("data: ") and line != "data: [DONE]"
        ]
        choices = [choice for chunk in chunks for choice in chunk.get("choices", [])]
        require(
            [c["finish_reason"] for c in choices if c.get("finish_reason")] == ["stop"],
            "Incomplete model generation",
        )
        model_text = "".join(c.get("delta", {}).get("content") or "" for c in choices)
        # No cancellation receipt applies to these cases. Replay the exact C
        # presentation kernels; typography changes must not change answer words.
        displayed = subprocess.check_output(
            [str(renderer)], input=model_text.encode()
        ).decode()
        require(displayed == row["answer"], "Model and product text disagree")
        model_request = exchange["request"]
        params = {k: v for k, v in model_request.items() if k != "messages"}
        if settings is None:
            settings = params
        require(params == settings, "Generation settings differ")
        messages = model_request["messages"]
        require(
            [m["role"] for m in messages] == ["system", "user"],
            "Unexpected model history",
        )
        system = direct + ("\n\n" + grounding if condition == "live_retrieval" else "")
        require(messages[0]["content"] == system, "System prompt differs")
        retrieval = read(path / "retrieval/model-exchanges.json")
        require(
            len(retrieval) == row["retrieval_calls"], "Retrieval call count differs"
        )
        if condition == "direct":
            require(not retrieval, "Direct trial used retrieval")
            require(messages[1]["content"] == message, "Direct model input differs")
            continue
        require(retrieval, "Missing live backend exchange")
        records = {}
        fetched = {}
        for call in retrieval:
            require(
                call["status"] == 200 and "error" not in call,
                "Retrieval transport failed",
            )
            query = call["request"]
            require(
                query["collectionName"] == collection and 0 < query["limit"] <= 4,
                "Retrieval scope changed",
            )
            for field in (
                corpus,
                'owner_user_id == ""',
                'visibility == "public"',
                'ruleset == "dnd-5e-2014"',
            ):
                require(field in query["filter"], "Missing scoped filter")
            response = json.loads(call["response_sse"])
            require(response["code"] == 0, "Milvus rejected query")
            exact = call["path"] == "/v2/vectordb/entities/query"
            if exact:
                match = re.search(r"record_id in (\[[^\]]+\])$", query["filter"])
                require(match is not None, "Missing exact witness filter")
                requested = json.loads(match[1])
                require(
                    len(requested) == len(set(requested)) == query["limit"]
                    and set(requested) == {r["record_id"] for r in response["data"]}
                    and len(response["data"]) == len(requested),
                    "Exact response record set differs",
                )
            for record in response["data"]:
                metadata = json.loads(record["metadata_json"])
                for field in (
                    "record_id",
                    "document_id",
                    "book_slug",
                    "source_kind",
                    "visibility",
                    "owner_user_id",
                    "campaign_id",
                    "corpus_version",
                    "source_etag",
                    "page_start",
                    "page_end",
                ):
                    require(
                        record[field] == metadata[field],
                        "Backend scalar metadata differs",
                    )
                require(
                    metadata["owner_user_id"] == ""
                    and metadata["visibility"] == "public"
                    and metadata["knowledge_scope"] == "shared_rulebook",
                    "Backend ownership scope differs",
                )
                require(
                    metadata["corpus_version"] == corpus
                    and metadata["ruleset"] == "dnd-5e-2014",
                    "Backend corpus or ruleset differs",
                )
                require(
                    digest(record["content"].encode()) == metadata["content_hash"],
                    "Backend text hash differs",
                )
                book = documents[record["document_id"]]
                require(
                    metadata["source_sha256"] == book["source_sha256"]
                    and record["book_slug"] == book["book_slug"],
                    "Book source differs",
                )
                rid = record["record_id"]
                if rid in records:
                    old_record, old_metadata = records[rid]
                    require(
                        old_record["content"] == record["content"]
                        and old_metadata == metadata,
                        "Backend record changed between requests",
                    )
                records[rid] = (record, metadata)
                if exact:
                    fetched[rid] = (record, metadata)
        final_meta = finals[0]["metadata"]
        require(
            final_meta["cascade_route"] == "retrieve_then_escalate"
            and final_meta["cascade_retrieval_used"] == "true",
            "Wrong grounded route",
        )
        require(
            final_meta["cascade_grounding_prompt_sha256"] == digest(grounding.encode()),
            "Grounding prompt identity differs",
        )
        citations = json.loads(final_meta["cascade_retrieval_citations"])
        require(
            0 < len(citations) <= 4
            and len(citations) == int(final_meta["cascade_retrieved_documents"]),
            "Citation count differs",
        )
        require(
            len({c.get("passage_id") or c["record_id"] for c in citations})
            == len(citations),
            "Duplicate citation",
        )
        supplied = "Retrieved excerpts (source data):\n"
        for i, citation in enumerate(citations, 1):
            if citation.get("kind") == "complete_passage":
                require(
                    citation["collection"] == collection, "Passage collection differs"
                )
                text = passage_text(citation, fetched, source_data, passages)
                supplied += f"\n[Source {i}; name {citation['book_slug']}; passage {citation['passage_id']}; pages {citation['page_start']}-{citation['page_end']}; section {citation['section']}; original records {len(citation['witnesses'])}]\n{text}\n[End source {i}]\n"
                passages_total += 1
                witnesses_total += len(citation["witnesses"])
                continue
            require(
                not citation.get("passage_id") and not citation.get("witnesses"),
                "Unexpected record witness identity",
            )
            record, metadata = records[citation["record_id"]]
            for field in (
                "record_id",
                "document_id",
                "book_slug",
                "content_hash",
                "source_sha256",
                "page_start",
                "page_end",
                "chunk_index",
                "corpus_version",
            ):
                require(
                    citation[field] == metadata[field],
                    "Citation metadata differs: " + field,
                )
            require(citation["collection"] == collection, "Citation collection differs")
            text = excerpt_text(record["content"], citation, allowed_spans)
            supplied += f"\n[Source {i}; name {citation['book_slug']}; record {citation['record_id']}; pages {citation['page_start']}-{citation['page_end']}; section {citation['section']}; chunk {citation['chunk_index']}]\n{text}\n[End source {i}]\n"
        supplied += "\nQuestion: " + message
        require(
            messages[1]["content"] == supplied,
            "Model evidence differs from citations and backend records",
        )
        citations_total += len(citations)
    return {
        "trials": len(results),
        "grounded_trials": len(results) // 2,
        "verified_citations": citations_total,
        "verified_passages": passages_total,
        "verified_original_witnesses": witnesses_total,
        "semantic_quality_graded": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--case-pack", type=Path)
    args = parser.parse_args()
    print(
        json.dumps(verify(args.root, args.source, args.repo, args.case_pack), indent=2)
    )
