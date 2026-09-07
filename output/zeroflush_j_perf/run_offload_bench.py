#!/usr/bin/env python3
"""ZeroFlush offload-bench log parser.

Parses a db_bench run log and extracts the pieces needed for the CPU-vs-CSD
offload experiment report:
  - the fillrandom summary line  (micros/op, ops/sec, seconds, operations)
  - per-interval ZFPROGRESS rows (every N ops -> 10 rows for a 100M run)
  - ZeroFlush property lines (zf.* counters / timers, incl. csd_files/
    attempts/fallbacks and materialize_micros)
  - the exact invocation if recorded

Usage:
    python3 run_offload_bench.py <db_bench.log> [--json out.json]
Output:
    prints a compact table to stdout; with --json also writes a dict.
"""

import json
import re
import sys

FILLRANDOM_RE = re.compile(
    r"^(?P<bench>\w+)\s+:\s+(?P<micros_per_op>[0-9.]+) micros/op "
    r"(?P<ops_per_sec>[0-9.]+) ops/sec;?\s+(?P<seconds>[0-9.]+) seconds "
    r"(?P<operations>[0-9]+) operations")
ZFPROGRESS_RE = re.compile(
    r"ZFPROGRESS writes_done=(?P<done>\d+) seg_ops=(?P<seg>\d+) "
    r"wall_sec=(?P<wall>[0-9.]+) us_per_op=(?P<us>[0-9.]+) "
    r"ops_per_sec=(?P<ops>[0-9.]+)")
PROP_RE = re.compile(r"^(?P<name>zf\.[A-Za-z0-9_]+)\s*:\s*(?P<value>[-0-9.]+)")


def parse(path):
    out = {"log": path, "props": {}, "progress_rows": []}
    with open(path, errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            m = FILLRANDOM_RE.search(line)
            if m:
                out["fillrandom"] = {k: (float(v) if k not in
                                        ("bench", "operations") else v)
                                     for k, v in m.groupdict().items()}
                continue
            m = ZFPROGRESS_RE.search(line)
            if m:
                out["progress_rows"].append({k: float(v) for k, v in
                                            m.groupdict().items()})
                continue
            m = PROP_RE.match(line)
            if m:
                v = m.group("value")
                out["props"][m.group("name")] = (
                    float(v) if "." in v else int(v))
    return out


def main():
    path = sys.argv[1]
    out = parse(path)
    json_path = None
    if "--json" in sys.argv:
        json_path = sys.argv[sys.argv.index("--json") + 1]
    fr = out.get("fillrandom")
    print(f"== {path} ==")
    if fr:
        print("fillrandom : {micros_per_op:.1f} micros/op  {ops_per_sec:.0f} "
              "ops/sec  {seconds:.1f} s  {operations} ops".format(**fr))
    rows = out["progress_rows"]
    if rows:
        n = len(rows)
        print(f"per-interval rows: {n}")
        # normalize each row to a cumulative % of the interval that it closes
        for i, r in enumerate(rows, 1):
            print("  [{:2d}/{:2d}] done={:>12.0f} seg={:>10.0f} "
                  "wall={:8.3f}s us/op={:9.3f} ops/s={:9.0f}".format(
                      i, n, r["done"], r["seg"], r["wall"], r["us"], r["ops"]))
    p = out["props"]
    if p:
        want = ("epochs_sealed", "epochs_materialized", "epochs_reclaimed",
                "live_wal_bytes", "sealed_wal_bytes", "materialize_micros",
                "install_direct_base", "install_fallback_l0",
                "csd_files", "csd_attempts", "csd_fallbacks",
                "csd_merge_files", "partition_skew")
        print("props:")
        for k, v in p.items():
            if any(w in k for w in want):
                print("  {:<22} = {}".format(k, v))
    if json_path:
        with open(json_path, "w") as fh:
            json.dump(out, fh, indent=2)
        print(f"json -> {json_path}")


if __name__ == "__main__":
    main()
