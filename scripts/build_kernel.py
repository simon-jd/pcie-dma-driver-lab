#!/usr/bin/env python3
"""Build the pinned Linux 6.12.41 guest kernel with the kdc driver and debug options.

The driver sources in kernel/ and include/ are copied into drivers/misc/kdc of
the extracted tree (build/linux-6.12.41, from scripts/fetch_kernel.py), the
config fragments are merged, and the static in-guest test client is built.

Three variants share the source tree:
  kasan  build/kernel        KASAN, kmemleak, lockdep, DMA-API debug, fault injection
  kcsan  build/kernel-kcsan  KCSAN instead of KASAN and kmemleak (they exclude each other)
  perf   build/kernel-perf   no debug options, KVM paravirt guest: for timing only
                             (scripts/run_bench.py); every correctness run uses kasan or kcsan
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build/linux-6.12.41"
BUILD = ROOT / "build/kernel"
KERNELS = {"kasan": (BUILD, ROOT / "build/build-record.json"),
           "kcsan": (ROOT / "build/kernel-kcsan", ROOT / "build/build-record-kcsan.json"),
           "perf": (ROOT / "build/kernel-perf", ROOT / "build/build-record-perf.json")}
CONFIGS = {"kasan": (ROOT / "configs/kernel.config", ["kdc.fragment"]),
           "kcsan": (ROOT / "configs/kernel-kcsan.config", ["kdc.fragment", "kcsan.fragment"]),
           "perf": (ROOT / "configs/kernel-perf.config", ["kdc.fragment", "perf.fragment"])}
COMMON = ("KDC_PCI", "KDC_PCI_KUNIT_TEST", "KUNIT", "PROVE_LOCKING", "DEBUG_ATOMIC_SLEEP", "DMA_API_DEBUG",
          "DMA_API_DEBUG_SG", "INTEL_IOMMU", "PCI_MSI", "HOTPLUG_PCI_PCIE", "HOTPLUG_PCI_ACPI", "DEBUG_FS", "SERIAL_8250_CONSOLE",
          "DEVTMPFS", "SMP", "INTERVAL_TREE", "FAULT_INJECTION", "FAILSLAB", "FAIL_PAGE_ALLOC",
          "FAULT_INJECTION_DEBUG_FS", "FAULT_INJECTION_STACKTRACE_FILTER")
REQUIRED = {"kasan": COMMON + ("KASAN", "KASAN_GENERIC", "DEBUG_KMEMLEAK"),
            "kcsan": COMMON + ("KCSAN",),
            "perf": ("KDC_PCI", "PCI_MSI", "INTEL_IOMMU", "DEBUG_FS", "SERIAL_8250_CONSOLE", "DEVTMPFS", "SMP",
                     "INTERVAL_TREE", "NO_HZ_IDLE", "PARAVIRT", "KVM_GUEST", "X86_X2APIC")}
# Options that would distort timing; the perf build fails if any of them survives olddefconfig.
ABSENT = {"kasan": (), "kcsan": (),
          "perf": ("KASAN", "KCSAN", "DEBUG_KMEMLEAK", "LOCKDEP", "PROVE_LOCKING", "DEBUG_LOCK_ALLOC",
                   "DEBUG_SPINLOCK", "DEBUG_MUTEXES", "DEBUG_RWSEMS", "DEBUG_ATOMIC_SLEEP", "DMA_API_DEBUG",
                   "DEBUG_LIST", "DEBUG_OBJECTS", "SLUB_DEBUG", "FAULT_INJECTION", "KUNIT", "KFENCE",
                   "DEBUG_PAGEALLOC", "LOCK_STAT", "TRACE_IRQFLAGS")}


def environment(local_tools=None):
    env = dict(os.environ, KBUILD_BUILD_USER="kdc", KBUILD_BUILD_HOST="local", KBUILD_BUILD_VERSION="1",
               KBUILD_BUILD_TIMESTAMP="2026-11-06 00:00:00 +0000")
    if local_tools:
        prefix = local_tools.resolve() / "usr"
        env.update(PATH=f"{prefix / 'bin'}:{env['PATH']}", BISON_PKGDATADIR=str(prefix / "share/bison"),
                   HOSTCFLAGS=f"-O2 -I{prefix / 'include'}", HOSTLDFLAGS=f"-L{prefix / 'lib/x86_64-linux-gnu'}")
    return env


def inputs(variant="kasan"):
    files = [p for d in ("kernel", "include", "tests") for p in (ROOT / d).rglob("*")
             if p.is_file() and "__pycache__" not in p.parts]
    files.append(CONFIGS[variant][0])
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)}


def overlay():
    target = SOURCE / "drivers/misc/kdc"
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(ROOT / "kernel", target)
    for header in ("kdc_abi.h", "kdc_hw.h"):
        shutil.copy(ROOT / "include" / header, target / header)
    for path, line in ((SOURCE / "drivers/misc/Kconfig", 'source "drivers/misc/kdc/Kconfig"'),
                       (SOURCE / "drivers/misc/Makefile", "obj-y += kdc/")):
        text = path.read_text()
        if line not in text:
            path.write_text(text + "\n" + line + "\n")


def configure(env, variant):
    build = KERNELS[variant][0]
    saved, fragments = CONFIGS[variant]
    command = ["make", "-C", str(SOURCE), f"O={build}"]
    build.mkdir(parents=True, exist_ok=True)
    if not (build / ".config").exists():
        shutil.copy2(saved if saved.exists() else CONFIGS["kasan"][0], build / ".config")
    fragment = [line for name in fragments for line in (ROOT / "configs" / name).read_text().splitlines()]
    keys = {m.group(1) for line in fragment if (m := re.match(r"(?:# )?(CONFIG_\w+)", line))}
    old = (build / ".config").read_text().splitlines()
    kept = [line for line in old if not (m := re.match(r"(?:# )?(CONFIG_\w+)", line)) or m.group(1) not in keys]
    (build / ".config").write_text("\n".join(kept + fragment) + "\n")
    subprocess.run(command + ["olddefconfig"], env=env, check=True, stdout=subprocess.DEVNULL)
    config = (build / ".config").read_text()
    missing = [c for c in REQUIRED[variant] if f"CONFIG_{c}=y\n" not in config]
    if missing:
        raise RuntimeError(f"Required configuration silently disabled: {missing}")
    present = [c for c in ABSENT[variant] if f"CONFIG_{c}=y\n" in config]
    if present:
        raise RuntimeError(f"Debug options left on in the {variant} kernel: {present}")
    shutil.copy2(build / ".config", saved)
    return command


def build_test_client():
    """The in-guest test client and the benchmark client; returns the test client."""
    for name in ("kdc_test", "kdc_bench"):
        subprocess.run(["gcc", "-static", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-I", str(ROOT / "include"), str(ROOT / f"tests/{name}.c"), "-o", str(ROOT / f"build/{name}")],
                       check=True)
    return ROOT / "build/kdc_test"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--local-tools", type=pathlib.Path, help="Extracted dependency prefix containing usr/")
    p.add_argument("--jobs", type=int, default=16)
    p.add_argument("--variant", choices=tuple(KERNELS), default="kasan")
    p.add_argument("--client-only", action="store_true", help="Rebuild only the in-guest test client")
    args = p.parse_args()
    if not SOURCE.is_dir():
        raise RuntimeError("Run scripts/fetch_kernel.py first")
    env = environment(args.local_tools)
    build, record_path = KERNELS[args.variant]
    if not args.client_only:
        overlay()
        command = configure(env, args.variant)
        log = ROOT / f"build/kernel-build-{args.variant}.log"
        with log.open("w") as output:
            result = subprocess.run(["nice"] + command + [f"-j{min(args.jobs, 16)}", "bzImage"], env=env,
                                    stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"Kernel build failed: inspect {log}")
    client = build_test_client()
    kernel = build / "arch/x86/boot/bzImage"
    fresh = inputs(args.variant)
    if args.client_only:
        # The kernel was not rebuilt: keep its recorded inputs, refresh only tests/.
        old = json.loads(record_path.read_text())["inputs"]
        fresh = {k: v for k, v in old.items() if not k.startswith("tests/")} | \
            {k: v for k, v in fresh.items() if k.startswith("tests/")}
    record = {"inputs": fresh,
              "kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(),
              "client_sha256": hashlib.sha256(client.read_bytes()).hexdigest(),
              "bench_sha256": hashlib.sha256((ROOT / "build/kdc_bench").read_bytes()).hexdigest(),
              "gcc": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0]}
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    print("built", kernel, client)


if __name__ == "__main__":
    main()
