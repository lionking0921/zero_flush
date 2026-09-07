# ZeroFlush CSD 物化卸载收益实验报告 —— fillrandom CPU vs FPGA（真 U2 卡）

> 范围：把引擎「封存 epoch → SST」的物化 build 步交给 SmartSSD U2 FPGA（`krnl_vadd` hw xclbin），
> 与同引擎 host 物化（csd=off）对比端到端 fillrandom 吞吐与 materialize 耗时。
> 状态戳：2026-09-07。**结论：本 workload 几何下 CSD 卸载为显著负收益（端到端慢 ~32×、materialize
> 累计慢 ~164×），卸载 100% engage、零回落（`csd_files=attempts=2704, fallbacks=0`）。**

---

## 0. 摘要 / 结论先行

用户要求「完成 100M fillrandom，与 CPU 比较，生成实验报告；跑批中每 10M 记一次性能，须有 10 对数据」。
磁盘/时长权衡后（100M@1KB 需 ~85GB+，CSD 组实测 ~9k ops/s ⇒ 100M ≈ 3h），按用户决定**两组先缩到 10M 出结论**
（interval 逻辑按 total/10 取档，10M 组每 1M 一行 → **仍恰 10 对数据**）；另保留一组完整 **CPU 100M**（已跑完）
作规模退化参考。

**主要数字（10M×2，同几何同 workload，唯一差异 `csd_materialize`）：**

| 指标 | CPU (csd=off) | CSD (真卡, csd=on) | 倍率 |
|---|---|---|---|
| fillrandom ops/sec | **288,272** | **8,986** | CSD 慢 **32.1×** |
| 总 wall（10M） | 34.7 s | 1,112.8 s | CSD 慢 **32.1×** |
| materialize_micros（累计） | 20.13 s | 3,298.5 s | CSD 慢 **163.9×** |
| 平均每次物化 | 7.5 ms | 1,219.8 ms | CSD 慢 ~164× |
| SST 安装数 | 2,700 | 2,704 | ~1:1 |
| epochs sealed | 676 | 676 | 相等 |
| csd_files / attempts / fallbacks | 0 / 0 / 0 | **2,704 / 2,704 / 0** | 100% 卸载 |

**为什么负收益（根因）**：这套 workload 的物化单元极小——每次 RunAb 只有 ~3.7k 键、~4MB SST（10M 拆成
2,704 个设备调用）。CSD seam 每次卸载串行付全价（host 打包 A 槽 → OpenCL 每次现建 buffer + 同步
map/unmap ×~7 块 → PCIe 搬入搬出 → enqueueTask/finish 往返 → ZfSeal），且**单内核实例 + 会话 mutex 把
并行的物化 worker 串行化**到单设备队列 → 实测 ~2.4 次设备 run/秒（~0.41 s/次）。而 CPU 组把同样的物化
并到 36 核上并行 host build（78 文件/s 聚合），无此串行瓶颈。卸载只在「物化单元大、kernel 计算时间占比
高」时才有机会赢；4MB 单元下纯亏。**FPGA kernel 本身不是瓶颈，瓶颈在主机侧每次卸载的固定往返 + 串行化。**

> 前情：本跑批最初发现 CSD 卸载**零 engage**（524/524 次 RunAb 被内核拆成 2 文件而判拒回落，csd_files=0、
> 吞吐崩到 ~10k→再雪崩）。根因与修复见 §6，修复后 10M 组 **fallbacks=0、csd_files=attempts**。

---

## 1. 实验配置

### 1.1 workload / 引擎 / 硬件

| 项 | 值 |
|---|---|
| 引擎 | ZeroFlush（RocksDB fork），csd=off（host 物化）vs csd=on（FPGA 物化） |
| workload | `fillrandom`，10M 键 / 组（另有一组完整 CPU 100M 参考） |
| 键 | user key 恰 **24B**（卸载硬限，GenerateKeyFromInt 低 8 字节 LE 计数 + '0' 填充） |
| 值 | **1,024B**（≤ kernel 上限 1024，近卸载上限档） |
| 线程 | 16 writer；`--max_background_jobs=16` |
| 几何 | `--zf_partitions=64 --zf_partition_target_mb=32 --zf_epoch_target_mb=128`（代表性几何；10M 组 epochs_sealed=676） |
| DB 路径 | `/mnt/smartssd2/zf_j_perf/groupA2_cpu_10m_db`（CPU）、`…/groupB2_csd_10m_db`（CSD） |
| 硬件 | SmartSSD U2 gen3x4（XRT 2.14.354），device 0 = `6d:00.1`（xbutil：4 卡均 Ready） |
| xclbin | `AcceleratorKernelSstV2/build/hw/krnl_vadd.xclbin`（28,201,289 B，阶段 J hw 真卡已验证） |
| 正确性 | 卸载产物字节档位在阶段 E/F/J 已门证（sw_emu + hw 真卡 `files=216/216`、`merge_files=31`、reopen CRC==oracle）；本跑批 exit 0，未重复全量 oracle |

### 1.2 命令（两组仅 csd 开关/xclbin 差异）

```bash
BIN=source/rocksdb-zeroflush/build/db_bench
COMMON="--benchmarks=fillrandom --num=10000000 --writes=625000 \
  --key_size=24 --value_size=1024 --threads=16 --compression_type=none \
  --max_background_jobs=16 --zeroflush --zf_partitions=64 \
  --zf_partition_target_mb=32 --zf_epoch_target_mb=128 \
  --level0_slowdown_writes_trigger=64 --level0_stop_writes_trigger=72 --zf_props=1"
# 组 A2 CPU：csd 缺省关
env -u XCL_EMULATION_MODE LD_LIBRARY_PATH=/opt/xilinx/xrt/lib:$PWD/.../build \
  $BIN $COMMON --db=/mnt/smartssd2/zf_j_perf/groupA2_cpu_10m_db
# 组 B2 CSD：开卸载，真卡
env -u XCL_EMULATION_MODE LD_LIBRARY_PATH=/opt/xilinx/xrt/lib:$PWD/.../build \
  $BIN $COMMON --db=/mnt/smartssd2/zf_j_perf/groupB2_csd_10m_db \
  --zf_csd=1 --zf_csd_xclbin=$PWD/AcceleratorKernelSstV2/build/hw/krnl_vadd.xclbin --zf_csd_device=0
```

> 说明：`--writes=625000` 为**每线程配额** ⇒ 总写入 = 625,000×16 = 10M；`num=10,000,000` 保 key 唯一。
> `ReportWriteIntervalOps` 按 total/10 自动取间隔 ⇒ 每 1M 打一行 `ZFPROGRESS`，两组各 10 行。
> props 经 `GetProperty` 在进程内（关库前）打印，计数器为本次 Open 会话累计。

---

## 2. 端到端结果汇总

| 指标 | CPU A2（csd=off） | CSD B2（csd=on 真卡） | 倍率（CSD/CPU） |
|---|---|---|---|
| fillrandom ops/sec | 288,272 | 8,986 | 32.1× 慢 |
| micros/op | 3.47 | 111.3 µs | — |
| 总 wall | 34.7 s | 1,112.8 s | 32.1× 慢 |
| DB 规模 | ~9 GB | ~9 GB | ~1:1 |
| epochs sealed / materialized | 676 / 674 | 676 / 673 | 相等 |
| SST 安装（fallback_l0 + direct_base） | 2,699+1=2,700 | 2,703+1=2,704 | ~1:1 |
| csd_files / attempts / fallbacks | 0 / 0 / 0 | **2,704 / 2,704 / 0** | — |

---

## 3. 主指标：materialize（卸载精确替换的那一步）

物化在封存点同步执行（task pool + join）→ 写吞吐受物化门控，materialize 是「每字节必经」步。

| | CPU | CSD | 倍率 |
|---|---|---|---|
| `zf.materialize_micros`（累计，跨 worker） | 20,129,570 µs（20.1 s） | 3,298,464,222 µs（3,298.5 s） | **163.9× 慢** |
| 平均每次物化（累计/安装数） | 7.5 ms | 1,219.8 ms | ~164× 慢 |

> 口径如实：materialize_micros 是物化各次调用耗时之和（并行 worker 内累计），反映卸载替换的 build 步
> 总 CPU 计算账；端到端 ops/s/wall 反映物化背压对写吞吐的约束。两个口径 CSD 均显著劣于 CPU。

---

## 4. 每 1M 的 10 对性能数据（用户要求：跑完要有 10 对）

两组成对点列（writes_done 对齐，`ZFPROGRESS` 每 1M 一行）。CSD 各区间 ~8.7–10.3k ops/s 全程平稳，
CPU ~250–338k ops/s；**10 对全部显示 CPU 快 27–35×**。

| # | writes_done | CPU wall(s) | CPU ops/s | CSD wall(s) | CSD ops/s | CPU/CSD |
|---|---|---|---|---|---|---|
| 1 | 1,000,000 | 2.954 | 338,568 | 97.281 | 10,279 | 32.9× |
| 2 | 2,000,000 | 3.216 | 310,965 | 112.464 | 8,892 | 35.0× |
| 3 | 3,000,000 | 3.345 | 298,931 | 115.461 | 8,661 | 34.5× |
| 4 | 4,000,000 | 3.478 | 287,504 | 110.683 | 9,035 | 31.8× |
| 5 | 5,000,000 | 3.346 | 298,897 | 110.677 | 9,035 | 33.1× |
| 6 | 6,000,000 | 3.499 | 285,811 | 114.005 | 8,772 | 32.6× |
| 7 | 7,000,000 | 3.220 | 310,569 | 113.994 | 8,772 | 35.4× |
| 8 | 8,000,000 | 3.984 | 250,988 | 110.430 | 9,056 | 27.7× |
| 9 | 9,000,000 | 3.378 | 296,034 | 112.300 | 8,905 | 33.2× |
| 10 | 10,000,000 | 4.270 | 234,180 | 115.532 | 8,656 | 27.1× |

（CPU/CSD 列 = CPU ops/s ÷ CSD ops/s；越大于 1 表示 CSD 越慢。）

---

## 5. 100M 规模参考（CPU 已跑完）+ CSD 100M 外推

按用户「先缩 10M 出结论」决定，**CSD 100M 未跑**；给出参照与线性外推供决策。

**CPU 100M（组 A，真实跑完，10M 分辨率 10 行）：**

| | 值 |
|---|---|
| fillrandom ops/sec（全程平均） | 164,515（首 10M 271k → 末 10M 137k，随 DB 增长单调降） |
| 总 wall | 607.8 s（10.1 min） |
| `zf.materialize_micros` | 408.6 s（累计） |
| epochs sealed / materialized | 6,809 / 6,807；SST 安装 27,228 |
| csd_files | 0 |

**CSD 100M 线性外推**（按 10M 实测 8,986 ops/s、18.5 min/10M）：100M ≈ **185 min ≈ 3.1 h**；
若叠加 CPU 组同款 DB 增长退化（~-40%），实际会更长。磁盘需 ~85GB+（本机原 `/` 仅 84G free，
已迁移 `/mnt/smartssd2` 3.4T）。

**参考：CPU 100M 的 10 行（每 10M 一行，随规模退化轨迹）**——voluntarily 供报告对比，非配对数：

| # | writes_done | wall(s) | ops/s |
|---|---|---|---|
| 1 | 10,000,000 | 36.888 | 271,094 |
| 2 | 20,000,000 | 50.355 | 198,591 |
| 3 | 30,000,000 | 58.547 | 170,804 |
| 4 | 40,000,000 | 68.803 | 145,343 |
| 5 | 50,000,000 | 58.159 | 171,943 |
| 6 | 60,000,000 | 67.945 | 147,178 |
| 7 | 70,000,000 | 65.762 | 152,063 |
| 8 | 80,000,000 | 61.661 | 162,177 |
| 9 | 90,000,000 | 66.855 | 149,577 |
| 10 | 100,000,000 | 72.873 | 137,225 |

---

## 6. 卸载 engage 证据 + 关键修复（本跑批促成）

### 6.1 曾阻塞：CSD 卸载零 engage（100% 回落）

首次 2M CSD 跑批（同几何）:全 524 次 RunAb 判拒回落 → `csd_files=0, attempts=fallbacks=524`，
吞吐崩到 9.9k（比修复后更糟：每次先付设备 run 又丢结果再 host 重做）。

根因（读 kernel + seam 定位）：engine 传给会话的 `sst_bytes` 预算被 kernel 当**总预算**并按
`MAX_OUTPUT_FILE_NUM=4` 等分出**每文件配额** `file_limit = round4k(sst_bytes/4)`（`krnl_vadd.cpp` RunAb 尾）；
encoder 数据超该配额即滚下一文件 ⇒ `file_num>1`。engine 原传 `sst_bytes = 2×staged+64KB` ⇒ 配额 ≈
staged/2 < 全量输出 ≈ staged ⇒ **恒 2 文件**（实测 `files=2 pps1≈n/2`），触发 seam 的
「单文件契约 `file_num==1 ∧ pps[1]==n`」判拒 → host 回落。正确性机制本身工作正常。

**修复**（`materialize_job.cc` TryCsdDirectMaterialize，一处）：`sst_bytes = 2×staged+64KB` → **`8×staged+64KB`**
（配额 ≈ 2×staged，~2× 裕量留单文件；读回为 PPS 前缀直读，多余预算只增缓冲容量、不增 DMA）。
修复后 500k sanity：`A-only run failed` 524/524 → **0**；`csd_files=108, attempts=112, fallbacks=0`。

### 6.2 本跑批 CSD 10M：100% engage、零回落

```
zf.csd_files     = 2704
zf.csd_attempts  = 2704
zf.csd_fallbacks = 0        # 10M 全部卸载成功，无一回落 host
```

即：**负收益不是「卸载没发生」造成的**——2,704/2,704 次物化都真实在 FPGA 上完成并安装了产物；
负收益来自每次设备卸载的固定往返/串行开销 ≫ 它省下的 host 编码时间。

---

## 7. 根因分析（为什么 CSD 慢 32×，kernel 不是瓶颈）

证据：CPU 组 78 文件/s 聚合（2,700 文件/34.7 s），CSD 组 ~2.4 设备 run/s（2,704/1,112.8 s），
即每次设备 run 墙钟 ~0.41 s。对照 CPU 平均每次物化累计仅 7.5 ms。0.41 s 里 kernel 编码 ~4MB 只占
几 ms 级；大头是主机侧 seam 开销 + 串行化：

1. **每次 RunAb 的固定 OpenCL 往返**：`csd_session_opencl.cc` 每次现建 8 个 `cl::Buffer`，对 ~5–7 块
   做同步 `enqueueMapBuffer`/`enqueueUnmapMemObject`（隐含 PCIe 同步），再 `enqueueTask`+`clFinish`——
   每 run 的启动/同步延迟远大于 kernel 实际算时。
2. **单内核实例串行化**：`nk=krnl_vadd:1` + 会话 `run_mu_` 把引擎并行的物化 worker（
   `materialize_parallelism`）全部压到单设备队列；设备侧本就必须一次一 run，CPU 多核并行优势尽失。
3. **单元太小**：fillrandom 这套几何每次物化仅 ~3.7k 键 / ~4MB SST ⇒ 10M 需 2,704 次卸载，固定开销被
   乘 2,704 次。单元 ~10× 大时固定开销可摊薄 ~10×，但单内核吞吐上限仍在（kernel 编码 ~4MB 若 ~ms 级，
   摊到每键的往返仍是几十 µs，远高于 CPU ~3.5 µs/键 的多核物化）。
4. CSD 还额外付：A 槽 host 打包（虽然排序在引擎已完成）+ 卸载产物 ZfSeal 落盘与 CRC 校验回读。

**推论**：在「小 SST、大并发写、CPU 多核便宜」的 fillrandom 形态下，把 ~20 ms 的多核并行 host 物化换成
~1.2 s 串行设备卸载是结构性倒挂；卸载要体现收益需单次物化 ≥ 数十 MB 且物化为热瓶颈的场景。

---

## 8. 诚实边界

- 卸载只替换「epoch 物化 → SST」build 步；写路径（partitioned-WAL）、ZfSeal 封口、RocksDB 背景 compaction
  两组成分相同 ⇒ 差异即卸载本身。**两组成分等价、文件数相等（2,700 vs 2,704）、epoch 数相等（676）**。
- 性能数字是**生产 seam as-integrated** 的实测：未单独 profiling 去剥离 cl 往返/串行/打包各自占比
  （§7 的分解是代码审读级，非逐项计时）。
- CSD 10M 产物正确性未在本跑批重复全量 oracle；正确性以阶段 E/F/J 门证 + 本跑批 exit 0 为准。
- 用户已决定两组缩 10M 先出结论 ⇒ **CSD 100M 未跑**（§5 外推 ~3.1h）。CPU 100M 已真实跑完，保留为
  规模退化参考。
- 磁盘约束：100M@1KB ≈ 85GB+，本机 `/` 仅 84G free ⇒ 已迁移至 `/mnt/smartssd2`（3.4T）。

---

## 9. 复现 / 产物清单

- 脚本：`run_offload_bench.py <db_bench.log> [--json out.json]`（解析 fillrandom / 10 行 ZFPROGRESS / zf props）
- 原始日志：`groupA2_cpu_10m.log`、`groupB2_csd_10m.log`、`groupA_cpu_100m.log`（均在
  `output/zeroflush_j_perf/`，DB 在 `/mnt/smartssd2/zf_j_perf/`）
- 结构化：`groupA2_cpu_10m.json`、`groupB2_csd_10m.json`、`groupA_cpu_100m.json`
- 代码改动：`tools/db_bench_tool.cc`（CSD flags + props + `ReportWriteIntervalOps`）、
  `tools/db_bench_tool.cc` ~Benchmark 的 Shutdown、`CMakeLists.txt`（db_bench 链 zeroflush_csd）、
  `zeroflush/materialize_job.cc`（§6.1 卸载预算修复 `sst_bytes 2×→8×`）
- 复现命令：见 §1.2（num/writes 换成对应规模即可：100M → `--num=100000000 --writes=6250000`）
