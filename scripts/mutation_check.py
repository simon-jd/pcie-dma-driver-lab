#!/usr/bin/env python3
"""Deliberate-bug checks: break the built code on purpose and require a named failure.

Each mutant is one or more text replacements at unique anchors, applied only to
the copy of the source inside the build trees (build/linux-6.12.41/drivers/misc/kdc
or .cache/src/qemu-10.2.1/hw/misc); the repository sources are never touched.
A mutant counts as caught only if the run fails *and* the expected named check
fires: the first failed client assertion (kdc_test: FAIL <case>:<line>), a qtest
assertion, or, for memory-safety bugs, the KASAN report naming the access. A
build error, timeout or unrelated panic does not count. The correct code is
restored and rebuilt afterwards, and the restored build must pass again.

Mutants:
  unmap-before-completion  driver unmaps and unpins zero-copy buffers right
                           after building the SG lists, before the device runs
  cancel-trusts-abort      driver treats a device abort as synchronous: cancel
                           of a device-owned command releases it immediately
  device-skips-sgl-sum     device model accepts SG lists whose lengths do not
                           add up to the command length (qtest suite)
  release-before-reset     when an abort times out, the driver releases the
                           command's buffers before the device has been reset
  abort-never-escalates    an unanswered abort never leads to a reset, so close
                           and KDC_IOC_RESET can wait forever
  remove-frees-queues      remove() frees the per-queue array that commands of
                           still-open files point into
  overlap-check-off        the destination overlap check is skipped (caught by
                           DMA-API debug's overlapping-mapping report in fixed_buffers)
  read-leaks-command       read() never frees a consumed command (a positive
                           control for the kmemleak scan after the suite)
  coalesce-ignored         the debugfs coalescing setting is stored but never
                           written to the queues
  cq-head-never-written    the driver counts reaped completions toward the
                           batched CQ head doorbell but never writes the head
  pass-end-irq-skipped     device model raises the first interrupt of a
                           processing pass but not the one after its last
                           completion (qtest suite)
  shadow-ring-only-idle    with shadow doorbells the driver rings only when
                           the queue had nothing outstanding, assuming a busy
                           device looks at the shadow tail by itself
  event-index-not-published  device model never writes the event index, so a
                           driver would skip doorbells an idle device needs
                           (qtest suite)
  unregister-in-use        KDC_IOC_UNREGISTER unmaps a registered buffer that
                           a command still in the device uses (the driver's
                           WARN in kdc_regbuf_destroy is the named check)
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys
import types

from build_kernel import BUILD, SOURCE, environment
import run_vm

ROOT = pathlib.Path(__file__).resolve().parents[1]
KDC_TREE = SOURCE / "drivers/misc/kdc"
QEMU_FILE = ROOT / ".cache/src/qemu-10.2.1/hw/misc/kdc_dma.c"
WATCH_RESET = "\t\t\tcase KDC_WATCH_RESET:\n\t\t\t\treset = true;\n\t\t\t\tbreak;\n"

MUTANTS = {
    "unmap-before-completion": dict(
        file=KDC_TREE / "kdc_cmd.c",
        edits=[("\tcmd->sqe.dst_nents = cpu_to_le16(n);\n\treturn 0;\n",
                "\tcmd->sqe.dst_nents = cpu_to_le16(n);\n"
                "\t{ /* deliberate bug: release data buffers before the device used them */\n"
                "\t\tvoid *keep = cmd->sgl;\n\n\t\tcmd->sgl = NULL;\n"
                "\t\tkdc_cmd_release(kdev, cmd, false);\n\t\tcmd->sgl = keep;\n\t}\n\treturn 0;\n")],
        # Without an IOMMU the device still reaches the (unpinned) pages, but
        # since round 2 the early release also drops the bounced partial cache
        # lines, so check_copy usually sees wrong bytes before the recycling
        # test runs; either check names this bug.
        runs=[("tcg-iommu", True, ["FAIL check_copy:"], [r"DMAR: \[DMA Read"]),
              ("tcg-noiommu", False, ["FAIL check_copy:", "FAIL pinned_page_not_recycled:"], [])]),
    "cancel-trusts-abort": dict(
        file=KDC_TREE / "kdc_fops.c",
        edits=[("\t} else if (action == KDC_CANCEL_SEND_ABORT) {\n"
                "\t\tkdc_send_abort_locked(q, cmd, KDC_ABORT_CANCEL);\n\t}\n",
                "\t} else if (action == KDC_CANCEL_SEND_ABORT) {\n"
                "\t\tkdc_send_abort_locked(q, cmd, KDC_ABORT_CANCEL);\n"
                "\t\t/* deliberate bug: assume the abort took effect at once */\n"
                "\t\tWRITE_ONCE(cmd->phase, KDC_COMPLETING);\n\t\taction = KDC_CANCEL_NOW;\n\t}\n")],
        runs=[("tcg-iommu", True, ["FAIL cancel_device_owned:"], [])]),
    "device-skips-sgl-sum": dict(
        file=QEMU_FILE,
        edits=[("    return total == length ? nents : -KDC_SC_INVALID_SGL;\n",
                "    return nents; /* deliberate bug: no length check */\n")],
        qtest=("malformed", "entries short of length")),
    "release-before-reset": dict(
        file=KDC_TREE / "kdc_recover.c",
        edits=[(WATCH_RESET,
                "\t\t\tcase KDC_WATCH_RESET:\n\t\t\t\treset = true;\n"
                "\t\t\t\t/* deliberate bug: hand the buffers back once the abort timed out */\n"
                "\t\t\t\tq->slots[tag] = NULL;\n\t\t\t\t__clear_bit(tag, q->tag_map);\n"
                "\t\t\t\tq->outstanding--;\n\t\t\t\tkdc_cmd_end(cmd, -ETIMEDOUT, &early);\n"
                "\t\t\t\tbreak;\n"),
               ("\tbool busy = false, reset = false;\n",
                "\tbool busy = false, reset = false;\n\tLIST_HEAD(early);\n"),
               ("\t\tspin_unlock(&q->lock);\n\t}\n\tif (reset)\n",
                "\t\tspin_unlock(&q->lock);\n\t}\n\tkdc_end_list(kdev, &early);\n\tif (reset)\n")],
        runs=[("tcg-iommu", True, ["FAIL timeout_reset:", "FAIL timeout_escalation:"], [])]),
    "abort-never-escalates": dict(
        file=KDC_TREE / "kdc_recover.c",
        edits=[(WATCH_RESET,
                "\t\t\tcase KDC_WATCH_RESET:\n"
                "\t\t\t\tbreak; /* deliberate bug: an unanswered abort is never escalated */\n")],
        runs=[("tcg-iommu", True, ["FAIL timeout_reset:"], [])]),
    "remove-frees-queues": dict(
        file=KDC_TREE / "kdc_pci.c",
        edits=[("\tkdc_teardown(kdev);\n\tmutex_unlock(&kdev->files_mutex);\n"
                "\tmutex_unlock(&kdev->reset_lock);\n",
                "\tkdc_teardown(kdev);\n"
                "\tkfree(kdev->queues); /* deliberate bug: open files' commands still point here */\n"
                "\tkdev->queues = NULL;\n\tmutex_unlock(&kdev->files_mutex);\n"
                "\tmutex_unlock(&kdev->reset_lock);\n")],
        runs=[("tcg-iommu", True, [], [r"BUG: KASAN: \S*use-after-free in kdc_"])]),
    "overlap-check-off": dict(
        file=KDC_TREE / "kdc_cmd.c",
        edits=[("\t\tif (interval_tree_iter_first(&t->tree, it->start, it->last))\n\t\t\tbreak;\n",
                "\t\t/* deliberate bug: no overlap check */\n")],
        # Since registered buffers, fixed_buffers is the first case that puts a destination
        # on pages another mapping holds; without the check DMA-API debug reports it there,
        # before dst_overlap runs. Either one names this bug.
        runs=[("tcg-iommu", True, [], [r"DMA-API: kdc \S+: cacheline tracking EEXIST, overlapping mappings"])]),
    "read-leaks-command": dict(
        file=KDC_TREE / "kdc_fops.c",
        edits=[("\t\tctx->nr_live--;\n\t\tspin_unlock(&ctx->lock);\n\t\tkdc_cmd_free(cmd);\n\t\tn++;\n",
                "\t\tctx->nr_live--;\n\t\tspin_unlock(&ctx->lock);\n"
                "\t\t/* deliberate bug: the consumed command is never freed */\n\t\tn++;\n")],
        runs=[("tcg-iommu", True, [], [r"kmemleak_dump: .*kdc_cmd_prepare"])]),
    "coalesce-ignored": dict(
        file=KDC_TREE / "kdc_pci.c",
        edits=[("\t\tfor (i = 0; i < kdev->nr_queues; i++)\n"
                "\t\t\tkdc_wr(kdev, KDC_QREG(i, KDC_Q_COALESCE), val);\n",
                "\t\ti = 0; /* deliberate bug: the setting never reaches the queues */\n")],
        runs=[("tcg-iommu", True, ["FAIL coalescing:"], [])]),
    "cq-head-never-written": dict(
        file=KDC_TREE / "kdc_pci.c",
        edits=[("\t\twritel(q->cq_head, q->cq_db);\n\t\tq->cq_head_db = q->cq_head;\n",
                "\t\tq->cq_head_db = q->cq_head; /* deliberate bug: the head never reaches the device */\n")],
        runs=[("tcg-iommu", True, ["FAIL cq_head_batching:"], [])]),
    "pass-end-irq-skipped": dict(
        file=QEMU_FILE,
        edits=[("    if (q->irq_pending && !kdc_coalescing(q)) {\n        kdc_queue_fire(q);\n    }\n"
                "    q->pass_fired = false;\n",
                "    q->pass_fired = false; /* deliberate bug: the rest of the pass is never announced */\n")],
        qtest=("pass_interrupts", "first and last completion of the pass interrupt")),
    "shadow-ring-only-idle": dict(
        file=KDC_TREE / "kdc_pci.c",
        edits=[("\t\t\tring = kdc_need_event(le32_to_cpu(READ_ONCE(q->db->sq_event)),\n"
                "\t\t\t\t\t      q->sq_tail, old, q->depth);\n",
                "\t\t\t/* deliberate bug: a busy queue is assumed to be polled */\n"
                "\t\t\tring = q->outstanding == kdc_ring_used(q->sq_tail, old, q->depth);\n")],
        runs=[("tcg-iommu", True, ["FAIL shadow_doorbell:"], [])]),
    "event-index-not-published": dict(
        file=QEMU_FILE,
        edits=[("        if (pci_dma_write(PCI_DEVICE(s), q->db_addr + offsetof(struct kdc_hw_dbbuf, sq_event),\n"
                "                          &ev, sizeof(ev)) != MEMTX_OK) {\n",
                "        if (0) { /* deliberate bug: the event index is never published */\n")],
        qtest=("shadow_doorbell", "event index published after the pass")),
    "unregister-in-use": dict(
        file=KDC_TREE / "kdc_fops.c",
        edits=[("\tif (b && atomic_read(&b->users)) {\n",
                "\tif (0) { /* deliberate bug: unmapped while a command may still use it */\n")],
        # The driver's own check fires first: freeing a buffer a command still holds WARNs.
        runs=[("tcg-iommu", True, [], [r"WARNING: CPU: \d+ PID: \d+ at drivers/misc/kdc/kdc_cmd\.c:\d+ "
                                       r"kdc_regbuf_destroy"])]),
}


def sh(cmd, log, **kw):
    with open(log, "a") as f:
        return subprocess.run([str(c) for c in cmd], stdout=f, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL, **kw).returncode


def rebuild_kernel(log, local_tools):
    return sh(["nice", "make", "-C", SOURCE, f"O={BUILD}", "-j16", "bzImage"], log,
              env=environment(local_tools))


def rebuild_qemu(log):
    return sh(["nice", "ninja", "-C", ROOT / ".cache/build/qemu-asan", "-j16", "qemu-system-x86_64"], log)


def guest_run(out, iommu, mutant):
    args = types.SimpleNamespace(accel="tcg", iommu=iommu, qemu="release", kernel="kasan",
                                 timeout=900 if mutant else 1800)
    out.mkdir(parents=True)
    return run_vm.run(args, out, mutant=mutant), (out / "serial.log").read_text(errors="replace")


def first_failure(serial):
    m = re.search(r"kdc_test: (FAIL \S+)", serial)
    return m.group(1) if m else None


def check_mutant(name, spec, out, local_tools):
    path = spec["file"]
    original = path.read_text()
    mutated = original
    for anchor, replacement in spec["edits"]:
        if mutated.count(anchor) != 1:
            raise RuntimeError(f"{name}: anchor not found exactly once in {path}: {anchor!r}")
        mutated = mutated.replace(anchor, replacement)
    record = {"mutant": name, "file": str(path.relative_to(ROOT)), "runs": [], "caught": False}
    mdir = out / name
    mdir.mkdir(parents=True)
    (mdir / "mutation.diff").write_text("".join(f"--- {path.name}\n-{a}+{b}" for a, b in spec["edits"]))
    is_qemu = path == QEMU_FILE
    try:
        path.write_text(mutated)
        rc = rebuild_qemu(mdir / "build.log") if is_qemu else rebuild_kernel(mdir / "build.log", local_tools)
        if rc:
            raise RuntimeError(f"{name}: mutant build failed (does not count as caught)")
        if is_qemu:
            test, expected = spec["qtest"]
            log = mdir / "qtest.log"
            rc = subprocess.run([sys.executable, str(ROOT / "tests/qtest/kdc_qtest.py"), "--qemu",
                                 str(ROOT / ".cache/build/qemu-asan/qemu-system-x86_64"), "--log", str(log),
                                 "--only", test], stdin=subprocess.DEVNULL).returncode
            text = log.read_text()
            caught = rc != 0 and re.search(rf"^FAIL AssertionError: {expected}", text, re.M) is not None
            record["runs"].append({"config": "qtest-asan", "failed": rc != 0, "expected": expected,
                                   "caught": caught})
        else:
            for config, iommu, allowed, evidence in spec["runs"]:
                result, serial = guest_run(mdir / config, iommu, name)
                fired = first_failure(serial)
                seen = [e for e in evidence if re.search(e, serial)]  # regexes
                if allowed:
                    caught = fired is not None and any(fired.startswith(a.rstrip(":")) for a in allowed)
                else:
                    caught = bool(seen)
                caught = caught and not result["passed"] and len(seen) == len(evidence)
                record["runs"].append({"config": config, "failed": not result["passed"],
                                       "first_failed_assertion": fired, "allowed": allowed,
                                       "evidence_required": evidence, "evidence_seen": seen,
                                       "caught": caught})
        record["caught"] = all(r["caught"] for r in record["runs"])
    finally:
        path.write_text(original)
        # Full rebuild from the repository sources re-copies the overlay and re-records the build.
        if is_qemu:
            rc = sh([sys.executable, ROOT / "scripts/build_qemu.py", "--flavor", "asan"], mdir / "restore.log")
        else:
            cmd = [sys.executable, ROOT / "scripts/build_kernel.py"]
            if local_tools:
                cmd += ["--local-tools", local_tools]
            rc = sh(cmd, mdir / "restore.log")
        record["restored_build_ok"] = rc == 0
    (mdir / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=pathlib.Path, required=True)
    p.add_argument("--local-tools", type=pathlib.Path)
    p.add_argument("--only", nargs="*", choices=tuple(MUTANTS))
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    results = [check_mutant(n, s, out, args.local_tools) for n, s in MUTANTS.items()
               if not args.only or n in args.only]
    # The restored kernel and device must pass again.
    restored, _ = guest_run(out / "restored-tcg-iommu", True, None)
    qrc = subprocess.run([sys.executable, str(ROOT / "tests/qtest/kdc_qtest.py"), "--qemu",
                          str(ROOT / ".cache/build/qemu-asan/qemu-system-x86_64"),
                          "--log", str(out / "restored-qtest-asan.log")], stdin=subprocess.DEVNULL).returncode
    summary = {"mutants": results, "restored_guest_passed": restored["passed"], "restored_qtest_passed": qrc == 0}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    for r in results:
        print(("CAUGHT " if r["caught"] else "MISSED ") + r["mutant"],
              [(x["config"], x.get("first_failed_assertion") or x.get("evidence_seen") or x.get("expected"))
               for x in r["runs"]])
    print("restored guest:", "PASS" if restored["passed"] else "FAIL", "restored qtest:", "PASS" if qrc == 0 else "FAIL")
    ok = all(r["caught"] and r["restored_build_ok"] for r in results) and restored["passed"] and qrc == 0
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
