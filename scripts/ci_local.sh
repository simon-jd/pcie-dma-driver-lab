#!/bin/bash
# Run the build and test steps of .github/workflows/ci.yml from a fresh clone of
# this repository's current commit, with -j4 on the given CPUs, and print how long
# each step took. Usage: scripts/ci_local.sh OUT_DIR [CPU_LIST]
#
# Differences from the workflow: there is no apt step (install the packages the
# workflow lists, or set LOCAL_TOOLS to a prefix with flex, bison and libelf, which
# is passed to build_kernel.py --local-tools), and if SEED_DIR is set, the pinned
# QEMU, glib and Linux tarballs found there are copied in instead of downloaded
# (the scripts still check their SHA256).
set -euo pipefail
OUT=$(realpath -m "$1")
CPUS=${2:-0-3}
REPO=$(cd "$(dirname "$0")/.." && pwd)
rm -rf "$OUT"
mkdir -p "$OUT"
git clone -q "$REPO" "$OUT/repo"
cd "$OUT/repo"
echo "commit $(git log --oneline -1)"
mkdir -p .cache/downloads build
if [ -n "${SEED_DIR:-}" ]; then
  for f in qemu-10.2.1.tar.xz glib-2.86.5.tar.xz; do
    [ -e "$SEED_DIR/$f" ] && cp -L "$SEED_DIR/$f" .cache/downloads/
  done
  [ -e "$SEED_DIR/linux-6.12.41.tar.xz" ] && cp -L "$SEED_DIR/linux-6.12.41.tar.xz" build/
fi
TOOLS=()
[ -n "${LOCAL_TOOLS:-}" ] && TOOLS=(--local-tools "$LOCAL_TOOLS")
RUN=(env -i "HOME=$HOME" PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 taskset -c "$CPUS" nice)
step() {
  local name=$1 start
  shift
  start=$(date +%s)
  if ! "${RUN[@]}" "$@" > "$OUT/$name.log" 2>&1; then
    echo "STEP $name FAILED after $(( $(date +%s) - start )) s (log: $OUT/$name.log)"
    exit 1
  fi
  echo "STEP $name $(( $(date +%s) - start )) s"
}
total=$(date +%s)
step build_qemu python3 scripts/build_qemu.py --jobs 4
step qtest_asan python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu-asan/qemu-system-x86_64 --log logs/qtest-asan.log
step qtest_release python3 tests/qtest/kdc_qtest.py --qemu .cache/build/qemu/qemu-system-x86_64 --log logs/qtest-release.log
step fetch_kernel python3 scripts/fetch_kernel.py
step build_kernel python3 scripts/build_kernel.py --jobs 4 "${TOOLS[@]}"
step tcg_guest python3 scripts/run_vm.py --accel tcg --iommu --out logs/tcg-iommu
echo "TOTAL $(( $(date +%s) - total )) s on CPUs $CPUS ($(nproc --all) online, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs))"
tail -n1 logs/qtest-asan.log logs/qtest-release.log | grep KDC_QTEST
grep -h "^PASS\|^FAIL" "$OUT/tcg_guest.log"
