"""Compare turn-only, prose-only, and mixed adapters with an explicit turn contract."""

import argparse
import copy
import gc
import importlib.metadata
import json
import math
import os
import random
import subprocess
import time
from pathlib import Path

from run_mlx_adapter import (
    METADATA,
    MODEL,
    REVISION,
    WEIGHT,
    evaluation_cases,
    fetch,
    load_base_model,
    supervised_tokens,
    verified_data,
)
from semantic_eval import HERE, PROMPTS, assess_target, sha, write

SEEDS = (0, 1, 2)
VARIANTS = ("turn-only", "prose-only", "mixed")
KEYS = [f"self_attn.{p}_proj" for p in ("q", "k", "v", "o")]


def prepare_ablation(rows, input_files):
    derived = copy.deepcopy(rows)
    for split, group in derived.items():
        counts = {"turn": 0, "prose": 0}
        for row in group:
            if row["example_id"].startswith("turn-"):
                row["messages"][0]["content"] = PROMPTS["contract"]
                row["ablation_source"] = "turn"
            elif row["example_id"].startswith("cohesion-"):
                row["ablation_source"] = "prose"
            else:
                raise ValueError("Unknown supervised source")
            counts[row["ablation_source"]] += 1
        if counts != (
            {"turn": 12, "prose": 40} if split == "train" else {"turn": 4, "prose": 8}
        ):
            raise ValueError("Unexpected ablation source counts")
    cases = evaluation_cases(input_files)
    for case in cases:
        if "target" in case:
            case["system"] = PROMPTS["contract"]
    return derived, cases


def epoch_indices(rows, variant, seed):
    if variant not in VARIANTS:
        raise ValueError("Unknown training variant")
    rng = random.Random(seed)
    # Filter the same mixed ordering. Each included row appears once per epoch.
    order = []
    for _ in range(2):
        epoch = list(range(len(rows)))
        rng.shuffle(epoch)
        order.extend(epoch)
    return [
        int(i)
        for i in order
        if variant == "mixed"
        or rows[int(i)]["ablation_source"] == variant.split("-")[0]
    ]


def snapshot_adapter(model):
    import mlx.core as mx
    import numpy as np
    from mlx.utils import tree_flatten

    parameters = tree_flatten(model.trainable_parameters())
    if not parameters or any(
        not n.endswith((".lora_a", ".lora_b")) for n, _ in parameters
    ):
        raise ValueError("Only adapter parameters may be retained")
    return [(name, mx.array(np.asarray(value).copy())) for name, value in parameters]


def restore_adapter(model, snapshot):
    from mlx.utils import tree_flatten

    expected = {
        n: (a.shape, a.dtype) for n, a in tree_flatten(model.trainable_parameters())
    }
    actual = {n: (a.shape, a.dtype) for n, a in snapshot}
    if not expected or len(actual) != len(snapshot) or actual != expected:
        raise ValueError("Adapter snapshot does not match the current model")
    model.load_weights(snapshot, strict=False)


def run(data_root, output):
    import mlx.core as mx
    import mlx.optimizers as optim
    import numpy as np
    from mlx import nn
    from mlx.utils import tree_flatten, tree_map, tree_unflatten
    from mlx_lm import stream_generate
    from mlx_lm.sample_utils import make_sampler
    from mlx_lm.tuner.lora import LoRALinear
    from mlx_lm.tuner.trainer import default_loss
    from mlx_lm.tuner.utils import linear_to_lora_layers
    from mlx_lm.utils import load_tokenizer

    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    rows, receipt, files = verified_data(data_root)
    derived, cases = prepare_ablation(rows, files)
    conditions = ["base"] + [
        f"{variant}/seed-{seed}" for seed in SEEDS for variant in VARIANTS
    ]
    recipe = {
        "schema": "waterdeep-mlx-ablation-recipe/v1",
        "model": MODEL,
        "revision": REVISION,
        "weight_sha256": WEIGHT[1],
        "seeds": list(SEEDS),
        "variants": list(VARIANTS),
        "conditions": conditions,
        "epochs": 2,
        "batch_size": 1,
        "updates_per_seed": {"turn-only": 24, "prose-only": 80, "mixed": 104},
        "optimizer": "Adam",
        "learning_rate": 0.0001,
        "gradient_norm_limit": 1.0,
        "rank": 8,
        "scale": 20.0,
        "dropout": 0.0,
        "layers": 16,
        "keys": KEYS,
        "maximum_sequence_tokens": 768,
        "maximum_new_tokens": 160,
        "temperature": 0,
        "turn_system_prompt_sha256": sha(PROMPTS["contract"].encode()),
        "derivation": "Replace only the system message of authorized turn rows with the explicit turn contract. Preserve user messages and assistant targets.",
        "ordering": "Each epoch filters the same shuffled 52-row sequence for each variant. Evaluation rotates ten conditions by case index.",
        "selection": "Fixed recipe; no early stopping or evaluation-based selection.",
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
        "input_receipt_sha256": sha((data_root / "receipt.json").read_bytes()),
        "adapter_storage": "RAM only; no adapter survives process exit",
        "production_gate": False,
    }
    write(output / "recipe.json", recipe)
    write(output / "input-receipt.json", receipt)
    write(output / "derived-supervision.json", derived)
    write(output / "evaluation-cases.json", cases)
    for name in ("run_mlx_ablation.py", "run_mlx_adapter.py", "semantic_eval.py"):
        (output / name).write_bytes((HERE / name).read_bytes())
    for name, content in files.items():
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
        split: [supervised_tokens(row, tokenizer, maximum=768) for row in group]
        for split, group in derived.items()
    }
    write(
        output / "token-lengths.json",
        {
            split: [
                {
                    "example_id": row["example_id"],
                    "tokens": len(tokens),
                    "assistant_start": offset,
                }
                for row, (tokens, offset) in zip(derived[split], group, strict=True)
            ]
            for split, group in datasets.items()
        },
    )

    def prompt_tokens(case):
        prompt = tokenizer.apply_chat_template(
            [
                {"role": "system", "content": case["system"]},
                {"role": "user", "content": case["prompt"]},
            ],
            tokenize=True,
            return_dict=False,
            add_generation_prompt=True,
        )
        if not 0 < len(prompt) <= 1024:
            raise ValueError("Evaluation prompt bound failed")
        return prompt

    prompts = {case["case_id"]: prompt_tokens(case) for case in cases}
    mx.set_memory_limit(12 * 1024**3)
    mx.set_cache_limit(256 * 1024**2)
    mx.set_wired_limit(10 * 1024**3)
    model = load_base_model(metadata, 0)
    model.freeze()
    linear_to_lora_layers(
        model, 16, {"rank": 8, "scale": 20.0, "dropout": 0.0, "keys": KEYS}
    )

    def modules():
        values = [(n, m) for n, m in model.named_modules() if isinstance(m, LoRALinear)]
        if len(values) != 64:
            raise ValueError("Unexpected adapter layer count")
        return values

    def hashes(adapter):
        return {
            n: sha(np.asarray(a.view(mx.uint8)).tobytes())
            for n, a in tree_flatten(model.parameters())
            if n.endswith((".lora_a", ".lora_b")) == adapter
        }

    frozen = hashes(False)
    write(output / "frozen-parameters.json", frozen)
    started = time.monotonic()
    deadline = started + 1800

    def check_time():
        if time.monotonic() > deadline:
            raise TimeoutError("Ablation deadline exceeded")

    def validation():
        model.eval()
        totals = {"turn": [0.0, 0], "prose": [0.0, 0]}
        for row, (tokens, offset) in zip(
            derived["validation"], datasets["validation"], strict=True
        ):
            loss, count = default_loss(
                model, mx.array([tokens]), mx.array([[offset, len(tokens)]])
            )
            value = float(loss.item())
            n = int(count.item())
            if not math.isfinite(value):
                raise ValueError("Nonfinite validation loss")
            totals[row["ablation_source"]][0] += value * n
            totals[row["ablation_source"]][1] += n
            check_time()
        return {source: total / count for source, (total, count) in totals.items()}

    baseline_loss = validation()
    saved = {}
    training = []
    write(output / "base-validation.json", baseline_loss)
    for seed in SEEDS:
        mx.random.seed(seed)
        replacements = [
            (name, LoRALinear.from_base(module.linear, r=8, scale=20.0, dropout=0.0))
            for name, module in modules()
        ]
        model.update_modules(tree_unflatten(replacements))
        initial = snapshot_adapter(model)
        initial_hashes = hashes(True)
        for variant in VARIANTS:
            condition = f"{variant}/seed-{seed}"
            restore_adapter(model, initial)
            if hashes(True) != initial_hashes:
                raise ValueError("Variant initialization changed")
            optimizer = optim.Adam(learning_rate=0.0001)
            value_and_grad = nn.value_and_grad(model, default_loss)
            order = epoch_indices(derived["train"], variant, seed)
            train_start = time.monotonic()
            with (output / f"training-{variant}-{seed}.jsonl").open("x") as journal:
                for step, index in enumerate(order, 1):
                    model.train()
                    tokens, offset = datasets["train"][index]
                    (loss, _), grads = value_and_grad(
                        model, mx.array([tokens]), mx.array([[offset, len(tokens)]])
                    )
                    norm = mx.sqrt(
                        sum(
                            mx.sum(g.astype(mx.float32) ** 2)
                            for _, g in tree_flatten(grads)
                        )
                    )
                    mx.eval(loss, norm)
                    value = float(loss.item())
                    norm_value = float(norm.item())
                    if not math.isfinite(value) or not math.isfinite(norm_value):
                        raise ValueError("Nonfinite training loss or gradient")
                    scale = min(1.0, 1.0 / max(norm_value, 1e-12))
                    optimizer.update(
                        model, tree_map(lambda g, scale=scale: g * scale, grads)
                    )
                    mx.eval(model.parameters(), optimizer.state)
                    journal.write(
                        json.dumps(
                            {
                                "step": step,
                                "example_id": derived["train"][index]["example_id"],
                                "loss": value,
                                "gradient_norm": norm_value,
                            }
                        )
                        + "\n"
                    )
                    journal.flush()
                    check_time()
            losses = validation()
            saved[condition] = snapshot_adapter(model)
            if hashes(False) != frozen or hashes(True) == initial_hashes:
                raise ValueError("Base changed or adapter stayed unchanged")
            result = {
                "condition": condition,
                "steps": len(order),
                "training_and_validation_seconds": time.monotonic() - train_start,
                "validation_loss": losses,
                "initial_adapter_hashes": initial_hashes,
                "final_adapter_hashes": hashes(True),
                "base_frozen_verified": True,
            }
            training.append(result)
            write(output / f"training-{variant}-{seed}.json", result)
            print(
                json.dumps({k: v for k, v in result.items() if "hashes" not in k}),
                flush=True,
            )
            del optimizer, grads, value_and_grad
            gc.collect()
            mx.clear_cache()
    model.eval()
    generation_start = time.monotonic()

    def select(condition):
        if condition != "base":
            restore_adapter(model, saved[condition])
        for _, module in modules():
            module.scale = 0.0 if condition == "base" else 20.0

    def generate(prompt, condition):
        select(condition)
        text = ""
        token_ids = []
        last = None
        start = time.monotonic()
        for last in stream_generate(
            model, tokenizer, prompt, max_tokens=160, sampler=make_sampler(temp=0)
        ):
            text += last.text
            token_ids.append(int(last.token))
            check_time()
        if last is None:
            raise ValueError("Empty model response")
        return {
            "response": text,
            "response_sha256": sha(text.encode()),
            "token_ids": token_ids,
            "finish_reason": last.finish_reason,
            "milliseconds": (time.monotonic() - start) * 1000,
            "generation_tokens": last.generation_tokens,
            "prompt_tokens": last.prompt_tokens,
            "generation_tokens_per_second": last.generation_tps,
        }

    warmup = prompt_tokens({"system": "Answer briefly.", "prompt": "Say ready."})
    for condition in conditions:
        generate(warmup, condition)
    observed = []
    with (output / "trials.jsonl").open("x") as journal:
        for index, case in enumerate(cases):
            offset = index % len(conditions)
            for condition in conditions[offset:] + conditions[:offset]:
                result = {
                    "case_id": case["case_id"],
                    "group": case["group"],
                    "condition": condition,
                    **generate(prompts[case["case_id"]], condition),
                }
                if "target" in case:
                    result["score"] = assess_target(case["target"], result["response"])
                    if result["finish_reason"] != "stop":
                        result["score"].update(
                            valid=False, exact=False, primary_correct=False
                        )
                journal.write(json.dumps(result) + "\n")
                journal.flush()
                observed.append(result)
            print(
                json.dumps(
                    {
                        "stage": "comparison",
                        "completed_cases": index + 1,
                        "total_cases": len(cases),
                        "conditions": len(conditions),
                    }
                ),
                flush=True,
            )
    if hashes(False) != frozen:
        raise ValueError("Base changed during comparison")
    report = {
        "schema": "waterdeep-mlx-ablation-result/v1",
        "training_performed": True,
        "base_frozen_verified": True,
        "conditions": len(conditions),
        "cases": len(cases),
        "generations": len(observed),
        "training_updates": sum(t["steps"] for t in training),
        "base_validation_loss": baseline_loss,
        "validation": {t["condition"]: t["validation_loss"] for t in training},
        "elapsed_seconds": time.monotonic() - started,
        "generation_seconds": time.monotonic() - generation_start,
        "mlx_peak_bytes": mx.get_peak_memory(),
        "production_gate": False,
        "adapter_persisted": False,
        "human_audio": False,
        "independent_human_review": False,
        "groups": {},
    }
    for group in sorted({case["group"] for case in cases}):
        report["groups"][group] = {}
        for condition in conditions:
            selected = [
                r
                for r in observed
                if r["group"] == group and r["condition"] == condition
            ]
            scores = [r["score"] for r in selected if "score" in r]
            report["groups"][group][condition] = {
                "cases": len(selected),
                "valid": sum(s["valid"] for s in scores) if scores else None,
                "primary_correct": sum(s["primary_correct"] for s in scores)
                if scores
                else None,
                "exact": sum(s["exact"] for s in scores) if scores else None,
                "quality_reviewed": False,
                "length_limited": sum(r["finish_reason"] != "stop" for r in selected),
            }
    write(output / "report.json", report)
    write(
        output / "receipt.json",
        {
            "schema": "waterdeep-mlx-ablation-receipt/v1",
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
    args = parser.parse_args()
    run(args.data, args.output)
