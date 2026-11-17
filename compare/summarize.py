#!/usr/bin/env python3
# Reads the output files in compare/results/original/ and compare/results/event/ and puts one row
# per matrix into compare/results/summary.csv, so the two versions (and the paper) can be compared.
#
#   python3 compare/summarize.py

import csv
import glob
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results")
PAPER_CSV = os.path.join(HERE, "fig7_reference_ms.csv")  # paper's Figure 7, in ms

# name in the csv -> regex on the output file. the first group is the value
AUTHOR_STATS = {
    "total_cycle": r"^total cycle = (\d+)",
    "load_cycle": r"load cycle = (\d+), multiply",
    "multiply_cycle": r"multiply cycle = (\d+), merge",
    "merge_cycle": r"merge and writeback cycle = (\d+)",
    "dram_A": r"total DRAM access A = (\d+)",
    "dram_B": r"total DRAM access B = (\d+)",
    "dram_C": r"total DRAM access C = (\d+)",
    "hitrate": r"hitrate = ([\d.na-]+)",
}
# only our version prints these
EVENT_STATS = {
    "event_total": r"event total cycle = (\d+)",
    "event_load": r"event load cycle = (\d+)",
    "event_multiply": r"event multiply cycle = (\d+)",
    "tiles": r"event multiply cycle = \d+ \((\d+) tiles\)",
    "event_wait_last_writes": r"waiting for last DRAM writes = (\d+)",
    "event_merge": r"waiting for last DRAM writes = \d+, merge = (\d+)",
    "prefetch_size_end": r"prefetch size at end = (\d+)",
    "prefetch_adjustments": r"adjusted (\d+) times",
    "pe_busy_pct": r"PE busy = ([\d.]+)%",
    "pe_idle_pct": r"idle = ([\d.]+)%",
    "lat_A": r"A row = ([\d.]+) cycles",
    "lat_B": r"B fiber = ([\d.]+) cycles",
    "lat_C": r"C write = ([\d.]+) cycles",
    "fiber_hits": r"B fibers: (\d+) hits",
    "fiber_misses": r"(\d+) misses, \d+ waited",
    "fiber_merged": r"(\d+) waited for a miss",
    "event_dram_A": r"DRAM access in event timing: A = (\d+)",
    "event_dram_B": r"DRAM access in event timing: A = \d+, B = (\d+)",
    "event_dram_C": r"DRAM access in event timing: A = \d+, B = \d+, C = (\d+)",
    "dram_space_waits": r"waited for space in a full DRAM queue = (\d+)",
}


def read_output(version, matrix):
    files = glob.glob(os.path.join(RESULTS, version, f"*__{matrix}_{matrix}_*.txt"))
    if not files:
        return {}
    text = open(files[0]).read()
    stats = {}
    for name, pattern in {**AUTHOR_STATS, **EVENT_STATS}.items():
        found = re.search(pattern, text, re.M)
        if found:
            stats[name] = found.group(1)
    # average busy % over the DRAM channels
    channels = re.findall(r"ch\d+ = ([\d.]+)%", text)
    if channels:
        stats["dram_channel_busy_pct"] = f"{sum(map(float, channels)) / len(channels):.1f}"
    stats["stopped_early"] = "yes" if "simulation stopped" in text else ""
    return stats


def ratio(a, b):
    try:
        return f"{float(a) / float(b):.3f}"
    except (TypeError, ValueError, ZeroDivisionError):
        return ""


def main():
    paper = {}
    if os.path.exists(PAPER_CSV):
        for row in csv.DictReader(open(PAPER_CSV)):
            # 1 GHz, so ms * 1e6 = cycles
            paper[row["matrix"]] = float(row["seacache_ms_paper"]) * 1e6

    matrices = set()
    for f in glob.glob(os.path.join(RESULTS, "*", "*.txt")):
        found = re.search(r"__(.+?)_\1_", os.path.basename(f))
        if found:
            matrices.add(found.group(1))

    seconds = {}
    times_file = os.path.join(HERE, "logs", "times.csv")
    if os.path.exists(times_file):
        for line in open(times_file):
            m, version, status, secs = line.strip().split(",")
            seconds[(m, version)] = secs  # the last run of each wins

    rows = []
    for m in sorted(matrices):
        orig = read_output("original", m)
        event = read_output("event", m)
        row = {"matrix": m, "paper_cycles": f"{paper[m]:.0f}" if m in paper else ""}
        for name in AUTHOR_STATS:
            row[f"orig_{name}"] = orig.get(name, "")
        # our run also prints the authors' stats (old formula), from its own cache behaviour
        for name in AUTHOR_STATS:
            row[f"ev_{name}"] = event.get(name, "")
        for name in list(EVENT_STATS) + ["dram_channel_busy_pct", "stopped_early"]:
            row[name] = event.get(name, "")
        # the main comparisons
        row["event_vs_orig"] = ratio(event.get("event_total"), orig.get("total_cycle"))
        row["event_vs_oldformula_same_run"] = ratio(event.get("event_total"), event.get("total_cycle"))
        row["orig_vs_paper"] = ratio(orig.get("total_cycle"), paper.get(m))
        row["seconds_orig"] = seconds.get((m, "original"), "")
        row["seconds_event"] = seconds.get((m, "event"), "")
        rows.append(row)

    if not rows:
        print("no results yet")
        return

    out = os.path.join(RESULTS, "summary.csv")
    with open(out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    # short version on screen
    print(f"{'matrix':24} {'paper':>11} {'orig total':>11} {'event total':>11} {'ev/orig':>8} "
          f"{'orig hit':>8} {'ev hit':>7} {'PE busy%':>8}")
    for r in rows:
        print(f"{r['matrix']:24} {r['paper_cycles']:>11} {r['orig_total_cycle']:>11} {r['event_total']:>11} "
              f"{r['event_vs_orig']:>8} {r['orig_hitrate'][:6]:>8} {r['ev_hitrate'][:6]:>7} {r['pe_busy_pct']:>8}"
              + ("  STOPPED EARLY" if r["stopped_early"] else ""))
    print(f"\nall columns: {out}")


if __name__ == "__main__":
    main()
