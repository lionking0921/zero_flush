#!/bin/bash
# 50GB fillrandom + readrandom 对比：ZeroFlush vs 原生 RocksDB
# 参数对齐 rocksdb_vs_zeroflush_comparison.md 公共配置（1KB 值）。
set -u
cd /home/embed/hyl/metadata_offload

ZF_BIN=source/rocksdb-zeroflush/build/db_bench
NATIVE_BIN=source/rocksdb/build/db_bench
# 原生库路径仅对 native 命令生效（全局 export 会污染 ZF 二进制）。
NATIVE_LD="LD_LIBRARY_PATH=/home/embed/hyl/metadata_offload/source/rocksdb/build:/usr/lib/x86_64-linux-gnu"

COMMON="--num=48000000 --writes=3000000 --threads=16 --key_size=16 --value_size=1024 \
--compression_type=none --disable_wal=false --cache_size=536870912 --max_background_jobs=24 --subcompactions=16"
ZF_FLAGS="--zeroflush --zf_partitions=16 --zf_routing=sampled --zf_base_merge \
--zf_skip_batching=false --zf_value_cache_mb=64"
READ_FLAGS="--reads=1000000 --use_existing_db=true"

echo "=== start $(date) load=$(cat /proc/loadavg | cut -d' ' -f1-3)"

# ---- 1. ZF fillrandom ----
rm -rf /tmp/zf50
echo "=== ZF fillrandom start $(date)"
$ZF_BIN --benchmarks=fillrandom --db=/tmp/zf50 $COMMON $ZF_FLAGS > /tmp/zf50_fill.log 2>&1
echo "ZF fill rc=$? $(date)"

# ---- 2. ZF readrandom ----
echo "=== ZF readrandom start $(date)"
$ZF_BIN --benchmarks=readrandom --db=/tmp/zf50 $COMMON $ZF_FLAGS $READ_FLAGS > /tmp/zf50_read.log 2>&1
echo "ZF read rc=$? $(date)"

# ---- 3. native fillrandom ----
rm -rf /tmp/native50
echo "=== native fillrandom start $(date)"
env $NATIVE_LD $NATIVE_BIN --benchmarks=fillrandom --db=/tmp/native50 $COMMON > /tmp/native50_fill.log 2>&1
echo "native fill rc=$? $(date)"

# ---- 4. native readrandom ----
echo "=== native readrandom start $(date)"
env $NATIVE_LD $NATIVE_BIN --benchmarks=readrandom --db=/tmp/native50 $COMMON $READ_FLAGS > /tmp/native50_read.log 2>&1
echo "native read rc=$? $(date)"

echo "=== all done $(date) load=$(cat /proc/loadavg | cut -d' ' -f1-3)"
