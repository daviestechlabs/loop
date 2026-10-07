#!/usr/bin/env python3
"""Make a local, tracked-source candidate. This tool never publishes or fetches."""

import argparse
import fnmatch
import hashlib
import json
import os
import subprocess
from pathlib import Path, PurePosixPath


def safe_path(value):
    path = PurePosixPath(value)
    return (
        bool(value)
        and not path.is_absolute()
        and all(part not in ("", ".", "..", ".git") for part in value.split("/"))
        and "\\" not in value
    )


def selected(path, boundary):
    if not safe_path(path):
        return False
    denied_parts = [
        part for part in path.split("/") if part in boundary["exclude_parts"]
    ]
    approved_fixture = path in boundary.get("approved_fixture_files", [])
    if denied_parts and not (approved_fixture and set(denied_parts) == {"fixtures"}):
        return False
    if path not in boundary.get("approved_source_files", []) and any(
        fnmatch.fnmatchcase(path, pattern) for pattern in boundary["exclude_globs"]
    ):
        return False
    if not any(
        path == item or (item.endswith("/") and path.startswith(item))
        for item in boundary["include"]
    ):
        return False
    return (
        PurePosixPath(path).suffix in boundary["text_suffixes"]
        or PurePosixPath(path).name in boundary["text_names"]
        or path in boundary.get("approved_fonts", {})
    )


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def inventory(root, revision, boundary):
    commit = (
        git(root, "rev-parse", "--verify", "--end-of-options", revision + "^{commit}")
        .decode()
        .strip()
    )
    tree = git(root, "ls-tree", "-r", "-z", commit)
    entries = []
    for record in tree.split(b"\0"):
        if not record:
            continue
        meta, raw_path = record.split(b"\t", 1)
        mode, kind, oid = meta.decode().split()
        path = raw_path.decode("utf-8", errors="strict")
        if selected(path, boundary):
            if mode not in ("100644", "100755") or kind != "blob":
                raise ValueError("Selected path is not a regular tracked file: " + path)
            entries.append((path, mode, oid))
    if not entries:
        raise ValueError("Empty source boundary")
    published = [
        boundary.get("publish_as", {}).get(item[0], item[0]) for item in entries
    ]
    if len(set(published)) != len(published) or not all(
        safe_path(path) for path in published
    ):
        raise ValueError("Invalid or duplicate publication path")
    return commit, entries


def export(root, commit, entries, boundary, destination):
    # Creating a new root prevents merges with previous candidates or symlink trees.
    destination.mkdir(mode=0o700, parents=False, exist_ok=False)
    manifest = {
        "schema": "loop-source-candidate/v1",
        "source_revision": commit,
        "status": "held-local-review",
        "history_included": False,
        "lfs_payloads_included": False,
        "files": [],
        "publication_holds": boundary["publication_holds"],
    }
    process = subprocess.Popen(
        ["git", "-C", str(root), "cat-file", "--batch"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )
    try:
        for source_path, mode, oid in entries:
            path = boundary.get("publish_as", {}).get(source_path, source_path)
            process.stdin.write((oid + "\n").encode())
            process.stdin.flush()
            header = process.stdout.readline().decode().split()
            if len(header) != 3 or header[0] != oid or header[1] != "blob":
                raise ValueError("Invalid Git blob response")
            size = int(header[2])
            if size > 8 * 1024 * 1024:
                raise ValueError("Oversized source file: " + path)
            body = process.stdout.read(size)
            if len(body) != size or process.stdout.read(1) != b"\n":
                raise ValueError("Truncated Git blob")
            if body.startswith(b"version https://git-lfs.github.com/spec/v1\n"):
                raise ValueError("Binary or LFS pointer in source boundary: " + path)
            font_hash = boundary.get("approved_fonts", {}).get(source_path)
            if font_hash:
                if (
                    not path.endswith(".woff2")
                    or not body.startswith(b"wOF2")
                    or hashlib.sha256(body).hexdigest() != font_hash
                ):
                    raise ValueError("Unverified font asset: " + path)
            else:
                body.decode("utf-8", errors="strict")
                if b"\0" in body:
                    raise ValueError("Binary source file: " + path)
            target = destination / path
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with target.open("xb") as output:
                output.write(body)
            target.chmod(0o700 if mode == "100755" else 0o600)
            manifest["files"].append(
                {
                    "path": path,
                    "source_path": source_path,
                    "git_blob": oid,
                    "bytes": size,
                    "sha256": hashlib.sha256(body).hexdigest(),
                    "executable": mode == "100755",
                }
            )
    finally:
        process.stdin.close()
        process.stdout.close()
        process.wait()
    if process.returncode:
        raise ValueError("Git blob reader failed")
    manifest["file_count"] = len(manifest["files"])
    manifest["source_bytes"] = sum(item["bytes"] for item in manifest["files"])
    (destination / "SOURCE-CANDIDATE.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    return manifest


def verify(destination):
    receipt = destination / "SOURCE-CANDIDATE.json"
    if destination.is_symlink() or receipt.is_symlink():
        raise ValueError("Candidate roots and receipts cannot be symbolic links")
    manifest = json.loads(receipt.read_text())
    if (
        manifest.get("schema") != "loop-source-candidate/v1"
        or manifest.get("status") != "held-local-review"
        or manifest.get("history_included") is not False
        or manifest.get("lfs_payloads_included") is not False
    ):
        raise ValueError("Invalid source candidate receipt")
    paths = [item["path"] for item in manifest["files"]]
    if len(set(paths)) != len(paths) or not all(safe_path(path) for path in paths):
        raise ValueError("Invalid or duplicate candidate path")
    actual = set()
    for item in destination.rglob("*"):
        if item.is_symlink():
            raise ValueError("Symbolic link in candidate")
        if item.is_file():
            actual.add(item.relative_to(destination).as_posix())
    if actual != set(paths) | {"SOURCE-CANDIDATE.json"}:
        raise ValueError("Candidate contains missing or untracked files")
    for entry in manifest["files"]:
        item = destination / entry["path"]
        if item.stat().st_size != entry["bytes"] or entry["bytes"] > 8 * 1024 * 1024:
            raise ValueError("Candidate size mismatch")
        if hashlib.sha256(item.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("Candidate hash mismatch")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--revision", help="Existing local Git commit or ref")
    action.add_argument(
        "--verify", type=Path, help="Check candidate hashes and reject extra files"
    )
    parser.add_argument(
        "--output", type=Path, help="New local directory; omit for inventory only"
    )
    args = parser.parse_args()
    os.umask(0o077)
    if args.verify:
        if args.output:
            parser.error("--output requires --revision")
        manifest = verify(args.verify)
        print(
            json.dumps(
                {
                    "verified": True,
                    "source_revision": manifest["source_revision"],
                    "file_count": manifest["file_count"],
                    "status": manifest["status"],
                }
            )
        )
        return
    root = Path(
        git(Path(__file__).parent, "rev-parse", "--show-toplevel").decode().strip()
    )
    boundary = json.loads(Path(__file__).with_name("boundary.json").read_text())
    commit, entries = inventory(root, args.revision, boundary)
    if args.output:
        manifest = export(root, commit, entries, boundary, args.output)
        print(
            json.dumps(
                {
                    key: manifest[key]
                    for key in (
                        "source_revision",
                        "status",
                        "file_count",
                        "source_bytes",
                    )
                }
            )
        )
    else:
        print(
            json.dumps(
                {
                    "source_revision": commit,
                    "status": "held-local-review",
                    "file_count": len(entries),
                    "paths": [item[0] for item in entries],
                }
            )
        )


if __name__ == "__main__":
    main()
