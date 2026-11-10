# kdc-dma device interface

`kdc-dma` is a PCIe endpoint emulated in QEMU (`qemu/kdc_dma.c`). The
authoritative definitions are in `include/kdc_hw.h`; this page explains them.
PCI ID 1234:4b44 is a lab-only choice under the QEMU community vendor ID.

## PCI resources

| Resource | Content |
|---|---|
| BAR0 (64-bit, 16 KiB) | registers: global block at 0x000, queue `q` at `0x1000 + q * 0x100` |
| BAR2 (64-bit) | MSI-X table and PBA |
| MSI-X | vector `q` per queue (default), vector `queues` for device errors |
| Capabilities | PCIe endpoint, FLR, MSI-X |
| Properties | `queues` (1..16, default 4); `hw-crc` (default on, see Execution model); `shadow-db` (default on: offer shadow doorbells); `ioeventfd` (default on: shadow queues' doorbells through an ioeventfd); `iothreads` (unset: QEMU's main loop; `io0:io1:...`, IOThread object ids, queue `q` in entry `q mod n`, see Where the queues run); runtime QOM knobs and read-only counters below |

## Global registers

| Offset | Name | Access | Meaning |
|---|---|---|---|
| 0x000 | ID | RO | `0x6b646331` |
| 0x004 | VERSION | RO | major << 16 \| minor (1.0) |
| 0x008 | CAP | RO | [7:0] queues, [11:8] log2 max ring entries (12), [31:16] SG entries per list (128) |
| 0x00c | MAX_XFER | RO | 1 MiB |
| 0x010 | FEATURES | RO | CRC32C, ABORT, COALESCE, FAULTS, FLR, SHADOW_DB (0x20, with `shadow-db=on`) |
| 0x020 | CTRL | RW | bit 0 ENABLE; 1 -> 0 drops all queues and in-flight work without completions |
| 0x024 | STATUS | RO | bit 0 READY, bit 1 FATAL |
| 0x028 | RESET | WO | write `0x52534554` for a controller reset (same effect as FLR) |
| 0x030 | ERR_STATUS | RW1C | DOORBELL, RING_DMA, QUEUE_CONFIG, FATAL |
| 0x034 | ERR_INFO | RO | queue of the last error |
| 0x038 | ERR_VECTOR | RW | MSI-X vector for errors |
| 0x040 | SERVICE_NS | RW | service time per command; also QOM `service-ns`; survives reset |
| 0x044 | FAULT | RW | bit 0 stall, bit 1 enter fatal, bit 2 reset-hang, bit 3 stuck; QOM `fault-stall`, `fault-fatal`, `fault-reset-hang`, `fault-stuck` |
| 0x048..0x050 | FAULT_DROP, FAULT_BAD_CID, FAULT_BAD_DMA | RW | apply to the next N completions / copies; QOM `fault-drop`, `fault-bad-cid`, `fault-bad-dma` |
| 0x054 | FAULT_HOLD | RW | [15:0] N, [23:16] queue + 1 (0: any queue): hold the next N commands fetched on that queue; N counts down; QOM `fault-hold` |
| 0x060..0x074 | STAT_DONE, STAT_ABORTED, STAT_ERRORS, STAT_IRQS, STAT_DOORBELLS, STAT_CQ_DOORBELLS | RO | counters, cleared by reset; DONE, IRQS (MSI-X messages sent), DOORBELLS (SQ tail writes) and CQ_DOORBELLS (CQ head writes) are also read-only QOM properties `stat-done`, `stat-irqs`, `stat-doorbells`, `stat-cq-doorbells` |
| 0x078 | SCRATCH | RW | no effect on the device; cleared by reset; also QOM `scratch`. The benchmark uses it to pass a token between host and guest |
| 0x080/0x084 | DBBUF lo/hi | RW | shadow doorbell page (4 KiB aligned) with bit 0 ENABLE; cleared by reset; queues enabled while it is set use it (see Shadow doorbells); ignored with `shadow-db=off` |

All accesses are 32-bit. The fault knobs exist so that recovery paths can be
tested; the QOM properties make them settable from the host with QMP `qom-set`
while the guest runs, and the registers make them reachable from the guest
(the driver exposes them in debugfs). The counters as QOM properties let the
host read them with `qom-get` without a guest MMIO access, which would itself
be a VM exit inside the measurement (`scripts/run_bench.py`).

| Fault | Effect | Cleared by |
|---|---|---|
| stall | the engine hangs: no completions, aborts not honoured | writing 0, controller reset, FLR |
| fatal | the device stops and raises the error vector (FATAL) | controller reset, FLR |
| reset-hang | CTRL 1 -> 0 and the RESET register are ignored; the device keeps running | writing 0, FLR |
| stuck | CTRL 0 -> 1 never sets READY | writing 0 only (survives every reset) |
| drop / bad-cid / bad-dma | the next N completions are dropped / get a corrupted cid; the next N copies write to an unmapped address | counting down |
| hold | the next N commands fetched (on one queue, or any) stay device-owned and are never serviced; they end by an abort, a reset or the device going away, and keep their slots, so a queue stops fetching once a ring's worth is behind them | counting down; controller reset, FLR |
| `fault-gone` (QOM only) | BAR0 and config-space reads return all-ones, writes are ignored, no DMA, no interrupts: a device that has gone away | QOM only |

## Queue registers (`0x1000 + q * 0x100`)

| Offset | Name | Meaning |
|---|---|---|
| 0x00/0x04 | SQ_BASE lo/hi | submission ring, 4 KiB aligned; latched at enable |
| 0x08/0x0c | CQ_BASE lo/hi | completion ring, 4 KiB aligned; latched at enable |
| 0x10 | SIZE_LOG2 | entries per ring = 1 << value, 1..12 |
| 0x14 | CTRL | bit 0 ENABLE (needs controller READY; bad config sets QUEUE_CONFIG) |
| 0x18 | STATUS | bit 0 ENABLED, bit 1 HALTED (a ring access failed) |
| 0x1c | IRQ_VECTOR | MSI-X vector |
| 0x20 | COALESCE | [15:0] completions per interrupt, [31:16] max delay in microseconds; without coalescing see Execution model |
| 0x24 | SQ_TAIL | doorbell; out-of-range values are ignored and flagged. For a shadow queue only a kick: the value is ignored and the tail read from the DBBUF page |
| 0x28 | CQ_HEAD | doorbell; frees completion slots |
| 0x2c | SQ_HEAD | RO: next entry to fetch |
| 0x30 | ABORT | WO: cid of an in-flight command |
| 0x34 | INFLIGHT | RO: commands the device holds |

## Descriptors

Submission entry, 64 bytes: `opcode, flags, cid, length, src, dst, src_nents,
dst_nents, crc_seed, rsvd[4]`. Opcodes: `NOP` (all other fields zero) and
`COPY_CRC`. With `SRC_SGL`/`DST_SGL` set, `src`/`dst` point to arrays of 16-byte
SG entries `{addr, len, rsvd}`; otherwise they are contiguous buffers.

Completion entry, 16 bytes: `result (CRC32C), bytes, sq_head, sqid, cid,
status`. `status = code << 1 | phase`; the device writes the first 14 bytes, then
the status word, so a driver that sees the new phase can read the rest after a
read barrier. The phase starts at 1 and flips on every wrap.

Status codes: 0 success, 1 invalid opcode, 2 invalid field (reserved bits,
unknown flags, length 0 or over 1 MiB, nents without the SGL flag), 3 invalid SG
list (0 or > 128 entries, zero-length entry, reserved bits, lengths not summing
to `length`, summed in 64 bits), 4 data error (a DMA read or write failed, for
example an IOMMU fault), 5 aborted, 6 internal.

## Execution model

- A doorbell schedules a fetch in a bottom half; the MMIO handler does no DMA.
  Fetch, execution and completion run in bottom halves registered with the
  device's re-entrancy guard, so a descriptor that points DMA at the device's
  own BARs gets a data error instead of re-entering the MMIO handlers (in
  IOThread mode, see below, DMA reaches RAM only, with the same result).
- Every fetched command takes a device slot (one per ring entry). From that
  point the device owns it.
- Each queue has one serial engine: a command starts when it has been fetched
  and the engine is free, and finishes `SERVICE_NS` later. Queues run in
  parallel. All data movement happens when the command finishes: the engine
  reads the SG lists, reads the source, computes CRC32C, writes the
  destination, then posts the completion. The CRC uses the host's SSE4.2
  `crc32` instruction when it has one (`hw-crc=on`, the default); `hw-crc=off`
  uses QEMU's byte-wise `crc32c()` table, which on the build host cost the
  emulator about as much per 4 KiB as the whole driver path around it
  (`results/round3/perf/crc/`). An abort before that point
  completes the command with status 5 and no data movement; aborting a queued
  command costs no engine time.
- If the completion ring is full the device keeps finished commands and posts
  them after the next CQ_HEAD doorbell.
- Interrupts without coalescing (count <= 1 or delay 0): within one run of the
  completion bottom half (a pass), the first completion raises the queue's
  vector at once and the rest of the pass is announced by one more message
  after its last completion. A handler that reaps everything posted when it
  runs sees every completion, and a burst costs two messages instead of one
  per completion. With coalescing, the count or the delay decides, as before.
- Shadow doorbells: entry `q` of the DBBUF page is `{sq_tail, sq_event,
  rsvd[2]}` (`struct kdc_hw_dbbuf`). A kick makes the device read `sq_tail`
  (an out-of-range value is flagged as a doorbell error and ignored). Whenever
  it has fetched every entry up to the tail it has seen, it writes that tail to
  `sq_event`, issues a full barrier and reads `sq_tail` again, fetching on if it
  moved. A driver that orders its `sq_tail` store before its `sq_event` load
  with a full barrier may therefore skip the kick unless `sq_event` lies in
  [old tail, new tail): an older index means the device has not finished the
  pass it was kicked for and will see the new tail at its end. With
  `ioeventfd=on` the device registers an ioeventfd (any write size) on the
  SQ_TAIL address of each shadow queue while the queue is enabled: under KVM
  the guest's write is handled in the host kernel and wakes QEMU's main loop
  through the eventfd, without an exit to QEMU. Without KVM the write reaches
  the MMIO handler, which does the same. The completion side does not change.
- All guest memory access uses `pci_dma_read`/`pci_dma_write`, so an emulated
  IOMMU translates (and can fault) every ring, SG list and data access.
- Migration is not supported (the device registers an unmigratable vmstate).

## Where the queues run

Without `iothreads` the whole device model runs in QEMU's main loop under the BQL. With
it, each queue's bottom halves, timers and doorbell eventfd run in its IOThread, without
the BQL:

- each queue has a mutex for its rings, slots and counters; changes that concern the
  whole device (CTRL, RESET, FLR, unplug, the fault knobs, SERVICE_NS, DBBUF, the QOM
  properties) take every queue's mutex, so a processing pass is never cut short; the
  error registers and the one-shot fault counters, which queues write, have a device
  mutex taken last;
- BAR0 is lockless: SQ_TAIL, CQ_HEAD and ABORT take only their queue's mutex; CTRL,
  RESET and a queue's CTRL take the BQL first, because they add or remove ioeventfds;
- the device's DMA carries `MemTxAttrs.memory`, so it reaches RAM only: a descriptor
  aimed at any device's registers ends with a data error (status 4), where the main-loop
  mode would reach other devices' registers;
- interrupts: with KVM through an irqfd per vector (MSI-X vector notifiers re-route a
  vector when the guest changes it and detach it while masked; a masked vector's
  message waits in the eventfd and sets the PBA bit when the guest polls it), without
  KVM through a bottom half in the main loop;
- without KVM, an emulated IOMMU would raise its fault interrupt from the IOThread
  without the BQL, which TCG's interrupt code asserts against, so `iothreads` behind an
  emulated IOMMU needs KVM; realize refuses the combination.

## Tests

The qtest suite (`tests/qtest/kdc_qtest.py`, 21 tests, each run twice: in the main
loop with an exact virtual clock, and with the queues in two IOThreads, where `settle()`
also waits 30 ms of real time) checks each of these statements, plus 15
malformed-descriptor classes and a seeded random descriptor fuzz, under the ASan/UBSan
QEMU build; the copy test runs with both CRC implementations, `shadow_db_off` checks
that the device without the feature ignores DBBUF, and `fault_hold` the hold knob.
