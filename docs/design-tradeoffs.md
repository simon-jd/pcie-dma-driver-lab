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

## Performance on KVM

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
the queue is deep, as NAPI does) is the obvious next step. A polled completion
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

## CI

`.github/workflows/ci.yml` (push and pull request, `ubuntu-24.04`) builds QEMU
10.2.1 with the device (release and ASan/UBSan), runs the qtest suite on both,
fetches Linux 6.12.41 and builds the KASAN kernel with the driver and the
guest clients. The QEMU, glib and Linux versions and SHA256 checksums are the
ones pinned in `scripts/`; the actions are pinned by commit; the Ubuntu
packages (compiler, flex, bison, libelf, static BusyBox) come from the runner
image and are not pinned. It then runs the guest suite under TCG with a strict
IOMMU. The whole job fits easily in a 30-minute budget: from a fresh clone the
same steps took 363 s on four threads of the build host with `-j4`
(`scripts/ci_local.sh`, output in `results/round3/ci-local.txt`; tarballs
copied in, no apt step). A GitHub runner is slower per thread; its time is not
known until the workflow first runs there. The runner has no KVM, so CI times
nothing.

## Not done

- Streaming data path (partial writes after an abort), PCIe AER recovery, an
  io_uring `uring_cmd` path.
- Adaptive interrupt coalescing; writing the completion-queue head only every
  few completions (one of the two exits per command at depth 1); doorbells
  through an ioeventfd with a shadow tail in memory, which would turn the
  submission exit into a lighter in-kernel one.
- Profiling QEMU's global-lock contention on the host (needs perf).
