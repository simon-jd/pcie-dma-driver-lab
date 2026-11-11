# Check D: an independent rerun of the improvement round

Fresh clones of `improve` (`7793fab`), `main` (`1a8de5c`), o3 (`68094ba`, the step before
registered buffers) and r1c (`eaf9e60`, registered buffers moved to `kdc_fixed.c`) on the same
host on 2026-11-10, in `~/.cache/kl-check-d/`. Every QEMU, kernel and client was built from source
in its own clone with the README commands; nothing came from the round's `.cache/` except the
pinned Linux tarball, and flex, bison and libelf came from the parent directory's `.cache/tools`
(read only, as in check C). The four timing kernels came out byte-identical to the round's frozen
builds of the same commits (`builds.json`: bzImage `17cb5ea...` as i4, `01025f9...` as base,
`5092f34...` as o3r, `875bc5a...` as r1c); the QEMU binaries and benchmark clients have other
hashes. Correctness runs used CPUs 10-15 and 24-31 at nice 10. Timing ran at nice 0 inside a
quiet window, with the round's plans, pinning and rejection thresholds: session `s1` 15:55-16:18
and `s2` 17:23-17:46, load average 2.3 to 4.8 at boot, 46 of 633 and 57 of 420 windows rejected
for foreign load in `s1` (ab, fixed), 46 of 634 and 26 of 390 in `s2`.

## Commands

`check-d.sh` ran all of it (phases and times in `run-phases.txt`). `TOOLS` is the parent
directory's `.cache/tools`, `K` its `downloads/linux-6.12.41.tar.xz`, `P` is
`taskset -c 10-15,24-31 nice -n 10`.

```sh
git clone -b improve REPO improve; git clone -b main REPO main
git clone REPO o3 && git -C o3 checkout 68094ba; git clone REPO r1c && git -C r1c checkout eaf9e60
# improve clone
$P python3 scripts/build_qemu.py && $P python3 scripts/fetch_kernel.py --archive K
$P python3 scripts/build_kernel.py --local-tools TOOLS             # and --variant kcsan, --variant perf
python3 scripts/snapshot_build.py improve
# main, o3, r1c clones: frozen as .cache/ab/NAME and copied into the improve clone's .cache/ab/
$P python3 scripts/build_qemu.py --flavor release && $P python3 scripts/fetch_kernel.py --archive K
$P python3 scripts/build_kernel.py --local-tools TOOLS --variant perf && python3 scripts/snapshot_build.py NAME
# correctness, improve clone
$P python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu-asan/qemu-system-x86_64 --log qtest-asan.txt   # and .cache/build/qemu
$P python3 scripts/run_vm.py --accel tcg --iommu --out readme-run-vm
$P python3 scripts/run_matrix.py --name check-d-matrix                   # -> matrix/
$P python3 scripts/mutation_check.py --local-tools TOOLS --out mutants    # -> mutants/
SEED_DIR=SEED LOCAL_TOOLS=TOOLS scripts/ci_local.sh OUT 12-15             # -> ci-local.txt
# timing, each session in one quiet window, improve clone
taskset -c 10-15,24-31 python3 scripts/run_bench.py --plan bench/ab.plan --builds main,o3,r1c,improve \
  --configs noiommu,iothread1,iothreads --skip-configs main=iothread1,iothreads --skip-configs o3=iothread1,iothreads \
  --skip-configs r1c=iothread1,iothreads --rounds 7 --out sN-ab
taskset -c 10-15,24-31 python3 scripts/run_bench.py --plan bench/fixed.plan --builds main,improve \
  --configs noiommu,iommu-strict --rounds 7 --out sN-fixed
python3 scripts/bench_report.py sN-ab                                     # and sN-fixed
python3 scripts/bench_compare.py sN-ab --base main --new improve          # also --new-config iothread1 / iothreads
python3 scripts/bench_compare.py sN-ab --base improve --new improve --new-config iothread1   # and iothreads
python3 scripts/bench_compare.py sN-ab --base o3 --new improve            # and o3/r1c, main/r1c, o3/main
python3 scripts/bench_compare.py sN-fixed --base main --new improve       # and --config iommu-strict
python3 results/improve/check-d/claims.py results/improve/check-d > claims.md
```

`SEED` held the QEMU and glib tarballs the improve clone had downloaded and the pinned Linux
tarball. Every README command worked as written; on this host, which has no flex, bison or
libelf packages, `--local-tools` was needed on every `build_kernel.py` line and on
`mutation_check.py`, not only where the README mentions it.

## How a claim was judged

`rule.txt`, written before session 1: a stated ratio of 1.03x or more (or 0.97x or less) holds
if the median ratio over the 7 pairs is on the same side of 1 with at least 6 of 7 pairs, within
5% of the stated value; a smaller one (ties, "unchanged", small gains and losses) if the median
is within 0.02 of it and on the stated side of 1; a range if every arm it covers is inside it
widened by 0.02; a claim holds only if it holds in both sessions. A loss is missing if the final
code is below 0.985 of `main` with at most 2 of 7 pairs won in both sessions and the text does
not name it. `claims.py` applies this to the comparison files and writes `claims.md` (the full
claims table, every paired table, the counters and the per-boot rates).

## Results

**Correctness.** qtest 42/42 on the ASan+UBSan and the release QEMU (`qtest-*.txt`); the README's
`run_vm.py` line passed (TCG, strict IOMMU, 75.0 s, `readme-run-vm/`); the full matrix passed in
all 10 configurations, each with KUnit 13/13, 37/37 client cases and kmemleak 0 (`matrix/`:
KVM 23.1 and 22.6 s, TCG 74.1 and 64.8 s, ASan QEMU 1106.9 s, KCSAN 25.2 s, IOThreads 22.6, 22.0,
23.9 and 64.4 s); under the ASan QEMU `removal_surprise` ended 6 commands by the removal plus the 4
held ones (27 to 39 in the other configurations); 16 of 16 deliberate bugs caught with the
round's named checks, the restored build passed the guest suite and qtest (`mutants/`);
`ci_local.sh` from a fresh clone of `7793fab` passed in 436 s on four CPUs (`ci-local.txt`;
the round: 407 s).

**Timing claims.** 72 of the 79 rows of `claims.md` hold in both sessions, among them the README
table's ratios except the two listed below: against `main`, 32 in flight 1.075 and 1.106, one shared file 1.431/1.448
(NOP) and 1.253/1.289 (4 KiB), 32 batched 1.020 and 1.019, one at a time (64 KiB aside) and three threads 0.987
to 1.009; with one IOThread three threads 1.356/1.357 and batched 1.487/1.480, NOP and 4 KiB one
at a time 1.159-1.217, 32 in flight 0.916/0.936; the IOThread-against-main-loop table of design section 4
for one and three IOThreads, and its counters (doorbells per command at 32 in flight 0.24 -> 0.48,
interrupts 0.47 -> 0.73, vCPU 1.47 -> 1.88 us; three IOThreads at three threads 0.77 doorbells and
1.00 interrupts against 0.15 and 0.29). What did not hold or needs a correction:

| Claim (where) | round | s1 | s2 | finding |
|---|---|---|---|---|
| batching at three threads "0.99x of not batching in the main loop" (design, limits) | 0.856 in `iE` (803.7k/938.9k), 0.892 in `iD` | 0.882 | 0.891 | the text's number is wrong |
| behind the IOMMU "every fixed arm within 1% or better" (design 5) | 0.981 (`iCfixed`, fixed 4 KiB one at a time), 0.988 (`iDfixed`, fixed 64 KiB 8 batched) | 0.979 (fixed 64 KiB 8 batched) | 0.988 (fixed 4 KiB one at a time) | within 2.1%, not 1% |
| one IOThread, one command at a time "1.15-1.21x" (design 4) | 64 KiB 1.089 (`iE`) | 1.088 | 1.094 | 64 KiB left out |
| one IOThread, one file NOP 1.10x (README) | 1.104 | 1.173 | 1.161 | larger than stated |
| strict IOMMU, fixed 4 KiB 32 batched 1.01x (README) | 1.012 | 1.033 | 1.016 | larger in `s1` |
| loss: 4 KiB bounce one at a time 0.99x (README) | 0.988 (`iDfixed`) | 0.988 | 1.008 | not reproduced in `s2` |
| the move "took back most of the 1-4%" (README, design 2) | r1 vs o3r 1.004, 1.002 at 32 in flight | main/o3 0.978, r1c/o3 0.984 (2 of 7) | main/o3 0.972, r1c/o3 0.992 (1 of 7) | took back about a quarter in `s1`, 70% in `s2` |
| r1 vs o3r one file NOP -3.2% (design 2, `i2b`) | 0.968 | 0.994 | 1.004 | not reproduced |
| r1 vs base, one file 4 KiB, 7 of 7 in both (design 2) | 1.034, 1.022 | 1.020 (5 of 7) | 1.024 (6 of 7) | the gain holds, 7 of 7 does not |

**Losses missing from the text.**

- 64 KiB one at a time against `main`: 0.962 (2 of 7) and 0.961 (1 of 7). Three of the final
  build's seven boots in each session ran about 9% slower on that arm (64.0-68.0k against
  72.7-75.1k/s), as two or three of o3's and r1c's did; none of `main`'s 14 boots did
  (`claims.md`, per-boot rates). The round's own `iE` has 0.985 there (2 of 7).
- 4 KiB zero-copy one at a time in the fixed plan without an IOMMU: 0.949 (2 of 6) and 0.980
  (2 of 7), again from slow boots of the final build (5 and 3 of 7, 124.5-143.2k against
  145.5-152.6k/s); the round's `iDfixed` has 0.991 with 0 of 7 pairs won. In the A/B plan the same
  arm was 0.994 and 0.991, the README's 0.99x.
- From the round's own files: i4 against i3 at 4 in flight 0.984 (`iD`) and 0.981 (`iE`), 3 of 7
  pairs each, not mentioned under "costs on the main-loop path". This check has no i3 build; the
  final code against `main` there was 0.991 (2 of 7) and 0.994 (1 of 7).

**Not checked.** Everything that needs the intermediate builds (i1, i3, the p16/p32/p3536 and
p3536r layout controls): the hold knob's cost (`iC`), i3 against i1, i4 against i3, and the layout
sessions; the round's counts of commands ended by the removal before the knob; the vCPUs "busy
2.94 of 3" (not in `summary.md`); the GitHub runner.

## Files

`s1-ab/`, `s1-fixed/`, `s2-ab/`, `s2-fixed/`: each run's `plan.txt`, `boot-N/run.json` (build
hashes, quiet-window marker, load average), `windows.jsonl.gz`, `serial.log.gz`, `summary.md/json`
and `compare-<new>-vs-<base>[-<config>].md/json`. `matrix/`, `mutants/` (without serial logs and
build logs), `qtest-*.txt`, `readme-run-vm/run.json`, `ci-local.txt`, `builds.json` (the four
frozen builds' `build.json`), `rule.txt`, `claims.py`, `claims.md`, `check-d.sh`,
`run-phases.txt`. The full serial logs and build trees were deleted after the check.
