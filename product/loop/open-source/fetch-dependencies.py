#!/usr/bin/env python3
"""Fetch public locked dependencies, verify bytes, and run no package scripts."""

import argparse
import base64
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, build_opener


def public_url(url):
    parsed = urlsplit(url)
    hosts = {
        "registry.npmjs.org",
        "dl-cdn.alpinelinux.org",
        "github.com",
        "release-assets.githubusercontent.com",
        "objects.githubusercontent.com",
    }
    if (
        parsed.scheme != "https"
        or parsed.hostname not in hosts
        or parsed.username
        or parsed.password
        or parsed.port not in (None, 443)
        or parsed.fragment
    ):
        raise ValueError("Dependency URL is outside the public source allowlist")
    return url


class PublicRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        return super().redirect_request(
            request, response, code, message, headers, public_url(new_url)
        )


def checked_artifacts(lock, group):
    selected = []
    paths = set()
    for item in lock["artifacts"]:
        if group == "npm" and not item.get("npmPackage"):
            continue
        if group == "bun" and not item["path"].endswith(".zip"):
            continue
        path = item["path"]
        if (
            not re.fullmatch(r"vendor-artifacts/[A-Za-z0-9_.-]+", path)
            or path in paths
            or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])
            or type(item["size"]) is not int
            or not 0 < item["size"] <= 128 * 1024 * 1024
        ):
            raise ValueError("Invalid or duplicate locked dependency")
        public_url(item["source"])
        if item.get("npmPackage") and not re.fullmatch(
            r"sha512-[A-Za-z0-9+/]+={0,2}", item.get("integrity", "")
        ):
            raise ValueError("Missing npm integrity")
        paths.add(path)
        selected.append(item)
    if not selected:
        raise ValueError("No dependencies selected")
    return selected


def matches(path, item):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != item["size"]:
        return False
    sha256, sha512 = hashlib.sha256(), hashlib.sha512()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            sha256.update(chunk)
            sha512.update(chunk)
    integrity = "sha512-" + base64.b64encode(sha512.digest()).decode()
    return sha256.hexdigest() == item["sha256"] and (
        not item.get("npmPackage") or integrity == item["integrity"]
    )


def fetch(root, item, opener):
    target = root / item["path"]
    if target.parent.is_symlink():
        raise ValueError("Dependency directory cannot be a symbolic link")
    target.parent.mkdir(mode=0o700, exist_ok=True)
    if target.exists() or target.is_symlink():
        if not matches(target, item):
            raise ValueError("Existing dependency fails integrity: " + item["path"])
        return "verified-existing"
    descriptor, temporary = tempfile.mkstemp(prefix=".download-", dir=target.parent)
    scratch = Path(temporary)
    try:
        with (
            os.fdopen(descriptor, "wb") as output,
            opener.open(item["source"], timeout=60) as response,
        ):
            public_url(response.url)
            total = 0
            while chunk := response.read(1024 * 1024):
                total += len(chunk)
                if total > item["size"]:
                    raise ValueError("Dependency exceeds locked size")
                output.write(chunk)
        if not matches(scratch, item):
            raise ValueError("Downloaded dependency fails integrity: " + item["path"])
        # Publish without overwriting an existing archive or a concurrent download.
        os.link(scratch, target)
        return "downloaded-verified"
    finally:
        scratch.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", choices=("npm", "bun", "all"), default="npm")
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    os.umask(0o077)
    root = args.root.resolve(strict=True)
    lock = json.loads((root / "vendor.lock.json").read_text())
    artifacts = checked_artifacts(lock, args.group)
    opener = build_opener(PublicRedirects())
    for item in artifacts:
        print(item["path"], fetch(root, item, opener))
    print(
        "Verified", len(artifacts), "public dependencies; no install scripts executed"
    )


if __name__ == "__main__":
    main()
