# pcie-dma-driver-lab

A PCIe DMA engine written as a QEMU device model (`kdc-dma`), and the Linux
6.12 PCI driver for it. The device copies a source scatter list to a destination
scatter list and returns the CRC32C; the driver moves user buffers through it
with pinned zero-copy DMA, a bounce buffer or registered buffers, MSI-X
completions, and a per-file contract for who owns a buffer while the device may
still write to it.

Scope: the device model, the driver and their tests (cancel, reset, close,
timeouts escalating to controller reset and FLR, hot-unplug under load, DMA
faults with and without an IOMMU, a no-overlapping-destinations rule), a KVM
study of what each command costs, and a round of optimizations along that path.
The hardware exists only in QEMU; nothing here ran on a physical device.

| KVM, 4 KiB copies unless noted (commands/s, p50) | published `main` | this branch |
| --- | --- | --- |
| one command at a time; NOP | 106.7k, 9.1 us; 115.3k | 152.1k, 6.1 us; 177.2k |
| 32 in flight: a doorbell each; batched | 133.2k; 559.0k | 577.8k (4.4x); 689.2k (1.2x) |
| three threads, 8 in flight each: own files; one file | 201.0k; 174.4k | 951.6k (4.7x); 486.3k (2.8x) |
| VM exits per command, one at a time (to QEMU) | 3.65 (2.00) | 2.70 (0.06) |
| strict emulated IOMMU, best path: one at a time; 32 batched | bounce 98.1k; 453.3k | registered buffer 148.0k; 660.5k |
| Losses | | registered-buffer code costs the paths that do not use it 1–4% at 32 in flight (five sessions); one file still half of three; batching now slower than not at three threads (852.2k) |
| Correctness | | qtest 20/20 on both QEMU builds; KUnit 13/13 and 37/37 client cases on KVM and TCG, IOMMU strict or none, ASan QEMU, KCSAN; 14 of 14 deliberate bugs caught |

What changed: the CQ head is written every 16 completions; the device sends two
MSI-X messages per processing pass instead of one per completion; SQ tails go through
a shadow doorbell page with an event index and an ioeventfd, so a doorbell no longer
exits to QEMU and a busy device needs none. Each of these beat the previous step in
seven paired boots in one quiet window. Registered buffers, pinned and mapped once,
did not: the paths that do not use them got 1–4% slower at 32 in flight in five
sessions, the cost is in the guest kernel (swapping the QEMU binaries or the benchmark
clients did not move it), and moving the new fields to the ends of the structs did not
remove it. They are kept for what they give the paths that use them, 1.1x (4 KiB) to
1.7x (64 KiB) over bounce in the same build. Waking one reader per completion did not
pay and was reverted. An independent rerun from fresh clones
([check C](results/perf/check-c/README.md)) reproduced every before/after ratio in the
table within 3% and the correctness results. Numbers are the
guest driver plus KVM exits plus QEMU's device model around a modeled service time of
0, not device throughput ([design trade-offs](docs/design-tradeoffs.md#performance-round-2026-11-09-optimizing-along-the-command-path),
[results](results/README.md)).

Limitations: emulated device and IOMMU only, one host; QEMU's single main loop runs
the device model and is 92% busy at three threads, so multi-queue numbers describe
QEMU as much as the driver; no partial writes after an abort; one Linux version; no
AER or io_uring path. Details: [driver ownership and recovery](docs/driver-ownership.md),
[device interface](docs/device-interface.md).

## Layout

- `qemu/kdc_dma.c` - the device model, copied into a pinned QEMU 10.2.1 tree by `scripts/build_qemu.py`
- `include/kdc_hw.h` - register map and descriptor formats, shared by QEMU, the driver and the qtest suite
- `include/kdc_abi.h` - the `/dev/kdcN` ioctl ABI (version 3, plus registered buffers)
- `kernel/` - the driver, built into a pinned Linux 6.12.41: `kdc_pci.c` (probe, remove, rings, interrupts),
  `kdc_recover.c` (timeouts, resets, failure), `kdc_cmd.c` (pinning, DMA mapping, registered buffers, overlap tracker),
  `kdc_fops.c` (the per-file contract), `kdc_state.h`/`kdc_ring.h` (pure logic) and `kdc_kunit.c`
- `tests/qtest/kdc_qtest.py` - device-model tests through QEMU's qtest protocol, no guest kernel
- `tests/kdc_test.c` - static in-guest test client; `tests/kdc_bench.c` - in-guest benchmark client
- `scripts/` - fetch and build QEMU and the kernel, boot the guest, run the matrix, the deliberate bugs
  and the KVM benchmark (`run_bench.py`, `bench_report.py`; `snapshot_build.py` and `bench_compare.py`
  for A/B runs of frozen builds)
- `bench/` - benchmark plans; `.github/workflows/ci.yml` - CI

## Build and test (Linux x86-64 host)

Needs Python 3.12+, GCC, make, ninja, static BusyBox, network access for the
pinned downloads, and for the kernel flex, bison, libelf and OpenSSL headers.
QEMU's only library dependency, glib, is built from a pinned tarball into `.cache/`.

```sh
python3 scripts/build_qemu.py                 # QEMU 10.2.1 release + ASan/UBSan builds in .cache/ (SHA256-checked)
python3 scripts/fetch_kernel.py               # Linux 6.12.41, SHA256-checked (--archive FILE to reuse a download)
python3 scripts/build_kernel.py               # KASAN kernel + client; add --local-tools DIR if flex/bison/libelf live there
python3 scripts/build_kernel.py --variant kcsan   # the KCSAN kernel (used by run_matrix.py)
python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu-asan/qemu-system-x86_64 --log /tmp/qtest.log
python3 scripts/run_vm.py --accel tcg --iommu --out /tmp/kdc-run   # or --no-iommu, --qemu asan, --kernel kcsan, --accel kvm
python3 scripts/run_matrix.py --name my-run   # qtest x2 + every guest configuration into results/my-run/
python3 scripts/mutation_check.py --out /tmp/mutants   # deliberate bugs must fail named checks
python3 scripts/build_kernel.py --variant perf    # kernel without debug options, for timing only
python3 scripts/run_bench.py --plan bench/crc.plan --configs noiommu,tablecrc --out /tmp/kdc-bench   # needs /dev/kvm
python3 scripts/bench_report.py /tmp/kdc-bench    # medians per arm into summary.md
python3 scripts/snapshot_build.py base            # freeze this timing build (QEMU, perf kernel, client) as .cache/ab/base
python3 scripts/run_bench.py --plan bench/ab.plan --builds base,tree --rounds 7 --out /tmp/kdc-ab   # boots alternate
python3 scripts/bench_compare.py /tmp/kdc-ab --base base --new tree --target zc4k-qd1   # pairs boots, applies the rule
```

`run_bench.py` pins the vCPUs to host CPUs 2, 4 and 6 and QEMU's main loop to
CPU 8 by default (`--vcpus`, `--main-cpu`, `--watch-cpus`) and runs QEMU at nice 0;
`--builds tree` is the checkout's own build. The published runs and their exact
commands are listed in [results/README.md](results/README.md).

CI (`.github/workflows/ci.yml`, `ubuntu-24.04`) builds QEMU with the device,
runs the qtest suite on both builds and builds the KASAN kernel with the
driver and the clients, from the pinned versions and checksums in `scripts/`,
then runs the guest suite under TCG with a strict IOMMU. From a fresh clone the
same steps took 360 s on four threads of the build host (`scripts/ci_local.sh`,
[results/perf/ci-local.txt](results/perf/ci-local.txt)); on a GitHub runner the
first run of the published commit took 21 minutes.

A guest run fails on any kernel, sanitizer, lockdep, DMA-API, DMAR or driver
report outside the windows in which a test expects one, on a kmemleak record
with a driver frame, and on any sanitizer report from QEMU.
