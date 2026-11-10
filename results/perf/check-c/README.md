# Check C: an independent rerun of the performance round

Fresh clones of `perf` (`1518384`), `main` (`b95f7ae`, with `scripts/snapshot_build.py`
copied in, since `main` predates it) and o3 (`68094ba`, the step before registered
buffers), on the same host on 2026-11-09. Every QEMU, kernel and client was built again
from source in the clone; nothing was reused from the round's own `.cache/`. Correctness
runs used CPUs 10-15 and 24-31 at nice 10; timing ran at nice 0 inside a quiet window
(`~/.cache/rework-quiet`), with the same plans, pinning and rejection thresholds as the
round.

## Commands

In the `perf` clone unless noted; `CHECK` is the check's working directory.

```sh
python3 scripts/build_qemu.py && python3 scripts/fetch_kernel.py --archive linux-6.12.41.tar.xz
python3 scripts/build_kernel.py --local-tools TOOLS && python3 scripts/build_kernel.py --local-tools TOOLS --variant kcsan
python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu-asan/qemu-system-x86_64 --log CHECK/qtest-asan.txt   # and the release QEMU
python3 scripts/run_matrix.py --name coord-perf                                   # -> matrix/
python3 scripts/run_matrix.py --name coord-perf-asan2 --only tcg-iommu-asanqemu   # -> matrix-asan2/ (and asan3)
python3 scripts/mutation_check.py --local-tools TOOLS --out CHECK/mutants         # -> mutants/
# in each clone: release QEMU, perf kernel, client, frozen as .cache/ab/{perf,main,o3}
python3 scripts/build_qemu.py --flavor release && python3 scripts/build_kernel.py --local-tools TOOLS --variant perf
python3 scripts/snapshot_build.py NAME
taskset -c 10-15,24-31 python3 scripts/run_bench.py --plan bench/ab.plan --builds main,o3,perf --rounds 7 --out CHECK/bench-final   # -> final/
taskset -c 10-15,24-31 python3 scripts/run_bench.py --plan bench/fixed.plan --builds main,perf --configs noiommu,iommu-strict \
  --rounds 7 --skip-arms main=fixed4k-qd1,fixed64k-qd1,fixed4k-qd32-batch,fixed64k-qd8-batch --out CHECK/bench-fixed       # -> fixed/
python3 scripts/bench_compare.py CHECK/bench-final --base main --new perf --tag vs-main     # and --base o3
python3 scripts/bench_compare.py CHECK/bench-fixed --base perf --new perf --config iommu-strict --tag fixed-iommu-strict \
  --pair fixed4k-qd1:bounce4k-qd1 --pair fixed64k-qd1:bounce64k-qd1 \
  --pair fixed4k-qd32-batch:bounce4k-qd32-batch --pair fixed64k-qd8-batch:bounce64k-qd8-batch   # and noiommu
```

The three experiments use hybrid frozen builds: a directory under `.cache/ab/` holding
one build's `bzImage` and `kdc_bench` with another build's `qemu-system-x86_64` and
`qemu-bundle` link, and a `build.json` whose `files` hashes match (each boot's
`run.json` records them under `build_info`). The plan is `swap-qemu/plan.txt`: the
warm-up and six arms of `bench/ab.plan`.

| Directory | Builds (`--rounds 7`) | Question |
|---|---|---|
| `swap-qemu/` | o3, perf, `pkoq` (perf kernel and client, o3's QEMU), `okpq` (o3 kernel and client, perf's QEMU) | QEMU binary or guest? |
| `swap-client/` | o3, perf, `okpc` (o3 kernel, perf client), `pkoc` (perf kernel, o3 client), both on perf's QEMU | kernel or client? |
| `reorder/` | o3, perf, `o5b` (perf with `reorder.diff` applied, full `bench/ab.plan`) | struct layout? |

`reorder.diff` applies to `1518384`; the `kernel/kdc_drv.h` it produces has the hash
recorded in `reorder/boot-2/run.json`. The matrix started on `o5b` afterwards was
stopped once the timing showed no gain; it is not reported.

## Results

Correctness: qtest 20/20 on the ASan+UBSan and release QEMU; all six guest
configurations pass with KUnit 13/13, 37/37 client cases and kmemleak 0 (`matrix/`);
the ASan QEMU configuration passed in all three runs, its `removal_surprise` ending
9, 10 and 15 of the commands by the removal (27 to 36 in the other configurations,
so its margin there is the smallest); 14 of 14 deliberate bugs caught and the restored
build passes (`mutants/`).

Before and after (`final/compare-perf-vs-main-vs-main.md`), median over 7 pairs, every
arm 7 of 7:

| Arm | main k/s | perf k/s | ratio | round (`perf/final`) |
|---|---|---|---|---|
| NOP one at a time | 112.7 | 178.0 | 1.576 | 1.541 |
| 4 KiB one at a time | 104.8 | 151.8 | 1.448 | 1.422 |
| 4 KiB, 32 in flight | 131.4 | 567.7 | 4.293 | 4.381 |
| 4 KiB, 32 batched | 554.3 | 691.6 | 1.245 | 1.230 |
| 4 KiB, three threads | 202.9 | 948.9 | 4.675 | 4.748 |
| 4 KiB, three threads, one file | 174.0 | 485.9 | 2.807 | 2.777 |
| NOP, three threads, one file | 187.9 | 518.6 | 2.788 | 2.675 |

Registered buffers against bounce in the same build (`fixed/`), 7 of 7 pairs (32
batched without an IOMMU 6 of 6): without an IOMMU 4 KiB one at a time 160.1k vs
139.4k/s (1.14x), 64 KiB 1.74x, 32 batched 1.24x, 64 KiB 8 batched 1.71x; behind the
strict emulated IOMMU 148.4k vs 133.9k/s (1.11x), 1.61x, 1.26x and 1.60x. `main`'s best
path there, bounce, did 98.6k/s.

The final build against o3 on the paths that do not use registered buffers, 4 KiB with
32 in flight and a doorbell each (median ratio, pairs faster of 7):

| Session | perf vs o3 | Also measured |
|---|---|---|
| round, `perf/final` | 0.991, 1 | |
| `final/` | 0.963, 0 | |
| `swap-qemu/` | 0.985, 2 | pkoq vs o3 0.982, 1; okpq vs o3 1.003, 6 |
| `reorder/` | 0.981, 3 | o5b vs o3 0.990, 0; o5b vs perf 0.990, 3 |
| `swap-client/` | 0.983, 1 | okpc vs perf 1.023, 6; pkoc vs o3 0.984, 2 |

The loss follows the guest kernel: o3's kernel is as fast as o3 on either QEMU binary
and with either client, the final kernel is slower with either. Moving the new fields
to the ends of `kdc_cmd` and `kdc_ctx` restored the old field offsets (checked with
pahole on a `-g` build of `kdc_fops.o`) but not the speed. See
[design trade-offs](../../../docs/design-tradeoffs.md), change 5.
