"""Read native source layers with an independent tar implementation."""

import hashlib
import io
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tarfile
import tempfile
import unittest

from test_dnd_source_compiler import fixture

BUNDLE = Path(sys.argv.pop(1)).resolve()
COMPILER = Path(sys.argv.pop(1)).resolve()


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class SourceBundle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        data, manifest = fixture()
        cls.scope = [data[key] for key in ("collection", "corpus_version", "ruleset")]
        cls.manifest_bytes = json.dumps(manifest).encode()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = json.dumps(data).encode()
            (root / "input").write_bytes(raw)
            (root / "manifest").write_bytes(cls.manifest_bytes)
            subprocess.run(
                [
                    str(COMPILER),
                    str(root / "input"),
                    sha(raw),
                    str(root / "manifest"),
                    sha(cls.manifest_bytes),
                    "3" * 64,
                    str(root / "index"),
                ],
                capture_output=True,
                check=True,
                timeout=30,
            )
            cls.index_bytes = (root / "index").read_bytes()

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.index = self.root / "input index"
        self.manifest = self.root / "input manifest"
        self.output = self.root / "layer.tar"
        self.index.write_bytes(self.index_bytes)
        self.manifest.write_bytes(self.manifest_bytes)
        self.arguments = [
            str(self.index),
            str(self.manifest),
            sha(self.index_bytes),
            sha(self.manifest_bytes),
            "3" * 64,
            *self.scope,
            str(self.output),
        ]

    def execute(self, success=True, **kwargs):
        result = subprocess.run(
            [str(BUNDLE), *self.arguments], capture_output=True, timeout=20, **kwargs
        )
        self.assertEqual(result.returncode, 0 if success else 1, result.stderr.decode())
        self.assertNotIn(b"WARDING STEP", result.stdout + result.stderr)
        self.assertNotIn(str(self.root).encode(), result.stdout + result.stderr)
        if success:
            report = json.loads(result.stdout)
            self.assertEqual(report["archive_sha256"], sha(self.output.read_bytes()))
            self.assertEqual(
                report["layer_sha256"], sha(self.layer(self.output.read_bytes()))
            )
            self.assertEqual(report["index_bytes"], len(self.index_bytes))
            self.assertEqual(report["manifest_bytes"], len(self.manifest_bytes))
            return self.output.read_bytes()
        self.assertEqual(result.stdout, b"")
        self.assertFalse(list(self.root.glob("*.tmp.*")))
        return None

    def layer(self, raw):
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
            description = json.load(archive.extractfile("manifest.json"))
            self.assertEqual(len(description), 1)
            descriptor = description[0]
            self.assertEqual(descriptor["RepoTags"], [])
            self.assertEqual(descriptor["Layers"], ["layer.tar"])
            self.assertEqual(
                archive.getnames(), [descriptor["Config"], "manifest.json", "layer.tar"]
            )
            self.assertTrue(all(member.isfile() for member in archive.getmembers()))
            config_raw = archive.extractfile(descriptor["Config"]).read()
            self.assertEqual(descriptor["Config"], sha(config_raw) + ".json")
            config = json.loads(config_raw)
            self.assertEqual(
                (config["os"], config["architecture"], config["config"]),
                ("linux", "amd64", {}),
            )
            layer = archive.extractfile("layer.tar").read()
            self.assertEqual(
                config["rootfs"],
                {"type": "layers", "diff_ids": ["sha256:" + sha(layer)]},
            )
            return layer

    def test_layer_contains_only_exact_readonly_source_pair(self):
        raw = self.execute()
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(len(raw) % 512, 0)
        self.assertEqual(raw[-1024:], bytes(1024))
        with tarfile.open(fileobj=io.BytesIO(self.layer(raw)), mode="r:") as archive:
            members = archive.getmembers()
            self.assertEqual(
                [m.name for m in members], ["index.dndsidx", "manifest.json"]
            )
            for member, expected in zip(
                members, [self.index_bytes, self.manifest_bytes], strict=True
            ):
                self.assertTrue(member.isfile())
                self.assertEqual(
                    (member.mode, member.uid, member.gid, member.mtime),
                    (0o444, 65532, 65532, 0),
                )
                self.assertEqual(member.linkname, "")
                self.assertEqual(archive.extractfile(member).read(), expected)
            archive.extractall(self.root / "unpacked", filter="data")
        # Re-admit the extracted pair through the actual C loader, not the tar reader.
        self.arguments[0:2] = [
            str(self.root / "unpacked/index.dndsidx"),
            str(self.root / "unpacked/manifest.json"),
        ]
        self.output.unlink()
        self.assertEqual(self.execute(), raw)

    def test_packaging_is_deterministic_and_ignores_host_metadata(self):
        first = self.execute()
        self.output.unlink()
        os.utime(self.index, (1_234_567, 7_654_321))
        self.index.chmod(0o400)
        self.manifest.chmod(0o600)
        self.assertEqual(self.execute(), first)

    def test_tar_padding_at_block_edges(self):
        original_manifest = self.manifest_bytes
        original_index = self.index_bytes
        for remainder in (0, 1, 511):
            with self.subTest(remainder=remainder):
                padding = (remainder - len(original_manifest)) % 512
                self.manifest_bytes = original_manifest + b" " * padding
                # The fixture artifact binds the exact manifest bytes in its header.
                self.index_bytes = (
                    original_index[:8]
                    + bytes.fromhex(sha(self.manifest_bytes))
                    + original_index[40:]
                )
                self.index.write_bytes(self.index_bytes)
                self.manifest.write_bytes(self.manifest_bytes)
                self.arguments[2:4] = [sha(self.index_bytes), sha(self.manifest_bytes)]
                raw = self.execute()
                with tarfile.open(
                    fileobj=io.BytesIO(self.layer(raw)), mode="r:"
                ) as archive:
                    self.assertEqual(
                        archive.extractfile("manifest.json").read(), self.manifest_bytes
                    )
                self.output.unlink()

    def test_existing_file_directory_and_symlink_are_preserved(self):
        sentinel = b"existing private output"
        self.output.write_bytes(sentinel)
        self.execute(False)
        self.assertEqual(self.output.read_bytes(), sentinel)
        self.output.unlink()
        self.output.mkdir()
        self.execute(False)
        self.assertTrue(self.output.is_dir())
        self.output.rmdir()
        target = self.root / "target"
        target.write_bytes(sentinel)
        self.output.symlink_to(target)
        self.execute(False)
        self.assertTrue(self.output.is_symlink())
        self.assertEqual(target.read_bytes(), sentinel)

    def test_wrong_pins_and_scope_publish_nothing(self):
        for index, value in [
            (2, "0" * 64),
            (3, "0" * 64),
            (4, "0" * 64),
            (5, "wrong_collection"),
            (6, "sha256:" + "2" * 64),
            (7, "dnd-5e-2024"),
            (2, "f" * 65),
            (3, ""),
        ]:
            with self.subTest(argument=index):
                previous = self.arguments[index]
                self.arguments[index] = value
                self.execute(False)
                self.assertFalse(self.output.exists())
                self.arguments[index] = previous

    def test_changed_content_rejects_even_with_updated_outer_hash(self):
        raw = self.index_bytes.replace(b"Once per turn", b"Once per year")
        self.assertNotEqual(raw, self.index_bytes)
        self.index.write_bytes(raw)
        self.arguments[2] = sha(raw)
        self.execute(False)
        self.assertFalse(self.output.exists())

    def test_changed_manifest_rejects_even_with_updated_outer_hash(self):
        manifest = json.loads(self.manifest_bytes)
        manifest["sources"][0]["source_sha256"] = "0" * 64
        raw = json.dumps(manifest).encode()
        self.manifest.write_bytes(raw)
        self.arguments[3] = sha(raw)
        self.execute(False)
        self.assertFalse(self.output.exists())

    def test_truncated_and_missing_pair_publish_nothing(self):
        for path in (self.index, self.manifest):
            raw = path.read_bytes()
            for length in (0, 1, len(raw) - 1):
                with self.subTest(file=path.name, length=length):
                    path.write_bytes(raw[:length])
                    self.execute(False)
                    self.assertFalse(self.output.exists())
            path.unlink()
            self.execute(False)
            path.write_bytes(raw)

    def test_symlink_directory_fifo_and_oversize_inputs_reject(self):
        for argument, maximum in [(0, 64 * 1024 * 1024), (1, 256 * 1024)]:
            original = self.arguments[argument]
            path = self.root / "bad-input"
            self.arguments[argument] = str(path)
            path.symlink_to(original)
            self.execute(False)
            path.unlink()
            path.mkdir()
            self.execute(False)
            path.rmdir()
            os.mkfifo(path)
            self.execute(False)
            path.unlink()
            with path.open("wb") as stream:
                stream.truncate(maximum + 1)
            self.execute(False)
            path.unlink()
            self.arguments[argument] = original
        self.assertFalse(self.output.exists())

    def test_partial_write_failure_removes_unpublished_file(self):
        def limit_file():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        self.execute(False, preexec_fn=limit_file)
        self.assertFalse(self.output.exists())

    def test_missing_output_parent_does_not_create_directories(self):
        self.arguments[-1] = str(self.root / "absent" / "layer.tar")
        self.execute(False)
        self.assertFalse((self.root / "absent").exists())


if __name__ == "__main__":
    unittest.main()
