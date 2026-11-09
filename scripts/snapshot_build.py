#!/usr/bin/env python3
"""Freeze the current timing build under a name, for A/B runs with run_bench.py --builds.

Copies the release QEMU binary, the perf kernel and the benchmark client of this
checkout into .cache/ab/<name>/ and writes build.json (commit, whether the
sources differ from it, SHA256 of every file). QEMU finds its firmware through a
qemu-bundle link to the release build directory; firmware does not depend on
the device model. Refuses to snapshot if the perf build record does not match
the sources, or if the QEMU tree does not hold this checkout's device model.

Usage: snapshot_build.py NAME [--force]
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

from build_kernel import KERNELS, inputs

ROOT = pathlib.Path(__file__).resolve().parents[1]
QEMU_BUILD = ROOT / ".cache/build/qemu"
QEMU_SRC = ROOT / ".cache/src/qemu-10.2.1/hw/misc"
AB = ROOT / ".cache/ab"


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("name")
    p.add_argument("--force", action="store_true", help="replace an existing snapshot of that name")
    args = p.parse_args()
    record = json.loads(KERNELS["perf"][1].read_text())
    kernel = KERNELS["perf"][0] / "arch/x86/boot/bzImage"
    bench = ROOT / "build/kdc_bench"
    qemu = QEMU_BUILD / "qemu-system-x86_64"
    if record["inputs"] != inputs("perf") or record["kernel_sha256"] != sha256(kernel) or \
            record.get("bench_sha256") != sha256(bench):
        raise SystemExit("perf kernel or client out of date: run build_kernel.py --variant perf")
    for name, src in (("kdc_dma.c", ROOT / "qemu/kdc_dma.c"), ("kdc_hw.h", ROOT / "include/kdc_hw.h")):
        if sha256(QEMU_SRC / name) != sha256(src):
            raise SystemExit(f"QEMU tree holds another {name}: run build_qemu.py")
        if (QEMU_SRC / name).stat().st_mtime > qemu.stat().st_mtime:
            raise SystemExit(f"QEMU binary older than its {name}: run build_qemu.py --flavor release")
    out = AB / args.name
    if out.exists():
        if not args.force:
            raise SystemExit(f"{out} exists (use --force)")
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copy2(qemu, out / "qemu-system-x86_64")
    (out / "qemu-bundle").symlink_to(QEMU_BUILD / "qemu-bundle")
    shutil.copy2(kernel, out / "bzImage")
    shutil.copy2(bench, out / "kdc_bench")
    git = lambda *a: subprocess.check_output(["git", "-C", str(ROOT), *a], text=True).strip()  # noqa: E731
    info = {"name": args.name, "commit": git("rev-parse", "HEAD"), "subject": git("log", "-1", "--format=%s"),
            "sources_differ_from_commit": bool(git("status", "--porcelain", "--untracked-files=no", "--",
                                                   "kernel", "include", "qemu", "tests", "configs")),
            "source_sha256": {k: v for k, v in record["inputs"].items()} |
            {"qemu/kdc_dma.c": sha256(ROOT / "qemu/kdc_dma.c")},
            "files": {f: sha256(out / f) for f in ("qemu-system-x86_64", "bzImage", "kdc_bench")}}
    (out / "build.json").write_text(json.dumps(info, indent=2) + "\n")
    print(f"{out}: {info['commit'][:7]} {info['subject']}" + (" (+ uncommitted changes)"
                                                              if info["sources_differ_from_commit"] else ""))


if __name__ == "__main__":
    main()
