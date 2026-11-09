#!/usr/bin/env python3
"""Run the qtest device suite and the guest suite across configurations.

Writes results/<name>/: one directory per guest configuration (serial.log,
qemu-stderr.log, run.json), qtest-<flavor>.log, and summary.json. Guest
configurations are {kvm, tcg} x {IOMMU strict, no IOMMU} with the KASAN kernel,
TCG with the ASan/UBSan QEMU build, and TCG with the KCSAN kernel. KVM
configurations are recorded as skipped (with the reason) when /dev/kvm is not
accessible to the current user.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIGS = [("kvm-iommu", "kvm", True, "release", "kasan"), ("kvm-noiommu", "kvm", False, "release", "kasan"),
           ("tcg-iommu", "tcg", True, "release", "kasan"), ("tcg-noiommu", "tcg", False, "release", "kasan"),
           ("tcg-iommu-asanqemu", "tcg", True, "asan", "kasan"),
           ("tcg-iommu-kcsan", "tcg", True, "release", "kcsan")]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--name", required=True, help="results/<name>/ is created (must not exist)")
    p.add_argument("--only", nargs="*", help="subset of configuration names")
    args = p.parse_args()
    out = ROOT / "results" / args.name
    out.mkdir(parents=True, exist_ok=False)
    summary = {"qtest": {}, "guest": {}}
    for flavor in ("asan", "release"):
        log = out / f"qtest-{flavor}.log"
        qemu = ROOT / f".cache/build/{'qemu-asan' if flavor == 'asan' else 'qemu'}/qemu-system-x86_64"
        rc = subprocess.run([sys.executable, str(ROOT / "tests/qtest/kdc_qtest.py"), "--qemu", str(qemu),
                             "--log", str(log)], stdin=subprocess.DEVNULL).returncode
        summary["qtest"][flavor] = {"passed": rc == 0, "log": log.name}
    kvm_ok = os.access("/dev/kvm", os.R_OK | os.W_OK)
    for name, accel, iommu, qemu, kernel in CONFIGS:
        if args.only and name not in args.only:
            continue
        if accel == "kvm" and not kvm_ok:
            summary["guest"][name] = {"skipped": "/dev/kvm is not readable and writable by this user"}
            print(f"SKIP {name}: no access to /dev/kvm")
            continue
        cmd = [sys.executable, str(ROOT / "scripts/run_vm.py"), "--out", str(out / name), "--accel", accel,
               "--iommu" if iommu else "--no-iommu", "--qemu", qemu, "--kernel", kernel]
        subprocess.run(cmd, stdin=subprocess.DEVNULL)
        run = json.loads((out / name / "run.json").read_text())
        summary["guest"][name] = {k: run.get(k) for k in ("passed", "seconds", "problems", "qemu_version", "kernel",
                                                          "kernel_sha256", "load_average", "expectations",
                                                          "kmemleak_records", "kmemleak_driver_records",
                                                          "host_actions", "counts")}
        summary["guest"][name]["client_cases"] = len(run["cases"])
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    ok = all(v["passed"] for v in summary["qtest"].values()) and \
        all(v.get("passed", True) for v in summary["guest"].values())
    print("ALL PASS" if ok else "FAILURES", "->", out)
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
