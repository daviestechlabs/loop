import hashlib
import io
import json
import os
import tempfile
import unittest
import urllib.request
from pathlib import Path

from run_mlx_adapter import (
    PublicModelRedirect,
    evaluation_cases,
    read_verified,
    supervised_tokens,
    verified_data,
)
from semantic_eval import sha


class AdapterInputTests(unittest.TestCase):
    def test_repository_evaluation_preflight_and_oracle_exclusion(self):
        from run_mlx_adapter import ROOT

        new_cases = json.loads(
            (
                ROOT / "workflows/qwen-qlora-runner/data/turn-repair-v1/examples.json"
            ).read_bytes()
        )
        # Match the preparer's evaluation payload; keep expected answers outside prompts.
        selected = [c for c in new_cases["cases"] if c["split"] == "evaluation"]
        inputs = {
            "evaluation-only/turn-cases.json": json.dumps(
                {"system_prompt": "Interpret the utterance.", "cases": selected}
            ).encode(),
            "evaluation-only/cohesion-cases.json": (
                ROOT
                / "workflows/qwen-qlora-runner/data/cohesion-v1/evaluation-cases.json"
            ).read_bytes(),
        }
        cases = evaluation_cases(inputs)
        self.assertEqual(len(cases), 46)
        for case in cases:
            if "target" in case:
                self.assertEqual(
                    set(json.loads(case["prompt"])), {"utterance", "evidence"}
                )

    def test_download_rejects_truncation_overflow_and_changed_content(self):
        data = b"pinned model bytes"
        self.assertEqual(
            read_verified(io.BytesIO(data), len(data), sha(data)).read(), data
        )
        for content in (data[:-1], data + b"!", b"x" * len(data)):
            with self.assertRaises(ValueError):
                read_verified(io.BytesIO(content), len(data), sha(data))
        with self.assertRaises(ValueError):
            read_verified(io.BytesIO(), 0, sha(b""))

    def test_metadata_uses_git_blob_identity(self):
        raw = b"small metadata"
        digest = hashlib.sha1(
            b"blob " + str(len(raw)).encode() + b"\0" + raw
        ).hexdigest()
        self.assertEqual(
            read_verified(io.BytesIO(raw), len(raw), digest, git_blob=True).read(), raw
        )

    def test_redirect_cannot_leak_to_an_unrelated_or_plaintext_host(self):
        handler = PublicModelRedirect()
        request = urllib.request.Request("https://huggingface.co/model")
        for url in (
            "http://huggingface.co/file",
            "https://huggingface.co.evil.test/file",
            "https://user@cdn.hf.co/file",
            "https://localhost/file",
        ):
            with self.assertRaises(ValueError):
                handler.redirect_request(request, None, 302, "Found", {}, url)
        self.assertEqual(
            handler.redirect_request(
                request, None, 302, "Found", {}, "https://cdn.hf.co/file"
            ).full_url,
            "https://cdn.hf.co/file",
        )

    def fixture(self, root):
        files = {
            "training-inputs/corpus-authorization.json": b'{"rights_basis":"original"}',
            "evaluation-only/turn-cases.json": b'{"cases":[]}',
            "evaluation-only/cohesion-cases.json": b'{"cases":[]}',
        }
        manifest = {
            "purpose": "training",
            "authorization": {
                "usage": "training-authorized",
                "evidence_sha256": sha(
                    files["training-inputs/corpus-authorization.json"]
                ),
            },
            "splits": {},
        }
        for split in ("train", "validation"):
            raw = json.dumps(
                {
                    "example_id": split,
                    "messages": [
                        {"role": "system", "content": "S"},
                        {"role": "user", "content": split},
                        {"role": "assistant", "content": "A"},
                    ],
                }
            ).encode()
            digest = sha(raw)
            manifest["splits"][split] = [digest]
            files[f"training-inputs/objects/{digest}.jsonl"] = raw
        files["training-inputs/training.json"] = json.dumps(manifest).encode()
        for name, content in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        receipt = {
            "schema": "waterdeep-turn-data-preparation/v1",
            "files": {n: sha(c) for n, c in files.items()},
        }
        (root / "receipt.json").write_text(json.dumps(receipt))
        return receipt

    def test_input_bytes_remain_bound_to_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            self.assertEqual(len(verified_data(root)[0]["train"]), 1)
            (root / "evaluation-only/turn-cases.json").write_text("changed")
            with self.assertRaisesRegex(ValueError, "changed"):
                verified_data(root)

    def test_existing_but_unlisted_object_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            receipt = self.fixture(root)
            name = next(n for n in receipt["files"] if "/objects/" in n)
            del receipt["files"][name]
            (root / "receipt.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "absent from the receipt"):
                verified_data(root)

    def test_parent_symlink_and_traversal_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            receipt = self.fixture(root)
            directory = root / "evaluation-only"
            directory.rename(root / "redirected")
            directory.symlink_to(root / "redirected", target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "Invalid prepared input path"):
                verified_data(root)
            receipt["files"] = {"../outside": sha(b"")}
            (root / "receipt.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "Invalid prepared input path"):
                verified_data(root)

    def test_authorization_is_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            receipt = self.fixture(root)
            name = "training-inputs/training.json"
            manifest = json.loads((root / name).read_bytes())
            manifest["authorization"]["usage"] = "evaluation-only"
            (root / name).write_text(json.dumps(manifest))
            receipt["files"][name] = sha((root / name).read_bytes())
            (root / "receipt.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "authorization is absent"):
                verified_data(root)

    def test_validation_cannot_reuse_training_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            receipt = self.fixture(root)
            name = "training-inputs/training.json"
            manifest = json.loads((root / name).read_bytes())
            manifest["splits"]["validation"] = manifest["splits"]["train"]
            (root / name).write_text(json.dumps(manifest))
            receipt["files"][name] = sha((root / name).read_bytes())
            (root / "receipt.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "overlapping supervised"):
                verified_data(root)

    def test_changed_authorization_binding_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            receipt = self.fixture(root)
            name = "training-inputs/corpus-authorization.json"
            (root / name).write_text('{"rights_basis":"changed"}')
            receipt["files"][name] = sha((root / name).read_bytes())
            (root / "receipt.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "authorization binding changed"):
                verified_data(root)

    def test_assistant_prefix_must_match_without_truncation(self):
        class Tokenizer:
            def __init__(self, complete, prefix):
                self.complete, self.prefix = complete, prefix

            def apply_chat_template(self, messages, **kwargs):
                return self.complete if len(messages) == 3 else self.prefix

        row = {
            "messages": [
                {"role": r, "content": "x"} for r in ("system", "user", "assistant")
            ]
        }
        self.assertEqual(
            supervised_tokens(row, Tokenizer([1, 2, 3, 4], [1, 2])), ([1, 2, 3, 4], 2)
        )
        for complete, prefix in (
            ([1, 2, 3], [2]),
            ([1], [1]),
            (list(range(257)), [0]),
            ([True, 2], [True]),
        ):
            with self.assertRaises(ValueError):
                supervised_tokens(row, Tokenizer(complete, prefix))


@unittest.skipUnless(
    os.environ.get("RUN_WATERDEEP_MLX_TESTS") == "1",
    "Requires explicit native Metal runtime test",
)
class NativeAdapterTests(unittest.TestCase):
    def test_completion_mask_and_quantized_base_freeze(self):
        import mlx.core as mx
        import mlx.optimizers as optim
        import numpy as np
        from mlx import nn
        from mlx.utils import tree_flatten
        from mlx_lm.tuner.lora import LoRALinear
        from mlx_lm.tuner.trainer import default_loss

        class TinyModel(nn.Module):
            def __init__(self):
                super().__init__()
                self.embedding = nn.Embedding(64, 64)
                self.projection = nn.Linear(64, 64)

            def __call__(self, inputs):
                return self.projection(self.embedding(inputs))

        mx.random.seed(0)
        model = TinyModel()
        nn.quantize(model, group_size=64, bits=4)
        model.freeze()
        model.projection = LoRALinear.from_base(model.projection, r=2, scale=2.0)

        def identities(adapter):
            return {
                n: sha(np.asarray(a.view(mx.uint8)).tobytes())
                for n, a in tree_flatten(model.parameters())
                if n.endswith((".lora_a", ".lora_b")) == adapter
            }

        before_base, before_adapter = identities(False), identities(True)
        from run_mlx_ablation import restore_adapter, snapshot_adapter

        initial_snapshot = snapshot_adapter(model)
        batch = mx.array([[1, 2, 3, 4, 5]])
        (loss, count), grads = nn.value_and_grad(model, default_loss)(
            model, batch, mx.array([[3, 5]])
        )
        self.assertEqual(int(count.item()), 2)
        logits = model(batch[:, :-1])
        expected = nn.losses.cross_entropy(
            logits[:, 2:, :], batch[:, 3:], reduction="mean"
        )
        self.assertAlmostEqual(float(loss.item()), float(expected.item()), places=5)
        self.assertTrue(
            all(n.endswith((".lora_a", ".lora_b")) for n, _ in tree_flatten(grads))
        )
        optimizer = optim.Adam(learning_rate=0.001)
        optimizer.update(model, grads)
        mx.eval(model.parameters(), optimizer.state)
        self.assertEqual(identities(False), before_base)
        self.assertNotEqual(identities(True), before_adapter)
        trained_snapshot = snapshot_adapter(model)
        trained_hashes = identities(True)
        restore_adapter(model, initial_snapshot)
        self.assertEqual(identities(True), before_adapter)
        restore_adapter(model, trained_snapshot)
        self.assertEqual(identities(True), trained_hashes)
        self.assertEqual(identities(False), before_base)
        with self.assertRaises(ValueError):
            restore_adapter(model, trained_snapshot[:-1])
        with self.assertRaises(ValueError):
            restore_adapter(model, trained_snapshot + trained_snapshot[:1])
        model.projection.scale = 0.0
        self.assertTrue(
            bool(
                mx.allclose(
                    model(batch), model.projection.linear(model.embedding(batch))
                ).item()
            )
        )


if __name__ == "__main__":
    unittest.main()
