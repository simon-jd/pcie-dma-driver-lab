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
study of what each command costs, and two rounds of optimizations along that path.
The hardware exists only in QEMU; nothing here ran on a physical device.

The previous `main` (`1a8de5c`) against this branch, seven paired boots in one quiet window
(session `iE`; the IOMMU row `iDfixed`; check D reran both from fresh clones in two
sessions); the queues run in QEMU's main loop unless `iothreads=` is set:

| KVM, 4 KiB unless noted | previous `main` (`1a8de5c`) | this branch | this branch, one IOThread |
| --- | --- | --- | --- |
| one at a time; NOP | 150.0k, 6.2 us; 175.4k | 149.3k (0.99x); 176.1k | 176.2k (1.17x), 5.3 us; 212.2k (1.21x) |
| 32 in flight, a doorbell each; batched | 568.1k; 670.7k | 621.4k (1.10x); 690.2k (1.02x) | 522.0k (0.93x); 710.1k (1.07x) |
| three threads, 8 in flight each: own files; batched | 923.2k; 830.1k | 938.9k (1.00x); 803.7k (0.99x) | 1266.5k (1.38x); 1245.9k (1.50x) |
| three threads on one file: NOP; 4 KiB | 512.1k; 484.2k | 718.6k (1.39x); 614.5k (1.28x) | 568.6k (1.10x); 504.0k (1.07x) |
| strict emulated IOMMU, registered buffers: one at a time; 32 batched | 149.3k; 687.8k | 149.5k (1.00x); 700.7k (1.01x; 1.03x and 1.02x in check D) | not measured |
| Losses | | three threads 0.98x and batched three threads 0.99x in `iD` (1.00x, 0.99x in `iE`); 64 KiB one at a time 0.985 in `iE` and 0.96x in both check D sessions (5 and 6 of 7 pairs slower), in which some of this branch's boots ran 9% slower; in `iDfixed` without an IOMMU, 4 KiB one at a time 0.99x (check D: 0.95x with 4 of 6 pairs slower, 0.98x) and 4 KiB bounce one at a time 0.99x (check D: 0.99x, 1.01x) | 32 in flight 0.93x and one file 1.07-1.10x (1.09-1.17x in check D) instead of 1.28-1.39x (1.25-1.45x in check D): a device that keeps up makes the driver ring and interrupt more often |
| Correctness | | qtest 42/42 on both QEMU builds (21 tests, in the main loop and in IOThreads); KUnit 13/13 and 37/37 client cases in 10 configurations: KVM and TCG, IOMMU strict or none, ASan QEMU, KCSAN, and four with IOThreads; 16 of 16 deliberate bugs caught | in the same matrix |

What changed: the removal tests hold a known number of commands in the device, so they no
longer depend on timing; the registered-buffer code moved out of the functions the other
paths run, which took back part of the 1-4% it cost them; a file's commands are found
through a hash and `read()` takes a batch of completions in two lock rounds instead of two
per completion (one shared file 1.28x-1.39x, 1.25x-1.45x in check D); and the queues can
run in IOThreads without the BQL. One IOThread wins where QEMU's main loop was the limit
and loses where a busy device used to batch for free, so it is an option, not the default.
Numbers are the guest driver plus KVM exits plus QEMU's device model around a modeled
service time of 0, not device throughput; the IOThread column uses one more host CPU
([design trade-offs](docs/design-tradeoffs.md#improvement-round-2026-11-10-what-the-performance-round-left-open), [results](results/README.md)).

Limitations: emulated device and IOMMU only, one host; IOThread mode refuses TCG behind an
emulated IOMMU and refuses DMA to anything but RAM; no partial writes after an abort; one
Linux version; no AER or io_uring path. Details: [driver ownership and recovery](docs/driver-ownership.md),
[device interface](docs/device-interface.md).

## Layout

- `qemu/kdc_dma.c` - the device model, copied into a pinned QEMU 10.2.1 tree by `scripts/build_qemu.py`
- `include/kdc_hw.h` - register map and descriptor formats, shared by QEMU, the driver and the qtest suite
- `include/kdc_abi.h` - the `/dev/kdcN` ioctl ABI (version 3, plus registered buffers)
- `kernel/` - the driver, built into a pinned Linux 6.12.41: `kdc_pci.c` (probe, remove, rings, interrupts),
  `kdc_recover.c` (timeouts, resets, failure), `kdc_cmd.c` (pinning, DMA mapping, overlap tracker),
  `kdc_fops.c` (the per-file contract), `kdc_fixed.c` (registered buffers and their commands),
  `kdc_state.h`/`kdc_ring.h` (pure logic) and `kdc_kunit.c`
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
python3 scripts/run_vm.py --accel tcg --iommu --out /tmp/kdc-run   # or --no-iommu, --qemu asan, --kernel kcsan, --accel kvm, --iothreads 3
python3 scripts/run_matrix.py --name my-run   # qtest x2 + every guest configuration into results/my-run/
python3 scripts/mutation_check.py --out /tmp/mutants   # deliberate bugs must fail named checks
python3 scripts/build_kernel.py --variant perf    # kernel without debug options, for timing only
python3 scripts/run_bench.py --plan bench/crc.plan --configs noiommu,tablecrc --out /tmp/kdc-bench   # needs /dev/kvm
python3 scripts/bench_report.py /tmp/kdc-bench    # medians per arm into summary.md
python3 scripts/snapshot_build.py base            # freeze this timing build (QEMU, perf kernel, client) as .cache/ab/base
python3 scripts/run_bench.py --plan bench/ab.plan --builds base,tree --rounds 7 --out /tmp/kdc-ab   # boots alternate
python3 scripts/bench_compare.py /tmp/kdc-ab --base base --new tree --target zc4k-qd1   # pairs boots, applies the rule
python3 scripts/run_bench.py --plan bench/ab.plan --configs noiommu,iothread1 --rounds 7 --out /tmp/kdc-io   # main loop vs IOThread
python3 scripts/bench_compare.py /tmp/kdc-io --base tree --new tree --new-config iothread1   # pairs the two configurations
```

`run_bench.py` pins the vCPUs to host CPUs 2, 4 and 6 and QEMU's main loop to
CPU 8 by default (`--vcpus`, `--main-cpu`, `--watch-cpus`) and runs QEMU at nice 0;
the configuration `iothread1` adds one IOThread for the device on CPU 9, `iothreads`
three (one per vCPU, on CPUs 3, 5, 7, the vCPU cores' SMT siblings);
`--builds tree` is the checkout's own build. The published runs and their exact
commands are listed in [results/README.md](results/README.md).

CI (`.github/workflows/ci.yml`, `ubuntu-24.04`) builds QEMU with the device, runs
the qtest suite on both builds (each test in the main loop and with IOThreads)
and builds the KASAN kernel with the driver and the clients, from the pinned
versions and checksums in `scripts/`, then runs the guest suite under TCG with a
strict IOMMU. From a fresh clone the same steps took 407 s on four threads of the
build host (`scripts/ci_local.sh`,
[results/improve/ci-local.txt](results/improve/ci-local.txt)); on a GitHub runner
the first run of the first published commit took 21 minutes and the improvement
round's code (`8009a51`) 9 minutes on its branch and 14 on `main`, all steps
passing.

A guest run fails on any kernel, sanitizer, lockdep, DMA-API, DMAR or driver
report outside the windows in which a test expects one, on a kmemleak record
with a driver frame, and on any sanitizer report from QEMU.
