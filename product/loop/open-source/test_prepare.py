"""Exercise publication boundaries with disposable Git objects and local files."""

import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "prepare", Path(__file__).with_name("prepare.py")
)
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class BoundaryTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="loop-source-boundary-")
        self.root = Path(self.scratch.name) / "repo"
        self.root.mkdir()
        self.run_git("init", "--quiet")
        self.boundary = {
            "include": ["src/"],
            "exclude_parts": ["evidence", ".git", "fixtures"],
            "exclude_globs": ["**/*.key"],
            "text_suffixes": [".c"],
            "text_names": [],
            "publication_holds": ["local review"],
        }

    def tearDown(self):
        self.scratch.cleanup()

    def run_git(self, *args, data=None):
        return subprocess.check_output(["git", "-C", str(self.root), *args], input=data)

    def commit_file(self, body, mode="100644", path="src/main.c"):
        blob = self.run_git("hash-object", "-w", "--stdin", data=body).decode().strip()
        self.run_git(
            "update-index", "--add", "--cacheinfo", mode + "," + blob + "," + path
        )
        tree = self.run_git("write-tree").decode().strip()
        return (
            self.run_git(
                "-c",
                "user.name=Boundary Fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "-c",
                "commit.gpgsign=false",
                "commit-tree",
                tree,
                "-m",
                "synthetic fixture",
            )
            .decode()
            .strip()
        )

    def candidate(self, body=b"int main(void) { return 0; }\n", mode="100644"):
        revision = self.commit_file(body, mode)
        commit, entries = prepare.inventory(self.root, revision, self.boundary)
        output = Path(self.scratch.name) / "candidate"
        prepare.export(self.root, commit, entries, self.boundary, output)
        return output

    def test_export_uses_commit_bytes_and_excludes_untracked_files(self):
        revision = self.commit_file(b"original source\n")
        (self.root / "src").mkdir()
        (self.root / "src/main.c").write_text("changed working tree\n")
        (self.root / "src/private.c").write_text("untracked fixture\n")
        commit, entries = prepare.inventory(self.root, revision, self.boundary)
        output = Path(self.scratch.name) / "candidate"
        prepare.export(self.root, commit, entries, self.boundary, output)
        self.assertEqual((output / "src/main.c").read_bytes(), b"original source\n")
        self.assertFalse((output / "src/private.c").exists())
        self.assertFalse((output / ".git").exists())
        prepare.verify(output)

    def test_private_paths_and_traversal_are_rejected(self):
        for path in (
            "src/evidence/turn.c",
            "src/private.key",
            "../src/main.c",
            "/src/main.c",
            "src/../main.c",
            "src\\main.c",
            "src/fixtures/private.c",
        ):
            self.assertFalse(prepare.selected(path, self.boundary), path)

    def test_fixture_exception_cannot_admit_evidence(self):
        self.boundary["approved_fixture_files"] = [
            "src/fixtures/protocol.c",
            "src/evidence/fixtures/private.c",
        ]
        self.assertTrue(prepare.selected("src/fixtures/protocol.c", self.boundary))
        self.assertFalse(
            prepare.selected("src/evidence/fixtures/private.c", self.boundary)
        )

    def test_tracked_symbolic_link_is_rejected(self):
        with self.assertRaises(ValueError):
            self.candidate(b"/etc/passwd", "120000")

    def test_lfs_and_binary_source_are_rejected(self):
        for body in (
            b"version https://git-lfs.github.com/spec/v1\n",
            b"binary\0payload",
        ):
            with self.subTest(body=body), tempfile.TemporaryDirectory() as folder:
                revision = self.commit_file(body)
                commit, entries = prepare.inventory(self.root, revision, self.boundary)
                output = Path(folder) / "candidate"
                with self.assertRaises(ValueError):
                    prepare.export(self.root, commit, entries, self.boundary, output)
                self.assertFalse((output / "SOURCE-CANDIDATE.json").exists())

    def test_existing_destination_is_rejected(self):
        output = self.candidate()
        revision = self.commit_file(b"second source\n")
        commit, entries = prepare.inventory(self.root, revision, self.boundary)
        with self.assertRaises(FileExistsError):
            prepare.export(self.root, commit, entries, self.boundary, output)

    def test_extra_file_and_changed_bytes_fail_verification(self):
        output = self.candidate()
        extra = output / "private-recording.json"
        extra.write_text("synthetic fixture")
        with self.assertRaises(ValueError):
            prepare.verify(output)
        extra.unlink()
        (output / "src/main.c").write_text("changed source")
        with self.assertRaises(ValueError):
            prepare.verify(output)

    def test_manifest_traversal_fails_verification(self):
        output = self.candidate()
        receipt = output / "SOURCE-CANDIDATE.json"
        manifest = json.loads(receipt.read_text())
        manifest["files"][0]["path"] = "../outside.c"
        receipt.write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            prepare.verify(output)

    def test_public_readme_mapping_preserves_source_binding(self):
        self.boundary["publish_as"] = {"src/main.c": "README.md"}
        output = self.candidate(b"public introduction\n")
        manifest = prepare.verify(output)
        self.assertEqual((output / "README.md").read_text(), "public introduction\n")
        self.assertEqual(manifest["files"][0]["source_path"], "src/main.c")

    def test_unsafe_public_mapping_is_rejected(self):
        self.boundary["publish_as"] = {"src/main.c": "../private.md"}
        revision = self.commit_file(b"public introduction\n")
        with self.assertRaises(ValueError):
            prepare.inventory(self.root, revision, self.boundary)

    def test_only_exact_reviewed_font_bytes_can_pass_the_binary_boundary(self):
        body = b"wOF2\0synthetic-font-fixture"
        self.boundary["approved_fonts"] = {
            "src/font.woff2": hashlib.sha256(body).hexdigest()
        }
        revision = self.commit_file(body, path="src/font.woff2")
        commit, entries = prepare.inventory(self.root, revision, self.boundary)
        output = Path(self.scratch.name) / "font-candidate"
        prepare.export(self.root, commit, entries, self.boundary, output)
        prepare.verify(output)
        revision = self.commit_file(body + b"modified", path="src/font.woff2")
        commit, entries = prepare.inventory(self.root, revision, self.boundary)
        with self.assertRaises(ValueError):
            prepare.export(
                self.root,
                commit,
                entries,
                self.boundary,
                Path(self.scratch.name) / "rejected-font",
            )


if __name__ == "__main__":
    unittest.main()
