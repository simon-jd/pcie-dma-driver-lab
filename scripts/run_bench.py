#!/usr/bin/env python3
"""Timing runs on KVM: the driver plus virtualization overhead around a modeled
device service time. Not device throughput: the device is emulated.

Boots the timing kernel (build_kernel.py --variant perf) with one kdc-dma device
(4 queues) and runs the benchmark client (tests/kdc_bench.c) with a plan file.
Several boot configurations can be alternated (--configs, --rounds), so that
arms that need a different kernel command line are interleaved too.

Host side, per boot: QEMU starts on --main-cpu (main loop, the device model's
bottom halves and timers, and every helper thread); each vCPU thread is then
pinned to its own CPU from --vcpus. Around every measured window the guest asks
for a snapshot ("@M <token>" on the console) and this script records:
  - KVM's per-vCPU statistics through QMP query-stats (exits, MMIO/PIO/halt
    exits, halt polling), summed over vCPUs and per vCPU;
  - the device's own counters (QOM stat-*: doorbells, CQ head writes, MSI-X
    messages, completions);
  - CPU time of every QEMU thread (/proc/<pid>/task/<tid>/schedstat: run time
    and time spent waiting for a CPU);
  - busy time of every CPU in --watch-cpus (the VM's CPUs and their SMT
    siblings): wall time minus cpuidle residency, and the /proc/stat counters;
  - the CPU clock of the VM's CPUs, sampled every 20 ms.
"Foreign" time is busy time on the watched CPUs that no QEMU thread accounts
for (other processes, and interrupt time the kernel does not charge to a
task). A window is rejected when its foreign time exceeds --max-foreign or the
QEMU threads spent more than --max-qemu-wait waiting for a CPU (both in ms per
second of window): the guest is told so through the scratch register and runs
the window again (up to 4 times). The thresholds are recorded with each boot.

Output in --out: serial.log, windows.jsonl (one record per window, rejected
ones included), run.json per boot under boot-<n>/, and plan.txt.
"""
import argparse
import datetime
import gzip
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import statistics
import subprocess
import tempfile
import threading
import time

from build_kernel import KERNELS, inputs
from run_vm import QEMU, Qmp, newc_entry

ROOT = pathlib.Path(__file__).resolve().parents[1]
REJECT = 0x80000000
CONFIGS = {
    "noiommu": {"iommu": None, "append": [], "device": ""},
    "iommu-strict": {"iommu": "strict", "append": [], "device": ""},
    "iommu-lazy": {"iommu": "lazy", "append": [], "device": ""},
    "notrack": {"iommu": None, "append": ["kdc.track_overlap=0"], "device": ""},
    "tablecrc": {"iommu": None, "append": [], "device": ",hw-crc=off"},
}
INIT = b"""#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t debugfs debugfs /sys/kernel/debug
/bin/busybox sleep 2
/bin/kdc_bench /plan.txt
echo "kdc_init: BENCH_EXIT $?" > /dev/kmsg
/bin/busybox poweroff -f
"""
MARK = re.compile(r"kdc_bench: @M (\d+)\s*$")
RESULT = re.compile(r"kdc_bench: RESULT (.*)$")
KVM_SCALARS = ("exits", "mmio_exits", "io_exits", "halt_exits", "irq_exits", "irq_window_exits",
               "request_irq_exits", "signal_exits", "irq_injections", "insn_emulation", "halt_wakeup",
               "halt_successful_poll", "halt_attempted_poll", "halt_poll_invalid", "halt_poll_success_ns",
               "halt_poll_fail_ns", "halt_wait_ns", "host_state_reload", "hypercalls", "pf_taken",
               "tlb_flush", "nmi_window_exits", "notify_window_exits")
DEVICE_STATS = ("stat-doorbells", "stat-cq-doorbells", "stat-irqs", "stat-done")


def cpu_list(text):
    cpus = []
    for part in text.split(","):
        lo, _, hi = part.partition("-")
        cpus += range(int(lo), int(hi or lo) + 1)
    return cpus


def initramfs(plan, path):
    cpio = bytearray()
    for name in (".", "bin", "dev", "proc", "sys", "tmp"):
        cpio += newc_entry(name, stat.S_IFDIR | 0o755)
    cpio += newc_entry("bin/busybox", stat.S_IFREG | 0o755, pathlib.Path(shutil.which("busybox")).read_bytes())
    cpio += newc_entry("bin/sh", stat.S_IFLNK | 0o777, b"busybox")
    cpio += newc_entry("bin/kdc_bench", stat.S_IFREG | 0o755, (ROOT / "build/kdc_bench").read_bytes())
    cpio += newc_entry("plan.txt", stat.S_IFREG | 0o644, plan.encode())
    cpio += newc_entry("init", stat.S_IFREG | 0o755, INIT)
    cpio += newc_entry("TRAILER!!!", 0)
    path.write_bytes(gzip.compress(bytes(cpio), mtime=0))


def qemu_command(qemu, kernel, ramdisk, config, smp, qmp):
    cfg = CONFIGS[config]
    cmd = [str(qemu), "-machine", "q35", "-accel", "kvm", "-cpu", "host", "-m", "2048", "-smp", str(smp),
           "-nodefaults", "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot", "-nic", "none"]
    if cfg["iommu"]:
        cmd += ["-device", "intel-iommu"]
    cmd += ["-device", "pcie-root-port,id=rp0,bus=pcie.0,chassis=1,addr=0x2",
            "-device", f"kdc-dma,id=kdc0,bus=rp0,queues=4{cfg['device']}",
            "-qmp", f"unix:{qmp},server=on,wait=off"]
    append = ["console=ttyS0", "panic=-1", "nokaslr", "printk.devkmsg=on", "printk.time=0",
              "kdc.bounce_max=65536"] + cfg["append"]
    if cfg["iommu"]:
        append += ["intel_iommu=on", f"iommu.strict={1 if cfg['iommu'] == 'strict' else 0}"]
    return cmd + ["-kernel", str(kernel), "-initrd", str(ramdisk), "-append", " ".join(append)]


def read_int(path, default=None):
    try:
        return int(pathlib.Path(path).read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return default


class Host:
    """Host-side counters for one QEMU process and the watched CPUs."""

    def __init__(self, pid, watch, vcpu_cpus, main_cpu):
        self.pid, self.watch, self.vcpu_cpus, self.main_cpu = pid, watch, vcpu_cpus, main_cpu
        self.clock_cpus = vcpu_cpus + [main_cpu]
        self.idle_paths = {c: sorted(pathlib.Path(f"/sys/devices/system/cpu/cpu{c}/cpuidle").glob("state*/time"))
                           for c in watch}
        self.freq = []  # (t_ns, {cpu: kHz})
        self.stop = threading.Event()
        self.sampler = threading.Thread(target=self._sample, daemon=True)
        self.sampler.start()

    def _sample(self):
        while not self.stop.wait(0.02):
            self.freq.append((time.monotonic_ns(), {c: read_int(f"/sys/devices/system/cpu/cpu{c}/cpufreq/"
                                                               "scaling_cur_freq") for c in self.clock_cpus}))

    def threads(self):
        out = {}
        for task in pathlib.Path(f"/proc/{self.pid}/task").iterdir():
            try:
                run, wait, slices = (int(x) for x in (task / "schedstat").read_text().split())
                name = (task / "comm").read_text().strip()
            except (OSError, ValueError):
                continue
            out[task.name] = {"name": name, "run_ns": run, "wait_ns": wait}
        return out

    def touch_cpus(self):
        """Run for a moment on every watched CPU. cpuidle residency counters are
        updated when a CPU leaves an idle state, so without this a CPU that has
        been idle since before the snapshot would count that time as busy."""
        old = os.sched_getaffinity(0)
        for c in self.watch:
            os.sched_setaffinity(0, {c})
        os.sched_setaffinity(0, old)

    def snapshot(self, qmp):
        snap = {"t_ns": time.monotonic_ns()}
        self.touch_cpus()
        snap["idle_us"] = {c: sum(read_int(p, 0) for p in paths) for c, paths in self.idle_paths.items()}
        snap["idle_states_us"] = {c: [read_int(p, 0) for p in paths] for c, paths in self.idle_paths.items()}
        snap["wall_ns"] = time.monotonic_ns()
        with open("/proc/stat") as f:
            stat_lines = {line.split()[0]: [int(x) for x in line.split()[1:]] for line in f if line.startswith("cpu")}
        snap["proc_stat"] = {c: stat_lines.get(f"cpu{c}") for c in self.watch}
        snap["threads"] = self.threads()
        vcpus = []
        for entry in qmp.call("query-stats", target="vcpu"):
            values = {s["name"]: s["value"] for s in entry["stats"] if isinstance(s["value"], int)}
            vcpus.append({"path": entry.get("qom-path"), **{k: values.get(k) for k in KVM_SCALARS}})
        snap["kvm"] = vcpus
        snap["device"] = {k: qmp.call("qom-get", path="/machine/peripheral/kdc0", property=k) for k in DEVICE_STATS}
        return snap


def delta(a, b, window_ns, host, vcpu_tids):
    """Counters between two snapshots, and the foreign-load figure for the threshold."""
    d = {"host_ns": b["t_ns"] - a["t_ns"]}
    kvm = {}
    for key in KVM_SCALARS:
        vals = [y[key] - x[key] for x, y in zip(a["kvm"], b["kvm"]) if x[key] is not None and y[key] is not None]
        if vals:
            kvm[key] = sum(vals)
    d["kvm"] = kvm
    d["kvm_per_vcpu_exits"] = [y["exits"] - x["exits"] for x, y in zip(a["kvm"], b["kvm"]) if x["exits"] is not None]
    d["device"] = {k: (b["device"][k] - a["device"][k]) % (1 << 32) for k in DEVICE_STATS}
    ours_run = ours_wait = 0
    threads = {}
    for tid, t in b["threads"].items():
        old = a["threads"].get(tid, {"run_ns": 0, "wait_ns": 0})
        run, wait = t["run_ns"] - old["run_ns"], t["wait_ns"] - old["wait_ns"]
        ours_run += run
        ours_wait += wait
        role = "vcpu" if tid in vcpu_tids else ("main" if tid == str(host.pid) else "other")
        key = f"vcpu{vcpu_tids.index(tid)}" if role == "vcpu" else role
        cur = threads.setdefault(key, {"run_ns": 0, "wait_ns": 0})
        cur["run_ns"] += run
        cur["wait_ns"] += wait
    d["threads"] = threads
    wall_us = (b["wall_ns"] - a["wall_ns"]) / 1000
    busy_us = {c: max(0.0, wall_us - (b["idle_us"][c] - a["idle_us"][c])) for c in host.watch}
    d["cpu_busy_us"] = {str(c): round(v) for c, v in busy_us.items()}
    # where our threads ran: vCPU i on vcpu_cpus[i], everything else on the main CPU
    ours_us = {c: 0.0 for c in host.watch}
    for key, t in threads.items():
        cpu = host.vcpu_cpus[int(key[4:])] if key.startswith("vcpu") else host.main_cpu
        if cpu in ours_us:
            ours_us[cpu] += t["run_ns"] / 1000
    foreign_us = {c: max(0.0, busy_us[c] - ours_us[c]) for c in host.watch}
    hosting = set(host.vcpu_cpus) | {host.main_cpu}
    d["foreign_on_vm_cpus_ms"] = sum(v for c, v in foreign_us.items() if c in hosting) / 1000
    d["foreign_on_siblings_ms"] = sum(v for c, v in foreign_us.items() if c not in hosting) / 1000
    d["idle_state_us"] = {str(c): [y - x for x, y in zip(a["idle_states_us"][c], b["idle_states_us"][c])]
                          for c in host.watch}
    tick = os.sysconf("SC_CLK_TCK")
    stat_busy = 0
    for c in host.watch:
        x, y = a["proc_stat"][c], b["proc_stat"][c]
        # user nice system idle iowait irq softirq steal guest guest_nice; guest is inside user
        stat_busy += sum(y[i] - x[i] for i in (0, 1, 2, 5, 6, 7))
    d["proc_stat_busy_ms"] = stat_busy * 1000 / tick
    d["ours_run_ms"] = ours_run / 1e6
    d["ours_wait_ms"] = ours_wait / 1e6
    d["foreign_ms"] = max(0.0, sum(busy_us.values()) / 1000 - ours_run / 1e6)
    d["foreign_ms_per_s"] = d["foreign_ms"] / (window_ns / 1e9) if window_ns else 0.0
    d["proc_stat_foreign_ms"] = max(0.0, d["proc_stat_busy_ms"] - ours_run / 1e6)
    samples = [f for t, f in host.freq if a["t_ns"] <= t <= b["t_ns"]]
    d["clock_mhz"] = {str(c): (round(statistics.median(s[c] for s in samples) / 1000) if samples and
                               all(s[c] for s in samples) else None) for c in host.clock_cpus}
    return d


def boot(args, config, index, plan_text, out, windows_out, serial_out):
    kernel = KERNELS["perf"][0] / "arch/x86/boot/bzImage"
    record = json.loads(KERNELS["perf"][1].read_text())
    kernel_sha = hashlib.sha256(kernel.read_bytes()).hexdigest()
    bench_sha = hashlib.sha256((ROOT / "build/kdc_bench").read_bytes()).hexdigest()
    if record["inputs"] != inputs("perf") or record["kernel_sha256"] != kernel_sha or \
            record.get("bench_sha256") != bench_sha:
        raise RuntimeError("Sources changed since the recorded perf build; rebuild first")
    bdir = out / f"boot-{index}"
    bdir.mkdir()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="kdc-bench-"))
    ramdisk = tmp / "initramfs.cpio.gz"
    initramfs(plan_text, ramdisk)
    qmp_path = str(tmp / "qmp.sock")
    qemu = QEMU[args.qemu]
    vcpus = cpu_list(args.vcpus)
    command = qemu_command(qemu, kernel, ramdisk, config, len(vcpus), qmp_path)
    launch = ["taskset", "-c", str(args.main_cpu), "nice", "-n", str(args.nice)] + command
    info = {"config": config, "boot": index, "argv": launch,
            "utc_start": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "qemu_version": subprocess.check_output([str(qemu), "--version"], text=True).splitlines()[0],
            "kernel_sha256": kernel_sha, "bench_sha256": bench_sha, "vcpu_cpus": vcpus, "main_cpu": args.main_cpu,
            "watch_cpus": cpu_list(args.watch_cpus), "max_foreign_ms_per_s": args.max_foreign,
            "max_qemu_wait_ms_per_s": args.max_qemu_wait, "nice": args.nice,
            "load_average_start": open("/proc/loadavg").read().split()[:3],
            "host_kernel": os.uname().release,
            "kvm_halt_poll_ns": read_int("/sys/module/kvm/parameters/halt_poll_ns"),
            "governor": pathlib.Path("/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor").read_text().strip(),
            "quiet_marker": pathlib.Path("~/.cache/rework-quiet").expanduser().read_text().strip()
            if pathlib.Path("~/.cache/rework-quiet").expanduser().exists() else None}
    lines, windows, marks = [], [], {}
    started = time.monotonic()
    with open(bdir / "qemu-stderr.log", "w") as err:
        proc = subprocess.Popen(launch, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err,
                                env={"PATH": "/usr/bin:/bin"})
        qmp = Qmp(qmp_path)
        cpus = qmp.call("query-cpus-fast")
        vcpu_tids = [str(c["thread-id"]) for c in sorted(cpus, key=lambda c: c["cpu-index"])]
        for tid, cpu in zip(vcpu_tids, vcpus):
            os.sched_setaffinity(int(tid), {cpu})
        host = Host(proc.pid, cpu_list(args.watch_cpus), vcpus, args.main_cpu)

        def affinities():
            out = {}
            for task in pathlib.Path(f"/proc/{proc.pid}/task").iterdir():
                try:
                    out[f"{task.name} {(task / 'comm').read_text().strip()}"] = sorted(os.sched_getaffinity(int(task.name)))
                except OSError:
                    pass
            return out

        info["affinity_after_pinning"] = affinities()
        info["vcpu_tids"] = vcpu_tids
        for raw in proc.stdout:
            line = raw.decode(errors="replace")
            lines.append(line)
            if m := MARK.search(line.rstrip("\r\n")):
                token = int(m.group(1))
                snap = host.snapshot(qmp)
                marks[token] = snap
                reply = token
                if token % 2 == 0 and token - 1 in marks:
                    a = marks.pop(token - 1)
                    d = delta(a, snap, snap["t_ns"] - a["t_ns"], host, vcpu_tids)
                    d["qemu_wait_ms_per_s"] = d["ours_wait_ms"] / (d["host_ns"] / 1e9)
                    d.update(token=token, config=config, boot=index,
                             accepted=d["foreign_ms_per_s"] <= args.max_foreign and
                             d["qemu_wait_ms_per_s"] <= args.max_qemu_wait)
                    if not d["accepted"]:
                        reply |= REJECT
                    windows.append(d)
                    del marks[token]
                qmp.call("qom-set", path="/machine/peripheral/kdc0", property="scratch", value=reply)
            elif m := RESULT.search(line.rstrip("\r\n")):
                fields = dict(kv.split("=", 1) for kv in m.group(1).split())
                for w in windows:
                    if w["token"] == int(fields["token"]):
                        w["guest"] = fields
                        if int(fields["accepted"]) != int(w["accepted"]):
                            raise RuntimeError(f"guest and host disagree on window {fields}")
        proc.wait(timeout=60)
        info["affinity_at_exit_note"] = "threads listed after pinning; QEMU has exited"
        host.stop.set()
        qmp.close()
    shutil.rmtree(tmp, ignore_errors=True)
    info["seconds"] = round(time.monotonic() - started, 1)
    info["returncode"] = proc.returncode
    info["load_average_end"] = open("/proc/loadavg").read().split()[:3]
    serial = "".join(lines)
    info["bench_exit"] = (re.findall(r"kdc_init: BENCH_EXIT (\d+)", serial) or [None])[0]
    info["windows"] = len(windows)
    info["rejected"] = sum(not w["accepted"] for w in windows)
    (bdir / "run.json").write_text(json.dumps(info, indent=2) + "\n")
    serial_out.write(f"===== boot {index} ({config}) =====\n" + serial)
    for w in windows:
        windows_out.write(json.dumps(w) + "\n")
    problems = []
    if info["bench_exit"] != "0":
        problems.append(f"benchmark client exit {info['bench_exit']}")
    if any("guest" not in w for w in windows):
        problems.append("window without a RESULT line")
    if re.search(r"BUG:|WARNING:|Kernel panic|Oops|kdc_bench: FAIL", serial):
        problems.append("kernel or client report in the serial log")
    return info, problems


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--plan", required=True, type=pathlib.Path)
    p.add_argument("--out", required=True, type=pathlib.Path)
    p.add_argument("--configs", default="noiommu", help=f"comma-separated, alternated per round: {sorted(CONFIGS)}")
    p.add_argument("--rounds", type=int, default=1, help="boots per configuration")
    p.add_argument("--vcpus", default="2,4,6", help="host CPU per vCPU (also sets the vCPU count)")
    p.add_argument("--main-cpu", type=int, default=8, help="host CPU for QEMU's main loop and helper threads")
    p.add_argument("--watch-cpus", default="2-9", help="CPUs whose foreign load is checked (VM CPUs + siblings)")
    p.add_argument("--max-foreign", type=float, default=1500.0,
                   help="ms of foreign CPU time on the watched CPUs per s of window")
    p.add_argument("--max-qemu-wait", type=float, default=25.0,
                   help="ms the QEMU threads spent runnable but not running, per s of window")
    p.add_argument("--nice", type=int, default=10)
    p.add_argument("--qemu", choices=tuple(QEMU), default="release")
    args = p.parse_args()
    configs = args.configs.split(",")
    for c in configs:
        if c not in CONFIGS:
            raise SystemExit(f"unknown configuration {c}")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        raise SystemExit("/dev/kvm is not readable and writable by this user")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    plan_text = args.plan.read_text()
    (out / "plan.txt").write_text(plan_text)
    boots, problems = [], []
    with open(out / "windows.jsonl", "w") as windows_out, open(out / "serial.log", "w") as serial_out:
        index = 0
        for _ in range(args.rounds):
            for config in configs:
                info, probs = boot(args, config, index, plan_text, out, windows_out, serial_out)
                windows_out.flush()
                boots.append({k: info[k] for k in ("config", "boot", "seconds", "windows", "rejected", "bench_exit")})
                problems += [f"boot {index}: {x}" for x in probs]
                print(f"boot {index} {config}: {info['windows']} windows, {info['rejected']} rejected, "
                      f"{info['seconds']} s, client exit {info['bench_exit']}", flush=True)
                index += 1
    (out / "summary-boots.json").write_text(json.dumps({"boots": boots, "problems": problems}, indent=2) + "\n")
    for x in problems:
        print("  " + x)
    print("OK" if not problems else "PROBLEMS", "->", out)
    raise SystemExit(1 if problems else 0)


if __name__ == "__main__":
    main()
