#!/usr/bin/env python3
"""Paired comparison of two builds from one run_bench.py --builds session.

Boots alternate build by build within each round, so the n-th boot of one build
and the n-th boot of the other (same configuration) ran next to each other in
time: that is one pair. Per pair and arm, the value is the median throughput
(commands per second) of the arm's accepted windows in that boot. A pair is a
win when the new build's value is higher.

Decision rule (fixed before the runs it judges):
  - each targeted arm (--target) must win in all but at most one pair, with at
    least 6 pairs;
  - no arm may be worse beyond the spread: the new build's median over the
    pairs must not fall below the lowest value the base build had in any pair.
Writes compare-<new>-vs-<base>.md and .json into the run directory.

With --pair NEW_ARM:BASE_ARM the new build's arm is compared with another arm
of the base build (for example a new data path against the old one at the
same size); --base and --new may then name the same build.

With --new-config the new side is another boot configuration (for example the
same build with the device's queues in IOThreads); within a round the
configurations alternate too, so the k-th boots of the two still pair.

Usage: bench_compare.py DIR --base NAME --new NAME [--target ARM,ARM,...] [--config NAME]
                        [--new-config NAME] [--pair NEW_ARM:BASE_ARM ...] [--tag TEXT]
"""
import argparse
import gzip
import json
import pathlib
import statistics

from bench_report import window_metrics


def load(d):
    raw = d / "windows.jsonl"
    text = raw.read_text() if raw.exists() else gzip.decompress((d / "windows.jsonl.gz").read_bytes()).decode()
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def per_boot(windows, config, build):
    """{arm: [(boot, median k cmds/s, median p50 us), ...]} in boot order."""
    by = {}
    for w in windows:
        if w["config"] != config or w.get("build", "tree") != build or not w["accepted"]:
            continue
        m = window_metrics(w)
        if "kcmd_s" not in m:
            continue
        by.setdefault(w["guest"]["arm"], {}).setdefault(w["boot"], []).append(m)
    return {arm: [(b, statistics.median(x["kcmd_s"] for x in ms), statistics.median(x["lat_p50_us"] for x in ms))
                  for b, ms in sorted(boots.items())] for arm, boots in by.items()}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("dir", type=pathlib.Path)
    p.add_argument("--base", required=True)
    p.add_argument("--new", required=True)
    p.add_argument("--target", default="", help="comma-separated arms the change is meant to improve")
    p.add_argument("--config", default="noiommu")
    p.add_argument("--new-config", help="configuration of the new side (default: --config)")
    p.add_argument("--pair", action="append", default=[], help="NEW_ARM:BASE_ARM, compared instead of same-name arms")
    p.add_argument("--tag", default="", help="suffix of the output file names")
    args = p.parse_args()
    windows = load(args.dir)
    new_config = args.new_config or args.config
    base, new = per_boot(windows, args.config, args.base), per_boot(windows, new_config, args.new)
    boots = json.loads((args.dir / "summary-boots.json").read_text())["boots"]
    # the k-th boot of the base build pairs with the k-th boot of the new build
    bseq = [b["boot"] for b in boots if b["config"] == args.config and b["build"] == args.base]
    nseq = [b["boot"] for b in boots if b["config"] == new_config and b["build"] == args.new]
    same_side = args.base == args.new and new_config == args.config
    targets = [t for t in args.target.split(",") if t]
    rows, verdict = [], True
    pairs_wanted = [tuple(p.split(":")) for p in args.pair] or \
        [(arm, arm) for arm in base if arm in new and arm != "warmup"]  # warmup only warms the guest up
    for new_arm, base_arm in pairs_wanted:
        if new_arm not in new or base_arm not in base:
            continue
        arm = new_arm if new_arm == base_arm else f"{new_arm} vs {base_arm}"
        bb = {b: (v, lat) for b, v, lat in base[base_arm]}
        nb = {b: (v, lat) for b, v, lat in new[new_arm]}
        if same_side:  # two arms of one build: pair them within each boot
            pairs = [(bb[x], nb[x]) for x in bseq if x in bb and x in nb]
        else:
            pairs = [(bb[x], nb[y]) for x, y in zip(bseq, nseq) if x in bb and y in nb]
        if not pairs:
            continue
        wins = sum(n[0] > b[0] for b, n in pairs)
        ratios = [n[0] / b[0] for b, n in pairs]
        base_vals, new_vals = [b[0] for b, _ in pairs], [n[0] for _, n in pairs]
        row = {"arm": arm, "pairs": len(pairs), "wins": wins,
               "base_median": statistics.median(base_vals), "base_min": min(base_vals), "base_max": max(base_vals),
               "new_median": statistics.median(new_vals), "new_min": min(new_vals), "new_max": max(new_vals),
               "ratio_median": statistics.median(ratios), "ratio_min": min(ratios), "ratio_max": max(ratios),
               "base_p50_us": statistics.median(b[1] for b, _ in pairs),
               "new_p50_us": statistics.median(n[1] for _, n in pairs),
               "new_arm": new_arm, "base_arm": base_arm, "target": arm in targets or new_arm in targets}
        row["worse_beyond_spread"] = row["new_median"] < row["base_min"]
        row["target_ok"] = (len(pairs) >= 6 and wins >= len(pairs) - 1) if row["target"] else None
        if row["worse_beyond_spread"] or row["target_ok"] is False:
            verdict = False
        rows.append(row)
    missing = [t for t in targets if t not in {r["arm"] for r in rows} | {r["new_arm"] for r in rows}]
    if missing:
        verdict = False
    result = {"dir": str(args.dir), "config": args.config, "new_config": new_config, "base": args.base,
              "new": args.new, "targets": targets,
              "missing_targets": missing, "keep": verdict, "arms": rows}
    stem = f"compare-{args.new}-vs-{args.base}" + ("" if args.config == "noiommu" else f"-{args.config}") + \
        ("" if new_config == args.config else f"-{new_config}") + (f"-{args.tag}" if args.tag else "")
    (args.dir / f"{stem}.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = [f"# {args.new} vs {args.base} ({args.config}" +
             ("" if new_config == args.config else f" vs {new_config}") + ")", "",
             "Paired boots (same round). k cmds/s: median over pairs (min-max); ratio new/base: median (min-max). "
             "Generated by scripts/bench_compare.py.", "",
             f"Decision rule: targeted arms ({', '.join(targets) or 'none'}) win in all but at most one of at least "
             "6 pairs; no arm's new median below the base minimum. Result: "
             f"**{'keep' if verdict else 'reject'}**" + (f" (targets without data: {missing})" if missing else ""), "",
             "| arm | target | pairs | wins | base k/s | new k/s | ratio | base p50 us | new p50 us | worse beyond spread |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append(f"| {r['arm']} | {'yes' if r['target'] else ''} | {r['pairs']} | {r['wins']} "
                     f"| {r['base_median']:.1f} ({r['base_min']:.1f}-{r['base_max']:.1f}) "
                     f"| {r['new_median']:.1f} ({r['new_min']:.1f}-{r['new_max']:.1f}) "
                     f"| {r['ratio_median']:.3f} ({r['ratio_min']:.3f}-{r['ratio_max']:.3f}) "
                     f"| {r['base_p50_us']:.1f} | {r['new_p50_us']:.1f} | {'YES' if r['worse_beyond_spread'] else ''} |")
    (args.dir / f"{stem}.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
