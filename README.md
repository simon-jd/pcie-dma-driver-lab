# pcie-dma-driver-lab

A PCIe DMA engine written as a QEMU device model (`kdc-dma`), and the Linux
6.12 PCI driver for it. The device copies a source scatter list to a destination
scatter list and returns the CRC32C; the driver moves user buffers through it
with pinned zero-copy DMA or a bounce buffer, MSI-X completions, and a per-file
contract for who owns a buffer while the device may still write to it.

Scope: the device model, the driver and their tests (cancel, reset, close,
timeouts escalating to controller reset and FLR, hot-unplug under load, DMA
faults with and without an IOMMU, a no-overlapping-destinations rule), and a KVM
study of what each command costs the driver and the virtualization around it.
The hardware exists only in QEMU; nothing here ran on a physical device.

| Check (one machine) | Result |
| --- | --- |
| qtest device suite, ASan+UBSan QEMU and release QEMU | 17 of 17 tests, both |
| Guest suite: KUnit + 34 client cases on KVM and TCG, each with a strict Intel IOMMU and without, on the ASan QEMU, and with a KCSAN kernel | 6 of 6 configurations pass; no KASAN, KCSAN, lockdep, DMA-API or kmemleak report |
| Hung device: abort ignored, then the controller reset ignored; `close()` and `KDC_IOC_RESET` meanwhile | ended by a controller reset (765 ms) or an FLR, buffers pinned until the device stopped; close and reset return after 567 and 561 ms instead of never |
| Hot-unplug with 3 threads and 5 files busy; a device that starts reading all-ones | every command completed exactly once, nothing leaked |
| Deliberate bugs (8 from earlier rounds, 1 new for coalescing) | 9 of 9 caught by named checks |
| KVM, one 4 KiB command at a time | 9.2 us round trip (104.7k/s); 3.65 VM exits per command: doorbell and completion-head write (MMIO), one HLT |
| KVM, 32 commands in flight | 136.0k/s with a doorbell per command, 539.2k/s with batched doorbells (0.11 exits per command) |
| Loss: queue depth without batching | 2 to 32 in flight all stay at 128k-136k/s |
| Zero-copy vs bounce | tie at 2 KiB, zero-copy 1.4x faster at 64 KiB; behind the emulated strict IOMMU bounce wins up to 64 KiB, so `bounce_max` now defaults to 2048 or 65536 |
| Loss: interrupt coalescing | up to +75% at depth 32, but a lone command waits for the delay (104.7k/s falls to 45.3k/s): off by default |
| 3 threads on 3 queues | 1.5x one thread; one shared file loses 15-25%; the overlap tracker costs at most 7% |

Correctness rows come from `results/round3/matrix/kvm-iommu`, timing rows from
`results/round3/perf/` (main, iommu, tracker). Timing numbers are the guest
driver plus KVM exits plus QEMU's device model around a modeled service time
(0 here), with pinned CPUs and a kernel without debug options; they are not
device throughput. Method, the rejection of windows disturbed by other load on
the shared host, and every number: [design trade-offs](docs/design-tradeoffs.md),
[results](results/README.md).

Limitations: emulated device and IOMMU only, one host; QEMU runs the device
model under its global lock, so the multi-queue numbers describe QEMU as much as
the driver; no partial writes after an abort; one Linux version; PCIe AER and an
io_uring path are not done. Details: [driver ownership and recovery](docs/driver-ownership.md),
[device interface](docs/device-interface.md).

## Layout

- `qemu/kdc_dma.c` - the device model, copied into a pinned QEMU 10.2.1 tree by `scripts/build_qemu.py`
- `include/kdc_hw.h` - register map and descriptor formats, shared by QEMU, the driver and the qtest suite
- `include/kdc_abi.h` - the `/dev/kdcN` ioctl ABI (version 3)
- `kernel/` - the driver, built into a pinned Linux 6.12.41: `kdc_pci.c` (probe, remove, rings, interrupts),
  `kdc_recover.c` (timeouts, resets, failure), `kdc_cmd.c` (pinning, DMA mapping, overlap tracker),
  `kdc_fops.c` (the per-file contract), `kdc_state.h`/`kdc_ring.h` (pure logic) and `kdc_kunit.c`
- `tests/qtest/kdc_qtest.py` - device-model tests through QEMU's qtest protocol, no guest kernel
- `tests/kdc_test.c` - static in-guest test client; `tests/kdc_bench.c` - in-guest benchmark client
- `scripts/` - fetch and build QEMU and the kernel, boot the guest, run the matrix, the deliberate bugs
  and the KVM benchmark (`run_bench.py`, `bench_report.py`)
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
```

`run_bench.py` pins the vCPUs to host CPUs 2, 4 and 6 and QEMU's main loop to
CPU 8 by default (`--vcpus`, `--main-cpu`, `--watch-cpus`); the published runs
and their exact commands are listed in [results/README.md](results/README.md).

CI (`.github/workflows/ci.yml`, `ubuntu-24.04`) builds QEMU with the device,
runs the qtest suite on both builds and builds the KASAN kernel with the
driver and the clients, from the pinned versions and checksums in `scripts/`,
then runs the guest suite under TCG with a strict IOMMU. From a fresh clone the
same steps took 363 s on four threads of the build host (`scripts/ci_local.sh`,
[results/round3/ci-local.txt](results/round3/ci-local.txt)); the time on a
GitHub runner is not known until the workflow first runs there.

A guest run fails on any kernel, sanitizer, lockdep, DMA-API, DMAR or driver
report outside the windows in which a test expects one, on a kmemleak record
with a driver frame, and on any sanitizer report from QEMU.
