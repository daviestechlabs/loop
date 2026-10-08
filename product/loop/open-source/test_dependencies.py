"""Exercise dependency integrity and publication with disposable public fixtures."""

import base64
import hashlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "dependencies", Path(__file__).with_name("fetch-dependencies.py")
)
dependencies = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dependencies)


class Download(io.BytesIO):
    url = "https://registry.npmjs.org/fixture.tgz"


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.root = Path(self.scratch.name)
        self.body = b"synthetic public archive fixture"
        self.item = {
            "path": "vendor-artifacts/fixture.tgz",
            "source": Download.url,
            "sha256": hashlib.sha256(self.body).hexdigest(),
            "size": len(self.body),
            "npmPackage": "fixture",
            "integrity": "sha512-"
            + base64.b64encode(hashlib.sha512(self.body).digest()).decode(),
        }

    def tearDown(self):
        self.scratch.cleanup()

    def opener(self, body):
        class FixtureOpener:
            def open(self, url, timeout):
                return Download(body)

        return FixtureOpener()

    def test_public_urls_reject_credentials_private_hosts_and_http(self):
        for url in (
            "http://registry.npmjs.org/a",
            "https://user@registry.npmjs.org/a",
            "https://127.0.0.1/a",
            "https://registry.npmjs.org.evil.test/a",
            "https://registry.npmjs.org:9443/a",
        ):
            with self.assertRaises(ValueError):
                dependencies.public_url(url)

    def test_lock_rejects_traversal_duplicates_and_missing_integrity(self):
        for items in (
            [dict(self.item, path="../private")],
            [self.item, self.item],
            [dict(self.item, integrity="")],
        ):
            with self.assertRaises(ValueError):
                dependencies.checked_artifacts({"artifacts": items}, "npm")

    def test_successful_download_and_existing_archive_verification(self):
        self.assertEqual(
            dependencies.fetch(self.root, self.item, self.opener(self.body)),
            "downloaded-verified",
        )
        self.assertEqual(
            dependencies.fetch(self.root, self.item, self.opener(b"unused")),
            "verified-existing",
        )
        (self.root / self.item["path"]).write_bytes(b"modified")
        with self.assertRaises(ValueError):
            dependencies.fetch(self.root, self.item, self.opener(self.body))

    def test_complete_image_lock_passes_download_admission(self):
        lock_path = Path(__file__).resolve().parents[1] / "vendor.lock.json"
        lock = json.loads(lock_path.read_text())
        selected = dependencies.checked_artifacts(lock, "all")
        self.assertEqual(
            {item["path"] for item in selected},
            {item["path"] for item in lock["artifacts"]},
        )

    def test_hash_size_and_npm_integrity_fail_without_publishing(self):
        for body, item in (
            (b"wrong".ljust(len(self.body), b"x"), self.item),
            (self.body + b"extra", self.item),
            (self.body, dict(self.item, integrity="sha512-wrong")),
        ):
            with self.assertRaises(ValueError):
                dependencies.fetch(self.root, item, self.opener(body))
            self.assertFalse((self.root / self.item["path"]).exists())
            self.assertEqual(list((self.root / "vendor-artifacts").iterdir()), [])

    def test_symbolic_link_directories_are_rejected(self):
        (self.root / "vendor-artifacts").symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            dependencies.fetch(self.root, self.item, self.opener(self.body))


if __name__ == "__main__":
    unittest.main()
