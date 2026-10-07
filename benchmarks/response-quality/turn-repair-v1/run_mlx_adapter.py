"""Bounded local QLoRA experiment with base and adapter weights kept in RAM."""

import argparse
import gc
import hashlib
import importlib.metadata
import io
import json
import math
import os
import platform
import re
import resource
import subprocess
import time
import urllib.parse
import urllib.request
from pathlib import Path

from semantic_eval import (
    HERE,
    PROMPTS,
    assess_target,
    expected,
    model_input,
    sha,
    write,
)

MODEL = "mlx-community/Qwen2.5-3B-Instruct-4bit"
REVISION = "4f83f8f146fdf28b512a06562b671d7af4fab457"
WEIGHT = (
    1736293090,
    "f212cf6fb9923281a09c135e05d43a052ee5ef7121f5b1dc0b0fb2de80f97cfd",
)
METADATA = {
    "README.md": (755, "1b81610e6dffeae569e8b6c3fdb4668d228f4e5b"),
    "config.json": (785, "52de1c37b6e5c5f74d43973c65779cea39b4b7a7"),
    "tokenizer.json": (7031673, "d24314ef7f0afd1b678c2e24c767e19f24f86b0e"),
    "tokenizer_config.json": (7308, "482ccbc1096b0e9400e86e33f189681c2aebdab9"),
    "added_tokens.json": (605, "482ced4679301bf287ebb310bdd1790eb4514232"),
    "special_tokens_map.json": (613, "ac23c0aaa2434523c494330aeb79c58395378103"),
    "merges.txt": (1671853, "31349551d90c7606f325fe0f11bbb8bd5fa0d7c7"),
    "vocab.json": (2776833, "4783fe10ac3adce15ac8f358ef5462739852c569"),
}
ROOT = HERE.parents[2]


def read_verified(stream, size, digest, *, git_blob=False, progress=None):
    if not 0 < size <= WEIGHT[0]:
        raise ValueError("Invalid download bound")
    hasher = hashlib.sha1() if git_blob else hashlib.sha256()
    if git_blob:
        hasher.update(b"blob " + str(size).encode() + b"\0")
    buffer = io.BytesIO()
    last_report = 0
    deadline = time.monotonic() + 600
    while chunk := stream.read(min(4 * 1024 * 1024, size - buffer.tell() + 1)):
        if time.monotonic() > deadline:
            raise TimeoutError("Model download deadline exceeded")
        if buffer.tell() + len(chunk) > size:
            raise ValueError("Download exceeds pinned size")
        buffer.write(chunk)
        hasher.update(chunk)
        if progress and buffer.tell() - last_report >= 64 * 1024 * 1024:
            last_report = buffer.tell()
            progress(last_report)
    if buffer.tell() != size or hasher.hexdigest() != digest:
        raise ValueError("Downloaded bytes differ from the pinned artifact")
    buffer.seek(0)
    return buffer


class PublicModelRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parsed = urllib.parse.urlsplit(newurl)
        host = parsed.hostname or ""
        if (
            parsed.scheme != "https"
            or parsed.username
            or parsed.password
            or not (
                host == "huggingface.co" or host.endswith((".huggingface.co", ".hf.co"))
            )
        ):
            raise ValueError("Unexpected model artifact redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(name, identity, git_blob=False):
    url = f"https://huggingface.co/{MODEL}/resolve/{REVISION}/{name}"
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), PublicModelRedirect()
    )
    request = urllib.request.Request(
        url, headers={"User-Agent": "Waterdeep-Development-Experiment/1"}
    )
    with opener.open(request, timeout=60) as response:
        return read_verified(
            response,
            *identity,
            git_blob=git_blob,
            progress=lambda count: print(
                json.dumps({"download": name, "verified_size_pending_bytes": count}),
                flush=True,
            ),
        )


def verified_data(root):
    receipt = json.loads((root / "receipt.json").read_bytes())
    if receipt.get("schema") != "waterdeep-turn-data-preparation/v1":
        raise ValueError("Unexpected prepared dataset")
    files = {}
    for name, digest in receipt["files"].items():
        path = root / name
        if (
            Path(name).is_absolute()
            or ".." in Path(name).parts
            or any(
                (root / Path(*Path(name).parts[:i])).is_symlink()
                for i in range(1, len(Path(name).parts) + 1)
            )
        ):
            raise ValueError("Invalid prepared input path")
        if path.stat().st_size > 1024 * 1024:
            raise ValueError("Prepared input changed")
        files[name] = path.read_bytes()
        if sha(files[name]) != digest:
            raise ValueError("Prepared input changed")
    required = {
        "training-inputs/training.json",
        "training-inputs/corpus-authorization.json",
        "evaluation-only/turn-cases.json",
        "evaluation-only/cohesion-cases.json",
    }
    if not required <= files.keys():
        raise ValueError("Required input is absent from the receipt")
    manifest = json.loads(files["training-inputs/training.json"])
    if (
        manifest.get("purpose") != "training"
        or manifest.get("authorization", {}).get("usage") != "training-authorized"
    ):
        raise ValueError("Training authorization is absent")
    if manifest["authorization"].get("evidence_sha256") != sha(
        files["training-inputs/corpus-authorization.json"]
    ):
        raise ValueError("Training authorization binding changed")
    result = {}
    seen = set()
    for split in ("train", "validation"):
        result[split] = []
        for digest in manifest["splits"][split]:
            if not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest):
                raise ValueError("Invalid training object identity")
            name = f"training-inputs/objects/{digest}.jsonl"
            if name not in files or sha(files[name]) != digest:
                raise ValueError("Training object is absent from the receipt")
            for line in files[name].decode().splitlines():
                row = json.loads(line)
                identity = row["example_id"]
                prompt = sha(json.dumps(row["messages"][:-1], sort_keys=True).encode())
                if identity in seen or prompt in seen:
                    raise ValueError("Duplicate or overlapping supervised example")
                seen.update((identity, prompt))
                result[split].append(row)
    return result, receipt, files


def supervised_tokens(row, tokenizer, *, maximum=256):
    if type(maximum) is not int or not 0 < maximum <= 1024:
        raise ValueError("Invalid supervised token bound")
    messages = row["messages"]
    if [m["role"] for m in messages] != ["system", "user", "assistant"]:
        raise ValueError("Expected one supervised assistant completion")
    tokens = tokenizer.apply_chat_template(messages, tokenize=True, return_dict=False)
    prefix = tokenizer.apply_chat_template(
        messages[:-1], tokenize=True, return_dict=False, add_generation_prompt=True
    )
    if not isinstance(tokens, list) or not all(type(t) is int for t in tokens):
        raise ValueError("Tokenizer did not return token identities")
    if tokens[: len(prefix)] != prefix or not 0 < len(prefix) < len(tokens) <= maximum:
        raise ValueError("Assistant mask or sequence bound failed; do not truncate")
    return tokens, len(prefix)


def evaluation_cases(input_files):
    turn_pack = json.loads(input_files["evaluation-only/turn-cases.json"])
    cases = []
    for case in turn_pack["cases"]:
        target = {k: v for k, v in case["target"].items() if k != "answer"}
        cases.append(
            {
                "case_id": case["case_id"],
                "group": "new-turn-evaluation",
                "target": target,
                "system": turn_pack["system_prompt"],
                "prompt": json.dumps(
                    model_input(case), sort_keys=True, ensure_ascii=False
                ),
            }
        )
    for case in json.loads((HERE / "cases.json").read_bytes())["cases"]:
        cases.append(
            {
                "case_id": case["case_id"],
                "group": "original-turn-development",
                "target": expected(case),
                "system": PROMPTS["contract"],
                "prompt": json.dumps(
                    model_input(case), sort_keys=True, ensure_ascii=False
                ),
            }
        )
    style = (
        ROOT
        / "contracts/prompt-library/prompts/voice/product-response-style.system.txt"
    ).read_text()
    for case in json.loads(input_files["evaluation-only/cohesion-cases.json"])["cases"]:
        cases.append(
            {
                "case_id": case["id"],
                "group": "cohesion-evaluation",
                "system": style,
                "prompt": case["prompt"],
                "reference": case["reference"],
            }
        )
    if len(cases) != 46 or len({c["case_id"] for c in cases}) != 46:
        raise ValueError("Incomplete or duplicate evaluation matrix")
    return cases


def load_base_model(metadata, seed):
    import mlx.core as mx
    from mlx import nn
    from mlx_lm.utils import _get_classes

    mx.random.seed(seed)
    buffer = fetch("model.safetensors", WEIGHT)
    weights = mx.load(buffer, format="safetensors")
    mx.eval(weights)
    buffer.close()
    del buffer
    config = json.loads((metadata / "config.json").read_bytes())
    if (
        config.get("model_file")
        or config["model_type"] != "qwen2"
        or config["quantization"] != {"group_size": 64, "bits": 4}
    ):
        raise ValueError("Unexpected model implementation or quantization")
    model_class, args_class = _get_classes(config)
    model = model_class(args_class.from_dict(config))
    if hasattr(model, "sanitize"):
        weights = model.sanitize(weights)
    weight_keys = frozenset(weights)
    nn.quantize(
        model,
        group_size=64,
        bits=4,
        class_predicate=lambda p, m: (
            hasattr(m, "to_quantized") and f"{p}.scales" in weight_keys
        ),
    )
    model.load_weights(list(weights.items()), strict=True)
    mx.eval(model.parameters())
    del weights
    gc.collect()
    mx.clear_cache()
    return model


def run(data_root, output):
    import mlx.core as mx
    import mlx.optimizers as optim
    import numpy as np
    from mlx import nn
    from mlx.utils import tree_flatten, tree_map
    from mlx_lm import stream_generate
    from mlx_lm.sample_utils import make_sampler
    from mlx_lm.tuner.lora import LoRALinear
    from mlx_lm.tuner.trainer import default_loss
    from mlx_lm.tuner.utils import linear_to_lora_layers
    from mlx_lm.utils import load_tokenizer

    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    rows, data_receipt, input_files = verified_data(data_root)
    cases = evaluation_cases(input_files)
    write(output / "evaluation-cases.json", cases)
    seed = 0
    recipe = {
        "schema": "waterdeep-ram-qlora-recipe/v1",
        "model": MODEL,
        "revision": REVISION,
        "weight_sha256": WEIGHT[1],
        "seed": seed,
        "iterations": 104,
        "batch_size": 1,
        "optimizer": "Adam",
        "learning_rate": 0.0001,
        "gradient_norm_limit": 1.0,
        "lora_layers": 16,
        "rank": 8,
        "scale": 20.0,
        "dropout": 0.0,
        "keys": [f"self_attn.{p}_proj" for p in ("q", "k", "v", "o")],
        "maximum_sequence_tokens": 256,
        "maximum_new_tokens": 160,
        "temperature": 0,
        "assistant_only_loss": True,
        "selection": "Fixed two epochs; no evaluation-based checkpoint selection.",
        "weight_storage": "RAM only; no base or adapter weight files",
        "production_gate": False,
        "data_receipt_sha256": sha((data_root / "receipt.json").read_bytes()),
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip(),
        "source_dirty": bool(
            subprocess.check_output(["git", "status", "--porcelain"], text=True)
        ),
        "runtime_versions": {
            p: importlib.metadata.version(p)
            for p in ("mlx", "mlx-lm", "transformers", "numpy")
        },
    }
    write(output / "recipe.json", recipe)
    (output / "run_mlx_adapter.py").write_bytes(Path(__file__).read_bytes())
    (output / "semantic_eval.py").write_bytes((HERE / "semantic_eval.py").read_bytes())
    write(output / "data-receipt.json", data_receipt)
    for name, content in input_files.items():
        path = output / "inputs" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    metadata = output / "metadata"
    metadata.mkdir()
    for name, identity in METADATA.items():
        (metadata / name).write_bytes(fetch(name, identity, git_blob=True).read())
    tokenizer = load_tokenizer(
        metadata,
        tokenizer_config_extra={"trust_remote_code": False, "local_files_only": True},
    )
    datasets = {
        split: [supervised_tokens(row, tokenizer) for row in group]
        for split, group in rows.items()
    }
    if (len(datasets["train"]), len(datasets["validation"])) != (52, 12):
        raise ValueError("Unexpected training split sizes")
    write(
        output / "token-lengths.json",
        {
            split: [
                {"example_id": r["example_id"], "tokens": len(t), "assistant_start": n}
                for r, (t, n) in zip(rows[split], group, strict=True)
            ]
            for split, group in datasets.items()
        },
    )
    for case in cases:
        prompt = tokenizer.apply_chat_template(
            [
                {"role": "system", "content": case["system"]},
                {"role": "user", "content": case["prompt"]},
            ],
            tokenize=True,
            return_dict=False,
            add_generation_prompt=True,
        )
        if len(prompt) > 1024:
            raise ValueError("Evaluation prompt exceeds fixed bound")
    mx.set_memory_limit(12 * 1024**3)
    mx.set_cache_limit(256 * 1024**2)
    mx.set_wired_limit(10 * 1024**3)
    model = load_base_model(metadata, seed)
    model.freeze()
    linear_to_lora_layers(
        model, 16, {"rank": 8, "scale": 20.0, "dropout": 0.0, "keys": recipe["keys"]}
    )
    trainable = tree_flatten(model.trainable_parameters())
    if not trainable or any(
        not name.endswith((".lora_a", ".lora_b")) for name, _ in trainable
    ):
        raise ValueError("Only LoRA parameters may train")
    modules = [
        module for _, module in model.named_modules() if isinstance(module, LoRALinear)
    ]
    if len(modules) != 64:
        raise ValueError("Expected four attention projections in sixteen layers")

    def parameter_hashes(adapter):
        return {
            name: sha(np.asarray(array.view(mx.uint8)).tobytes())
            for name, array in tree_flatten(model.parameters())
            if name.endswith((".lora_a", ".lora_b")) == adapter
        }

    frozen_before = parameter_hashes(False)
    adapter_before = parameter_hashes(True)
    write(output / "frozen-before.json", frozen_before)
    started = time.monotonic()
    deadline = started + 1200

    def check_time():
        if time.monotonic() > deadline:
            raise TimeoutError("Bounded experiment deadline exceeded")

    def loss_for(example):
        tokens, offset = example
        return default_loss(
            model, mx.array([tokens]), mx.array([[offset, len(tokens)]])
        )

    def validation():
        model.eval()
        total = 0.0
        count = 0
        for example in datasets["validation"]:
            loss, tokens = loss_for(example)
            value = float(loss.item())
            n = int(tokens.item())
            if not math.isfinite(value):
                raise ValueError("Nonfinite validation loss")
            total += value * n
            count += n
            check_time()
        return total / count

    initial_loss = validation()
    print(
        json.dumps(
            {
                "stage": "loaded",
                "base_validation_loss": initial_loss,
                "trainable_parameters": sum(a.size for _, a in trainable),
            }
        ),
        flush=True,
    )
    optimizer = optim.Adam(learning_rate=recipe["learning_rate"])
    value_and_grad = nn.value_and_grad(model, default_loss)
    rng = np.random.default_rng(seed)
    with (output / "training.jsonl").open("x") as journal:
        for step, index in enumerate(
            np.concatenate([rng.permutation(52), rng.permutation(52)]), 1
        ):
            model.train()
            tokens, offset = datasets["train"][int(index)]
            (loss, _), grads = value_and_grad(
                model, mx.array([tokens]), mx.array([[offset, len(tokens)]])
            )
            leaves = [g for _, g in tree_flatten(grads)]
            norm = mx.sqrt(sum(mx.sum(g.astype(mx.float32) ** 2) for g in leaves))
            mx.eval(loss, norm)
            if not math.isfinite(float(loss.item())) or not math.isfinite(
                float(norm.item())
            ):
                raise ValueError("Nonfinite loss or gradient")
            scale = min(1.0, 1.0 / max(float(norm.item()), 1e-12))
            optimizer.update(model, tree_map(lambda g, scale=scale: g * scale, grads))
            mx.eval(model.parameters(), optimizer.state)
            row = {
                "step": step,
                "example_id": rows["train"][int(index)]["example_id"],
                "loss": float(loss.item()),
                "gradient_norm": float(norm.item()),
            }
            journal.write(json.dumps(row) + "\n")
            journal.flush()
            check_time()
            if step % 13 == 0:
                print(json.dumps({"stage": "training", **row}), flush=True)
    final_loss = validation()
    frozen_after = parameter_hashes(False)
    adapter_after = parameter_hashes(True)
    if frozen_after != frozen_before or adapter_after == adapter_before:
        raise ValueError("Frozen base changed or adapter did not learn")
    write(
        output / "adapter-hashes.json",
        {"before": adapter_before, "after": adapter_after},
    )
    del optimizer, grads, leaves
    gc.collect()
    mx.clear_cache()
    model.eval()
    training_seconds = time.monotonic() - started
    write(
        output / "training-summary.json",
        {
            "steps": 104,
            "initial_validation_loss": initial_loss,
            "adapter_validation_loss": final_loss,
            "base_frozen_verified": True,
            "adapter_changed_verified": True,
            "training_and_validation_seconds": training_seconds,
        },
    )

    def generate(case, enabled):
        for module in modules:
            module.scale = 20.0 if enabled else 0.0
        prompt = tokenizer.apply_chat_template(
            [
                {"role": "system", "content": case["system"]},
                {"role": "user", "content": case["prompt"]},
            ],
            tokenize=True,
            return_dict=False,
            add_generation_prompt=True,
        )
        if len(prompt) > 1024:
            raise ValueError("Evaluation prompt exceeds fixed bound")
        text = ""
        identities = []
        last = None
        start = time.monotonic()
        for last in stream_generate(
            model, tokenizer, prompt, max_tokens=160, sampler=make_sampler(temp=0)
        ):
            text += last.text
            identities.append(int(last.token))
            check_time()
        if last is None:
            raise ValueError("Empty generation stream")
        result = {
            "response": text,
            "token_ids": identities,
            "response_sha256": sha(text.encode()),
            "milliseconds": (time.monotonic() - start) * 1000,
            "finish_reason": last.finish_reason,
            "prompt_tokens": last.prompt_tokens,
            "generation_tokens": last.generation_tokens,
            "generation_tokens_per_second": last.generation_tps,
        }
        if "target" in case:
            result["score"] = assess_target(case["target"], text)
            if last.finish_reason != "stop":
                result["score"].update(valid=False, exact=False, primary_correct=False)
        return result

    warmup = {"system": "Answer briefly.", "prompt": "Say ready."}
    generate(warmup, False)
    generate(warmup, True)
    pairs = []
    with (output / "pairs.jsonl").open("x") as journal:
        for index, case in enumerate(cases):
            pair = {"case_id": case["case_id"], "group": case["group"]}
            for enabled in (False, True) if index % 2 == 0 else (True, False):
                pair["adapter" if enabled else "base"] = generate(case, enabled)
            pair["reference"] = case.get("reference", case.get("target"))
            journal.write(json.dumps(pair) + "\n")
            journal.flush()
            pairs.append(pair)
            print(
                json.dumps(
                    {
                        "stage": "comparison",
                        "completed": index + 1,
                        "total": len(cases),
                        "case_id": case["case_id"],
                    }
                ),
                flush=True,
            )
    report = {
        "schema": "waterdeep-ram-qlora-result/v1",
        "training_performed": True,
        "base_frozen_verified": True,
        "adapter_changed_verified": True,
        "adapter_persisted": False,
        "weights_written_to_disk": False,
        "production_gate": False,
        "independent_human_review": False,
        "human_audio": False,
        "initial_validation_loss": initial_loss,
        "adapter_validation_loss": final_loss,
        "training_and_validation_seconds": training_seconds,
        "elapsed_seconds": time.monotonic() - started,
        "mlx_peak_bytes": mx.get_peak_memory(),
        "process_peak_rss_platform_units": resource.getrusage(
            resource.RUSAGE_SELF
        ).ru_maxrss,
        "host": platform.platform(),
        "cases": len(pairs),
        "groups": {},
    }
    for group in sorted({p["group"] for p in pairs}):
        selected = [p for p in pairs if p["group"] == group]
        report["groups"][group] = {}
        for mode in ("base", "adapter"):
            scores = [p[mode]["score"] for p in selected if "score" in p[mode]]
            report["groups"][group][mode] = {
                "cases": len(selected),
                "quality_reviewed": False,
                "state_scored": len(scores),
                "valid": sum(s["valid"] for s in scores) if scores else None,
                "primary_correct": sum(s["primary_correct"] for s in scores)
                if scores
                else None,
                "exact": sum(s["exact"] for s in scores) if scores else None,
                "unsafe_proposals": sum(
                    s["unsafe_commit_proposal"] is True for s in scores
                )
                if scores
                else None,
            }
    write(output / "report.json", report)
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-ram-qlora-receipt/v1",
            "files": {
                str(p.relative_to(output)): sha(p.read_bytes())
                for p in output.rglob("*")
                if p.is_file()
            },
        },
    )
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    run(arguments.data, arguments.output)
