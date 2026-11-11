# Results

Raw output only; every number below points at the file it comes from. All runs are on
emulated hardware (QEMU 10.2.1) on one shared build host (Intel i9-13900K). The matrix
and deliberate-bug runs are correctness runs; their seconds include the whole guest boot
and, for the KASAN kernel, a kmemleak scan. Timing comes only from `round3/perf/`,
`perf/` and `improve/`, on KVM with a kernel without debug options. Commits named in the
round 1 to round 3 sections belong to the development history, which was squashed into a
single commit (`f5c0980`) when the repository was published; the commits named in the
performance and improvement round sections are in the published history.

## improvement round (2026-11-10)

### Timing on KVM: `improve/`

Same method as the perf round: frozen builds (`scripts/snapshot_build.py`, under
`.cache/ab/`), one quiet window per session, boots alternating build by build (and
configuration by configuration) round by round, `scripts/bench_report.py` and
`scripts/bench_compare.py` on the result; same guest, pinning, nice 0 and rejection
thresholds. New: `run_bench.py` configurations `iothread1` (one IOThread for the device,
pinned to CPU 9) and `iothreads` (one per vCPU, on CPUs 3, 5, 7), whose CPU time is
attributed to their CPUs and shown as "IOThreads us" and "host CPU us" (every QEMU
thread) per command; `--skip-configs` leaves a build out of a configuration its QEMU
does not have; `bench_compare.py --new-config` pairs two configurations of one build.

Build names: `base` the previous `main` (`1a8de5c`); `o3r` the perf round's step before
registered buffers (`68094ba`), its kernel and client rebuilt (the kernel byte-identical
to the perf round's frozen o3); `p16`, `p32`, `p3536` o3r plus a pad of inert text after
the driver, which came out as one binary (see below); `r1` registered buffers moved to
`kdc_fixed.c` (as a working tree; `r1c` the same as a commit); `i1` + the hold knob;
`i3` + the id hash and batched `read()`; `i4` + IOThread mode (the branch's final code).
All builds of a session share one QEMU binary except where the QEMU is the change (i1,
i4). Each boot's `run.json` has the build's `build_info` (commit, source and file
hashes). For r1c, i1, i3 and i4 that commit is an unpublished candidate: r1c's tree is
`eaf9e60`'s; i1's and i3's code in `iD` and `iE` is `600a551`'s and `675ab03`'s; i4's is
`b081e53`'s without the qtest settle described below; and `iC`'s i1 and i3 still had the
device-wide hold count.

| Directory | Command (`taskset -c 10-15,24-31 python3 scripts/run_bench.py ...`) | Boots |
|---|---|---|
| `improve/i2a`, `improve/i2b` | `--plan bench/layout.plan --builds base,o3r,p16,p32,p3536,r1 --rounds 7` | 42 each |
| `improve/iC` | `--plan bench/ab.plan --builds base,r1c,i1,i3 --rounds 7` | 28 |
| `improve/iCfixed` | `--plan bench/fixed.plan --builds base,r1c --configs noiommu,iommu-strict --rounds 7` | 28 |
| `improve/iD` | `--plan bench/ab.plan --builds base,i1,i3,i4 --configs noiommu,iothreads,iothread1 --skip-configs base=iothreads,iothread1 --skip-configs i1=iothreads,iothread1 --skip-configs i3=iothreads,iothread1 --rounds 7` | 42 |
| `improve/iDfixed` | `--plan bench/fixed.plan --builds base,i4 --configs noiommu,iommu-strict --rounds 7` | 28 |
| `improve/iE` | `--plan bench/ab.plan --builds base,i3,i4 --configs noiommu,iothread1,iothreads --skip-configs base=iothread1,iothreads --skip-configs i3=iothread1,iothreads --rounds 7` | 35 |
| `improve/iF` | `--plan bench/layout.plan --builds o3r,base,p3536r,r1c --rounds 7` | 28 |

`i2a`, `i2b` and `iF` ran with a copy of `bench/layout.plan`'s arms kept outside the
repository (each run's `plan.txt` is the plan it used, without the comment lines).

Sessions in order (host clock, 2026-11-10): i2a 00:17, i2b 04:22, iC 04:33, iD 05:38,
iE 06:02, iF 11:07 (the real 3536-byte pad `p3536r`, a hybrid like check C's: kernel and
client built from o3 plus the pad, `qemu-system-x86_64` copied from o3r; `iF/variants/` has
its pad source, `build.json`, object sizes and the addresses of functions linked after
the driver). Decisions (rules written before the sessions that judged them; the text of
each is in `docs/design-tradeoffs.md`, Improvement round):

| Step | Comparison file | Targets | Result |
|---|---|---|---|
| r1 registered buffers moved out | `i2a`, `i2b`: `compare-r1-vs-base.md`, `compare-r1-vs-o3r.md` | 32 in flight >= 6 of 7 vs base in both sessions | 5 and 5 of 7: miss; kept (ties o3r, never below base) |
| r1, the fixed path | `iCfixed/compare-r1c-vs-base*.md` | no fixed arm below base's minimum | strict IOMMU pass; without: 32 batched fixed 0.985, a miss |
| i1 hold knob | `iC/compare-i1-vs-r1c.md` | none (no arm below r1c's minimum) | keep |
| i3 id hash, batched read() | `iC/compare-i3-vs-i1.md`, `iD/compare-i3-vs-i1.md` | one-file arms | 1.45x and 1.29x, 7 of 7; bounce one at a time 0.992 below i1's minimum in iC, 1.001 in iD |
| i4 per-queue IOThreads (3, 5, 7) | `iD/compare-i4-vs-i4-iothreads.md` | three threads >= 6 of 7, > 1.03 | 0.870, 0 of 7: fails |
| i4 one IOThread (9) | `iE/compare-i4-vs-i4-iothread1.md` (and `iD/`) | three threads >= 6 of 7, > 1.03 | 1.342, 7 of 7 (iD 1.361): an option; default stays the main loop |
| i4 main-loop path | `iD/compare-i4-vs-i3.md`, `iE/compare-i4-vs-i3.md` | no arm below i3's minimum | iD NOPs one at a time 0.987, a miss; iE pass |
| layout, real 3536-byte pad | `iF/compare-p3536r-vs-o3r.md` | diagnostic | inconclusive: 32 batched 0.982 (base 0.988), three threads 0.989 (base 0.954) |
| before/after | `iE/compare-i4-vs-base.md`, `iE/compare-i4-vs-base-iothread1.md`, `iD/compare-i4-vs-base.md`, `iDfixed/` | | see the README table |

The layout controls: `i2a/variants/` has the pad sources (`kdc_pad-p16.c` etc., linked after
the driver's other objects by adding `kdc_pad.o` to `kernel/Makefile`) and the driver's
object sizes (`text-sizes.txt`, from `size` on the build). The p32 and p3536 sources were
copied over p16's with their old timestamps, make did not rebuild `kdc_pad.o`, and the three
kernels are byte-identical (`bzImage` hash in `build_info`), so p16, p32 and p3536 are one
binary under three names in both sessions; the design notes use them as such.

### Correctness: `improve/matrix/`, `improve/matrix-steps/`, `improve/mutants/`

Each step was verified on a frozen copy of its tree (sources plus the QEMU builds,
kernels and client built from it), the full matrix of that commit, its qtest suite on both
QEMU builds and the deliberate bugs of its new or moved mechanisms:
`improve/matrix-steps/<step>/summary.json` (qtest logs included; guest logs in
`~/.cache/kl-improve/verify/<step>v/results/matrix/` on the build host) and
`improve/mutants-steps/<step>/`. The final code's full matrix is `improve/matrix/` and
its full deliberate-bug run `improve/mutants/`.

| | r1 | i1 | i3 | i4 / final |
|---|---|---|---|---|
| qtest, ASan+UBSan and release QEMU | 20/20 | 21/21 | 21/21 | 42/42 (21 tests, main loop and IOThreads) |
| KUnit; client cases | 13; 37 | 13; 37 | 13; 37 | 13; 37 |
| guest configurations | 6 of 6 | 6 of 6 | 6 of 6 | 10 of 10 (`improve/matrix/`) |
| the step's deliberate bugs | overlap-check-off, unregister-in-use: caught | hold-not-honoured: caught | read-leaks-command: caught | iothread-irq-lost: caught |

The i4 step's matrix stopped after its first runs: its qtest suite under the ASan QEMU
failed once in `counters_and_scratch_iothreads` (`improve/matrix-steps/i4-partial/`):
two interrupts instead of three, because the IOThread fetched three commands in one pass
that the test had meant to give two passes. The test now settles between them; the
final matrix ran on the fixed code.

Final matrix (`improve/matrix/`, `python3 scripts/run_matrix.py`'s configurations run by
the same steps on a frozen copy of `b081e53`, whose code `fbb9d22` and the documentation
commits keep): qtest 42/42 on both QEMU builds; KVM with the IOMMU 23.0 s, without
22.1 s, TCG 72.4 and 68.0 s, ASan QEMU 1144.7 s, KCSAN 26.2 s, and with three IOThreads:
KVM with the IOMMU 22.4 s, without 22.7 s, KVM with the ASan QEMU 24.4 s, TCG without an
IOMMU 73.4 s; kmemleak 0 records, `DMA_API_ERRORS 0` and the module counters at zero in
every run. The step matrices and this one ran up to three at a time on the build CPUs;
r1's, i1's, i3's and this one passed on their first run (i4's step matrix: above).

`removal_surprise` and `removal_orderly` now print the held commands too. Under the ASan
QEMU, r1 (before the hold knob) ended 7 commands by the surprise removal; i1 and i3 ended
15 and 11 plus the 4 held ones, every held one with `ENODEV`.

Deliberate bugs (`improve/mutants/`, `python3 scripts/mutation_check.py --out ...` on
`fbb9d22`): 16 of 16 caught, the restored build passed the guest suite and qtest again.
The first full run stopped at `event-index-not-published`, whose anchor still named the
DMA call the IOThread commit replaced; `fbb9d22` fixed the anchor and the run was
repeated from the start.

| Mutant | Named check that fired |
|---|---|
| the perf round's 14 | as in `perf/mutants/` (`unregister-in-use`: the driver's `WARN` now in `kdc_fixed.c`) |
| new: device services held commands (`hold-not-honoured`) | qtest `fault_hold`: "held commands are not serviced" |
| new: IOThread interrupt marked pending, never delivered (`iothread-irq-lost`) | qtest `pass_interrupts_iothreads`: "queue vector fired" |

`improve/ci-local.txt`: `SEED_DIR=... LOCAL_TOOLS=... scripts/ci_local.sh OUT 12-15` from a
fresh clone of `b081e53`: 407 s, all passed (qtest 42/42 twice, the TCG guest run).
`improve/fresh-clone.txt`: the README's commands from a fresh clone of `b47bc21` (QEMU
and kernel builds, qtest on the ASan QEMU, the guest suite on TCG with the IOMMU and on
KVM with the IOMMU, in the main loop and with three IOThreads, the perf kernel, a frozen
build, `bench/crc.plan`, and one round each of the A/B of the clone against its own
frozen build and of the main loop against one IOThread): all passed, 329 s for builds and
tests, 486 s in all. Its one-round comparisons are single pairs and decide nothing (the
NOP pair of the identical-build A/B tied at 1.000; one IOThread 1.208 on NOPs one at a
time).

### Check D: `improve/check-d/`

An independent rerun of the round from fresh clones of `improve` (`7793fab`), `main`
(`1a8de5c`), o3 (`68094ba`) and r1c (`eaf9e60`), every build made again from source (the four
timing kernels came out byte-identical to the round's), with two timing sessions 88 minutes
apart. Rule, commands and the full claims table: [`improve/check-d/README.md`](improve/check-d/README.md).

| Run | Result |
|---|---|
| `check-d/matrix`, `mutants`, `qtest-*.txt`, `ci-local.txt` | qtest 42/42 on both QEMU builds; 10 of 10 guest configurations pass (KUnit 13/13, 37/37 client cases, kmemleak 0); 16 of 16 deliberate bugs caught, restored build passes; the CI steps 436 s |
| `check-d/s1-ab`, `s2-ab`: final vs main | 32 in flight 1.075, 1.106; one file 1.431, 1.448 (NOP) and 1.253, 1.289 (4 KiB), so the README's range 1.28x-1.39x, widened by 0.02, fails in both sessions (1.25-1.45 here); 32 batched 1.020, 1.019; 64 KiB one at a time 0.962, 0.961 (5 and 6 of 7 pairs slower), a loss the checked text did not name |
| the same, one IOThread | against main: three threads 1.356, 1.357; batched 1.487, 1.480; 32 in flight 0.916, 0.936; one file NOP 1.173, 1.161 (the README had 1.10x) |
| the same, r1c and main against o3 | 32 in flight: main 0.978, 0.972; r1c 0.984, 0.992: the move took back 30% of the cost in one session and 72% in the other |
| `check-d/s1-fixed`, `s2-fixed` | strict IOMMU: fixed arms 0.979 to 1.033 against main; without an IOMMU 4 KiB zero-copy one at a time 0.949 (4 of 6 pairs slower) and 0.980, from slow boots of the final build |
| `check-d/claims.md` | 63 of 75 claim rows hold in both sessions; what did not hold is listed in the check's README; the text was corrected or gives this check's numbers next to it |

## perf round (2026-11-08/09)

### Timing on KVM: `perf/`

Every run compares frozen builds (`scripts/snapshot_build.py`, under `.cache/ab/`) in
one quiet window: `scripts/run_bench.py --builds ...` boots them in turn, round by
round; `scripts/bench_report.py` writes `summary.md` (one table per configuration and
build), `scripts/bench_compare.py` the paired `compare-<new>-vs-<base>.md/json` with the
keep/reject rule. Same guest and pinning as round 3 (3 vCPUs on host CPUs 2, 4, 6,
QEMU's main loop on 8), QEMU at nice 0, `kdc.bounce_max=65536`, rejection thresholds
unchanged. Build names: `main` the code as first published (`b95f7ae`); `o1` + CQ head
every 16 completions; `o2` + two interrupts per pass; `o3` + shadow doorbells, event
index, ioeventfd; `o4` + one reader woken per completion (rejected, not on the branch);
`o6` = o4 + registered buffers (measured, then rebuilt without o4); `o5` = o3 +
registered buffers, the branch's final code. Each build's `build.json` (commit, source
hashes) is in the run's `boot-N/run.json` under `build_info`.

| Directory | Command (`taskset -c 10-15,24-31 python3 scripts/run_bench.py ...`) | Boots | Windows (rejected) |
|---|---|---|---|
| `perf/o1-vs-main` | `--plan bench/ab.plan --builds main,o1 --rounds 7` | 14 | 239 (46) |
| `perf/chain` | `--plan bench/ab.plan --builds main,o1,o2,o3,o4,o6 --rounds 7` | 42 | 629 (41) |
| `perf/final` | `--plan bench/ab.plan --builds main,o3,o5 --rounds 7` | 21 | 317 (23) |
| `perf/fixed` | `--plan bench/fixed.plan --builds main,o5 --configs noiommu,iommu-strict --rounds 7 --skip-arms main=fixed4k-qd1,...` | 28 | 337 (29) |
| `perf/decompose` | `--plan bench/ab.plan --builds o5 --configs noiommu,noioeventfd,noshadow --rounds 7` | 21 | 320 (26) |
| `perf/tracker` | `--plan bench/tracker.plan --builds o5 --configs noiommu,notrack --rounds 3` | 6 | 196 (16) |

Decisions (rule in `scripts/bench_compare.py` and `docs/design-tradeoffs.md`):

| Step | Comparison file | Targets | Result |
|---|---|---|---|
| o1 CQ head every 16 completions | `perf/o1-vs-main/compare-o1-vs-main.md` | one in flight, 4 and 32 in flight unbatched | keep (all 7/7) |
| o2 two interrupts per pass | `perf/chain/compare-o2-vs-o1.md` | batched 8 and 32, three threads batched | keep (7/7) |
| o3 shadow doorbells, event index, ioeventfd | `perf/chain/compare-o3-vs-o2.md` | one in flight, 4 and 32 in flight, three threads | keep (7/7) |
| o4 one reader woken per completion | `perf/chain/compare-o4-vs-o3.md` | shared-file arms | reject (3/7, 4/7) |
| registered buffers (as o6 on o4) | `perf/chain/compare-o6-vs-o4.md` | none | delayed-coalescing arm below the base minimum (median ratio 0.994) |
| o5 registered buffers on o3 | `perf/fixed/compare-o5-vs-o5-iommu-strict-paths.md`, `perf/fixed/compare-o5-vs-o5-paths.md` | fixed vs zero-copy and bounce, strict IOMMU and none | targets 7/7 (one 6/7) |
| o5, other paths | `perf/final/compare-o5-vs-o3.md` | none | 32 in flight 0.2% below the base minimum (median -0.9%): kept, stated as a miss |
| o5, other paths, four more sessions | `perf/check-c/*/compare-perf-vs-o3-*.md` | none | 32 in flight 0.963 to 0.985, 0 to 3 of 7: a 1-4% cost, kept (see Check C) |
| before/after | `perf/final/compare-o5-vs-main.md` | | every arm 7/7, 1.21x to 4.75x |
| before/after, fresh clones | `perf/check-c/final/compare-perf-vs-main-vs-main.md` | | every arm 7/7, 1.22x to 4.68x |

`perf/decompose` has no comparison file: its three configurations are the same build
with the device's `ioeventfd=off` and the driver's `kdc.shadow_doorbells=0`, read from
`summary.md`. `perf/tracker` is the round-3 tracker plan on the final build, 15
windows per arm, unpaired. Every boot's `run.json` records the build's `build_info`
(source hashes), the quiet-window marker and the load average (1 min) at boot: 2.1
to 7.1 over all boots (3.6 to 7.1 in `o1-vs-main`, 2.1 to 4.8 in the others); the
rejection rule, not the load average, decides which windows count.

`perf/ci-local.txt`: `SEED_DIR=... LOCAL_TOOLS=... scripts/ci_local.sh OUT 12-15` from a
fresh clone of `a85dafc`, the CI workflow's steps on four CPUs: 360 s, all passed.
`perf/fresh-clone.txt`: the README's commands from a fresh clone of `a85dafc`
(QEMU and kernel builds, qtest on the ASan QEMU, the guest suite on TCG and on KVM
with the IOMMU, the perf kernel, a frozen build, `bench/crc.plan` and a one-round A/B
of the clone against its own frozen build): all passed, 267 s for builds and tests,
364 s in all. That A/B compared identical code; its single nop-qd1 pair differed by
14% (156.5k vs 178.2k/s), which is why decisions use seven pairs and a rule rather
than one window.

### Correctness: `perf/matrix/`, `perf/matrix-steps/`, `perf/mutants/`

Every step was verified on a frozen copy of its own tree (sources plus the QEMU
builds, kernels and client it was built into), so the matrices of several steps
could run while the next step was built: `perf/matrix-steps/<step>/summary.json`
(qtest logs included; the guest logs are in `.cache/results-raw/perf/matrix-<step>/`
on the build host). The final code's full matrix is committed in `perf/matrix/`.

| Configuration | o1 | o2 | o3 | final |
|---|---|---|---|---|
| qtest, ASan+UBSan and release QEMU | 17/17 | 18/18 | 20/20 | 20/20 |
| KUnit; client cases | 12; 35 | 12; 35 | 13; 36 | 13; 37 |
| KVM and TCG, IOMMU strict or none; ASan QEMU; KCSAN kernel | 6 of 6 pass | 6 of 6 | 6 of 6 | 6 of 6 (`perf/matrix/`) |

Final matrix: KVM with the IOMMU 22.1 s, without 22.7 s, TCG 72.9 and 63.2 s, ASan
QEMU 1357.5 s, KCSAN 25.2 s; kmemleak 0 records, `DMA_API_ERRORS 0` and the module
counters at zero in every run. The final code's ASan configuration passed on its
third run; the two failed runs are kept in `.cache/results-raw/perf/attempts/` on the
build host and are not explained:

- `matrix-final-1`: the guest kernel reported `BUG: KASAN: wild-memory-access in
  do_raw_spin_lock` from `handle_edge_irq` 0.017 s into boot, during CPU
  identification and before any PCI device is probed, while five matrices ran at
  once on the build CPUs. Every other configuration of that run passed.
- `matrix-final-2-asan` (that configuration alone, rerun): `removal_surprise` found
  no command ended by the removal (`nodev >= 1` failed; every command of the three
  threads had already completed), after 35 cases had passed. The step copies o1 to o3
  passed the same case under the same QEMU (10 to 12 commands ended by the removal),
  and so did every other configuration of the final code (13 to 36). The client now
  prints the counts before checking them, so a repeat shows them; the third run
  (`perf/matrix/`) passed with 13.

Deliberate bugs: `perf/mutants/` (final code; the nine earlier mutants
and five for the new mechanisms), produced by `python3 scripts/mutation_check.py --out
...` and, for `overlap-check-off`, a rerun with `--only overlap-check-off`. 14 of 14
caught; the restored build passed the guest suite and qtest again
(`perf/mutants/restored-*`). Each mutant's `result.json` names the check that fired and
`mutation.diff` the edit; guest serial logs are in `.cache/results-raw/perf/mutants-final/`
on the build host.

| Mutant | Named check that fired |
|---|---|
| unmap/unpin zero-copy buffers before the device runs | `check_copy` (TCG with and without the IOMMU) |
| cancel treats the device abort as synchronous | `cancel_device_owned` |
| device model skips the SG length check | qtest `malformed` |
| buffers released when the abort times out | `timeout_reset` |
| an unanswered abort never escalates | `timeout_reset` |
| `remove()` frees the per-queue array | KASAN use-after-free in `kdc_read` |
| destination overlap check skipped | DMA-API debug: overlapping mapping, in `fixed_buffers` |
| `read()` never frees a consumed command | kmemleak record with `kdc_cmd_prepare` |
| debugfs coalescing never reaches the queues | `coalescing` |
| new: CQ head never written | `cq_head_batching` |
| new: end-of-pass interrupt skipped (device) | qtest `pass_interrupts` |
| new: shadow doorbell rung only when the queue was empty | `shadow_doorbell` |
| new: event index never published (device) | qtest `shadow_doorbell` |
| new: registered buffer unmapped while a command holds it | the driver's `WARN` in `kdc_regbuf_destroy` |

Two expectations changed during the round, both after a run in which the bug was
detected by something else first: `overlap-check-off` used to name `dst_overlap`, but
`fixed_buffers` now runs earlier and DMA-API debug reports the overlapping mapping there
(the full run counted it as missed; `perf/mutants/summary-full-run.json`); and
`unregister-in-use` first expected `fixed_buffers` to see `EBUSY`, but the driver's own
`WARN` fires before that (`.cache/results-raw/perf/attempts/` on the build host).

### Check C: `perf/check-c/`

An independent rerun of the round's final comparisons from fresh clones of `perf`
(`1518384`), `main` (`b95f7ae`) and o3 (`68094ba`), every build made again from source,
plus three experiments on the registered-buffer cost. Details and commands:
[`perf/check-c/README.md`](perf/check-c/README.md).

| Run | Result |
|---|---|
| `check-c/matrix`, `matrix-asan2`, `matrix-asan3` | qtest 20/20 on both QEMU builds; 6 of 6 guest configurations pass (KUnit 13/13, 37/37 client cases, kmemleak 0); the ASan QEMU configuration passed 3 of 3, its `removal_surprise` ending 9, 10 and 15 commands by the removal (27 to 36 in the other configurations) |
| `check-c/mutants` | 14 of 14 caught, restored build passes |
| `check-c/final` perf vs main | every arm 7/7, 1.22x to 4.68x; each ratio within 3% of `perf/final` except NOPs on one shared file (2.79x vs 2.68x) |
| `check-c/final` perf vs o3 | 32 in flight 0.963 (0 of 7); rule fails |
| `check-c/fixed` | fixed vs bounce in the same build 7/7 (one 6/6): 1.14x to 1.74x without an IOMMU, 1.11x to 1.61x behind the strict IOMMU |
| `check-c/swap-qemu`, `check-c/swap-client` | the cost follows the guest kernel, not the QEMU binary or the benchmark client |
| `check-c/reorder` | new fields moved to the ends of `kdc_cmd` and `kdc_ctx`: 0.990 against o3 (0 of 7), not applied |

## round3 (2026-11-08)

### Correctness: `round3/matrix/` and `round3/mutants/`

Produced by `python3 scripts/run_matrix.py --name round3/matrix` and
`python3 scripts/mutation_check.py --out results/round3/mutants` at commit
`d6927eb` (ABI version 3, 34 client cases, 12 KUnit cases, 17 qtest tests).
Guests as in round 2 (4 vCPUs, two devices with 4 and 2 queues). New since
round 2: the two KVM configurations (`/dev/kvm` became available), the
`coalescing` client case, the qtest counter/scratch test and the copy test with
the table CRC, and the `coalesce-ignored` deliberate bug.

| Configuration | Result | KUnit | Client cases | Seconds | Files |
|---|---|---|---|---|---|
| qtest, ASan+UBSan QEMU | pass | - | 17 device tests | - | `round3/matrix/qtest-asan.log` |
| qtest, release QEMU | pass | - | 17 device tests | - | `round3/matrix/qtest-release.log` |
| KVM, Intel IOMMU strict, KASAN kernel | pass | 12/12 | 34/34 | 22.4 | `round3/matrix/kvm-iommu/` |
| KVM, no IOMMU, KASAN kernel | pass | 12/12 | 34/34 | 21.9 | `round3/matrix/kvm-noiommu/` |
| TCG, Intel IOMMU strict, KASAN kernel | pass | 12/12 | 34/34 | 72.6 | `round3/matrix/tcg-iommu/` |
| TCG, no IOMMU, KASAN kernel | pass | 12/12 | 34/34 | 69.4 | `round3/matrix/tcg-noiommu/` |
| TCG, IOMMU strict, ASan+UBSan QEMU, KASAN kernel | pass | 12/12 | 34/34 | 1138.3 | `round3/matrix/tcg-iommu-asanqemu/` |
| TCG, IOMMU strict, KCSAN kernel | pass | 12/12 | 34/34 | 26.8 | `round3/matrix/tcg-iommu-kcsan/` |

kmemleak reported 0 records in every KASAN run; `DMA_API_ERRORS 0` and the
driver's module counters back to zero at the end of every run (`serial.log`).
Load average (1 min) at the start of the guest runs was 3.5 to 8.3 (`run.json`).
The client's counts (`serial.log`, `summary.json` under `counts`) match round 2
in kind; for example, with KVM and the IOMMU: `cancel_race` 1 cancelled before
the doorbell, 1490 aborted by the device, 9 completed first, 40 aborted while
held; `timeout_reset` ended after 765 ms; `bounded_close_and_reset` returned
after 567 ms (close) and 561 ms (reset); hot-unplug under load 73 commands, 43
completed and 30 ended with `ENODEV` (orderly), 38 and 35 (surprise); fault
injection 29 submit and 377 probe allocation failures, all unwound. On KVM the
device wins the random part of `cancel_race` far less often than under TCG
(9 vs 930 copies completed before the abort). The new `coalescing` case saw a
batch of 8 reaped with one interrupt and a lone command held back 50 ms by the
50 ms delay, in every configuration. `bounce_max` was 65536 in the IOMMU runs
and 2048 without (`device_info`).

KVM ran the suite about three times faster than TCG and exposed three test
races that TCG's timing had hidden; they were fixed in the tests before these
runs (commit `7b76676`): `timeout_escalation`, `lost_completions` and
`bounded_close_and_reset` read counters or set knobs right after a completion
ended by a reset, while the reset was still restarting the controller (and
re-enabling it clears the knobs); `reset_semantics` aborted a command the
device had not fetched yet. One of eight repeated KVM runs before the fix
failed in `lost_completions`.

Deliberate bugs (`round3/mutants/summary.json`), each caught by its named check:

| Mutant | Run | Named check that fired | Other evidence |
|---|---|---|---|
| unmap/unpin zero-copy buffers before the device runs | TCG + IOMMU; TCG, no IOMMU | `check_copy` (both) | `DMAR: [DMA Read ...]` with the IOMMU |
| cancel treats the device abort as synchronous | TCG + IOMMU | `cancel_device_owned` | |
| device model skips the SG length check | qtest, ASan QEMU | `malformed`: "entries short of length" | |
| buffers released when the abort times out, before the reset | TCG + IOMMU | `timeout_reset` | |
| an unanswered abort never escalates | TCG + IOMMU | `timeout_reset` | |
| `remove()` frees the per-queue array open files still point into | TCG + IOMMU | `BUG: KASAN: slab-use-after-free in kdc_read` | |
| destination overlap check skipped | TCG + IOMMU | `dst_overlap` | |
| `read()` never frees a consumed command (kmemleak control) | TCG + IOMMU | kmemleak record with `kdc_cmd_prepare` | `globals_idle` failed first |
| new: the debugfs coalescing setting never reaches the queues | TCG + IOMMU | `coalescing` (8 interrupts instead of 1) | |

The kmemleak control's committed `serial.log` is cut after 400 `kmemleak_dump`
lines; the full file is in
`.cache/results-raw/round3/mutants/read-leaks-command/tcg-iommu/` on the build
host. After restoring the code, the guest run and the qtest suite passed again
(`mutants/restored-*`). Besides these, the new SSE4.2 CRC path was broken on
purpose once by hand (a wrong final XOR): the qtest `copy_crc` failed on the
first length while `copy_crc_table` passed (not committed).

### Timing on KVM: `round3/perf/`

Produced by `scripts/run_bench.py` with the driver, device model and benchmark
client of commit `7b76676` and the `perf` kernel, inside a quiet window on the
shared host; `scripts/bench_report.py` wrote each `summary.md`/`summary.json`
from `windows.jsonl.gz` (one record per measured window, rejected ones
included: KVM statistics per vCPU, device counters, CPU time of every QEMU
thread, busy time and clock of the watched CPUs). Each `boot-N/run.json` has
the QEMU command line, pinning, host settings and thresholds; `serial.log`
has the guest's `RESULT` lines.

| Directory | Command (`taskset -c 10-15,24-31 python3 scripts/run_bench.py ...`) | Windows (rejected) |
|---|---|---|
| `perf/main` | `--plan bench/main.plan --configs noiommu --rounds 2` | 1128 (447) |
| `perf/iommu` | `--plan bench/iommu.plan --configs noiommu,iommu-strict,iommu-lazy --rounds 3` | 381 (67) |
| `perf/tracker` | `--plan bench/tracker.plan --configs noiommu,notrack --rounds 3` | 269 (100) |
| `perf/crc` | `--plan bench/crc.plan --configs noiommu,tablecrc --rounds 2` | 87 (7) |

Setup: guest with 3 vCPUs on host CPUs 2, 4, 6, QEMU's main loop and helper
threads on CPU 8, QEMU at nice 10, one `kdc-dma` device (3 queues, one per
vCPU), `kdc.bounce_max=65536` so both data paths exist at every size, KVM
halt polling at the host default (200 us), host governor `powersave`. A window
was rejected and run again when CPU time on CPUs 2-9 not used by QEMU exceeded
1500 ms per second, or QEMU's threads waited for a CPU more than 25 ms per
second; both limits were fixed from a pilot run before these runs. Every arm
kept at least 10 accepted windows. The background was mostly one unpinned job
of another project (about one CPU) that the scheduler kept moving onto idle
SMT siblings of the VM's CPUs.

Before the published runs, two pilots (not committed) found that QEMU's table
CRC dominated the device model, which led to `hw-crc`, and that cpuidle
residency counters are updated only when a CPU leaves idle, so the harness now
briefly runs on every watched CPU before reading them.

How to read `summary.md`: per command unless noted; "exits" are KVM exits
summed over vCPUs, split into MMIO, HLT and the rest (port I/O is in "exits"
but not shown separately); "SQ db", "CQ db" and "MSI-X" are the device's own
counters; "main loop us" and "vCPU us" are host CPU time per command of
QEMU's main thread and of the vCPU threads, the latter including KVM halt
polling, shown in the last column. The `idle` arm has no commands: about 270
exits per one-second window, 26 of them port I/O (the serial console, which
also carries the line that asks for the closing snapshot).

## round2 (2026-11-07)

Produced by `python3 scripts/run_matrix.py --name round2` followed by
`python3 scripts/mutation_check.py --out results/round2/mutants`, with the driver,
device model and tests of commit `d1eb32c` (kernel image sha256 prefixes
`267f9f21a09f` KASAN, `1fcc069afc2d` KCSAN, recorded in each `run.json`); the
commit holding these results adds the kmemleak control to the mutant list.

Guests: Linux 6.12.41, 4 vCPUs, 2 GiB, two `kdc-dma` devices (4 and 2 queues)
behind PCIe root ports, QEMU's default ACPI PCI hotplug. The KASAN kernel has
generic inline KASAN, kmemleak, lockdep (`PROVE_LOCKING`), `DEBUG_ATOMIC_SLEEP`,
`DMA_API_DEBUG` + `DMA_API_DEBUG_SG` and fail-nth fault injection; the KCSAN
kernel swaps KASAN and kmemleak for KCSAN. A run fails on any BUG, WARNING,
KASAN, KCSAN, lockdep, DMA-API, DMAR or driver report outside an expectation
window, on an expected report that never appears, on a kmemleak record with a
driver frame, or on any sanitizer report in QEMU's stderr.

| Configuration | Result | KUnit | Client cases | Seconds | Files |
|---|---|---|---|---|---|
| qtest, ASan+UBSan QEMU | pass | - | 15 device tests | - | `round2/qtest-asan.log` |
| qtest, release QEMU | pass | - | 15 device tests | - | `round2/qtest-release.log` |
| TCG, Intel IOMMU strict, KASAN kernel | pass | 12/12 | 33/33 | 63.6 | `round2/tcg-iommu/` |
| TCG, no IOMMU, KASAN kernel | pass | 12/12 | 33/33 | 65.0 | `round2/tcg-noiommu/` |
| TCG, IOMMU strict, ASan+UBSan QEMU, KASAN kernel | pass | 12/12 | 33/33 | 1348.4 | `round2/tcg-iommu-asanqemu/` |
| TCG, IOMMU strict, KCSAN kernel | pass | 12/12 | 33/33 | 23.7 | `round2/tcg-iommu-kcsan/` |
| KVM, IOMMU strict | **not run** | | | | `/dev/kvm` not accessible to the build user (`round2/summary.json`) |
| KVM, no IOMMU | **not run** | | | | same |

kmemleak reported 0 records in each KASAN run; `DMA_API_ERRORS 0` and the
driver's module counters (`cmds=0 pinned=0 mapped=0 dst_ranges=0`) at the end
of every run (`serial.log`). Load average (1 min) during the guest runs was
2.4 to 4.8, recorded in each `run.json`.

Counts printed by the client (`serial.log`, also in `summary.json` under `counts`):

| Case | tcg-iommu | tcg-noiommu | ASan QEMU | KCSAN |
|---|---|---|---|---|
| `cancel_race`: cancelled before doorbell / aborted by device / completed first | 2 / 1213 / 285 | 1 / 973 / 526 | 41 / 0 / 1459 | 18 / 1004 / 478 |
| `cancel_race`: aborted while held device-owned (deterministic) | 40 | 40 | 40 | 40 |
| `timeout_abort`: abort answered after | 201 ms | 205 ms | 265 ms | 210 ms |
| `timeout_reset`: ended by controller reset after | 761 ms | 760 ms | 847 ms | 765 ms |
| `bounded_close_and_reset`: close / reset returned after | 565 / 548 ms | 546 / 552 ms | 656 / 569 ms | 550 / 550 ms |
| `sysfs_reset_under_load`: 3 resets, verified copies, reissued | 172, 20 | 166, 21 | 77, 15 | 352, 23 |
| `fatal_recovery`: commands completed, reissued | 16, 16 | 16, 16 | 16, 16 | 16, 16 |
| `fault_injection`: submit / probe allocation failures, all unwound | 22 / 355 | 32 / 321 | 29 / 357 | 14 / 171 |
| `removal_orderly`: commands, completed, ended with ENODEV | 71, 36, 35 | 69, 32, 37 | 134, 86, 48 | 79, 46, 33 |
| `removal_surprise`: commands, completed, ended with ENODEV | 71, 42, 29 | 67, 37, 30 | 78, 65, 13 | 71, 43, 28 |

- The random part of `cancel_race` depends on emulation speed: on the ASan QEMU
  the device always finished first. The 40 held rounds (stall knob, then
  cancel) take the abort path in every configuration.
- The 200 ms timeout and 500 ms abort grace set the times above; the watchdog
  checks every 100 ms. They are bounds, not performance numbers.
- Fault-injection counts differ between kernels and runs because the debug
  options and the IOMMU allocate on the same paths, and some allocations depend
  on timing; each count is the number of injected failures until a run of the
  path completed without one.
- `dst_overlap` refused 7 overlapping submits on every run (bounce and
  zero-copy, same file, other file, other device, a second mapping of the same
  page, a destination mapping one page twice) and completed the 6 adjacent ones;
  `adjacent_destinations` kept 96 small and 6 large back-to-back destinations
  in flight together with no DMA-API report.
- `dma_fault`: with the IOMMU each faulting copy produced `DMAR: [DMA Write
  NO_PASID] ... fault addr 0xfffffffffff00000 [fault reason 0x04] Access beyond
  MGAW` inside its expectation window and ended with `-EIO`; without the IOMMU
  the write failed in the bus with no report.
- Expectation windows and the reports matched in each are listed in `run.json`
  (`expectations`); host actions (unplug, hot-add, the vanished device) with
  their timing in `host_actions`.

## Deliberate bugs (`round2/mutants/`)

| Mutant | Run | Named check that fired | Other evidence |
|---|---|---|---|
| unmap/unpin zero-copy buffers before the device runs | TCG + IOMMU | `check_copy` (status != 0) | `DMAR: [DMA Read ...]` |
| same | TCG, no IOMMU | `check_copy` (destination bytes wrong: the bounced partial lines are lost with the early release) | |
| cancel treats the device abort as synchronous | TCG + IOMMU | `cancel_device_owned` (cancel returned 0 instead of `EINPROGRESS`) | |
| device model skips the SG length check | qtest, ASan QEMU | `malformed`: "entries short of length" | |
| buffers released when the abort times out, before the reset | TCG + IOMMU | `timeout_reset` (the other command is never reissued: no reset happens) | |
| an unanswered abort never escalates | TCG + IOMMU | `timeout_reset` (the timed-out command never ends) | |
| `remove()` frees the per-queue array open files still point into | TCG + IOMMU | `BUG: KASAN: slab-use-after-free in kdc_read` | |
| destination overlap check skipped | TCG + IOMMU | `dst_overlap` (overlapping bounce submit accepted) | |
| `read()` never frees a consumed command (kmemleak control) | TCG + IOMMU | kmemleak record with `kdc_cmd_prepare` | `globals_idle` failed first (module counter `cmds` not zero) |

The kmemleak control leaked every command it read; its committed `serial.log`
and `run.json` are cut after 400 `kmemleak_dump` lines and 40 problems, with the
full files in `.cache/results-raw/round2/mutants/read-leaks-command/tcg-iommu/`
on the build host. In round 1, the no-IOMMU run of the first mutant failed in
`pinned_page_not_recycled`; with the head/tail bounce added in round 2 the same
bug now shows up earlier as wrong destination bytes.

After restoring the code and rebuilding, the guest run and the qtest suite
passed again (`mutants/summary.json`, `mutants/restored-*`).

## What the suite found during round 2

Problems found while the round-2 tests were written and run, all fixed before
the commit the results above were produced from. Listed because they show what
the checks catch, including where the flaw was in a test or outside this code.

| Found by | Problem | Where | Fix |
|---|---|---|---|
| `removal_surprise` on the ASan QEMU (slower timing) | `read()` could return `ENODEV` while the failing device's commands were still being ended, so a completion arrived after `ENODEV` | driver | `read()`/`poll()` report the end only when the device is gone and the file has nothing in flight |
| `sysfs_reset_under_load` | three resets from sysfs in quick succession ended a command with `-EIO`: the two-reissue cap counted resets the driver did not start | driver | only resets the driver starts count toward the cap |
| code review, no test | `close()` returned commands to the DMA pools after dropping `files_mutex`, so a concurrent `remove()` could destroy the pools first | driver | release under `files_mutex` |
| `cancel_race` (new held phase) | an abort written before the device has fetched the command is ignored and the copy completes; the test assumed the abort always lands | test | wait until the device reports the command in flight (`device_inflight` in debugfs) |
| host requests | records written to `/dev/kmsg` without a trailing newline stay open for continuation and reach the console only with the next message, so the host acted on requests late | test client | newline-terminated records |
| probe fault injection | an injected failure in `register_irq_proc()` is tolerated by the IRQ core at `request_irq()` and then makes `remove_proc_entry()` WARN when the IRQ is freed | Linux 6.12.41 IRQ core | allocations under `register_irq_proc()` are excluded from injection (stacktrace filter); every other allocation on the probe path fails once |
| hot-unplug through pciehp | `pciehp_unconfigure_device()` re-takes `ctrl->reset_lock` under `pci_rescan_remove_lock`; lockdep reports a possible circular dependency on every removal and then switches itself off | Linux 6.12.41 pciehp | the guest uses QEMU's default ACPI PCI hotplug (acpiphp) instead |

## round1 (2026-11-07)

Produced by `python3 scripts/run_matrix.py --name round1` and
`python3 scripts/mutation_check.py --out results/round1/mutants` at commit
`a6fe52e` (ABI version 2, no recovery yet). KASAN kernel without kmemleak,
19 client cases, 10 KUnit cases, 14 qtest tests.

| Configuration | Result | KUnit | Client cases | Seconds | Files |
|---|---|---|---|---|---|
| qtest, ASan+UBSan QEMU | pass | - | 14 device tests | - | `round1/qtest-asan.log` |
| qtest, release QEMU | pass | - | 14 device tests | - | `round1/qtest-release.log` |
| TCG, Intel IOMMU strict | pass | 10/10 | 19/19 | 10.5 | `round1/tcg-iommu/` |
| TCG, no IOMMU | pass | 10/10 | 19/19 | 9.8 | `round1/tcg-noiommu/` |
| TCG, IOMMU strict, ASan+UBSan QEMU | pass | 10/10 | 19/19 | 189.5 | `round1/tcg-iommu-asanqemu/` |
| KVM (both) | **not run** | | | | `/dev/kvm` not accessible |

Round-1 deliberate bugs (unmap before completion, cancel trusting the abort,
device skipping the SG length check) were caught; all three are part of round 2
again. Details in `round1/mutants/summary.json`.
