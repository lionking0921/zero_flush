#!/usr/bin/env python3
"""ZeroFlush(当前优化态) vs 原生 RocksDB — 50GB fillrandom/readrandom 对比。

与最近优化验证一致的资源参数：cache 512MB、max_background_jobs=24、
key 16B、value 1KB、16 线程、无压缩、WAL 开启。
仅 --zeroflush 相关开关区分两引擎。write_buffer 用默认（ZF Open 强制 256MB）。
"""
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

NATIVE = "/home/embed/hyl/metadata_offload/source/rocksdb/build/db_bench"
ZF = "/home/embed/hyl/metadata_offload/source/rocksdb-zeroflush/build/db_bench"
OUT = Path("/home/embed/hyl/metadata_offload/output/zeroflush_m3_perf/comp50gb")
OUT.mkdir(parents=True, exist_ok=True)

KEY_SIZE = 16
VALUE_SIZE = 1024
THREADS = 16
NUM = 50_000_000          # ≈50GB 逻辑数据（50M × 1040B ≈ 52GB）
WRITES = NUM // THREADS    # 每线程配额 → 总写 = NUM
READS = 1_000_000 // THREADS  # 每线程配额 → 总读 = 1M
CACHE = 512 * 1024 * 1024
MAXBG = 24

COMMON = [
    "--num=%d" % NUM,
    "--key_size=%d" % KEY_SIZE,
    "--value_size=%d" % VALUE_SIZE,
    "--threads=%d" % THREADS,
    "--compression_type=none",
    "--disable_wal=false",
    "--cache_size=%d" % CACHE,
    "--max_background_jobs=%d" % MAXBG,
]
ZF_FLAGS = [
    "--zeroflush",
    "--zf_partitions=16",
    "--zf_routing=sampled",
    "--zf_base_merge",
    "--zf_skip_batching=false",
    "--zf_value_cache_mb=64",
    "--subcompactions=16",
]
NATIVE_FLAGS = ["--subcompactions=16"]

RE = re.compile(
    r"(?P<name>\S+)\s*:\s*(?P<us>[\d.]+)\s*micros/op\s+"
    r"(?P<ops>[\d.]+)\s+ops/sec\s+"
    r"(?P<sec>[\d.]+)\s+seconds\s+"
    r"(?P<n>\d+)\s+operations;\s*(?P<mb>[\d.]+)\s+MB/s"
)
RE_FOUND = re.compile(r"\((\d+) of (\d+) found\)")


def parse(out: str) -> dict:
    d = {}
    for line in out.splitlines():
        m = RE.search(line)
        if m:
            d["us_per_op"] = float(m.group("us"))
            d["ops_per_sec"] = float(m.group("ops"))
            d["mb_per_sec"] = float(m.group("mb"))
            d["seconds"] = float(m.group("sec"))
            d["operations"] = int(m.group("n"))
        f = RE_FOUND.search(line)
        if f:
            d["found"] = int(f.group(1))
    return d


def run(engine: str, bench: str) -> dict:
    binp = NATIVE if engine == "native" else ZF
    db = f"/tmp/zf50_{engine}"
    shutil.rmtree(db, ignore_errors=True)
    cmd = [binp, f"--benchmarks={bench}", f"--db={db}"]
    cmd += COMMON
    cmd += (NATIVE_FLAGS if engine == "native" else ZF_FLAGS)
    if bench == "fillrandom":
        cmd += [f"--writes={WRITES}"]
    else:
        cmd += [f"--reads={READS}", "--use_existing_db=true"]
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    wall = time.time() - t0
    if r.returncode != 0:
        return {"error": "rc=%d\n%s" % (r.returncode, r.stderr[-2000:]), "wall_s": wall}
    m = parse(r.stdout)
    m["wall_s"] = round(wall, 1)
    # DB 尺寸
    sz = 0
    for p in Path(db).rglob("*"):
        if p.is_file():
            try:
                sz += p.stat().st_size
            except OSError:
                pass
    m["db_bytes"] = sz
    return m


def main():
    result = {"params": {
        "num": NUM, "value_size": VALUE_SIZE, "threads": THREADS,
        "cache_mb": CACHE // 1048576, "max_bg": MAXBG,
    }}
    for engine in ("native", "zeroflush"):
        result[engine] = {}
        for bench in ("fillrandom", "readrandom"):
            print(f"[{engine}] {bench} ...", flush=True)
            m = run(engine, bench)
            result[engine][bench] = m
            print(f"  {m.get('ops_per_sec', 'ERR')} ops/s  {m.get('seconds', '-')}s", flush=True)
    out = OUT / "comp50gb.json"
    out.write_text(json.dumps(result, indent=2))
    print("saved", out)


if __name__ == "__main__":
    main()
