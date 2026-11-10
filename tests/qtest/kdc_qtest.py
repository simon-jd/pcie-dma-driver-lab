#!/usr/bin/env python3
"""Device-model tests for kdc-dma through QEMU's qtest protocol (no guest kernel).

The test plays the role of a driver: it programs PCI config space, BARs, MSI-X
and the rings directly in guest RAM, and steps the virtual clock, so service
times are exact and deterministic. Run it against the ASan+UBSan QEMU build;
any sanitizer report on QEMU's stderr fails the run.

Constants come from include/kdc_hw.h, so the test and the C code cannot drift.

Every test runs twice: with the device's queues in QEMU's main loop, and with
them in two IOThreads (the iothreads property). An IOThread works on its own
time, so in that mode settle() also waits a little real time after each step.
"""
import argparse
import json
import os
import pathlib
import random
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_hw_constants():
    consts = {}
    for line in (ROOT / "include/kdc_hw.h").read_text().splitlines():
        m = re.match(r"#define (KDC_\w+) (\S.*?)(?:\s*/\*.*)?$", line)
        if not m or "(" in m.group(1):
            continue
        expr = re.sub(r"(0x[0-9a-fA-F]+|\d+)(?:ULL|UL|U|u)\b", r"\1", m.group(2))
        try:
            consts[m.group(1)] = eval(expr, {}, dict(consts))  # trusted repo header
        except Exception:
            pass
    return consts


HW = load_hw_constants()
globals().update(HW)

CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0x82F63B78 if _c & 1 else 0)
    CRC_TABLE.append(_c)


def crc32c(data, seed=0):
    crc = ~seed & 0xFFFFFFFF
    for b in data:
        crc = CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return ~crc & 0xFFFFFFFF


assert crc32c(b"123456789") == 0xE3069283
assert crc32c(b"6789", crc32c(b"12345")) == 0xE3069283

# Guest-physical layout used by the tests (no firmware runs under qtest).
BAR0 = 0xE0000000
BAR2 = 0xE0010000
MSI_BASE = 0x00800000
RING_BASE = 0x01000000  # queue q: SQ at +q MiB, CQ at +q MiB + 512 KiB
SRC = 0x02000000
DST = 0x04000000
SGL = 0x06000000
NQUEUES = 4
MAX_XFER = 1 << 20
MAX_SEGS = 128


class QemuDied(Exception):
    pass


class Qtest:
    def __init__(self, qemu, log, queues=NQUEUES, device_args="", iothreads=False):
        self.tmp = tempfile.mkdtemp(prefix="kdc-qtest-")
        self.qmp_path = os.path.join(self.tmp, "qmp.sock")
        self.stderr_path = os.path.join(self.tmp, "stderr.txt")
        self.log = log
        argv = [qemu, "-machine", "q35", "-accel", "qtest", "-qtest", "stdio", "-qtest-log", "/dev/null", "-display", "none",
                "-nodefaults", "-m", "256", "-serial", "none", "-monitor", "none",
                "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
                "-device", "pcie-root-port,id=rp0,bus=pcie.0,addr=0x2,chassis=1",
                "-device", f"kdc-dma,id=kdc,bus=rp0,addr=0x0,queues={queues}{device_args}"]
        if iothreads:
            argv[1:1] = ["-object", "iothread,id=qio0", "-object", "iothread,id=qio1"]
            argv[-1] += ",iothreads=qio0:qio1"
        self.threaded = iothreads
        self.stderr = open(self.stderr_path, "w")
        # QEMU's own allocations are not leak-clean at exit; leak checking would only add noise.
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.stderr, bufsize=0, env=env)
        self.out = self.p.stdout
        self.nqueues = queues
        self.qmp = None

    # -- qtest protocol --
    def cmd(self, line):
        self.p.stdin.write((line + "\n").encode())
        while True:
            reply = self.out.readline()
            if not reply:
                raise QemuDied(f"QEMU exited during '{line}'")
            reply = reply.decode().strip()
            if reply.startswith("IRQ"):
                continue
            if not reply.startswith("OK"):
                raise RuntimeError(f"qtest '{line}' -> '{reply}'")
            return reply[2:].strip()

    def outl(self, port, val):
        self.cmd(f"outl 0x{port:x} 0x{val:x}")

    def inl(self, port):
        return int(self.cmd(f"inl 0x{port:x}"), 16)

    def readl(self, addr):
        return int(self.cmd(f"readl 0x{addr:x}"), 16)

    def writel(self, addr, val):
        self.cmd(f"writel 0x{addr:x} 0x{val & 0xFFFFFFFF:x}")

    def read(self, addr, size):
        return bytes.fromhex(self.cmd(f"read 0x{addr:x} 0x{size:x}")[2:])

    def write(self, addr, data):
        for off in range(0, len(data), 65536):
            chunk = data[off:off + 65536]
            self.cmd(f"write 0x{addr + off:x} 0x{len(chunk):x} 0x{chunk.hex()}")

    def zero(self, addr, size):
        self.cmd(f"memset 0x{addr:x} 0x{size:x} 0x00")

    def step(self, ns):
        """Advance virtual time, then let the bottom halves it scheduled run."""
        now = int(self.cmd(f"clock_step {ns}"))
        self.settle()
        return now

    def settle(self):
        """Give the main loop a few iterations to run pending bottom halves (and
        the IOThreads a moment to run theirs)."""
        for _ in range(3):
            self.cmd(f"readl 0x{BAR0 + KDC_REG_ID:x}")
            if self.threaded:
                time.sleep(0.01)

    # -- PCI config space --
    def cfg_addr(self, bus, dev, fn, reg):
        return 0x80000000 | bus << 16 | dev << 11 | fn << 8 | (reg & 0xFC)

    def cfg_read(self, bus, dev, reg, size=4):
        self.outl(0xCF8, self.cfg_addr(bus, dev, 0, reg))
        val = self.inl(0xCFC)
        return (val >> ((reg & 3) * 8)) & ((1 << (size * 8)) - 1)

    def cfg_write32(self, bus, dev, reg, val):
        self.outl(0xCF8, self.cfg_addr(bus, dev, 0, reg))
        self.outl(0xCFC, val)

    def find_cap(self, cap_id):
        ptr = self.cfg_read(1, 0, 0x34, 1)
        while ptr:
            if self.cfg_read(1, 0, ptr, 1) == cap_id:
                return ptr
            ptr = self.cfg_read(1, 0, ptr + 1, 1)
        return None

    def setup_pci(self):
        # Root port 00:02.0: bus numbers, a 16 MiB memory window, no I/O or prefetch window.
        self.cfg_write32(0, 2, 0x18, 0x00010100)
        self.cfg_write32(0, 2, 0x1C, 0x000000F0)
        self.cfg_write32(0, 2, 0x20, 0xE0F0E000)
        self.cfg_write32(0, 2, 0x24, 0x0000FFF0)
        self.cfg_write32(0, 2, 0x04, 0x00000006)
        assert self.cfg_read(1, 0, 0) == KDC_PCI_DEVICE_ID << 16 | KDC_PCI_VENDOR_ID
        self.cfg_write32(1, 0, 0x10, BAR0 | 0x4)
        self.cfg_write32(1, 0, 0x14, 0)
        self.cfg_write32(1, 0, 0x18, BAR2 | 0x4)
        self.cfg_write32(1, 0, 0x1C, 0)
        self.cfg_write32(1, 0, 0x04, 0x00000006)
        # MSI-X: vector v writes data 0x100+v to MSI_BASE + 4v in RAM.
        cap = self.find_cap(0x11)
        table = self.cfg_read(1, 0, cap + 4)
        assert table & 7 == KDC_BAR_MSIX
        self.msix_table = BAR2 + (table & ~7)
        for v in range(self.nqueues + 1):
            entry = self.msix_table + 16 * v
            self.writel(entry, MSI_BASE + 4 * v)
            self.writel(entry + 4, 0)
            self.writel(entry + 8, 0x100 + v)
            self.writel(entry + 12, 0)
        ctrl = self.cfg_read(1, 0, cap, 4)
        self.cfg_write32(1, 0, cap, (ctrl | 0x80000000) & ~0x40000000)
        self.zero(MSI_BASE, 4 * (self.nqueues + 1))

    def irq_fired(self, vector, clear=True):
        hit = struct.unpack("<I", self.read(MSI_BASE + 4 * vector, 4))[0] == 0x100 + vector
        if clear:
            self.zero(MSI_BASE + 4 * vector, 4)
        return hit

    # -- device registers --
    def reg(self, off):
        return self.readl(BAR0 + off)

    def set_reg(self, off, val):
        self.writel(BAR0 + off, val)

    def qreg(self, q, off):
        return self.readl(BAR0 + KDC_QUEUE_BASE + q * KDC_QUEUE_STRIDE + off)

    def set_qreg(self, q, off, val):
        self.writel(BAR0 + KDC_QUEUE_BASE + q * KDC_QUEUE_STRIDE + off, val)

    def enable(self):
        self.set_reg(KDC_REG_CTRL, KDC_CTRL_ENABLE)
        assert self.reg(KDC_REG_STATUS) & KDC_STATUS_READY

    def queue(self, q, depth_log2=4):
        return Ring(self, q, depth_log2)

    # -- QMP for runtime knobs --
    def qom(self, command, **arguments):
        if self.qmp is None:
            for _ in range(100):
                try:
                    self.qmp = socket.socket(socket.AF_UNIX)
                    self.qmp.connect(self.qmp_path)
                    break
                except OSError:
                    time.sleep(0.05)
            self.qmp_file = self.qmp.makefile("rw")
            json.loads(self.qmp_file.readline())
            self._qmp_send("qmp_capabilities")
        return self._qmp_send(command, arguments)

    def _qmp_send(self, command, arguments=None):
        msg = {"execute": command}
        if arguments:
            msg["arguments"] = arguments
        self.qmp_file.write(json.dumps(msg) + "\n")
        self.qmp_file.flush()
        while True:
            reply = json.loads(self.qmp_file.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(f"QMP {command}: {reply['error']}")

    def qom_set(self, prop, value):
        self.qom("qom-set", path="/machine/peripheral/kdc", property=prop, value=value)

    def qom_get(self, prop):
        return self.qom("qom-get", path="/machine/peripheral/kdc", property=prop)

    def close(self):
        alive = self.p.poll() is None
        if self.qmp:
            self.qmp.close()
        if alive:
            self.p.stdin.close()
            self.p.terminate()
            try:
                self.p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.stderr.close()
        err = pathlib.Path(self.stderr_path).read_text(errors="replace")
        if err.strip():
            self.log.write(err)
        bad = re.search(r"AddressSanitizer|LeakSanitizer|runtime error:|Assertion .* failed|Aborted", err)
        if bad:
            raise RuntimeError(f"QEMU sanitizer/assert report: {bad.group(0)}")
        if not alive and self.p.returncode not in (0, None):
            raise QemuDied(f"QEMU exited with {self.p.returncode}")


class Ring:
    """A queue pair driven from the test, like a minimal driver."""

    def __init__(self, t, q, depth_log2):
        self.t, self.q = t, q
        self.depth = 1 << depth_log2
        self.sq = RING_BASE + q * 0x100000
        self.cq = self.sq + 0x80000
        self.tail = self.head = 0
        self.phase = 1
        t.zero(self.sq, self.depth * KDC_SQE_SIZE)
        t.zero(self.cq, self.depth * KDC_CQE_SIZE)
        t.set_qreg(q, KDC_Q_SQ_BASE_LO, self.sq)
        t.set_qreg(q, KDC_Q_SQ_BASE_HI, 0)
        t.set_qreg(q, KDC_Q_CQ_BASE_LO, self.cq)
        t.set_qreg(q, KDC_Q_CQ_BASE_HI, 0)
        t.set_qreg(q, KDC_Q_SIZE_LOG2, depth_log2)
        t.set_qreg(q, KDC_Q_CTRL, KDC_QCTRL_ENABLE)
        assert t.qreg(q, KDC_Q_STATUS) == KDC_QSTATUS_ENABLED

    def put(self, sqe, ring=True):
        self.t.write(self.sq + self.tail * KDC_SQE_SIZE, sqe)
        self.tail = (self.tail + 1) % self.depth
        if ring:
            self.doorbell()

    def doorbell(self):
        self.t.set_qreg(self.q, KDC_Q_SQ_TAIL, self.tail)

    def poll(self, consume=True):
        """Return new completion entries as dicts."""
        out = []
        while True:
            raw = self.t.read(self.cq + self.head * KDC_CQE_SIZE, KDC_CQE_SIZE)
            result, nbytes, sq_head, sqid, cid, status = struct.unpack("<IIHHHH", raw)
            if status & 1 != self.phase:
                break
            out.append(dict(result=result, bytes=nbytes, sq_head=sq_head, sqid=sqid, cid=cid,
                            code=status >> 1))
            self.head = (self.head + 1) % self.depth
            if self.head == 0:
                self.phase ^= 1
        if out and consume:
            self.t.set_qreg(self.q, KDC_Q_CQ_HEAD, self.head)
        return out


def sqe(opcode=KDC_OP_COPY_CRC, flags=0, cid=0, length=0, src=0, dst=0, src_nents=0, dst_nents=0,
        seed=0, rsvd=(0, 0, 0, 0)):
    return struct.pack("<BBHIQQHHI4Q", opcode, flags, cid, length, src, dst, src_nents, dst_nents,
                       seed, *rsvd)


def sgl(entries):
    return b"".join(struct.pack("<QII", a, n, r) for a, n, r in entries)


def copy_cmd(t, cid, length, seed=0, src_segs=None, dst_segs=None, src=SRC, dst=DST, data=None):
    """Write random source data and SG lists; return (sqe bytes, expected data)."""
    data = data if data is not None else random.randbytes(length)
    flags = 0
    src_field, dst_field, ns, nd = src, dst, 0, 0
    if src_segs:
        entries, off = [], 0
        for i, n in enumerate(src_segs):
            addr = src + off + i * 512  # gaps between segments
            t.write(addr, data[off:off + n])
            entries.append((addr, n, 0))
            off += n
        t.write(SGL + cid * 0x1000, sgl(entries))
        flags |= KDC_SQE_F_SRC_SGL
        src_field, ns = SGL + cid * 0x1000, len(entries)
    else:
        t.write(src, data)
    if dst_segs:
        entries, off = [], 0
        for i, n in enumerate(dst_segs):
            entries.append((dst + off + i * 512, n, 0))
            off += n
        t.write(SGL + cid * 0x1000 + 0x800, sgl(entries))
        flags |= KDC_SQE_F_DST_SGL
        dst_field, nd = SGL + cid * 0x1000 + 0x800, len(entries)
    return sqe(flags=flags, cid=cid, length=length, src=src_field, dst=dst_field, src_nents=ns,
               dst_nents=nd, seed=seed), data


def read_dst(t, length, dst_segs=None, dst=DST):
    if not dst_segs:
        return t.read(dst, length)
    out, off = b"", 0
    for i, n in enumerate(dst_segs):
        out += t.read(dst + off + i * 512, n)
        off += n
    return out


def check(cond, what):
    if not cond:
        raise AssertionError(what)


# ---------------------------------------------------------------- tests

def t_identity(t):
    check(t.cfg_read(1, 0, 0x08) >> 8 == 0x088000, "class code is system peripheral")
    check(t.cfg_read(1, 0, 0x08) & 0xFF == KDC_PCI_REVISION, "revision")
    check(t.find_cap(0x10) is not None, "PCIe capability present")
    cap = t.find_cap(0x11)
    check((t.cfg_read(1, 0, cap + 2, 2) & 0x7FF) + 1 == NQUEUES + 1, "one MSI-X vector per queue + error")
    check(t.reg(KDC_REG_ID) == KDC_ID_VALUE, "ID register")
    check(t.reg(KDC_REG_VERSION) == KDC_VERSION_MAJOR << 16 | KDC_VERSION_MINOR, "version")
    cap_reg = t.reg(KDC_REG_CAP)
    check(cap_reg & KDC_CAP_QUEUES_MASK == NQUEUES, "CAP queues")
    check(cap_reg >> KDC_CAP_SEGS_SHIFT == MAX_SEGS, "CAP segments")
    check(t.reg(KDC_REG_MAX_XFER) == MAX_XFER, "max transfer")
    check(t.reg(KDC_REG_STATUS) == 0, "not ready before enable")
    t.enable()


def t_queue_config(t):
    q = 1
    t.set_qreg(q, KDC_Q_SIZE_LOG2, 4)
    t.set_qreg(q, KDC_Q_SQ_BASE_LO, RING_BASE + 0x10)  # unaligned
    t.set_qreg(q, KDC_Q_CQ_BASE_LO, RING_BASE + 0x80000)
    t.set_qreg(q, KDC_Q_CTRL, 1)
    check(t.qreg(q, KDC_Q_STATUS) == 0, "queue enable refused before controller enable")
    t.enable()
    t.set_qreg(q, KDC_Q_CTRL, 1)
    check(t.qreg(q, KDC_Q_STATUS) == 0, "unaligned ring refused")
    check(t.reg(KDC_REG_ERR_STATUS) & KDC_ERR_QUEUE_CONFIG, "QUEUE_CONFIG error latched")
    check(t.irq_fired(NQUEUES), "error vector fired")
    t.set_reg(KDC_REG_ERR_STATUS, KDC_ERR_QUEUE_CONFIG)
    check(t.reg(KDC_REG_ERR_STATUS) == 0, "error status is write-1-to-clear")
    t.set_qreg(q, KDC_Q_SQ_BASE_LO, RING_BASE)
    for bad in (0, 13):
        t.set_qreg(q, KDC_Q_SIZE_LOG2, bad)
        t.set_qreg(q, KDC_Q_CTRL, 1)
        check(t.qreg(q, KDC_Q_STATUS) == 0, f"size log2 {bad} refused")
    ring = t.queue(q, 4)
    check(ring.depth == 16, "queue enabled")


def t_copy_crc(t):
    t.enable()
    ring = t.queue(0, 4)
    cases = [(1, None, None), (4096, None, None), (5000, [1000, 4000], [2500, 2500]),
             (65536, [4096] * 16, None), (MAX_XFER, None, [MAX_XFER // 2] * 2),
             (777, [1] * 7 + [770], [700, 77])]
    for cid, (length, ss, ds) in enumerate(cases, start=10):
        seed = random.getrandbits(32) if cid % 2 else 0
        t.zero(DST, length + 64 * 512)
        cmd, data = copy_cmd(t, cid, length, seed, ss, ds)
        ring.put(cmd)
        t.settle()
        done = ring.poll()
        check(len(done) == 1, f"one completion for len {length}")
        c = done[0]
        check(c["code"] == KDC_SC_SUCCESS and c["cid"] == cid and c["sqid"] == 0, f"success cqe {c}")
        check(c["bytes"] == length and c["sq_head"] == ring.tail, "bytes and sq_head")
        check(c["result"] == crc32c(data, seed), f"CRC32C for len {length}")
        check(read_dst(t, length, ds) == data, f"destination bytes for len {length}")
        check(t.irq_fired(0), "queue vector fired")
    check(t.reg(KDC_REG_STAT_DONE) == len(cases), "done counter")


def t_copy_crc_table(t):
    """The same copies with the device's table CRC (hw-crc=off) instead of SSE4.2."""
    t_copy_crc(t)


t_copy_crc_table.device_args = ",hw-crc=off"


def t_service_time(t):
    t.enable()
    t.set_reg(KDC_REG_SERVICE_NS, 1_000_000)
    r0, r1 = t.queue(0), t.queue(1)
    for cid in range(3):
        r0.put(copy_cmd(t, cid, 64, src=SRC + cid * 4096, dst=DST + cid * 4096)[0])
    r1.put(copy_cmd(t, 9, 64, src=SRC + 0x80000, dst=DST + 0x80000)[0])
    t.settle()
    check(t.qreg(0, KDC_Q_INFLIGHT) == 3 and not r0.poll(), "commands held in flight")
    t.step(999_999)
    check(not r0.poll() and not r1.poll(), "nothing before the service time")
    t.step(1)
    a, b = r0.poll(), r1.poll()
    check([c["cid"] for c in a] == [0] and [c["cid"] for c in b] == [9], "queues run in parallel")
    t.step(1_000_000)
    check([c["cid"] for c in r0.poll()] == [1], "serial engine per queue (2nd)")
    t.step(1_000_000)
    check([c["cid"] for c in r0.poll()] == [2], "serial engine per queue (3rd)")
    check(t.qreg(0, KDC_Q_INFLIGHT) == 0, "nothing left in flight")


def t_abort(t):
    t.enable()
    t.set_reg(KDC_REG_SERVICE_NS, 1_000_000_000)
    ring = t.queue(2)
    for cid in (7, 8, 9):
        ring.put(copy_cmd(t, cid, 256, src=SRC + cid * 4096, dst=DST + cid * 4096)[0])
    t.zero(DST + 8 * 4096, 256)
    t.settle()
    t.set_qreg(2, KDC_Q_ABORT, 0x0BAD)  # unknown cid: ignored
    t.set_qreg(2, KDC_Q_ABORT, 8)
    t.settle()
    done = ring.poll()
    check([(c["cid"], c["code"]) for c in done] == [(8, KDC_SC_ABORTED)], f"abort completes promptly {done}")
    check(t.read(DST + 8 * 4096, 256) == bytes(256), "aborted command did no DMA")
    t.step(1_000_000_000)
    check([(c["cid"], c["code"]) for c in ring.poll()] == [(7, KDC_SC_SUCCESS)], "others continue")
    t.step(1_000_000_000)
    check([(c["cid"], c["code"]) for c in ring.poll()] == [(9, KDC_SC_SUCCESS)], "aborted slot freed")
    check(t.reg(KDC_REG_STAT_ABORTED) == 1, "abort counter")


def t_phase_and_backpressure(t):
    t.enable()
    ring = t.queue(3, 2)  # 4 entries, 3 usable
    cid = 0
    for _ in range(10):
        for _ in range(3):
            ring.put(copy_cmd(t, cid & 0xFFF, 32, src=SRC + (cid % 3) * 4096,
                              dst=DST + (cid % 3) * 4096)[0], ring=False)
            cid += 1
        ring.doorbell()
        t.settle()
        check(len(ring.poll()) == 3, "three completions per round across wraps")
    flips = ring.phase
    # Completion ring full: the device must hold completions until CQ_HEAD moves.
    for i in range(3):
        ring.put(copy_cmd(t, 100 + i, 32, src=SRC + i * 4096, dst=DST + i * 4096)[0], ring=False)
    ring.doorbell()
    t.settle()
    first = ring.poll(consume=False)
    check(len(first) == 3, "CQ fills")
    for i in range(3):
        ring.put(copy_cmd(t, 200 + i, 32, src=SRC + i * 4096, dst=DST + i * 4096)[0], ring=False)
    ring.doorbell()
    t.settle()
    check(not ring.poll(consume=False), "no completion written into a full CQ")
    check(t.qreg(3, KDC_Q_INFLIGHT) == 3, "device holds the finished commands")
    t.set_qreg(3, KDC_Q_CQ_HEAD, ring.head)
    t.settle()
    check([c["cid"] for c in ring.poll()] == [200, 201, 202], "held completions posted after CQ_HEAD")
    check(flips in (0, 1), "phase tracked")


def malformed_cases():
    good = dict(flags=KDC_SQE_F_SRC_SGL | KDC_SQE_F_DST_SGL, length=8192, src=SGL, dst=SGL + 0x800,
                src_nents=2, dst_nents=2)
    ok_src = [(SRC, 4096, 0), (SRC + 8192, 4096, 0)]
    ok_dst = [(DST, 4096, 0), (DST + 8192, 4096, 0)]
    c = [
        ("bad opcode", dict(good, opcode=0x7F), ok_src, ok_dst, KDC_SC_INVALID_OPCODE),
        ("reserved nonzero", dict(good, rsvd=(0, 0, 1, 0)), ok_src, ok_dst, KDC_SC_INVALID_FIELD),
        ("unknown flag", dict(good, flags=0x83), ok_src, ok_dst, KDC_SC_INVALID_FIELD),
        ("zero length", dict(good, length=0), ok_src, ok_dst, KDC_SC_INVALID_FIELD),
        ("length over max", dict(good, length=MAX_XFER + 1), ok_src, ok_dst, KDC_SC_INVALID_FIELD),
        ("nents without SGL flag", dict(good, flags=KDC_SQE_F_DST_SGL), ok_src, ok_dst,
         KDC_SC_INVALID_FIELD),
        ("zero src entries", dict(good, src_nents=0), ok_src, ok_dst, KDC_SC_INVALID_SGL),
        ("too many dst entries", dict(good, dst_nents=MAX_SEGS + 1), ok_src, ok_dst, KDC_SC_INVALID_SGL),
        ("entry of length 0", good, [(SRC, 0, 0), (SRC, 8192, 0)], ok_dst, KDC_SC_INVALID_SGL),
        ("entry reserved nonzero", good, ok_src, [(DST, 4096, 5), (DST + 8192, 4096, 0)],
         KDC_SC_INVALID_SGL),
        ("entries short of length", good, [(SRC, 4096, 0), (SRC, 4095, 0)], ok_dst, KDC_SC_INVALID_SGL),
        ("entries beyond length", good, ok_src, [(DST, 4096, 0), (DST, 4097, 0)], KDC_SC_INVALID_SGL),
        ("u32 wrap in entry sum", dict(good, length=1), [(SRC, 0xFFFFFFFF, 0), (SRC, 2, 0)], ok_dst,
         KDC_SC_INVALID_SGL),
        ("NOP with a length", dict(opcode=KDC_OP_NOP, length=4), [], [], KDC_SC_INVALID_FIELD),
        ("NOP with a seed", dict(opcode=KDC_OP_NOP, seed=1), [], [], KDC_SC_INVALID_FIELD),
    ]
    return c


def t_malformed(t):
    t.enable()
    ring = t.queue(1, 5)
    t.write(SRC, random.randbytes(16384))
    for cid, (name, fields, src_entries, dst_entries, expect) in enumerate(malformed_cases()):
        t.zero(DST, 16384)
        t.write(SGL, sgl(src_entries))
        t.write(SGL + 0x800, sgl(dst_entries))
        ring.put(sqe(cid=cid, **fields))
        t.settle()
        done = ring.poll()
        check(len(done) == 1 and done[0]["cid"] == cid and done[0]["code"] == expect,
              f"{name}: expected status {expect}, got {done}")
        check(t.read(DST, 16384) == bytes(16384), f"{name}: no destination write")
    cmd, data = copy_cmd(t, 99, 4096)
    ring.put(cmd)
    t.settle()
    done = ring.poll()
    check(done and done[0]["code"] == KDC_SC_SUCCESS and t.read(DST, 4096) == data,
          "device still works after malformed commands")
    t.log.write(f"  {len(malformed_cases())} malformed descriptor classes rejected\n")


def t_doorbell_errors(t):
    t.enable()
    ring = t.queue(0, 3)
    t.set_qreg(0, KDC_Q_SQ_TAIL, 8)
    check(t.reg(KDC_REG_ERR_STATUS) & KDC_ERR_DOORBELL, "out-of-range SQ tail flagged")
    check(t.irq_fired(NQUEUES), "error vector")
    t.set_qreg(0, KDC_Q_CQ_HEAD, 0xFFFFFFFF)
    check(t.qreg(0, KDC_Q_SQ_HEAD) == 0, "bad doorbells ignored")
    t.set_reg(KDC_REG_ERR_STATUS, 0xFFFFFFFF)
    ring.put(copy_cmd(t, 1, 100)[0])
    t.settle()
    check(ring.poll()[0]["code"] == KDC_SC_SUCCESS, "queue usable after bad doorbells")
    t.set_qreg(3, KDC_Q_SQ_TAIL, 1)
    check(t.reg(KDC_REG_ERR_STATUS) & KDC_ERR_DOORBELL, "doorbell on a disabled queue flagged")


def t_ring_dma_fault(t):
    t.enable()
    good = t.queue(0)
    q = 1
    t.set_qreg(q, KDC_Q_SQ_BASE_LO, 0)
    t.set_qreg(q, KDC_Q_SQ_BASE_HI, 0x100)  # 1 TiB: nothing mapped there
    t.set_qreg(q, KDC_Q_CQ_BASE_LO, RING_BASE + 0x180000)
    t.set_qreg(q, KDC_Q_CQ_BASE_HI, 0)
    t.set_qreg(q, KDC_Q_SIZE_LOG2, 2)
    t.set_qreg(q, KDC_Q_CTRL, 1)
    t.set_qreg(q, KDC_Q_SQ_TAIL, 1)
    t.settle()
    check(t.qreg(q, KDC_Q_STATUS) & KDC_QSTATUS_HALTED, "failed SQ fetch halts the queue")
    check(t.reg(KDC_REG_ERR_STATUS) & KDC_ERR_RING_DMA and t.reg(KDC_REG_ERR_INFO) == q, "error info")
    check(t.irq_fired(NQUEUES), "error vector")
    good.put(copy_cmd(t, 3, 512)[0])
    t.settle()
    check(good.poll()[0]["code"] == KDC_SC_SUCCESS, "other queues unaffected")


def t_dma_into_own_bar(t):
    t.enable()
    ring = t.queue(0)
    t.write(SRC, random.randbytes(64))
    ring.put(sqe(length=64, src=SRC, dst=BAR0 + KDC_REG_RESET))
    t.settle()
    done = ring.poll()
    check(done and done[0]["code"] == KDC_SC_DATA_ERROR, f"re-entrant DMA into own BAR blocked {done}")
    check(t.reg(KDC_REG_STATUS) & KDC_STATUS_READY, "device not reset by its own DMA")
    ring.put(sqe(cid=2, length=64, src=BAR2, dst=DST))
    t.settle()
    done = ring.poll()
    check(done and done[0]["code"] == KDC_SC_DATA_ERROR, "DMA read of own MSI-X BAR blocked")
    ring.put(sqe(cid=3, length=64, src=0xFFFF_FFFF_FFFF_0000, dst=DST))
    t.settle()
    check(ring.poll()[0]["code"] == KDC_SC_DATA_ERROR, "unmapped address gives a data error")


def t_reset_paths(t):
    t.enable()
    t.set_reg(KDC_REG_SERVICE_NS, 1_000_000)
    ring = t.queue(0)
    ring.put(copy_cmd(t, 1, 64)[0])
    t.settle()
    check(t.qreg(0, KDC_Q_INFLIGHT) == 1, "in flight")
    t.set_reg(KDC_REG_CTRL, 0)
    check(t.reg(KDC_REG_STATUS) == 0 and t.qreg(0, KDC_Q_STATUS) == 0, "disable drops queues")
    t.step(2_000_000)
    check(not ring.poll(consume=False), "dropped work posts no completion")
    t.enable()
    ring = t.queue(0)
    t.set_reg(KDC_REG_FAULT_DROP, 3)
    t.set_reg(KDC_REG_RESET, 0x12345678)
    check(t.reg(KDC_REG_FAULT_DROP) == 3, "wrong reset magic ignored")
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    check(t.reg(KDC_REG_STATUS) == 0 and t.reg(KDC_REG_FAULT_DROP) == 0, "reset clears state")
    check(t.reg(KDC_REG_SERVICE_NS) == 1_000_000, "service time survives reset")
    t.enable()
    t.queue(0)
    pcie = t.find_cap(0x10)
    devctl = t.cfg_read(1, 0, pcie + 8)
    t.cfg_write32(1, 0, pcie + 8, devctl | 0x8000)  # initiate function-level reset
    check(t.reg(KDC_REG_STATUS) == 0 and t.qreg(0, KDC_Q_STATUS) == 0, "FLR resets the device")


def t_fault_knobs(t):
    t.enable()
    t.qom_set("service-ns", 500)
    check(t.reg(KDC_REG_SERVICE_NS) == 500 and t.qom_get("service-ns") == 500, "service-ns via QMP")
    t.qom_set("service-ns", 0)
    ring = t.queue(0)
    t.qom_set("fault-stall", True)
    ring.put(copy_cmd(t, 1, 64)[0])
    t.settle()
    t.set_qreg(0, KDC_Q_ABORT, 1)
    t.step(10_000_000)
    check(not ring.poll(), "stalled engine: no completion, abort not honoured")
    t.qom_set("fault-stall", False)
    t.settle()
    check([c["code"] for c in ring.poll()] == [KDC_SC_ABORTED], "abort lands after unstall")
    t.qom_set("fault-drop", 1)
    ring.put(copy_cmd(t, 2, 64)[0])
    ring.put(copy_cmd(t, 3, 64)[0])
    t.settle()
    check([c["cid"] for c in ring.poll()] == [3], "one completion dropped")
    t.qom_set("fault-bad-cid", 1)
    ring.put(copy_cmd(t, 4, 64)[0])
    t.settle()
    check([c["cid"] for c in ring.poll()] == [4 ^ 0xFFF], "completion cid corrupted")
    t.qom_set("fault-bad-dma", 1)
    ring.put(copy_cmd(t, 5, 64)[0])
    t.settle()
    check([c["code"] for c in ring.poll()] == [KDC_SC_DATA_ERROR], "copy to invalid address fails")
    t.irq_fired(NQUEUES)
    t.qom_set("fault-fatal", True)
    check(t.reg(KDC_REG_STATUS) & KDC_STATUS_FATAL and t.reg(KDC_REG_ERR_STATUS) & KDC_ERR_FATAL,
          "fatal state latched")
    check(t.irq_fired(NQUEUES), "fatal raises the error vector")
    ring.put(copy_cmd(t, 6, 64)[0])
    t.settle()
    check(not ring.poll(), "fatal device processes nothing")
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    check(t.reg(KDC_REG_STATUS) == 0 and t.qom_get("fault-fatal") is False, "reset clears fatal")


def t_fault_hold(t):
    t.enable()
    t.set_reg(KDC_REG_SERVICE_NS, 1000)
    ring = t.queue(1)
    t.qom_set("fault-hold", 2)
    check(t.reg(KDC_REG_FAULT_HOLD) == 2, "hold count readable")
    for cid in (1, 2, 3):
        ring.put(copy_cmd(t, cid, 64, src=SRC + cid * 4096, dst=DST + cid * 4096)[0])
    t.settle()
    t.step(1_000_000)
    done = ring.poll()
    check([c["cid"] for c in done] == [3], f"held commands are not serviced, the next one is {done}")
    check(t.qreg(1, KDC_Q_INFLIGHT) == 2 and t.reg(KDC_REG_FAULT_HOLD) == 0, "both held in flight")
    t.step(100_000_000)
    check(not ring.poll(), "held commands never complete by themselves")
    t.set_qreg(1, KDC_Q_ABORT, 2)
    t.settle()
    check([(c["cid"], c["code"]) for c in ring.poll()] == [(2, KDC_SC_ABORTED)], "a held command can be aborted")
    check(t.qreg(1, KDC_Q_INFLIGHT) == 1, "the other stays held")
    other = t.queue(2)
    t.set_reg(KDC_REG_FAULT_HOLD, (2 + 1) << KDC_HOLD_QUEUE_SHIFT | 1)  # one command, queue 2 only
    ring.put(copy_cmd(t, 4, 64, src=SRC + 4 * 4096, dst=DST + 4 * 4096)[0])
    other.put(copy_cmd(t, 5, 64, src=SRC + 5 * 4096, dst=DST + 5 * 4096)[0])
    other.put(copy_cmd(t, 6, 64, src=SRC + 6 * 4096, dst=DST + 6 * 4096)[0])
    t.settle()
    t.step(1_000_000)
    check([c["cid"] for c in ring.poll()] == [4], "a hold for queue 2 leaves queue 1 alone")
    check([c["cid"] for c in other.poll()] == [6] and t.qreg(2, KDC_Q_INFLIGHT) == 1,
          "the first command of queue 2 is held")
    check(t.reg(KDC_REG_FAULT_HOLD) & KDC_HOLD_COUNT_MASK == 0, "the count ran down")
    t.set_reg(KDC_REG_FAULT_HOLD, 5)
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    check(t.reg(KDC_REG_FAULT_HOLD) == 0 and t.qom_get("fault-hold") == 0, "reset clears the hold count")


def flr(t):
    """Function-level reset through the PCIe capability, then reprogram config space."""
    pcie = t.find_cap(0x10)
    devctl = t.cfg_read(1, 0, pcie + 8)
    t.cfg_write32(1, 0, pcie + 8, devctl | 0x8000)
    t.setup_pci()


def t_recovery_knobs(t):
    t.enable()
    ring = t.queue(0)
    # A controller that ignores its reset: disable and RESET do nothing; FLR clears it.
    t.set_reg(KDC_REG_FAULT, KDC_FAULT_RESET_HANG)
    check(t.reg(KDC_REG_FAULT) == KDC_FAULT_RESET_HANG and t.qom_get("fault-reset-hang") is True,
          "reset-hang set from the guest, visible over QMP")
    t.set_reg(KDC_REG_CTRL, 0)
    check(t.reg(KDC_REG_STATUS) & KDC_STATUS_READY and t.qreg(0, KDC_Q_STATUS) == KDC_QSTATUS_ENABLED,
          "disable not acknowledged")
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    check(t.reg(KDC_REG_STATUS) & KDC_STATUS_READY and t.reg(KDC_REG_FAULT) == KDC_FAULT_RESET_HANG,
          "controller reset ignored")
    ring.put(copy_cmd(t, 1, 64)[0])
    t.settle()
    check([c["code"] for c in ring.poll()] == [KDC_SC_SUCCESS], "still executing commands")
    flr(t)
    check(t.reg(KDC_REG_ID) == KDC_ID_VALUE and t.reg(KDC_REG_STATUS) == 0 and t.reg(KDC_REG_FAULT) == 0,
          "FLR resets the controller and clears reset-hang")
    # A controller that never becomes ready again, through every kind of reset.
    t.set_reg(KDC_REG_FAULT, KDC_FAULT_STUCK)
    t.set_reg(KDC_REG_CTRL, KDC_CTRL_ENABLE)
    check(not t.reg(KDC_REG_STATUS) & KDC_STATUS_READY, "stuck: enable does not make it ready")
    t.set_reg(KDC_REG_CTRL, 0)
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    t.set_reg(KDC_REG_CTRL, KDC_CTRL_ENABLE)
    check(not t.reg(KDC_REG_STATUS) & KDC_STATUS_READY and t.reg(KDC_REG_FAULT) == KDC_FAULT_STUCK,
          "stuck survives a controller reset")
    flr(t)
    t.set_reg(KDC_REG_CTRL, KDC_CTRL_ENABLE)
    check(not t.reg(KDC_REG_STATUS) & KDC_STATUS_READY and t.qom_get("fault-stuck") is True,
          "stuck survives FLR")
    t.qom_set("fault-stuck", False)
    t.set_reg(KDC_REG_CTRL, 0)
    t.enable()
    # A device that stopped responding: all-ones from BAR and config space,
    # no DMA, no interrupts; it behaves again once the knob is cleared.
    ring = t.queue(0)
    t.qom_set("fault-gone", True)
    check(t.reg(KDC_REG_ID) == 0xFFFFFFFF and t.reg(KDC_REG_STATUS) == 0xFFFFFFFF, "BAR reads all-ones")
    check(t.cfg_read(1, 0, 0) == 0xFFFFFFFF, "config reads all-ones")
    t.zero(DST, 64)
    ring.put(copy_cmd(t, 2, 64)[0])
    t.step(10_000_000)
    check(not ring.poll(consume=False) and not t.irq_fired(0), "no DMA, no interrupt")
    check(t.read(DST, 64) == bytes(64), "nothing written")
    t.qom_set("fault-gone", False)
    check(t.reg(KDC_REG_ID) == KDC_ID_VALUE, "answers again")
    ring.doorbell()  # the doorbell written while gone was lost
    t.settle()
    check([c["code"] for c in ring.poll()] == [KDC_SC_SUCCESS], "processes the command afterwards")


def t_coalescing(t):
    t.enable()
    ring = t.queue(0)
    t.set_qreg(0, KDC_Q_COALESCE, 100 << 16 | 4)
    for cid in range(3):
        ring.put(copy_cmd(t, cid, 32, src=SRC + cid * 4096, dst=DST + cid * 4096)[0])
    t.settle()
    check(len(ring.poll()) == 3 and not t.irq_fired(0), "below the count: interrupt deferred")
    t.step(99_999)
    check(not t.irq_fired(0), "not before the delay")
    t.step(1)
    check(t.irq_fired(0), "delay expiry fires")
    for cid in range(4):
        ring.put(copy_cmd(t, 10 + cid, 32, src=SRC + cid * 4096, dst=DST + cid * 4096)[0], ring=False)
    ring.doorbell()
    t.settle()
    check(len(ring.poll()) == 4 and t.irq_fired(0), "count reached: immediate interrupt")


def t_pass_interrupts(t):
    """Without coalescing, one pass interrupts for its first completion and once after its last."""
    t.enable()
    ring = t.queue(1)
    irqs = t.reg(KDC_REG_STAT_IRQS)
    ring.put(copy_cmd(t, 1, 32, src=SRC, dst=DST)[0])
    t.settle()
    check(len(ring.poll()) == 1 and t.reg(KDC_REG_STAT_IRQS) == irqs + 1, "a lone completion interrupts once")
    check(t.irq_fired(1), "queue vector fired")
    for cid in range(8):
        ring.put(copy_cmd(t, 10 + cid, 32, src=SRC + cid * 4096, dst=DST + cid * 4096)[0], ring=False)
    ring.doorbell()
    t.settle()
    check(len(ring.poll()) == 8, "eight completions in one pass")
    check(t.reg(KDC_REG_STAT_IRQS) == irqs + 3,
          "first and last completion of the pass interrupt, nothing in between")
    check(t.irq_fired(1), "queue vector fired for the pass")


def t_shadow_doorbell(t):
    """DBBUF: the SQ tail comes from host memory, the doorbell value is ignored, and the
    device publishes the tail it has fetched up to as the event index."""
    check(t.reg(KDC_REG_FEATURES) & KDC_FEAT_SHADOW_DB, "feature offered")
    t.enable()
    dbbuf = 0x00900000
    t.zero(dbbuf, 4096)
    t.set_reg(KDC_REG_DBBUF_HI, 0)
    t.set_reg(KDC_REG_DBBUF_LO, dbbuf | KDC_DBBUF_ENABLE)
    ring = t.queue(2)
    entry = dbbuf + 2 * KDC_DBBUF_SIZE
    for cid in range(3):
        ring.put(copy_cmd(t, cid, 32, src=SRC + cid * 4096, dst=DST + cid * 4096)[0], ring=False)
    t.writel(entry, ring.tail)
    t.set_qreg(2, KDC_Q_SQ_TAIL, 0)  # a kick; the value is ignored
    t.settle()
    check(len(ring.poll()) == 3, "three commands fetched up to the shadow tail")
    check(t.readl(entry + 4) == 3, "event index published after the pass")
    ring.put(copy_cmd(t, 3, 32)[0], ring=False)
    t.writel(entry, ring.tail)
    t.settle()
    check(not ring.poll() and t.qreg(2, KDC_Q_INFLIGHT) == 0, "no kick, no fetch")
    t.set_qreg(2, KDC_Q_SQ_TAIL, 0)
    t.settle()
    check(len(ring.poll()) == 1 and t.readl(entry + 4) == 4, "kicked: fetched and index moved")
    t.writel(entry, 99)
    t.set_qreg(2, KDC_Q_SQ_TAIL, 0)
    t.settle()
    check(t.reg(KDC_REG_ERR_STATUS) & KDC_ERR_DOORBELL, "out-of-range shadow tail reported")
    check(t.readl(dbbuf + 4) == 0, "other queues' entries untouched")
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    check(t.reg(KDC_REG_DBBUF_LO) == 0 and t.reg(KDC_REG_DBBUF_HI) == 0, "reset clears DBBUF")


def t_shadow_db_off(t):
    """With shadow-db=off the feature is not offered and DBBUF ignores writes."""
    check(not t.reg(KDC_REG_FEATURES) & KDC_FEAT_SHADOW_DB, "feature not offered")
    t.enable()
    t.set_reg(KDC_REG_DBBUF_LO, 0x00900000 | KDC_DBBUF_ENABLE)
    check(t.reg(KDC_REG_DBBUF_LO) == 0, "DBBUF ignores writes")
    ring = t.queue(0)
    ring.put(copy_cmd(t, 1, 32)[0])
    t.settle()
    check(len(ring.poll()) == 1, "doorbell value used as before")


t_shadow_db_off.device_args = ",shadow-db=off"


def t_counters_and_scratch(t):
    t.enable()
    check(t.reg(KDC_REG_SCRATCH) == 0, "scratch starts at 0")
    t.set_reg(KDC_REG_SCRATCH, 0xA5A50001)
    check(t.qom_get("scratch") == 0xA5A50001, "scratch written by MMIO, read by QMP")
    t.qom_set("scratch", 7)
    check(t.reg(KDC_REG_SCRATCH) == 7, "scratch written by QMP, read by MMIO")
    ring = t.queue(0)
    for cid in range(3):
        ring.put(copy_cmd(t, cid, 64, src=SRC + cid * 4096, dst=DST + cid * 4096)[0], ring=cid != 1)
        if cid == 0:
            # the first command's own pass (one interrupt) before the next two (two):
            # an IOThread could otherwise fetch all three in one pass
            t.settle()
    t.settle()
    check(len(ring.poll()) == 3, "three completions, reaped with one CQ head write")
    stats = {name: t.reg(reg) for name, reg in (("doorbells", KDC_REG_STAT_DOORBELLS),
                                                ("cq_doorbells", KDC_REG_STAT_CQ_DOORBELLS),
                                                ("irqs", KDC_REG_STAT_IRQS), ("done", KDC_REG_STAT_DONE))}
    check(stats["doorbells"] == 2 and stats["cq_doorbells"] == 1, f"doorbell counters {stats}")
    check(stats["done"] == 3 and stats["irqs"] == 3, f"completion counters {stats}")
    check(all(t.qom_get(f"stat-{name.replace('_', '-')}") == v for name, v in stats.items()),
          "counters readable over QMP")
    try:
        t.qom_set("stat-irqs", 0)
        check(False, "counters are read-only over QMP")
    except RuntimeError:
        pass
    t.set_qreg(0, KDC_Q_CQ_HEAD, 99)
    check(t.reg(KDC_REG_STAT_CQ_DOORBELLS) == 1, "rejected CQ head write not counted")
    t.set_reg(KDC_REG_RESET, KDC_RESET_MAGIC)
    check(t.reg(KDC_REG_SCRATCH) == 0 and t.reg(KDC_REG_STAT_CQ_DOORBELLS) == 0, "reset clears both")


def t_fuzz(t, iterations=2000):
    """Random descriptors, SG lists, doorbells and aborts; QEMU must survive."""
    t.enable()
    rng = random.Random(0x6B6463)
    rings = [t.queue(q, 4) for q in range(NQUEUES)]
    t.set_reg(KDC_REG_SERVICE_NS, 2000)

    def addr():
        r = rng.random()
        if r < 0.5:
            return rng.randrange(SRC, SRC + 0x100000)
        if r < 0.6:
            return rng.choice([BAR0, BAR2, MSI_BASE, RING_BASE]) + rng.randrange(0, 0x1000)
        if r < 0.8:
            return rng.randrange(DST, DST + 0x100000)
        return rng.getrandbits(64)

    submitted = completed = 0
    for i in range(iterations):
        r = rings[rng.randrange(NQUEUES)]
        if (r.tail + 1) % r.depth == (r.head + (r.depth - 1)) % r.depth:
            r.poll()
        nents = rng.choice([0, 1, 2, 5, MAX_SEGS, MAX_SEGS + 1, rng.getrandbits(16)])
        entries = [(addr(), rng.choice([1, 7, 4096, rng.getrandbits(32)]), rng.choice([0, 0, 0, 1]))
                   for _ in range(min(nents, 8))]
        t.write(SGL + (i % 64) * 0x200, sgl(entries))
        fields = dict(opcode=rng.choice([0, 1, 1, 1, rng.getrandbits(8)]),
                      flags=rng.choice([0, 1, 2, 3, rng.getrandbits(8)]), cid=rng.getrandbits(16),
                      length=rng.choice([0, 1, 512, 4096, MAX_XFER, rng.getrandbits(32)]),
                      src=rng.choice([SGL + (i % 64) * 0x200, addr()]), dst=addr(),
                      src_nents=nents & 0xFFFF, dst_nents=rng.choice([0, 1, nents & 0xFFFF]),
                      seed=rng.getrandbits(32),
                      rsvd=(0, 0, 0, 0) if rng.random() < 0.9 else (rng.getrandbits(64), 0, 0, 0))
        if rng.random() < 0.02:
            raw = bytes(rng.getrandbits(8) for _ in range(KDC_SQE_SIZE))
        else:
            raw = sqe(**fields)
        if (r.tail + 1) % r.depth != r.head:
            r.put(raw)
            submitted += 1
        if rng.random() < 0.1:
            t.set_qreg(r.q, KDC_Q_ABORT, rng.getrandbits(16))
        if rng.random() < 0.02:
            t.set_qreg(r.q, rng.choice([KDC_Q_SQ_TAIL, KDC_Q_CQ_HEAD]), rng.getrandbits(32))
        t.step(rng.choice([1, 1000, 5000]))
        for ring in rings:
            completed += len(ring.poll())
    t.step(1_000_000_000)
    for ring in rings:
        completed += len(ring.poll())
    check(t.reg(KDC_REG_ID) == KDC_ID_VALUE, "device still answers after fuzzing")
    t.log.write(f"  fuzz: {iterations} iterations, {submitted} descriptors queued, "
                f"{completed} completions read (queues may halt on bad doorbells)\n")


TESTS = [t_identity, t_queue_config, t_copy_crc, t_copy_crc_table, t_service_time, t_abort, t_phase_and_backpressure,
         t_malformed, t_doorbell_errors, t_ring_dma_fault, t_dma_into_own_bar, t_reset_paths,
         t_fault_knobs, t_fault_hold, t_recovery_knobs, t_coalescing, t_pass_interrupts, t_shadow_doorbell, t_shadow_db_off,
         t_counters_and_scratch, t_fuzz]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--qemu", required=True)
    p.add_argument("--log", type=pathlib.Path, required=True)
    p.add_argument("--only")
    args = p.parse_args()
    random.seed(1234)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]
    failures = ran = 0
    with args.log.open("w") as log:
        log.write(f"qemu: {args.qemu}\n{version}\n")
        for test, threaded in [(t, False) for t in TESTS] + [(t, True) for t in TESTS]:
            name = test.__name__[2:] + ("_iothreads" if threaded else "")
            if args.only and args.only not in name:
                continue
            ran += 1
            started = time.monotonic()
            t = Qtest(args.qemu, log, device_args=getattr(test, "device_args", ""), iothreads=threaded)
            try:
                t.setup_pci()
                if test is t_fuzz and threaded:
                    test(t, iterations=500)
                else:
                    test(t)
                status = "PASS"
            except Exception as e:  # report and continue with the next test
                status = f"FAIL {type(e).__name__}: {e}"
                failures += 1
            try:
                t.close()
            except Exception as e:
                status = f"FAIL {type(e).__name__}: {e}"
                failures += 1
            line = f"{status} {name} ({time.monotonic() - started:.1f}s)"
            print(line, flush=True)
            log.write(line + "\n")
        summary = f"KDC_QTEST {'PASS' if not failures else 'FAIL'} tests={ran} failures={failures}"
        print(summary)
        log.write(summary + "\n")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
