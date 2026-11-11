#!/usr/bin/env python3
"""Check D: the improvement round's timing claims against this check's two sessions.

Usage: claims.py OUT_DIR   (OUT_DIR holds s1-ab, s1-fixed, s2-ab, s2-fixed run directories,
each with the compare-*.json files bench_compare.py wrote and the summary.md of bench_report.py).
Prints markdown: the claims table (rule in rule.txt), the full paired tables, the loss scan and
the per-command counters behind the IOThread explanation (vCPU busy time from summary.json)."""
import gzip, json, pathlib, re, sys

OUT = pathlib.Path(sys.argv[1])
SESSIONS = ("s1", "s2")


def load_compares(d):
    idx = {}
    for f in d.glob("compare-*.json"):
        j = json.loads(f.read_text())
        key = (j["base"], j["new"], j["config"], j.get("new_config") or j["config"])
        idx[key] = {a["arm"]: a for a in j["arms"]} | {"_file": f.name}
    return idx


def load_summary(d):
    rows, sec, hdr = {}, None, None
    for line in (d / "summary.md").read_text().splitlines():
        m = re.match(r"## (\S+), build (\S+)", line)
        if m:
            sec, hdr = m.groups(), None
            continue
        if line.startswith("| arm |"):
            hdr = [c.strip() for c in line.strip().strip("|").split("|")]
            continue
        if hdr and line.startswith("| ") and not line.startswith("|---"):
            c = [x.strip().split(" ")[0] for x in line.strip().strip("|").split("|")]
            rows[(sec[0], sec[1], c[0])] = dict(zip(hdr, c))
    return rows


def load_summary_json(d):
    return {(r["config"], r["build"], r["arm"]): r["metrics"] for r in json.loads((d / "summary.json").read_text())}


CMP = {s: {p: load_compares(OUT / f"{s}-{p}") for p in ("ab", "fixed")} for s in SESSIONS}
SUM = {s: {p: load_summary(OUT / f"{s}-{p}") for p in ("ab", "fixed")} for s in SESSIONS}
SUMJ = {s: load_summary_json(OUT / f"{s}-ab") for s in SESSIONS}


def get(s, plan, base, new, cfg, ncfg, arm):
    return CMP[s][plan].get((base, new, cfg, ncfg), {}).get(arm)


def judge_point(a, stated):
    if a is None:
        return "n/a"
    r, w, n = a["ratio_median"], a["wins"], a["pairs"]
    if stated >= 1.03 or stated <= 0.97:
        side = (r > 1) == (stated > 1)
        count = w if stated > 1 else n - w
        if not side or count < n - 1:
            return "no"
        return "yes" if 0.95 <= r / stated <= 1.05 else "size differs"
    ok = abs(r - stated) <= 0.02 + 1e-9
    if stated >= 1.01 and r <= 1.0:
        ok = False
    if stated <= 0.99 and r >= 1.0:
        ok = False
    return "yes" if ok else "no"


def judge_range(a, lo, hi):
    if a is None:
        return "n/a"
    return "yes" if lo - 0.02 - 1e-9 <= a["ratio_median"] <= hi + 0.02 + 1e-9 else "no"


def fmt(a):
    return "-" if a is None else f"{a['ratio_median']:.3f} ({a['wins']}/{a['pairs']})"


ARM_NAMES = {"nop-qd1": "NOP one at a time", "zc4k-qd1": "4 KiB one at a time", "bounce4k-qd1": "bounce 4 KiB one at a time",
             "zc64k-qd1": "64 KiB one at a time", "zc4k-qd4": "4 in flight", "zc4k-qd32": "32 in flight",
             "zc4k-qd8-batch": "8 batched", "zc4k-qd32-batch": "32 batched", "zc4k-qd32-c16": "32 in flight, coalescing",
             "zc4k-t3-qd8": "three threads", "zc4k-t3-qd8-batch": "three threads batched",
             "nop-t3-qd8-sharedfile": "one file, NOP", "zc4k-t3-qd8-sharedfile": "one file, 4 KiB",
             "fixed4k-qd1": "fixed 4 KiB one at a time", "fixed64k-qd1": "fixed 64 KiB one at a time",
             "fixed4k-qd32-batch": "fixed 4 KiB 32 batched", "fixed64k-qd8-batch": "fixed 64 KiB 8 batched",
             "bounce64k-qd1": "bounce 64 KiB one at a time", "bounce4k-qd32-batch": "bounce 4 KiB 32 batched",
             "zc64k-qd8-batch": "64 KiB 8 batched", "bounce64k-qd8-batch": "bounce 64 KiB 8 batched"}

MAIN = ("ab", "main", "improve", "noiommu", "noiommu")
IO1 = ("ab", "main", "improve", "noiommu", "iothread1")
LOOP_IO1 = ("ab", "improve", "improve", "noiommu", "iothread1")
LOOP_IO3 = ("ab", "improve", "improve", "noiommu", "iothreads")
O3 = ("ab", "o3", "improve", "noiommu", "noiommu")
R1_O3 = ("ab", "o3", "r1c", "noiommu", "noiommu")
R1_MAIN = ("ab", "main", "r1c", "noiommu", "noiommu")
MAIN_O3 = ("ab", "o3", "main", "noiommu", "noiommu")
FIX = ("fixed", "main", "improve", "noiommu", "noiommu")
FIX_S = ("fixed", "main", "improve", "iommu-strict", "iommu-strict")

# (where, claim text, comparison, arm, stated) point claims; ranges below
POINTS = [
    ("README", "4 KiB one at a time 0.99x", MAIN, "zc4k-qd1", 0.99),
    ("README", "NOP one at a time unchanged (176.1k vs 175.4k)", MAIN, "nop-qd1", 1.00),
    ("README", "32 in flight 1.10x", MAIN, "zc4k-qd32", 1.10),
    ("README", "32 batched 1.02x", MAIN, "zc4k-qd32-batch", 1.02),
    ("README", "three threads 1.00x (iE), 0.98x (iD)", MAIN, "zc4k-t3-qd8", 1.00),
    ("README", "three threads batched 0.99x", MAIN, "zc4k-t3-qd8-batch", 0.99),
    ("README", "one file NOP 1.39x", MAIN, "nop-t3-qd8-sharedfile", 1.39),
    ("README", "one file 4 KiB 1.28x", MAIN, "zc4k-t3-qd8-sharedfile", 1.28),
    ("README", "one IOThread: NOP one at a time 1.21x", IO1, "nop-qd1", 1.21),
    ("README", "one IOThread: 4 KiB one at a time 1.17x", IO1, "zc4k-qd1", 1.17),
    ("README", "one IOThread: 32 in flight 0.93x", IO1, "zc4k-qd32", 0.93),
    ("README", "one IOThread: 32 batched 1.07x", IO1, "zc4k-qd32-batch", 1.07),
    ("README", "one IOThread: three threads 1.38x", IO1, "zc4k-t3-qd8", 1.38),
    ("README", "one IOThread: three threads batched 1.50x", IO1, "zc4k-t3-qd8-batch", 1.50),
    ("README", "one IOThread: one file NOP 1.10x", IO1, "nop-t3-qd8-sharedfile", 1.10),
    ("README", "one IOThread: one file 4 KiB 1.07x", IO1, "zc4k-t3-qd8-sharedfile", 1.07),
    ("README", "strict IOMMU, fixed 4 KiB one at a time 1.00x", FIX_S, "fixed4k-qd1", 1.00),
    ("README", "strict IOMMU, fixed 4 KiB 32 batched 1.01x", FIX_S, "fixed4k-qd32-batch", 1.01),
    ("README", "loss: bounce 4 KiB one at a time 0.99x (iDfixed)", FIX, "bounce4k-qd1", 0.99),
    ("design 2", "r1 vs o3r at 32 in flight 1.004, 1.002", R1_O3, "zc4k-qd32", 1.00),
    ("design 2", "r1 ties o3r within 0.5%: 4 KiB one at a time", R1_O3, "zc4k-qd1", 1.00),
    ("design 2", "r1 ties o3r within 0.5%: 32 batched", R1_O3, "zc4k-qd32-batch", 1.00),
    ("design 2", "r1 ties o3r within 0.5%: three threads", R1_O3, "zc4k-t3-qd8", 1.00),
    ("design 2", "r1 vs o3r one file NOP -3.2% (i2b)", R1_O3, "nop-t3-qd8-sharedfile", 0.968),
    ("design 2", "r1 vs o3r one file 4 KiB +1.5% (i2b)", R1_O3, "zc4k-t3-qd8-sharedfile", 1.015),
    ("design 2", "r1 vs base 32 in flight 1.015, 1.008 (5 of 7)", R1_MAIN, "zc4k-qd32", 1.01),
    ("design 2", "r1 vs base one file 4 KiB 1.034, 1.022 (7 of 7)", R1_MAIN, "zc4k-t3-qd8-sharedfile", 1.03),
    ("design 4", "one IOThread vs main loop: NOP one at a time 1.210", LOOP_IO1, "nop-qd1", 1.21),
    ("design 4", "one IOThread vs main loop: 4 KiB one at a time 1.178", LOOP_IO1, "zc4k-qd1", 1.178),
    ("design 4", "one IOThread vs main loop: 32 in flight 0.846 (0.86x and 0.85x)", LOOP_IO1, "zc4k-qd32", 0.85),
    ("design 4", "one IOThread vs main loop: 32 batched 1.020", LOOP_IO1, "zc4k-qd32-batch", 1.02),
    ("design 4", "one IOThread vs main loop: three threads 1.342 (1.36x, 1.34x)", LOOP_IO1, "zc4k-t3-qd8", 1.34),
    ("design 4", "one IOThread vs main loop: three threads batched 1.516", LOOP_IO1, "zc4k-t3-qd8-batch", 1.516),
    ("design 4", "one IOThread vs main loop: one file NOP 0.790", LOOP_IO1, "nop-t3-qd8-sharedfile", 0.79),
    ("design 4", "one IOThread vs main loop: one file 4 KiB 0.837 (0.880 in iD)", LOOP_IO1, "zc4k-t3-qd8-sharedfile", 0.837),
    ("design 4", "three IOThreads vs main loop: NOP one at a time 1.001", LOOP_IO3, "nop-qd1", 1.00),
    ("design 4", "three IOThreads vs main loop: 4 KiB one at a time 1.011", LOOP_IO3, "zc4k-qd1", 1.011),
    ("design 4", "three IOThreads vs main loop: 32 in flight 0.573", LOOP_IO3, "zc4k-qd32", 0.573),
    ("design 4", "three IOThreads vs main loop: 32 batched 0.997", LOOP_IO3, "zc4k-qd32-batch", 0.997),
    ("design 4", "three IOThreads vs main loop: three threads 0.869 (lost 13%)", LOOP_IO3, "zc4k-t3-qd8", 0.87),
    ("design 4", "three IOThreads vs main loop: three threads batched 1.380", LOOP_IO3, "zc4k-t3-qd8-batch", 1.38),
    ("design 4", "three IOThreads vs main loop: one file NOP 0.720", LOOP_IO3, "nop-t3-qd8-sharedfile", 0.72),
    ("design 4", "three IOThreads vs main loop: one file 4 KiB 0.734", LOOP_IO3, "zc4k-t3-qd8-sharedfile", 0.734),
    ("design 5", "strict IOMMU fixed 4 KiB 32 batched 1.048 (iCfixed), 1.012 (iDfixed)", FIX_S, "fixed4k-qd32-batch", 1.012),
    ("design 5", "before/after 32 in flight 1.10x and 1.08x", MAIN, "zc4k-qd32", 1.08),
    ("design 5", "before/after one file NOP 1.39x and 1.45x", MAIN, "nop-t3-qd8-sharedfile", 1.42),
    ("design 5", "before/after one file 4 KiB 1.28x and 1.26x", MAIN, "zc4k-t3-qd8-sharedfile", 1.27),
    ("design 5", "one IOThread: three threads 1.38x", IO1, "zc4k-t3-qd8", 1.38),
    ("design 5", "one IOThread: batched three threads 1.50x", IO1, "zc4k-t3-qd8-batch", 1.50),
    ("design 5", "one IOThread: 32 in flight 0.93x", IO1, "zc4k-qd32", 0.93),
]
RANGES = [
    ("design 5", "one at a time and three threads on own files unchanged (0.98 to 1.01)", MAIN,
     ["nop-qd1", "zc4k-qd1", "bounce4k-qd1", "zc64k-qd1", "zc4k-t3-qd8", "zc4k-t3-qd8-batch"], 0.98, 1.01),
    ("design 5", "one IOThread vs main: one at a time 1.17x-1.21x", IO1,
     ["nop-qd1", "zc4k-qd1", "bounce4k-qd1", "zc64k-qd1"], 1.17, 1.21),
    ("design 4", "one IOThread vs main loop: one command at a time 1.15-1.21x", LOOP_IO1,
     ["nop-qd1", "zc4k-qd1", "bounce4k-qd1", "zc64k-qd1"], 1.15, 1.21),
    ("design 4", "one IOThread vs main loop: shared file 0.79-0.88x", LOOP_IO1,
     ["nop-t3-qd8-sharedfile", "zc4k-t3-qd8-sharedfile"], 0.79, 0.88),
    ("design 5", "strict IOMMU: every fixed arm within 1% or better", FIX_S,
     ["fixed4k-qd1", "fixed64k-qd1", "fixed4k-qd32-batch", "fixed64k-qd8-batch"], 0.99, 9.0),
    ("README", "one shared file 1.28x-1.39x (what changed; losses: instead of 1.28-1.39x)", MAIN,
     ["nop-t3-qd8-sharedfile", "zc4k-t3-qd8-sharedfile"], 1.28, 1.39),
    ("README", "one IOThread: one file 1.07-1.10x (losses)", IO1,
     ["nop-t3-qd8-sharedfile", "zc4k-t3-qd8-sharedfile"], 1.07, 1.10),
]
# "took back most of the 1-4%": rule.txt has no form for "most". Read as r1c taking back more than
# half of main's loss against o3 at 32 in flight (the arm of the 1-4% in check C), in both sessions.
TOOK_BACK = ("README, design 2", "registered-buffer move took back most of the 1-4%: share of main's loss against o3 "
             "that r1c took back, (r1c/o3 - main/o3) / (1 - main/o3), more than half", "zc4k-qd32")


def rule5_losses():
    """Rule 5: arms of the final code against main below 0.985 with at most 2 pairs won in both sessions."""
    out = []
    for c in (MAIN, IO1, FIX, FIX_S):
        for arm in [k for k in CMP["s1"][c[0]].get(c[1:], {}) if k != "_file"]:
            a = [get(s, *c, arm) for s in SESSIONS]
            if all(x and x["ratio_median"] < 0.985 and x["wins"] <= 2 for x in a):
                out.append((c, arm, a))
    return out


def main():
    losses = rule5_losses()
    loss_keys = {(c, arm) for c, arm, _ in losses}
    print("## Claims\n")
    print("| where | claim | arm | s1 | s2 | holds |")
    print("|---|---|---|---|---|---|")
    for where, text, c, arm, stated in POINTS:
        a1, a2 = (get(s, *c, arm) for s in SESSIONS)
        j1, j2 = judge_point(a1, stated), judge_point(a2, stated)
        verdict = "yes" if j1 == j2 == "yes" else ("size differs" if {j1, j2} <= {"yes", "size differs"} else "no")
        print(f"| {where} | {text} | `{arm}` | {fmt(a1)} {j1} | {fmt(a2)} {j2} | {verdict} |")
    for where, text, c, arms, lo, hi in RANGES:
        for arm in arms:
            a1, a2 = (get(s, *c, arm) for s in SESSIONS)
            j1, j2 = judge_range(a1, lo, hi), judge_range(a2, lo, hi)
            verdict = "yes" if j1 == j2 == "yes" else "no"
            if verdict == "yes" and (c, arm) in loss_keys:
                verdict = "yes; a loss by rule 5"
            print(f"| {where} | {text} | `{arm}` | {fmt(a1)} {j1} | {fmt(a2)} {j2} | {verdict} |")
    where, text, arm = TOOK_BACK
    cells, js = [], []
    for s in SESSIONS:
        m, r = get(s, *MAIN_O3, arm), get(s, *R1_O3, arm)
        if m is None or r is None or m["ratio_median"] >= 1:
            cells.append("n/a")
            js.append("n/a")
            continue
        share = (r["ratio_median"] - m["ratio_median"]) / (1 - m["ratio_median"])
        js.append("yes" if share > 0.5 else "no")
        cells.append(f"main {fmt(m)}, r1c {fmt(r)}: {share:.2f} {js[-1]}")
    print(f"| {where} | {text} | `{arm}` | {cells[0]} | {cells[1]} | {'yes' if js == ['yes', 'yes'] else 'no'} |")
    print("\n## Paired tables (median ratio new/base over the pairs, pairs won)\n")
    for title, c in (("final vs main, main loop", MAIN), ("final with one IOThread vs main", IO1),
                     ("final with three IOThreads vs main", ("ab", "main", "improve", "noiommu", "iothreads")),
                     ("final, one IOThread vs its main loop", LOOP_IO1), ("final, three IOThreads vs its main loop", LOOP_IO3),
                     ("final vs o3", O3), ("r1c vs o3", R1_O3), ("r1c vs main", R1_MAIN), ("main vs o3", MAIN_O3),
                     ("final vs main, fixed plan, no IOMMU", FIX), ("final vs main, fixed plan, strict IOMMU", FIX_S)):
        arms = [k for k in CMP["s1"][c[0]].get(c[1:], {}) if k != "_file"]
        if not arms:
            continue
        print(f"### {title} (`{CMP['s1'][c[0]][c[1:]]['_file']}`)\n")
        print("| arm | s1 base k/s | s1 new k/s | s1 ratio | s2 base k/s | s2 new k/s | s2 ratio |")
        print("|---|---|---|---|---|---|---|")
        for arm in arms:
            cells = []
            for s in SESSIONS:
                a = get(s, *c, arm)
                cells += ["-"] * 3 if a is None else [f"{a['base_median']:.1f}", f"{a['new_median']:.1f}", fmt(a)]
            print(f"| {ARM_NAMES.get(arm, arm)} | " + " | ".join(cells) + " |")
        print()
    print("## Losses of the final code against main in both sessions (median < 0.985, at most 2 of 7 won)\n")
    for c, arm, a in losses:
        print(f"- {c[3]}/{c[4]} {c[0]} `{arm}`: {fmt(a[0])}, {fmt(a[1])}")
    print("\n## Per-command counters, final build (summary.md medians)\n")
    print("| config | arm | k cmds/s | SQ db | MSI-X | vCPU us | main util | host CPU us | session |")
    print("|---|---|---|---|---|---|---|---|---|")
    for s in SESSIONS:
        for cfg in ("noiommu", "iothread1", "iothreads"):
            for arm in ("nop-qd1", "zc4k-qd1", "zc4k-qd32", "zc4k-qd32-batch", "zc4k-t3-qd8", "zc4k-t3-qd8-batch",
                        "nop-t3-qd8-sharedfile", "zc4k-t3-qd8-sharedfile"):
                r = SUM[s]["ab"].get((cfg, "improve", arm))
                if r:
                    print(f"| {cfg} | {arm} | {r['k cmds/s']} | {r['SQ db']} | {r['MSI-X']} | {r['vCPU us']} | {r['main util']} | {r['host CPU us']} | {s} |")
    for s in SESSIONS:
        r = SUM[s]["ab"]
        t3, t3b, sh = (float(r[("noiommu", "improve", a)]["k cmds/s"]) for a in ("zc4k-t3-qd8", "zc4k-t3-qd8-batch", "zc4k-t3-qd8-sharedfile"))
        i3, i3b = (float(r[("iothread1", "improve", a)]["k cmds/s"]) for a in ("zc4k-t3-qd8", "zc4k-t3-qd8-batch"))
        print(f"\n{s}: main loop, batched/unbatched three threads {t3b:.1f}/{t3:.1f} = {t3b / t3:.3f}; one file/three files "
              f"{sh:.1f}/{t3:.1f} = {sh / t3:.3f}; one IOThread batched/unbatched {i3b:.1f}/{i3:.1f} = {i3b / i3:.3f}")
    for s in SESSIONS:
        m = SUMJ[s]
        print(f"\n{s}: vCPUs busy at three threads (median `vcpu_util` of `summary.json`, of 3): " + ", ".join(
            f"{cfg} {m[(cfg, 'improve', 'zc4k-t3-qd8')]['vcpu_util']['median']:.2f}" for cfg in ("noiommu", "iothread1", "iothreads")))

    print("\n## One command at a time, per boot (k cmds/s of each accepted window, in boot order)\n")
    for s in SESSIONS:
        for plan, arm in (("ab", "zc64k-qd1"), ("ab", "zc4k-qd1"), ("fixed", "zc4k-qd1")):
            d = OUT / f"{s}-{plan}"
            f = d / "windows.jsonl"
            rows = [json.loads(x) for x in (f.open() if f.exists() else gzip.open(str(f) + ".gz", "rt")) if x.strip()]
            by = {}
            for r in rows:
                g = r["guest"]
                if g["arm"] == arm and r["accepted"] and r["config"] in ("noiommu", "iothread1"):
                    by.setdefault((r["build"], r["config"]), []).append(float(g["cmds"]) / float(g["ns"]) * 1e6)
            ref = sorted(by[("main", "noiommu")])[len(by[("main", "noiommu")]) // 2]
            print(f"- {s} {plan} `{arm}` (slow: below 0.95 of main's median {ref:.1f}):")
            for (b, c), v in sorted(by.items()):
                slow = sum(x < 0.95 * ref for x in v) if c == "noiommu" else "-"
                print(f"  - {b} {c}: " + ", ".join(f"{x:.1f}" for x in v) + f"; slow {slow} of {len(v)}")


if __name__ == "__main__":
    main()
