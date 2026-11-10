# Design decisions and the alternatives not taken

Each entry says what was chosen, what else was considered, and the measurement
or argument that decided it. Run results are indexed in `results/README.md`.

## Device model

**An emulated PCIe device in QEMU, not a kernel-only mock.** The driver talks
to config space, BARs, MSI-X and an IOMMU exactly as it would to hardware, so
probe, FLR, hot-unplug and DMA faults go through the real kernel paths
(`pci_reset_function`, pciehp, intel-iommu, DMA-API debug). A mock behind a
function-pointer table would have tested the ownership logic but none of that.
The cost is a QEMU build (about 5 minutes per flavour) and that nothing here
says anything about real silicon.

**Every guest-memory access goes through `pci_dma_read/write`.** That makes the
emulated IOMMU translate (and fault) every ring, SG-list and data access, and
it makes bus mastering meaningful: with the command register's bus-master bit
clear, QEMU drops the device's DMA. The driver's "bus mastering off means the
device can no longer write" rule depends on this.

**All data movement happens when a command's service time ends.** A real
engine streams; this one reads the whole source and writes the whole
destination at once. An abort therefore never leaves a half-written
destination. That made the abort contract simple to state and test, at the
price of not exercising partial writes (listed as a stretch goal).

**CRC32C with the SSE4.2 instruction (`hw-crc`, default on).** The first KVM
pilot showed QEMU's `crc32c()`, a byte-at-a-time table, dominating the device
model: one 64 KiB command cost the main loop 104.4 us with the table and
11.5 us with the instruction, and 64 KiB copies ran at 9.0k vs 58.3k per
second with one in flight (`results/round3/perf/crc/summary.md`). The table
made the emulator, not the driver, the bottleneck above a few KiB, which would
have hidden the zero-copy/bounce difference. `hw-crc=off` keeps the table for
comparison, and the qtest copy test runs with both.

**Fault knobs are device registers and QOM properties.** Registers let the
in-guest tests drive them through debugfs; QOM properties let the host change
them while the guest runs. Two knobs are deliberately host-only or sticky:
`fault-gone` (BAR and config reads return all-ones, no DMA, no interrupts),
because a device that has gone away cannot be told to come back by the guest;
and `STUCK` (never becomes ready), which survives every reset so that the
driver's last recovery step can be reached.

## Ownership contract

Commands move STAGED -> PENDING -> DEVICE -> COMPLETING -> DONE. Until the
doorbell the driver owns the buffers and cancel releases them at once; from the
doorbell on the device owns them and cancel can only send an abort. A command
id is a 12-bit tag plus a 4-bit generation, so a stale or corrupted completion
cannot complete the tag's next owner. `ctx->lock` is a spinlock rather than a
mutex because `close()` frees the context right after it sees the last
completion, and `spin_unlock()` does not touch the lock after releasing it.

## Timeouts

**A per-command timeout in a former reserved field, with a module default.**
`kdc_submit.timeout_ms` (0 = `cmd_timeout_ms`, 5000 by default) was reserved
and required to be zero, so callers that predate it keep their behaviour. The
alternative, a module-wide value only, cannot express "this command is
expected to be slow". The ABI version went to 3 anyway because `kdc_stats`
grew.

**Measured from the doorbell, checked by a watchdog every 100 ms.** Time spent
waiting for ring space in the driver is not the device's fault and does not
count. The watchdog is a delayed work item that runs only while the device
owns commands and scans the slot tables under the queue locks; the hot path
only stores a deadline. Per-command hrtimers were the alternative: exact, but
an arm and a cancel on every command, plus a timer callback racing the
completion for the same command. The watchdog's cost is precision: a timeout
fires up to 100 ms late, which is fine for timeouts in the hundreds of
milliseconds and above.

**Every abort has a deadline, whoever sent it.** Cancel, `KDC_IOC_RESET`,
`close()` and the timeout itself all send device aborts. An abort left
unanswered for `abort_timeout_ms` (500) escalates to a reset. This is what
bounds `close()` and `KDC_IOC_RESET`: they still wait for every device-owned
command, but those commands now always end, within the abort deadline plus the
reset (see `bounded_close_and_reset` in the results).

## Recovery

**Abort, then controller reset, then FLR, then give up.** The device defines
"controller disable acknowledged" (READY clears) as "no more DMA", which makes
the cheap reset sufficient in the common case. If the controller ignores its
reset, `pci_try_reset_function()` performs an FLR. If the device does not come
back after that, the driver clears bus mastering, ends every command and
refuses new ones (`-EIO`) until a later reset revives it.

**Buffers are released only when the device can no longer DMA into them:**
after its completion, after a controller disable has been acknowledged, after
an FLR, or after bus mastering is off. Releasing a timed-out command as soon as
its abort deadline passes would be simpler and is wrong: while the controller
reset is still being attempted, the device may complete the copy into pages
that have been unpinned and reused. `timeout_escalation` holds the device in
that window (stalled, controller reset ignored) and checks that the command's
pages are still pinned and mapped; the `release-before-reset` mutant fails it.

**`pci_try_reset_function()`, not `pci_reset_function()`.** `remove()` runs
under the device lock and waits for the recovery work to finish; a recovery
work item blocked on that lock would deadlock. The try variant fails with
`-EAGAIN` instead, and the recovery retries a bounded number of times unless
the device is being removed.

**`reset_prepare`/`reset_done` carry the whole reset, for every caller.** An
FLR started by recovery, a reset written to sysfs and any other user of
`pci_reset_function()` all go through the same two callbacks: quiesce the
queues, let the PCI core reset the function, then check that the device has
stopped, take back every command, re-initialise the rings and the MSI-X vector
registers (the PCI core restores the MSI-X table itself) and restart.
`reset_lock` is held from one callback to the other; the PCI core calls both in
the same task under the device lock.

**Commands caught in a reset are reissued, at most twice.** A copy is
idempotent, so a command that was merely in flight when another command timed
out, or when the device went fatal, is put back on its queue and runs again;
the caller sees success. Commands that somebody asked to abort end with that
reason (`-ETIMEDOUT`, `-ECANCELED`, `-ECONNRESET`). The cap of two reissues
keeps a command that itself brings the device down from looping; only resets
the driver starts on its own count toward it. The first version counted every
reset, and `sysfs_reset_under_load` caught it: with three resets from sysfs
in quick succession, a command caught by all three ended with `-EIO` although
nothing was wrong with it. The
alternative, failing every in-flight command with `-EIO`, is simpler but pushes
a retry loop into every caller for an event that is not their fault (the same
reasoning as blk-mq requeueing after an NVMe controller reset).

**A failed device stays bound.** NVMe removes a controller it cannot recover;
this driver keeps the device bound in a failed state, with bus mastering off,
so that open files get `-EIO` rather than disappearing and a later sysfs reset
(once the fault is gone) brings it back without rebinding. `recovery_failure`
tests both halves.

**A device that reads all-ones and has left config space is treated as
removed.** The watchdog reads the status register on every tick while the
device owns commands (the same check NVMe makes on CSTS), so a vanished device
is noticed within 100 ms instead of after each command's timeout. Such a device
is not reset: an FLR would make the PCI core poll config space for up to 60 s.
Every command ends with `-ENODEV` at once, new submits fail with `-ENODEV`, and
`remove()` later skips all hardware access.

## Destination overlap

DMA-API debug flagged overlapping destination mappings in the first version. The rule
adopted: no user input may cause a kernel warning, and device-writable memory
must not share a cache line with another live device-writable mapping (on
non-coherent platforms that corrupts data; DMA-API debug checks a part of it).

Options considered:

| Option | Why not (or why) |
|---|---|
| Reject overlap only inside one command | Does not cover two commands, two files or two devices writing the same page |
| Bounce every destination | Correct, but gives up zero-copy for large transfers |
| Bounce only the partial first and last cache lines | Makes adjacent destinations safe, but two commands writing the same whole lines would still overlap |
| `-EBUSY` on any overlap with an in-flight destination | Clear rule, but `EBUSY` already means "file reset in progress" |
| **Chosen:** refuse byte overlap with `EADDRINUSE`, bounce the partial lines | Both together keep every streaming device-writable mapping on whole cache lines that no other command and no CPU copy-back touches |

Details:

- Destinations are compared by **physical** address, module-wide (all files,
  all kdc devices), in an interval tree of physically contiguous runs. Virtual
  addresses would miss two mappings of one shared page; `dst_overlap` checks
  that case, and a destination that maps one physical page twice.
- The check runs after pinning and before any DMA mapping exists, because
  DMA-API debug checks at map time.
- The zero-copy path maps only whole cache lines inside the destination; the
  head and tail (under 64 bytes each) are written by the device into the
  command's coherent SG-list block and copied into place after completion.
  The bounce path has no streaming destination mapping at all.
- Cost: a global spinlock and a few tree operations per command. Measured on
  KVM with the tracker switched off at boot (`track_overlap=0`, a
  measurement-only parameter), interleaved boots
  (`results/round3/perf/tracker/summary.md`): no difference for 4 KiB
  commands (one in flight 103.7k vs 104.4k per second, 32 batched 545.6k vs
  545.3k, three threads 196.7k vs 199.7k), and 5-7% for the smallest batched
  commands (512 bytes, 32 in flight: 713.6k vs 751.2k; three threads: 747.6k
  vs 802.5k), where the run-to-run ranges overlap. A per-device or
  per-page-hash tree would shrink the lock's reach, but it is not what limits
  this driver (see scaling below).

## Checking

- **Two kernels**, because KCSAN and KASAN exclude each other: KASAN + kmemleak
  + lockdep + DMA-API debug + fault injection, and KCSAN + lockdep + DMA-API
  debug + fault injection. A third, `perf`, has none of them and is used only
  for timing; the build fails if a debug option survives in it.
- **KVM and TCG.** The guest suite runs under both accelerators. KVM's speed
  exposed three test races that TCG had hidden (a completion read before the
  reset that produced it had restarted the controller, and an abort sent
  before the device fetched the command); the tests now wait for the state
  they check.
- **kmemleak** runs after the suite (two scans, six seconds apart, because
  objects younger than five seconds are never reported); a leaked object with a
  `kdc_` frame fails the run, other records are counted.
- **Fault injection through `fail-nth`**, not a probability: the test fails the
  first, second, third, ... allocation of one submit (and one probe) until a
  run completes without an injected failure, so every allocation on those paths
  fails exactly once, deterministically. One exclusion: allocations under the
  IRQ core's `register_irq_proc()`. The core tolerates a failure there at
  `request_irq()` and then WARNs in `remove_proc_entry()` when the IRQ is freed,
  which is a 6.12.41 limitation outside this driver; the stacktrace filter of
  the fault-injection framework skips that function and nothing else.
- **kmemleak has a positive control**: the `read-leaks-command` deliberate bug
  must produce a kmemleak record naming `kdc_cmd_prepare`, so "no records" in
  the normal runs means something.
- **Hot-unplug through QEMU's default ACPI PCI hotplug, not pciehp.** With
  native PCIe hotplug, Linux 6.12.41's `pciehp_unconfigure_device()` re-takes
  `ctrl->reset_lock` under `pci_rescan_remove_lock`; lockdep reports a possible
  circular dependency on every removal and then turns itself off for the rest
  of the boot, which would leave the surprise-removal test without lockdep.
  ACPI hotplug is also what QEMU's q35 machine and most VMs use.
- **Expected reports.** Tests that provoke a report bracket it with markers
  written to `/dev/kmsg`; the console prints kernel-log records whole and in
  order, so "inside the window" is exact. The harness allows only matching
  reports inside a window and requires at least one; anything else fails. The
  client also checks the report itself through `/dev/kmsg`.
- **Host requests.** Hot-unplug and the vanished device need the host; the
  guest logs `KDC_HOST <request>` and the harness performs it over QMP.

## Performance on KVM (round 3 study of the published code)

The study below measured `main` and set the defaults it ships with; the
optimizations that followed, and their measurements, are in the next section.

**What the numbers measure.** The device is emulated, so these are the guest
driver plus KVM exits plus QEMU's device model around a modeled service time,
not device throughput. One host (Intel i9-13900K, Linux 7.0 host, KVM with
APICv, QEMU 10.2.1 release build). Guest: the `perf` kernel (Linux 6.12.41
without KASAN, lockdep, DMA-API debug or kmemleak; a normal KVM paravirt
guest), 3 vCPUs pinned to host CPUs 2, 4 and 6, QEMU's main loop (which runs
the device model) and its helper threads on CPU 8, one `kdc-dma` device with
one queue per vCPU, service time 0 unless an arm says otherwise. Every arm
forces its data path (`kdc.bounce_max=65536` at boot so both exist at every
size). A window is 1 s after a 150 ms warm-up; the 53 arms of the main run are
interleaved, 7 repetitions per boot, 2 boots; tables give medians and (min-max)
over accepted windows. Numbers from the driver code of `7b76676`; the `perf`
kernel is described by `scripts/build_kernel.py` and `configs/perf.fragment`.

**The shared host.** Other projects kept 0.5-1.5 CPUs busy on CPUs 2-9, mostly
one unpinned job that the scheduler moved onto idle SMT siblings. Before the
published runs, a pilot fixed two rejection rules: a window is run again when
more than 1500 ms per second of CPU time on CPUs 2-9 belongs to no QEMU thread,
or when QEMU's threads waited for a CPU more than 25 ms per second. That
rejected 447 of 1128 windows in the main run; every arm kept at least 11 of 14
repetitions. Accepted windows ran at 5.0-5.5 GHz on the vCPU CPUs. QEMU ran at
nice 10, as the shared-host rules require. All figures below are in
`results/round3/perf/<run>/summary.md` (main unless named).

**Where one command's time goes.** With one 4 KiB zero-copy command in flight
the round trip takes 9.2 us (median; 104.7k commands per second); a NOP takes
8.4 us, so the data path adds under 1 us at this size. Each command costs 3.65
VM exits: exactly two MMIO exits, which the device counters identify as the
submission doorbell and the completion-queue head write, one HLT while the
vCPU waits, and 0.65 others. The MSI-X message itself causes no exit (KVM's
`irq_injections` stays 0: posted interrupts). The vCPU thread is busy the whole
time (9.58 us of host CPU per command); the main loop spends 5.06 us per
command. Comparing the two 32-deep runs below, each doorbell exit costs about
3 us of vCPU time ((6.97 - 1.83) us / (1.81 - 0.11) exits).

**Queue depth alone does not help; doorbell batching does.** With one doorbell
per command, 2 to 32 commands in flight all give 128k-136k per second: every
submit is still an MMIO exit to QEMU, and the vCPU thread, at about 7 us per
command, is the bottleneck. Resubmitting everything one `read()` returned with
`KDC_SUBMIT_BATCH` and a single `KDC_IOC_FLUSH` gives 277k, 390k, 489k and
539k per second at depths 4, 8, 16 and 32 (4.0x at 32), cuts exits to 0.11 per
command, and halves the 32-deep latency (51.8 vs 118.4 us). Completion-queue
head writes drop to 0.03 per command as well, because completions come back in
bursts. Decision: no driver change. The flush is the ABI's explicit plug, the
way blk-mq uses the block layer's plug; deferring the doorbell inside the
driver would need a timer and would delay every lone command.

**Interrupt coalescing stays off by default.** At depth 32 with one doorbell
per command, coalescing at 4 completions or 10 us raises throughput from 136.0k
to 174.4k, and at 16 or 50 us to 238.7k (0.07 interrupts per command). With
batching it adds 14% and 22% (539.2k to 614.9k and 656.0k). But a lone command
then waits for the delay: one 4 KiB command at a time drops from 104.7k to 45.3k
per second, 21.7 us instead of 9.2 us. The driver cannot know the caller's
depth, and batching gains more at depth without that cost, so the default is
one interrupt per completion; `coalesce_count`/`coalesce_usecs` and the
debugfs `coalesce` file turn it on. An adaptive scheme (coalesce only while
the queue is deep, as NAPI does) is the obvious next step (the performance round
added its delay-free part: two interrupts per processing pass). A polled completion
path was not added: it needs a new ioctl, and the cost per command sits in the
two doorbell exits, which polling does not remove.

**`bounce_max` follows the DMA path: 2048 bytes direct, 65536 behind an
IOMMU.** Without an IOMMU, one command at a time, zero-copy vs bounce medians
are 9.0/8.8 us at 512 bytes, 8.9/8.8 at 1 KiB, 9.1/9.2 at 2 KiB, 9.2/9.7 at
4 KiB, 9.7/10.9 at 8 KiB, 11.0/12.9 at 16 KiB, 13.0/17.0 at 32 KiB and
17.2/24.5 at 64 KiB. With 8 in flight and batching, bounce is 5% faster at
1 KiB (450.7k vs 429.5k per second) and zero-copy 14% faster at 4 KiB (390.2k vs
341.5k). The old default, 4096, bounced 4 KiB commands at that loss. Behind
QEMU's emulated Intel IOMMU with strict invalidation
(`results/round3/perf/iommu/summary.md`) the picture reverses: each zero-copy
command makes two more MMIO exits that are not the device's (its counters
still show one doorbell and one head write), consistent with one strict IOTLB
invalidation per unmapped buffer, and 4 KiB zero-copy falls to 49.8k per second (19.7 us) while bounce
stays at 98.6k (9.9 us); at 64 KiB bounce still wins (37.6k vs 26.4k), and with
32 batched 4 KiB commands it is 452.7k vs 87.2k. The bounce path maps nothing
per command (its buffers come from a coherent pool), so it pays no
invalidations. Lazy invalidation narrows the gap (4 KiB: 97.6k vs 86.3k; 64 KiB:
37.5k vs 36.4k, a tie). The driver therefore picks 2048 when the device's DMA
is direct and 65536 when an IOMMU domain translates it; an explicit
`bounce_max` still wins. This is a measurement of an emulated IOMMU, whose
invalidations are VM exits; on bare metal the crossover would sit elsewhere.

**Scaling: 1.5x with three threads, limited outside the guest's locks.** One
thread per vCPU, each with its own file and its CPU's queue, 8 in flight: NOP
commands reach 162.3k, 225.7k and 245.8k per second with 1, 2 and 3 threads
(1.51x); 4 KiB commands 128.3k, 185.2k and 198.4k (1.55x); batched 4 KiB
390.2k with one thread and 602.0k with three. Neither side is saturated at
three threads: the vCPUs are about 65% busy (7.88 us per NOP command across
three vCPUs at 245.8k per second) and the main loop 69%. That pattern fits
QEMU running the device model and every doorbell exit under its global lock;
without host profiling (perf is not available to the build user) the split
cannot be shown. On the guest side: all three threads on one queue costs
little (NOP 226.4k, 4 KiB 191.6k); all three on one open file costs 25% for
NOPs (184.8k) and 15% for 4 KiB (167.9k), and the vCPU time per command rises
1.6-1.8x, consistent with contention on the file lock and with every
completion waking all three readers on the file's wait queue. The overlap tracker's lock costs at
most 7% (above). `gone_lock` is taken shared by every ioctl and cannot be
switched off safely, so it was not measured separately. When the modeled
device is the bottleneck (10 us per command), one queue gives exactly 100.0k
per second and three queues 291.0k (2.91x): the driver keeps every queue busy.

**Losses and surprises, plainly.** Queue depth without batching is worth
nothing past 2. Coalescing more than halves single-command throughput. Strict
vIOMMU makes zero-copy 2-6x slower. A shared file loses up to 25%. The
emulator's CRC was the bottleneck until it was replaced. The idle guest makes
about 270 exits per second (timer and housekeeping), which bounds what the
counters can resolve per window.

## Performance round (2026-11-09): optimizing along the command path

**Method.** Each change was frozen as a build (`scripts/snapshot_build.py`: release QEMU,
`perf` kernel, benchmark client) and measured against the published `main` and the
previous kept build inside one quiet window: `run_bench.py --builds main,o1,...` boots
the builds in turn, one 1-second window per arm per boot (`bench/ab.plan`), seven
rounds, QEMU at nice 0. `scripts/bench_compare.py` pairs the n-th boot of one build with
the n-th boot of the other, which ran next to each other in time. The rule, fixed
before the runs it judged: the arms a change targets must win in all but at most one
of at least six pairs against the previous kept build, and no arm may end with a
median below the lowest value the previous build had in any pair. Figures are medians
over the seven pairs. Change 1 was decided in its own session (`results/perf/o1-vs-main`),
changes 2 to 4 in one session of all frozen builds (`results/perf/chain`), change 5 in
`results/perf/final` and `results/perf/fixed`. What they measure is unchanged: the guest driver, KVM exits and
QEMU's device model around a modeled service time of 0, on one host; not device
throughput. Raw data and the comparison tables: `results/perf/`.

At the start, one 4 KiB command cost 3.65 VM exits: two MMIO exits to QEMU (the SQ
doorbell and the CQ head write, about 3 us of vCPU time each), one HLT, and the rest;
with 32 in flight and a doorbell per command the vCPU, at about 7 us per command, was
the limit (133k/s), and every completion sent its own MSI-X message.

### 1. CQ head written every 16 completions (driver): kept

The device reads the CQ head only to see free completion entries, so the driver now
writes it once `cq_head_batch` entries (a quarter of the ring, 16) have been reaped
since the last write, instead of after every reap. Progress argument: the device stalls
only when every entry it may post is in use from its point of view; all of those were
posted with an interrupt, and reaping them brings the unreported count to depth - 1,
which is at least the batch, so the head is written. `cq_head_batching` checks the
device's own count (exactly 16 head writes for 256 single commands) and runs 256
commands with 63 in flight and the largest batch (63), where the device often finds the
ring full; the `cq-head-never-written` mutant fails it.

`results/perf/o1-vs-main`: MMIO exits per command at one in flight 2.00 -> 1.06; NOP
112.1k -> 156.0k/s, 4 KiB 102.2k -> 139.8k/s (9.5 -> 6.7 us), 64 KiB 54.6k -> 70.1k/s,
4 in flight 128.5k -> 203.7k/s, 32 in flight 129.8k -> 205.4k/s, three threads 197.6k
-> 297.2k/s: every targeted arm won all 7 pairs. Batched arms did not change (they
wrote the head 0.03 times per command already); 32 batched was 550.0k vs 542.9k/s, 2 of
6 pairs, within the spread.

### 2. Two interrupts per processing pass (device model): kept

Without coalescing the device sent one MSI-X message per completion, each a
`KVM_SIGNAL_MSI` in QEMU's main loop and an interrupt in the guest. Now, within one
run of the completion bottom half, the first completion interrupts at once and the
rest of the pass is announced by one message after its last completion: the first
completion is not delayed, and a handler that reaps everything posted when it runs
sees every completion. This is the coalescing that costs no latency: it only merges
completions that are already there (the delay-based coalescing stays available).
qtest `pass_interrupts` checks 1 + 2 messages for a lone command and a pass of 8;
the `pass-end-irq-skipped` mutant fails it.

`results/perf/chain` (o2 vs o1): MSI-X per command with 32 batched 1.00 -> 0.06 and
QEMU main-loop time 1.03 -> 0.71 us per command; batched 8 in flight 421.8k ->
470.1k/s, 32 in flight 559.8k -> 695.2k/s, three threads batched 701.0k -> 858.0k/s,
all 7 pairs each. Arms with one completion per pass did not change. Loss, within the
rule: three threads on one file with 4 KiB copies 210.9k -> 204.6k/s, 1 of 7 pairs,
above the base minimum (198.8k).

### 3. Shadow doorbells, an event index and an ioeventfd (device + driver): kept

The remaining MMIO exit per command was the SQ doorbell: an exit to QEMU userspace,
the global lock, the device's MMIO handler. An ioeventfd lets KVM handle the write in
the host kernel and wake QEMU's main loop through an eventfd, but it drops the written
value, so the tail has to live in memory: the device offers a shadow doorbell page
(`DBBUF`), as NVMe controllers do for virtualized hosts. The driver writes the new tail
there (`wmb()` after the ring entries), issues `mb()`, reads the device's event index and
rings only if the index lies in [old tail, new tail). The device, whenever it has fetched
everything up to the tail it has seen, writes that tail as the event index, issues
`smp_mb()` and reads the shadow tail again; if the driver read an older index it skipped
its doorbell, but its tail store came before that read, so the device's second read sees
it. A busy device therefore needs no doorbell. KUnit `shadow_event_index` covers the
index arithmetic, qtest `shadow_doorbell` the device side (shadow tail used, doorbell
value ignored, index published, bad tail flagged, reset clears DBBUF), the client case
`shadow_doorbell` that an idle device is always rung; mutants `shadow-ring-only-idle`
(driver rings only when nothing is outstanding) and `event-index-not-published`
(device) fail them.

`results/perf/chain` (o3 vs o2): MMIO exits per command at one in flight 1.06 -> 0.06
(the doorbell exit is now a fast in-kernel MMIO exit); NOP 158.7k -> 178.1k/s, 4 KiB
141.1k -> 151.5k/s (6.7 -> 6.1 us); with a doorbell per command the event index does
the rest: at 32 in flight only 0.28 doorbells per command reach the device and
208.2k -> 583.0k/s (2.8x), 4 in flight 206.5k -> 401.5k/s, three threads 301.5k ->
958.0k/s (3.2x), three threads on one file 2.1-2.4x. Batched arms unchanged. Cost: QEMU's
main loop spends more per command at one in flight (2.80 -> 3.49 us: the eventfd wakeup
and the shadow page reads); the vCPU spends less (6.33 -> 5.64 us).
`results/perf/decompose` separates the two parts on the final build, with the
device's `ioeventfd=off` (shadow page and event index kept, every doorbell an exit
to QEMU) and the driver's `shadow_doorbells=0` (an MMIO doorbell per dispatch, as
before): 32 in flight 204.9k (no shadow) -> 226.8k (event index, 0.84 doorbells per
command) -> 575.6k/s (ioeventfd, 0.29); three threads 292.5k -> 451.2k -> 949.6k/s;
NOP one at a time 155.3k -> 155.4k -> 177.9k/s. Most of the gain is the ioeventfd;
the event index matters more once doorbells are cheap, because the guest then
submits faster than the device fetches and finds it busy more often.

### 4. One reader woken per completion (driver): rejected

Readers of a shared file slept non-exclusively, so each completion woke all of them.
With exclusive waits (`wait_event_interruptible_exclusive`, `wake_up()`), plus passing
the wakeup on when a reader puts a completion back, one reader wakes per completion. A
client case checked it (two blocked readers' voluntary context switches unchanged after
a completion woke the third; it passed on KVM, its deliberate bug was not run before
the measurement rejected the change). But once change 3 had removed the
doorbell exits, it did not pay (`results/perf/chain`, o4 vs o3): one file shared by three
threads 526.8k vs 518.7k/s for NOPs (3 of 7 pairs) and 482.5k vs 488.6k/s for 4 KiB
(4 of 7). Reverted. What a shared file still loses (4 KiB at three threads: 482.5k on
one file vs 958.0k/s on three, o3) is therefore not in the wakeups; the file lock and
its single completion list are the remaining shared state.

### 5. Registered buffers (driver, ABI): kept, although they miss the rule on the other paths

Behind QEMU's emulated Intel IOMMU in strict mode every zero-copy command maps and
unmaps its pages, and each unmap is an IOTLB invalidation, i.e. more VM exits; that is
why `bounce_max` is 65536 there. A registered buffer is pinned (`FOLL_LONGTERM`) and
mapped once, page by page for `DMA_BIDIRECTIONAL` (`KDC_IOC_REGISTER`); a
`KDC_SUBMIT_FIXED` command whose source and destination lie in the file's buffers
takes a reference on each, builds its SG lists from the per-page DMA addresses, syncs
its ranges (a no-op on x86) and keeps the head/tail split. The ownership contract does
not change: the references are dropped in `kdc_cmd_release()`, under the same release
rule as every other command; `KDC_IOC_UNREGISTER` fails with `EBUSY` while one is held;
`close()` frees the buffers after its last command; `remove()` unmaps every open file's
buffers once all commands have ended (they stay pinned until the file closes). A
registered buffer's pages enter the module-wide overlap tracker for as long as it
exists, so no other destination - zero-copy, bounce, another file's buffer - can share
them; fixed destinations are checked against each other in the buffer's own tree.
Per-page single mappings rather than one mapped scatter list, because the per-command
syncs then use `dma_sync_single_range_for_*` the way the DMA API defines it. Client
cases: `fixed_buffers` (ABI, copies at every size and alignment with guard bytes,
`EBUSY` while a stalled engine holds a command, the overlap rule both ways, pages and
mappings back to zero after unregister and after close), and the hot-unplug cases with a
registered buffer and a staged fixed command open across the removal (DMA-API debug would
report a mapping left behind at unbind). Mutant `unregister-in-use` (unregister ignores
the references) fails `fixed_buffers`.

`results/perf/fixed`, final build (o5) and `main`, 7 pairs each. Strict IOMMU: 4 KiB one
at a time 148.0k/s fixed vs 131.4k bounce and 70.2k zero-copy in the same build (main's
best, bounce: 98.1k); 64 KiB 67.3k vs 41.6k and 28.2k; 32 batched 4 KiB 660.5k vs 546.2k
and 104.3k (6 of 7 pairs against bounce); 8 batched 64 KiB 103.2k vs 64.5k and 37.1k.
Without an IOMMU fixed is the fastest path too: 4 KiB 159.3k vs 151.6k zero-copy, 32
batched 811.4k vs 685.1k, 64 KiB 80.4k vs 73.3k.

The miss: on the arms that do not use it, the final build against o3
(`results/perf/final`) has median ratios between 0.984 and 1.021 (median 0.996), and
32 in flight with a doorbell each ended at 577.8k/s, just below o3's lowest pair
(579.2k; median 586.5k, 1 win of 7): the rule's second condition fails there by 0.2%.
In the earlier session with the same code (`results/perf/chain`, o6 vs o4) that arm
passed and the delayed-coalescing arm missed by a similar margin instead.

The independent check (`results/perf/check-c/`, fresh clones, four more sessions) shows
the cost is larger than that session suggested and is real. At 32 in flight with a
doorbell each, the final build against o3 had median ratios of 0.991, 0.963, 0.985,
0.981 and 0.983 in the five sessions, with 1, 0, 2, 3 and 1 of 7 pairs faster: a cost
of 1-4%, about 2% typically (`vCPU us` per command 1.60 -> 1.65 and 1.58 -> 1.63 in two
of the sessions, unchanged in a third). Batched
at 32 and the three-thread arms lose up to about 2% in some sessions and tie in others;
one command at a time shows nothing (0.990 to 1.013). Where it is:

- **Not QEMU.** The two QEMU binaries come from the same source but differ by 208 bytes
  (their build directories). The final kernel and client on o3's QEMU still lose (0.982,
  1 of 7); o3's kernel and client on the final QEMU do not (1.003, 6 of 7) (`check-c/swap-qemu`).
- **Not the benchmark client.** o3's kernel with the final client beats the final build
  in 6 of 7 pairs on five of six arms; the final kernel with o3's client still loses
  (`check-c/swap-client`).
- **Not the struct layout, as far as tested.** The new fields had moved `sqe` off its
  cache line in `kdc_cmd` and pushed `kdc_ctx`'s counters onto a fourth line (pahole).
  Moving every new field to the end of both structs (`check-c/reorder/reorder.diff`)
  restored the old offsets but not the speed: 0.990 against o3, 0 of 7 pairs, and a
  tie with the final build. Not applied.

So the cost is in the guest kernel: either the added work in submit and release
(arguments, branches and stores per command) or the kernel image's code layout after
350 added lines; with no host profiler the two are not separated. Registered buffers stay
on the branch because the opt-in path is 1.11x (4 KiB) to 1.61x (64 KiB) faster than
bounce behind the strict IOMMU and 1.14x to 1.74x without one, in the same build
(`check-c/fixed`); dropping their commit restores o3 exactly.

### What limits it now, and what was not done

- **QEMU's main loop.** It runs the whole device model in one thread. After change 3,
  three threads with 8 in flight keep it 92% busy (0.98 us per command at 952k/s)
  while the three vCPUs run 86% of the time (2.72 us per command, halt polling
  included), so the next step is
  running queues outside the main loop: an IOThread per queue with irqfd interrupt
  injection, as virtio dataplane does, plus a lock for the device state the MMIO
  handlers share with it. Not done: it needs a host CPU per thread beyond the four
  P-core threads this setup measures on, and the comparison would then also be one of
  CPU budgets. The global lock itself is no longer in the doorbell path (no exit to
  QEMU), which was the point of an IOThread for the one-queue case.
- **Delay-based coalescing** still adds a third at depth without batching (32 in
  flight with 16 completions or 50 us: 765.8k vs 577.8k/s, `results/perf/final`), but
  a lone command still waits for the delay, so it stays off by default. Change 2 is
  the delay-free part of adaptive coalescing; a driver that turns the delay on only
  while a queue is deep (the remaining NAPI-like option) was not built.
- **Overlap tracker.** Measured again on the final build with the tracker off at boot
  (`results/perf/tracker`, 15 windows per arm, not paired): 512-byte batched commands
  1022.3k vs 1073.4k/s (5%), three threads 1232.7k vs 1303.4k/s (5%), 4 KiB three
  threads 955.3k vs 960.5k/s. The tracker's cost per command is unchanged, its share
  grew as the rest shrank, and it does not limit scaling; a sharded tree was not built.
  Registered buffers got their own per-buffer tree for fixed destinations.
- **The CQ head in the shadow page** would remove the last 0.06 MMIO exits per command
  (one per 16 completions), at most about 0.2 us per command; not done.
- **Polling in the device** (keep reading the shadow tail for a while after a pass)
  would let a device that is about to be busy skip doorbells at depth one too, at the
  price of a host CPU spinning; not done.
- **A shared file** still costs half the throughput of three files (486.3k vs
  951.6k/s for 4 KiB at three threads), and with cheap doorbells batching at three
  threads is now slower than not batching (852.2k vs 951.6k/s), presumably because a
  flush waits for the whole read batch, which delays resubmission more than the
  doorbells it saves are now worth.

## CI

`.github/workflows/ci.yml` (push and pull request, `ubuntu-24.04`) builds QEMU
10.2.1 with the device (release and ASan/UBSan), runs the qtest suite on both,
fetches Linux 6.12.41 and builds the KASAN kernel with the driver and the
guest clients. The QEMU, glib and Linux versions and SHA256 checksums are the
ones pinned in `scripts/`; the actions are pinned by commit; the Ubuntu
packages (compiler, flex, bison, libelf, static BusyBox) come from the runner
image and are not pinned. It then runs the guest suite under TCG with a strict
IOMMU. The whole job fits easily in a 30-minute budget: from a fresh clone the
same steps took 363 s on four threads of the build host with `-j4` for the
published commit and 360 s for this branch (`scripts/ci_local.sh`, output in
`results/round3/ci-local.txt` and `results/perf/ci-local.txt`; tarballs copied in,
no apt step). On a GitHub runner the first run of the published commit took 21
minutes, all steps passing; this branch has not run there. The runner has no KVM, so CI times
nothing.

## Not done

- Streaming data path (partial writes after an abort), PCIe AER recovery, an
  io_uring `uring_cmd` path.
- The device model outside QEMU's main loop, delay-based adaptive coalescing, the CQ
  head in the shadow page and device-side polling (see the performance round).
- Profiling QEMU on the host (needs perf).
