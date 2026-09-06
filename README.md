# ZeroFlush

基于 RocksDB 9.x 的分区 WAL 存储引擎 —— KV 直接追加分区 WAL，物化（WAL→SST）按分区并行直装，绕开原生 LSM 的 L0→L1 全范围串行归并。

## 核心思想

原生 RocksDB 的 L0 文件覆盖全 key 范围，L0→L1 compaction 每次重写整个 L1（O(|L1|)）且**并发恒为 1**（L0 文件两两重叠 → 不可并行）——它是写路径上每字节必经的唯一串行汇流点。ZeroFlush 把这一汇流点拆解为 P 个独立分区：

```
写路径：        KV → 分区 WAL（P 个分片） + Slim 索引（key → locator）
封存（seal）：  活跃字节达阈值 → 冻结分区索引 + 切表
物化：          读 WAL + 排序 + BuildTable → 分区范围文件 → 直装 L1（替换重叠文件）
读路径：        分区索引命中读 WAL（未物化数据）；否则原生 SST 查找
```

- **分区 WAL**：value 唯一持久副本在 P 个 WAL 分片中，写组按路由表分发，无 memtable 出清瓶颈
- **Slim MemTable**：仅存 key → locator（16B），value 定点读 WAL
- **物化直装**：输出分区范围 SST，直装 L1 并替换重叠文件（无 L0 循环、无全范围重写）
- **学习路由**：kSampled 首轮采样学习边界，后续按分区边界写入

## 目录结构

```
source/rocksdb-zeroflush/     ZeroFlush 引擎（RocksDB 9.x 源码 + ZF 改动）
  ├── zeroflush/               核心实现（分区 WAL、分区索引、物化 job、路由表）
  ├── db/db_impl/             写路径接线（封存触发、compaction 协调）
  ├── tools/zf_test.cc        回归测试套件（38 项：持久化/封存/物化/竞态）
  └── tools/zf_check_keys.cc  数据完整性工具（逐 key 枚举验证）
output/zeroflush_m3_perf/     实验套件（脚本 + 数据 + 报告）
  ├── integrated_report/      综合报告与数据附件（E1-E7 全系列）
  ├── l0_bottleneck/          L0 写路径瓶颈三角度互证（论文核心实验）
  ├── l0_l1_importance/       L0→L1 重要性实验（L1 放大四预测验证）
  ├── native_wa_stall_100gb*/ 原生写放大/三类写停顿画像（16/8/4/2 线程）
  └── bench_matrix*            ZeroFlush vs 原生 24 配置矩阵
```

## 构建与测试

```bash
cd source/rocksdb-zeroflush
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) db_bench zf_test
./zf_test                 # 回归套件（38 项，期望 ALL PASSED）
```

完整性校验（100GB 级数据逐 key 枚举）：

```bash
# 写入 100GB（16 线程）
./db_bench --benchmarks=fillrandom --db=/tmp/db --num=103244288 --threads=16 \
  --key_size=16 --value_size=1024 --zeroflush --zf_partitions=16 \
  --zf_routing=sampled --zf_base_merge --zf_skip_batching=false
# 重开后逐 key 校验命中率 = 63.2%（随机写覆盖期望 1-1/e）
./zf_check_keys /tmp/db 103244288 sampled
```

## 实验结果摘要（100GB/1KB fillrandom，16 线程）

| 指标 | 原生 RocksDB | ZeroFlush |
|---|---|---|
| 写吞吐（磁盘瓶颈规模） | 基线 | 差距 1.04-1.39× |
| 写停顿占比 | 89-95%（memtable stop 主导） | — |
| 总写放大 | 9.8×（L2/L3 占 68%） | — |
| 100GB 读吞吐 | 基线 | 0.45-0.96×（反超，尾部 P99 低一个量级） |

**L0→L1 瓶颈证据链**（详见 `output/zeroflush_m3_perf/integrated_report/REPORT.md`）：
1. L0→L1 最大并发恒为 1（8 组运行、625 job、约 20 小时），深层车道并发达 18；
2. 单 job 代价 O(|L1|)：L1 从 256MB 放大到 4GB，输入 SST 11→80、P99 18.9→50.2s；
3. 剂量-响应倒 U：默认 `level0_file_num_compaction_trigger=4` 为局部最优，任一方向调参吞吐降 11-22%；
4. 11 组参数（L1 × T × 线程数）停顿占比 76-95% 无一逃逸——L0 是写路径结构性瓶颈。

## 关键设计文档

- `source/rocksdb-zeroflush/zeroflush/M4_DESIGN.md` —— 里程碑设计（M4.8 迁移路径、M4.11 切片、R54-R57 竞态修复、读路径布隆优化）
- `output/zeroflush_m3_perf/integrated_report/REPORT.md` —— 综合实验报告（E1-E7 全系列 + 数据附件清单）
- `docs/ZF_MilestoneEFG_CSD_Final_Report.md` —— CSD-FPGA A+B 卸载里程碑 E·F·G 最终报告
  （hw 综合 + 真卡端到端 · 引擎写档锁 + `csd_materialize` 深接 · 双后端等价）
- `AcceleratorKernelSstV2/README.md` —— FPGA 加速器各里程碑（M1/顺序窗/A+B/E·F/阶段 I）
  （阶段 I = 真重写「裁剪/compaction mode」内核：mode2=每 user 键留最新版（含 tombstone）
  CPU-sim 证明 10/10 + 引擎封口直读锚定 20/20；encoder 文件收尾鲁棒性修复；本阶段不含综合）

## 说明

本仓库仅包含 ZeroFlush 引擎源码与实验套件（不含无关的并行项目）。RocksDB 上游基线为 11.2.0。
