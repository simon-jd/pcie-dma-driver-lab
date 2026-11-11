#!/bin/bash
# Check D of the kernel lab's improvement round: fresh clones of improve (7793fab), main (1a8de5c),
# o3 (68094ba) and r1c (eaf9e60, registered buffers moved out) under ~/.cache/kl-check-d, builds from source, two timing sessions in quiet
# windows at least an hour apart, and the correctness runs (qtest, matrix, mutants, ci_local.sh).
set -u
V=$HOME/.cache/kl-check-d
O=$V/out
T=$HOME/Desktop/driver-firnware-runtime/.cache/tools          # flex, bison, libelf (read only)
K=$HOME/Desktop/driver-firnware-runtime/.cache/downloads/linux-6.12.41.tar.xz   # pinned tarball (read only)
M=$HOME/.cache/rework-quiet
P="taskset -c 10-15,24-31 nice -n 10"
TAG=kl-check-d
mkdir -p "$O"
mine() { grep -q "^$TAG " "$M" 2>/dev/null; }
wait_quiet() {
  local said=0
  while [ -e "$M" ] && [ $(( $(date +%s) - $(stat -c %Y "$M") )) -lt 7200 ] && ! mine; do
    [ $said = 0 ] && echo "... waiting for quiet window: $(cat "$M") $(date -Is)"; said=1; sleep 30
  done
}
phase() {
  local name=$1; shift
  wait_quiet
  echo "=== $name start $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg)"
  local t0=$(date +%s)
  "$@" > "$O/$name.log" 2>&1; local rc=$?
  echo "=== $name rc=$rc $(( $(date +%s) - t0 )) s $(date -Is)"; tail -n 3 "$O/$name.log"
  return $rc
}
die() { mine && rm -f "$M"; echo "=== DONE (stopped: $1) $(date -Is)"; exit 1; }
timing_build() {  # clone name: release QEMU + perf kernel + client, frozen as .cache/ab/<name>, copied into the improve clone
  local n=$1
  ( cd "$V/$n" && $P python3 scripts/build_qemu.py --flavor release && $P python3 scripts/fetch_kernel.py --archive "$K" \
    && $P python3 scripts/build_kernel.py --local-tools "$T" --variant perf && python3 scripts/snapshot_build.py "$n" ) || return 1
  cp -a "$V/$n/.cache/ab/$n" "$V/improve/.cache/ab/$n"
}
take_window() {
  while :; do
    wait_quiet
    [ -e "$M" ] && [ $(( $(date +%s) - $(stat -c %Y "$M") )) -ge 7200 ] && ! mine && rm -f "$M"
    ( set -o noclobber; echo "$TAG $(date -Is) $(date -d '+50 min' -Is)" > "$M" ) 2>/dev/null && break
    sleep 30
  done
  echo "=== window taken $(date -Is): $(cat "$M")"
  sleep 60
}
session() {  # s1 or s2
  local s=$1
  take_window
  date +%s > "$O/$s.start"
  phase $s-ab taskset -c 10-15,24-31 python3 scripts/run_bench.py --plan bench/ab.plan --builds main,o3,r1c,improve \
    --configs noiommu,iothread1,iothreads --skip-configs main=iothread1,iothreads --skip-configs o3=iothread1,iothreads \
    --skip-configs r1c=iothread1,iothreads \
    --rounds 7 --out "$O/$s-ab"
  phase $s-fixed taskset -c 10-15,24-31 python3 scripts/run_bench.py --plan bench/fixed.plan --builds main,improve \
    --configs noiommu,iommu-strict --rounds 7 --out "$O/$s-fixed"
  mine && rm -f "$M"
  echo "=== window released $(date -Is)"
  python3 scripts/bench_report.py "$O/$s-ab" > "$O/$s-report-ab.log" 2>&1
  python3 scripts/bench_report.py "$O/$s-fixed" > "$O/$s-report-fixed.log" 2>&1
  { python3 scripts/bench_compare.py "$O/$s-ab" --base main --new improve
    python3 scripts/bench_compare.py "$O/$s-ab" --base main --new improve --new-config iothread1
    python3 scripts/bench_compare.py "$O/$s-ab" --base main --new improve --new-config iothreads
    python3 scripts/bench_compare.py "$O/$s-ab" --base improve --new improve --new-config iothread1
    python3 scripts/bench_compare.py "$O/$s-ab" --base improve --new improve --new-config iothreads
    python3 scripts/bench_compare.py "$O/$s-ab" --base o3 --new improve
    python3 scripts/bench_compare.py "$O/$s-ab" --base o3 --new main
    python3 scripts/bench_compare.py "$O/$s-ab" --base o3 --new r1c
    python3 scripts/bench_compare.py "$O/$s-ab" --base main --new r1c
    python3 scripts/bench_compare.py "$O/$s-fixed" --base main --new improve
    python3 scripts/bench_compare.py "$O/$s-fixed" --base main --new improve --config iommu-strict
  } > "$O/$s-compare.log" 2>&1
}

STAGE=${1:-all}
test -r /dev/kvm -a -w /dev/kvm || die "no /dev/kvm"
cd "$V/improve" || die cd
for r in improve main o3 r1c; do echo "$r HEAD $(git -C "$V/$r" log --oneline -1) dirty=$(git -C "$V/$r" status --porcelain | wc -l)"; done
if [ "$STAGE" = all ]; then
  phase build_qemu   $P python3 scripts/build_qemu.py || die build_qemu
  phase fetch_kernel $P python3 scripts/fetch_kernel.py --archive "$K" || die fetch_kernel
  phase build_kasan  $P python3 scripts/build_kernel.py --local-tools "$T" || die build_kasan
  phase build_kcsan  $P python3 scripts/build_kernel.py --local-tools "$T" --variant kcsan || die build_kcsan
  phase build_perf   $P python3 scripts/build_kernel.py --local-tools "$T" --variant perf || die build_perf
  phase snap_improve python3 scripts/snapshot_build.py improve || die snap_improve
  phase snap_main timing_build main || die snap_main
  phase snap_o3   timing_build o3 || die snap_o3
  phase snap_r1c  timing_build r1c || die snap_r1c
  phase qtest_asan    $P python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu-asan/qemu-system-x86_64 --log "$O/qtest-asan.txt"
  phase qtest_release $P python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu/qemu-system-x86_64 --log "$O/qtest-release.txt"
  STAGE=s1
fi
if [ "$STAGE" = s1 ]; then session s1; STAGE=correct; fi
if [ "$STAGE" = correct ]; then
  phase readme_run_vm $P python3 scripts/run_vm.py --accel tcg --iommu --out "$O/readme-run-vm"
  phase matrix  $P python3 scripts/run_matrix.py --name check-d-matrix
  phase mutants $P python3 scripts/mutation_check.py --local-tools "$T" --out "$O/mutants"
  mkdir -p "$V/seed" && cp -L .cache/downloads/qemu-10.2.1.tar.xz .cache/downloads/glib-2.86.5.tar.xz "$K" "$V/seed/"
  phase ci_local env SEED_DIR="$V/seed" LOCAL_TOOLS="$T" scripts/ci_local.sh "$O/ci-local" 12-15
  STAGE=s2
fi
if [ "$STAGE" = s2 ]; then
  s1=$(cat "$O/s1.start")
  while [ $(( $(date +%s) - s1 )) -lt 3900 ]; do sleep 60; done
  session s2
fi
echo "=== DONE $(date -Is)"
