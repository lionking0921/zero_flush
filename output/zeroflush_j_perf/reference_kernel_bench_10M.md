# Reference 内核 (compaction_k32v1024) 10M 实测 vs CPU —— 裸内核吞吐报告

> 状态戳：2026-09-09。用户指令（2026-09-08）：放弃当前内核 300MHz build 攻坚，用现成 reference
> 内核（`reference/CoKVKey24Value1024_kernel/compaction_k32v1024_20250825.xclbin`，kernel `krnl_vadd`）
> 与 CPU 做 **10M 次操作**吞吐比较；reference 无 test_bench → 本会话自写（交付物见 §5）。
> 前情：reference 裸核单次物化"接近 CPU" 只是 ICDE'26 全栈声明、从未实测；此报告给裸核真实 10M 数字。

## 0. 摘要 / 结论先行

**Reference 裸内核 10M ops 实测：kernel-only 28.60 s → 349,674 ops/s、373.9 MB/s、2.86 µs/op。**
13 个 launch 全部 `out_entries == in`（10M 输出条目精确吻合），且 12 个同尺寸 800k launch 的
kernel_ms 落在 2287.59–2288.00（极差 < 0.5 ms）→ 确定性、无退化。

**对照 CPU（同一 fillrandom 10M 几何，offload_bench_report.md 基线）：**

| 口径 | CPU | Reference 内核（kernel-only） | 倍率 |
|---|---|---|---|
| 单次物化 build 计算 | 7.5 ms/单元(~3.7k 键) ⇒ **2.01 µs/op**（materialize_micros 20.13s/10M） | **2.86 µs/op** | 内核 **1.42× 慢** |
| 端到端 fillrandom wall | 288,272 ops/s（34.7 s / 10M） | 349,674 ops/s（纯 build 片，非全管线） | 内核 build 片 1.21× 快* |

\* 非完全同口径：参考内核只算 build 步、输入已就位于设备；CPU 288k 是全 fillrandom 管线（含 memtable/
WAL/物化编排）。但足以证明 **build 计算本身不是 CSD 卸载失败的原因**。

**关键修正（对照旧 memory 估算）**：reference 裸核并非"接近 CPU"之 ~23× 慢。实测在 build 计算粒度
**只比 CPU 单核物化慢 ~1.4×**，与 CPU 同数量级。当前 CSD 端到端 32× 负收益的根因是 **host seam
固定往返 + 单实例串行 + 4MB 小单元**（offload report §7），与 kernel 计算能力无关——reference 内核
以 147 MHz 即可做到 2.6–2.9 µs/op，说明 lean 流式 encoder 是现实可达的（M1 当前内核残余 ~6.7 cyc/B
距此仍有 ~17× 每字节差距，见 §4）。

## 1. 测量对象与条件

| 项 | 值 |
|---|---|
| xclbin | `reference/CoKVKey24Value1024_kernel/compaction_k32v1024_20250825.xclbin`（27,315,215 B） |
| kernel | `krnl_vadd`（ABI arg0-7，PPS oracle，见计划/已锁契约） |
| 平台 | SmartSSD U2 gen3x4，device 0，XRT 2.14.354，真卡 |
| 驱动方式 | OpenCL `enqueueTask`（仿 reference vadd.cpp），CL_QUEUE_PROFILING 量 kernel_ev |
| 几何 | user key 24B + 8B seq（内部 key 32B），value 1024B，全局唯一递增 key |
| 输入形态 | 每 launch 由 zf_sst_writer.h 在内存生成 4 个 CoKV SST（每文件 ~200k 或 ~100k 键），
  作为 device 输入缓冲喂入；index 缓冲 = krnl_host.h `index_block_buffer_size` = **8MB（不放大）** |
| 总 ops / 总输入字节 | 10,000,000 / 10,692,477,380（~10.69 GB） |
| kernel 时钟 | **~146.7 MHz**（xclbin 元数据 `Achieved Freq`，kernel ref clk；HLS 目标 200 MHz、
  平台请求 300 MHz → 又一轮时钟 collapse，但参考以 200 MHz 为目标故仍 ~147 MHz） |
| 计数口径 | kernel_ev = CL 事件 END−START（纯设备执行）；correctness = Σ PPS out_entries == Σ in |

## 2. 逐 launch 表（原始，见 `reference_kernel_10m.log`）

```
launch    entries     in_bytes     gen_ms  kernel_ms    wall_ms  ofile   out_ent ok
0          800000    855398260      489.2    2288.00     3892.0      4    800000 OK
1          800000    855398260      521.1    2287.85     4158.9      4    800000 OK
...（2-11 同，kernel_ms 2287.59–2288.00，全部 OK）
12         400000    427698260      243.4    1144.08     2214.3      4    400000 OK

AGGREGATE  entries=10000000 in_bytes=10692477380 gen_s=6.187 io_s=10.188 kernel_s=28.598 wall_s=51.261
  kernel throughput: 349674.1 ops/s   373.89 MB/s  (28.598 s total kernel)
  end-to-end       : 195080.6 ops/s   208.59 MB/s  (51.261 s total wall incl host gen/io)
RESULT: PASS (all launches out_entries==in)
```

- `gen_ms` = host 侧内存生成该 launch 输入 SST 的耗时（10M 合计 6.19 s）；`wall` 含 host gen + 阻塞
  写入 + kernel + PPS 读回。**kernel_ev 与 wall 严格分列**，ops/s 主口径用 kernel-only。
- 每 launch 输出 4 文件、每文件 ~200k 键 ⇒ index/文件 ≈ (200k/4)×10B ≈ 0.5MB ≪ 8MB/4 = 2MB 区域上限
  ⇒ **8MB index 缓冲全程无压力**（用户"8MB 就够"判断成立）。

## 3. 与 CPU 的 10M 对照

CPU 基线取同几何 fillrandom 10M（offload_bench_report.md，16 writer / 24B / 1024B；唯一差异 csd 关）：

| 指标 | CPU (csd=off) | Reference 内核 (kernel-only) | 比值 |
|---|---|---|---|
| materialize_micros（build 计算累计） | 20.13 s | 28.60 s | 1.42×（内核慢） |
| 单次物化平均（~3.7k 键单元） | 7.5 ms | ~10.6 ms（2.86µs×3.7k） | 1.41× |
| 逐 op build 计算 | 2.01 µs | 2.86 µs | 1.42× |
| 数据率（~10.7GB build） | ~531 MB/s | 373.9 MB/s | 1.42× |
| 端到端（全 fillrandom） | 288,272 ops/s（34.7s） | （kernel 纯 build 28.6s） | 见 §3 注 |

**注（口径诚实）**：
1. CPU materialize_micros 是跨 16 并行 worker 的累计 build 账（2.01 µs/op 为单单元串行 build 计算的
   平均，与单内核实例同量纲）→ 与 kernel 2.86 µs/op 直接可比：**build 计算粒度内核慢 ~1.4×**，同一
   数量级，而非 ~23×。
2. kernel 10M 用了 28.6 s < CPU 整个 fillrandom 的 34.7 s，但这不是同口径：28.6 s 只含单设备 build 片、
   输入已备好；CPU 34.7 s 是含全部写管线。它只用来回答"**kernel 计算本身快不快**"——答案：与 CPU build
   相当，绝不构成 32× 负收益的来源。
3. 本数字是 **理想驱动 regime**（输入已为 SST 字节驻留设备、800k 键大 launch 摊薄固定开销、无会话
   mutex/无 ZfSeal）。生产 seam 每 4MB/3.7k 键单元串行卸载 + host 打包/封口会再叠加 seam 固定开销；
   本报告给的是 seam 理论上限，不是 seam 端到端预测。

**对上一 CSD 结论的修订含义**：offload_report §7"FPGA kernel 不是瓶颈"的判断在 reference 内核上得到
**直接实测支撑**——而且 reference 内核 147 MHz 就跑 2.6–2.9 µs/op，把生产单元(~3.7k 键)做掉只需
~10.6 ms，与 CPU 7.5 ms 同量级。32× 负收益 100% 来自 host seam（0.41 s/次固定往返 + 单实例串行），
不是 kernel 能力。

## 4. 每周期账（供 M3 / 当前内核对齐）

kernel-only 28.60 s @ ~146.7 MHz = 4.195 Gcycles 处理 10M 键 ⇒ **~420 cycles/entry ≈ 0.39 cyc/B**。

对比当前 AcceleratorKernelSstV2 内核历史实测（同 1024B value 几何）：
- M1 去 CRC 后 @97.7 MHz：26M cyc / 3.87 MB ≈ **6.7 cyc/B**（含 ~3k cyc/record 固定项 + 宽字搬运不
  充分）→ 距 reference **~17× 每字节差距**。
- 未去 CRC（Task#14）：~15.8 cyc/B（CRC 逐字节查表占 ~84%）。

reference 内核 0.39 cyc/B 的 lean 结构（restart=每 entry 无增量扫描、1024B value 宽切片直通、无 CRC
byte loop）证明**单核达到 CPU 级 build 吞吐无需等 M3**——当前内核的残余 17× 差距就是 encoder 主循环
未流水/宽字化 + 固定项未摊掉，是可对齐、非原理性瓶颈。

## 5. 交付物 / 复现

新增（均在 `reference/CoKVKey24Value1024_kernel/`，只读不破坏既有文件）：
- `zf_sst_writer.h` — CoKV 字节格式 host writer（header-only，CPU-only，参考 decoder 可解析）
- `zf_sst_defs.h` — host 数值常量（index=8MB 等，镜像 krnl_host.h）
- `zf_gen.cc` — 离卡批量生成 CoKV SST 的独立工具
- `zf_bench.cc` — XRT OpenCL driver（gen→4×enqueueWrite→enqueueTask→PPS 读回校验→kernel_ev 聚合）
- `/tmp/zf_bench` 二进制；`output/zeroflush_j_perf/reference_kernel_10m.log`（原始表）

复现（真卡）：
```bash
g++ -O2 -std=c++17 -DCL_HPP_TARGET_OPENCL_VERSION=120 -DCL_HPP_MINIMUM_OPENCL_VERSION=120 \
  reference/CoKVKey24Value1024_kernel/zf_bench.cc -o /tmp/zf_bench \
  -I/opt/xilinx/xrt/include -I/usr/include -L/opt/xilinx/xrt/lib -Wl,-rpath,/opt/xilinx/xrt/lib \
  -lxilinxopencl -pthread
timeout 600 /tmp/zf_bench --xclbin <…>/compaction_k32v1024_20250825.xclbin \
  --device 0 --gen-total 10000000 --launch 800000     # 通过条件：RESULT PASS + 落表
```

## 6. 门禁回顾（真卡实测）

| 门禁 | 结果 |
|---|---|
| 生成器自验（Python decoder mirror 2000 键/250 块×4） | PASS |
| 卡上小门禁：2000 键 1 launch | out=2000，kernel_ev 5.86 ms |
| 真实文件互证：000376(59,067)+000334(95,357)=154,424 | out=154,424，kernel_ev 399 ms（410 MB/s） |
| **10M 放大**：13 launch（12×800k+400k） | **out=10M，kernel 28.60 s，349,674 ops/s** |
| index 8MB 压力 | 无（每文件 index ~0.5 MB ≪ 2 MB 区域） |
