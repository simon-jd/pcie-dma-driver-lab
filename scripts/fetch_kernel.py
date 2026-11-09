#!/usr/bin/env python3
"""Fetch and verify the exact upstream kernel archive; never touch the host kernel."""
import argparse
import hashlib
import pathlib
import subprocess
import tarfile

VERSION = "6.12.41"
SHA256 = "6b19a3ae99423de2416964d67251d745910277af258b4c4c63e88fd87dbf0e27"
URL = f"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-{VERSION}.tar.xz"
ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=pathlib.Path, help="Reuse an already downloaded archive; hash still checked")
    args = parser.parse_args()
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    archive = args.archive or build / f"linux-{VERSION}.tar.xz"
    if not archive.exists():
        partial = archive.with_suffix(archive.suffix + ".partial")
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--output", str(partial), URL], check=True, timeout=600)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError("Downloaded archive checksum mismatch")
        partial.rename(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("Kernel archive checksum mismatch")
    destination = build / f"linux-{VERSION}"
    if destination.exists():
        raise RuntimeError(f"Refusing to overwrite an existing source tree: {destination}")
    # Python's data filter prevents archive path traversal and unsafe link targets.
    with tarfile.open(archive, "r:xz") as tar:
        tar.extractall(build, filter="data")
    print(f"Verified Linux {VERSION}: {SHA256}")
    print(destination)


if __name__ == "__main__":
    main()
