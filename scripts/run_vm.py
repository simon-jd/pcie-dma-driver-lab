#!/usr/bin/env python3
"""Boot the debug guest with two emulated kdc-dma devices and check it fail-closed.

The guest kernel is one of the builds from build_kernel.py (KASAN + kmemleak +
lockdep + DMA-API debug, or KCSAN + lockdep + DMA-API debug) with a static
BusyBox initramfs. Its init runs the KUnit suite (built in, at boot) and the
in-guest test client, then a kmemleak scan, then powers off.

Everything the guest reports goes through the kernel log, so lines reach the
serial console whole and in order. The run passes only if every required line
is present and no kernel, sanitizer, DMA-API, DMAR or driver report appears
outside an expectation window:

  kdc_test: EXPECT_BEGIN <tag> <regex>   ...   kdc_test: EXPECT_END <tag>

Inside a window, report lines that match <regex> are allowed and at least one
must appear; any other report still fails the run. The client checks the same
reports itself through /dev/kmsg before it closes the window.

The guest asks for actions only the host can take (hot-unplug, hot-plug, a
device that stops responding) with "kdc_test: KDC_HOST <verb> <args>" lines;
this script performs them over QMP while the guest runs and records them.
"""
import argparse
import datetime
import gzip
import hashlib
import json
import pathlib
import re
import shutil
import socket
import stat
import subprocess
import tempfile
import threading
import time

from build_kernel import KERNELS, inputs

ROOT = pathlib.Path(__file__).resolve().parents[1]
QEMU = {"release": ROOT / ".cache/build/qemu/qemu-system-x86_64",
        "asan": ROOT / ".cache/build/qemu-asan/qemu-system-x86_64"}
KUNIT_CASES = 13
CLIENT_CASES = 37
INIT = b"""#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t debugfs debugfs /sys/kernel/debug
log() { echo "kdc_init: $*" > /dev/kmsg; }
for f in failslab fail_page_alloc; do
    d=/sys/kernel/debug/$f
    if [ -d $d ]; then
        echo N > $d/ignore-gfp-wait
        echo 0 > $d/verbose
    fi
done
log KDC_GUEST_START
/bin/kdc_test
result=$?
if [ -e /sys/kernel/debug/kmemleak ]; then
    # Objects younger than 5 s are never reported; scan twice to settle.
    echo scan > /sys/kernel/debug/kmemleak
    /bin/busybox sleep 6
    echo scan > /sys/kernel/debug/kmemleak
    log KMEMLEAK_BEGIN
    while IFS= read -r line; do
        echo "kmemleak_dump: $line" > /dev/kmsg
    done < /sys/kernel/debug/kmemleak
    log KMEMLEAK_END
fi
log DMA_API_ERRORS $(cat /sys/kernel/debug/dma-api/error_count)
log KDC_COUNTERS cmds=$(cat /sys/kernel/debug/kdc/cmds) pinned=$(cat /sys/kernel/debug/kdc/pinned_pages) mapped=$(cat /sys/kernel/debug/kdc/mapped_sgts) dst_ranges=$(cat /sys/kernel/debug/kdc/dst_ranges)
if [ "$result" -eq 0 ]; then
    log KDC_GUEST_SUCCESS
else
    log KDC_GUEST_FAILURE exit=$result
fi
/bin/busybox sync
/bin/busybox poweroff -f
"""
# A line is a report if it matches here or is any kdc driver message other
# than the probe banner.
FORBIDDEN = re.compile(
    r"BUG:|WARNING:|KASAN:|UBSAN:|KCSAN:|Kernel panic|Oops|general protection|\bnot ok\b|"
    r"kdc_test: FAIL|KDC_GUEST_FAILURE|DMA-API: \w+ [0-9a-f]{4}:|DMAR: \[DMA|"
    r"DMAR: DRHD: handling fault|circular locking|inconsistent lock state|possible recursive locking|"
    r"INFO: task .* blocked|list_(add|del) corruption|refcount_t:|Call Trace:")
DRIVER_LINE = re.compile(r"\bkdc [0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.\d: (.*)$")
# Not reports: the driver's probe banner, and two one-time notices the PCI and
# IOMMU cores print for a hot-added function.
DRIVER_BANNER = re.compile(r"^(kdc\d+: \d+ queues x \d+ entries, bounce <= \d+ bytes(, shadow doorbells)?|"
                           r"enabling device \(0000 -> 0002\)|Using \d+-bit DMA addresses)$")
QEMU_FORBIDDEN = re.compile(r"AddressSanitizer|runtime error:|Blocked re-entrant|Assertion")
HOST_REQUEST = re.compile(r"kdc_test: KDC_HOST (\S+)(?: (.*))?$")


def newc_entry(name, mode, data=b""):
    encoded = name.encode() + b"\0"
    fields = (0, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0)
    header = b"070701" + b"".join(f"{x:08x}".encode() for x in fields)
    result = header + encoded
    result += b"\0" * (-len(result) % 4)
    return result + data + b"\0" * (-len(data) % 4)


def initramfs(init=INIT):
    cpio = bytearray()
    for name in (".", "bin", "dev", "proc", "sys", "tmp"):
        cpio += newc_entry(name, stat.S_IFDIR | 0o755)
    cpio += newc_entry("bin/busybox", stat.S_IFREG | 0o755, pathlib.Path(shutil.which("busybox")).read_bytes())
    cpio += newc_entry("bin/sh", stat.S_IFLNK | 0o777, b"busybox")
    cpio += newc_entry("bin/kdc_test", stat.S_IFREG | 0o755, (ROOT / "build/kdc_test").read_bytes())
    cpio += newc_entry("init", stat.S_IFREG | 0o755, init)
    cpio += newc_entry("TRAILER!!!", 0)
    path = ROOT / "build/initramfs.cpio.gz"
    path.write_bytes(gzip.compress(bytes(cpio), mtime=0))
    return path


def qemu_command(qemu, kernel, ramdisk, accel, iommu, qmp=None, smp=4, mem=2048):
    cmd = [str(qemu), "-machine", "q35", "-accel", accel, "-cpu", "host" if accel == "kvm" else "max",
           "-m", str(mem), "-smp", str(smp), "-nodefaults", "-display", "none", "-serial", "stdio",
           "-monitor", "none", "-no-reboot", "-nic", "none"]
    if iommu:
        cmd += ["-device", "intel-iommu"]  # must precede the devices it translates for
    cmd += ["-device", "pcie-root-port,id=rp0,bus=pcie.0,chassis=1,addr=0x2",
            "-device", "kdc-dma,id=kdc0,bus=rp0,queues=4",
            "-device", "pcie-root-port,id=rp1,bus=pcie.0,chassis=2,addr=0x3",
            "-device", "kdc-dma,id=kdc1,bus=rp1,queues=2"]
    if qmp:
        cmd += ["-qmp", f"unix:{qmp},server=on,wait=off"]
    append = "console=ttyS0 panic=-1 oops=panic panic_on_warn=1 nokaslr printk.devkmsg=on"
    if iommu:
        append += " intel_iommu=on iommu.strict=1"
    return cmd + ["-kernel", str(kernel), "-initrd", str(ramdisk), "-append", append]


def required_lines(iommu, kernel="kasan"):
    req = ["# Subtest: kdc", f"# kdc: pass:{KUNIT_CASES} fail:0 skip:0 total:{KUNIT_CASES}",
           "Lock dependency validator", "DMA-API: debugging enabled by kernel config",
           "kdc 0000:01:00.0: kdc0: 4 queues x 64 entries", "kdc 0000:02:00.0: kdc1: 2 queues x 64 entries",
           f"kdc_test: KDC_TEST_PASS cases={CLIENT_CASES}", "kdc_init: KDC_GUEST_SUCCESS",
           "kdc_init: DMA_API_ERRORS 0", "kdc_init: KDC_COUNTERS cmds=0 pinned=0 mapped=0 dst_ranges=0"]
    if kernel == "kasan":
        req += ["kasan: KernelAddressSanitizer initialized", "kmemleak: Kernel memory leak detector initialized",
                "kdc_init: KMEMLEAK_END"]
    else:
        req += ["kcsan: enabled early"]
    if iommu:
        req += ["DMAR: IOMMU enabled", "pci 0000:01:00.0: Adding to iommu group",
                "pci 0000:02:00.0: Adding to iommu group", "DMA domain TLB invalidation policy: strict mode"]
    return req


def is_report(line):
    if "kdc_test: EXPECT_" in line or "kmemleak_dump:" in line:
        return False
    if FORBIDDEN.search(line):
        return True
    m = DRIVER_LINE.search(line)
    return bool(m) and not DRIVER_BANNER.match(m.group(1).strip())


def check_reports(serial):
    """Reports outside expectation windows, and windows whose report never came."""
    problems, windows, expected = [], {}, []
    begin = re.compile(r"kdc_test: EXPECT_BEGIN (\S+) (.*)$")
    end = re.compile(r"kdc_test: EXPECT_END (\S+)")
    for line in serial.splitlines():
        if m := begin.search(line):
            windows[m.group(1)] = [re.compile(m.group(2).strip()), 0]
            continue
        if m := end.search(line):
            tag = m.group(1)
            if tag not in windows:
                problems.append(f"EXPECT_END without BEGIN: {tag}")
                continue
            pattern, count = windows.pop(tag)
            expected.append({"tag": tag, "pattern": pattern.pattern, "matched": count})
            if not count:
                problems.append(f"expected report never appeared: {tag} /{pattern.pattern}/")
            continue
        if not is_report(line):
            continue
        for w in windows.values():
            if w[0].search(line):
                w[1] += 1
                break
        else:
            problems.append(f"report: {line.strip()}")
    problems += [f"expectation window left open: {tag}" for tag in windows]
    return problems, expected


def kmemleak_records(serial):
    records, current, inside = [], None, False
    for line in serial.splitlines():
        if "kdc_init: KMEMLEAK_BEGIN" in line:
            inside = True
        elif "kdc_init: KMEMLEAK_END" in line:
            inside = False
        elif inside and "kmemleak_dump:" in line:
            text = line.split("kmemleak_dump:", 1)[1].strip()
            if text.startswith("unreferenced object"):
                current = [text]
                records.append(current)
            elif current is not None:
                current.append(text)
    driver = [r for r in records if any("kdc_" in x for x in r)]
    return records, driver


def check_output(serial, stderr, iommu, expect_success=True, kernel="kasan"):
    problems, expected = check_reports(serial)
    for m in QEMU_FORBIDDEN.finditer(stderr):
        problems.append(f"qemu stderr: {m.group(0)}")
    records, driver = kmemleak_records(serial)
    for r in driver[:20]:
        problems.append("kmemleak, driver object: " + " | ".join(r[:8]))
    if len(driver) > 20:
        problems.append(f"kmemleak: {len(driver) - 20} more driver objects")
    if expect_success:
        problems += [f"missing: {r}" for r in required_lines(iommu, kernel) if r not in serial]
        if not iommu and "DMAR" in serial:
            problems.append("DMAR present in a run without IOMMU")
    return problems, {"expectations": expected, "kmemleak_records": len(records),
                      "kmemleak_driver_records": len(driver)}


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.file = self.sock.makefile("rw")
        self.events = []
        json.loads(self.file.readline())
        self.call("qmp_capabilities")

    def call(self, command, **arguments):
        msg = {"execute": command}
        if arguments:
            msg["arguments"] = arguments
        self.file.write(json.dumps(msg) + "\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "event" in reply:
                self.events.append(reply)
            elif "return" in reply:
                return reply["return"]
            elif "error" in reply:
                raise RuntimeError(f"QMP {command}: {reply['error']}")

    def close(self):
        self.sock.close()


def host_action(qmp, verb, args):
    """Perform one guest request; returns a short result string."""
    if verb == "qom-set":
        dev, prop, value = args.split(" ", 2)
        qmp.call("qom-set", path=f"/machine/peripheral/{dev}", property=prop, value=json.loads(value))
    elif verb == "device_del":
        qmp.call("device_del", id=args.strip())
    elif verb == "device_add":
        # QEMU finishes deleting an ejected function slightly after the guest
        # has let go of it; retry while the slot is still occupied.
        for attempt in range(50):
            try:
                qmp.call("device_add", **json.loads(args))
                return f"ok after {attempt} retries"
            except RuntimeError as e:
                if "already occupied" not in str(e):
                    raise
                time.sleep(0.2)
        raise RuntimeError("slot still occupied after 10 s")
    else:
        raise RuntimeError(f"unknown host request {verb}")
    return "ok"


def run(args, out, mutant=None):
    """Boot once. With mutant set, the kernel is a deliberately broken build and
    is not expected to match the build record or to pass."""
    variant = getattr(args, "kernel", "kasan")
    build_dir, record_path = KERNELS[variant]
    kernel = build_dir / "arch/x86/boot/bzImage"
    record = json.loads(record_path.read_text())
    kernel_sha = hashlib.sha256(kernel.read_bytes()).hexdigest()
    if not mutant and (record["inputs"] != inputs(variant) or record["kernel_sha256"] != kernel_sha):
        raise RuntimeError(f"Sources changed since the recorded {variant} build; rebuild first")
    qemu = QEMU[args.qemu]
    ramdisk = initramfs()
    tmp = tempfile.mkdtemp(prefix="kdc-run-")  # unique QMP socket per run
    qmp_path = f"{tmp}/qmp.sock"
    command = qemu_command(qemu, kernel, ramdisk, args.accel, args.iommu, qmp=qmp_path)
    result = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
              "accel": args.accel, "iommu": args.iommu, "qemu": args.qemu, "kernel": variant, "argv": command,
              "qemu_version": subprocess.check_output([str(qemu), "--version"], text=True).splitlines()[0],
              "mutant": mutant, "kernel_sha256": kernel_sha, "client_sha256": record["client_sha256"],
              "load_average": open("/proc/loadavg").read().split()[:3], "passed": False, "host_actions": []}
    started = time.monotonic()
    lines = []
    with open(out / "qemu-stderr.log", "w") as err:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err,
                                env={"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"})
        qmp = None

        def reader():
            nonlocal qmp
            for raw in proc.stdout:
                line = raw.decode(errors="replace")
                lines.append(line)
                m = HOST_REQUEST.search(line.rstrip("\r\n"))
                if not m:
                    continue
                action = {"t_seen": round(time.monotonic() - started, 2),
                          "request": m.group(0)[len("kdc_test: "):]}
                try:
                    qmp = qmp or Qmp(qmp_path)
                    action["result"] = host_action(qmp, m.group(1), m.group(2) or "")
                except Exception as e:  # recorded; the guest sees the missing effect and fails
                    action["result"] = f"error: {e}"
                action["t_done"] = round(time.monotonic() - started, 2)
                result["host_actions"].append(action)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        try:
            proc.wait(timeout=args.timeout)
            result["returncode"] = proc.returncode
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            result["timed_out"] = True
        thread.join(timeout=10)
        if qmp:
            result["qmp_events"] = [e["event"] for e in qmp.events]
            qmp.close()
    shutil.rmtree(tmp, ignore_errors=True)
    serial = "".join(lines)
    result["seconds"] = round(time.monotonic() - started, 1)
    (out / "serial.log").write_text(serial)
    stderr = (out / "qemu-stderr.log").read_text(errors="replace")
    problems, info = check_output(serial, stderr, args.iommu, expect_success=not mutant, kernel=variant)
    result.update(info)
    if result.get("timed_out"):
        problems.append(f"timeout after {args.timeout}s")
    elif result.get("returncode"):
        problems.append(f"QEMU exit status {result['returncode']}")
    result["cases"] = re.findall(r"kdc_test: PASS (\w+) \((\d+) ms\)", serial)
    result["counts"] = re.findall(r"kdc_test: (  \w+: .*)$", serial, re.M)
    result["problems"] = problems
    result["passed"] = not problems
    (out / "run.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True, type=pathlib.Path)
    p.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    p.add_argument("--iommu", action=argparse.BooleanOptionalAction, default=True)
    p.add_argument("--qemu", choices=tuple(QEMU), default="release")
    p.add_argument("--kernel", choices=("kasan", "kcsan"), default="kasan")
    p.add_argument("--timeout", type=int, default=None)
    args = p.parse_args()
    args.timeout = args.timeout or (900 if args.accel == "kvm" else 3600)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = run(args, out)
    print(("PASS" if result["passed"] else "FAIL"),
          f"{args.accel} iommu={args.iommu} qemu={args.qemu} kernel={args.kernel}",
          f"{result['seconds']}s, {len(result['cases'])} client cases; logs in {out}")
    for problem in result["problems"][:20]:
        print("  " + problem)
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
