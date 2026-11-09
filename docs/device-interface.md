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
| Properties | `queues` (1..16, default 4); `hw-crc` (default on, see Execution model); runtime QOM knobs and read-only counters below |

## Global registers

| Offset | Name | Access | Meaning |
|---|---|---|---|
| 0x000 | ID | RO | `0x6b646331` |
| 0x004 | VERSION | RO | major << 16 \| minor (1.0) |
| 0x008 | CAP | RO | [7:0] queues, [11:8] log2 max ring entries (12), [31:16] SG entries per list (128) |
| 0x00c | MAX_XFER | RO | 1 MiB |
| 0x010 | FEATURES | RO | CRC32C, ABORT, COALESCE, FAULTS, FLR |
| 0x020 | CTRL | RW | bit 0 ENABLE; 1 -> 0 drops all queues and in-flight work without completions |
| 0x024 | STATUS | RO | bit 0 READY, bit 1 FATAL |
| 0x028 | RESET | WO | write `0x52534554` for a controller reset (same effect as FLR) |
| 0x030 | ERR_STATUS | RW1C | DOORBELL, RING_DMA, QUEUE_CONFIG, FATAL |
| 0x034 | ERR_INFO | RO | queue of the last error |
| 0x038 | ERR_VECTOR | RW | MSI-X vector for errors |
| 0x040 | SERVICE_NS | RW | service time per command; also QOM `service-ns`; survives reset |
| 0x044 | FAULT | RW | bit 0 stall, bit 1 enter fatal, bit 2 reset-hang, bit 3 stuck; QOM `fault-stall`, `fault-fatal`, `fault-reset-hang`, `fault-stuck` |
| 0x048..0x050 | FAULT_DROP, FAULT_BAD_CID, FAULT_BAD_DMA | RW | apply to the next N completions / copies; QOM `fault-drop`, `fault-bad-cid`, `fault-bad-dma` |
| 0x060..0x074 | STAT_DONE, STAT_ABORTED, STAT_ERRORS, STAT_IRQS, STAT_DOORBELLS, STAT_CQ_DOORBELLS | RO | counters, cleared by reset; DONE, IRQS (MSI-X messages sent), DOORBELLS (SQ tail writes) and CQ_DOORBELLS (CQ head writes) are also read-only QOM properties `stat-done`, `stat-irqs`, `stat-doorbells`, `stat-cq-doorbells` |
| 0x078 | SCRATCH | RW | no effect on the device; cleared by reset; also QOM `scratch`. The benchmark uses it to pass a token between host and guest |

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
| 0x20 | COALESCE | [15:0] completions per interrupt, [31:16] max delay in microseconds |
| 0x24 | SQ_TAIL | doorbell; out-of-range values are ignored and flagged |
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
  own BARs gets a data error instead of re-entering the MMIO handlers.
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
- All guest memory access uses `pci_dma_read`/`pci_dma_write`, so an emulated
  IOMMU translates (and can fault) every ring, SG list and data access.
- Migration is not supported (the device registers an unmigratable vmstate).

The qtest suite (`tests/qtest/kdc_qtest.py`) checks each of these statements
with an exact virtual clock, plus 15 malformed-descriptor classes and a seeded
random descriptor fuzz, under the ASan/UBSan QEMU build; the copy test runs
with both CRC implementations.
