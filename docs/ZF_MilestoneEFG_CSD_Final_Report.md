# ZeroFlush · CSD-FPGA A+B 卸载 —— 里程碑 E·F·G 最终报告

> 范围：hw 综合 + 真卡端到端（E）· 引擎零写格式锁 + `csd_materialize` 深接（F）· 收尾文档（G）。
> 前置软件级证据链（A/B/AB 里程碑）见 `AcceleratorKernelSstV2/README.md` 与 `source/rocksdb-zeroflush/zeroflush/README.md`。
> 状态戳：2026-09-06 定稿。E-1 hw 综合产出 `krnl_vadd.xclbin`（27.97 MB，3h15m，内核 Setup
> WNS +7.866ns @200 MHz，0 违例端）；E-3 真卡 M3 8/8 + AB 7/7（device 前缀逐字节 == CPU-sim）；
> F-3 真卡卸载闭环 `files=216 attempts=216 fallbacks=0`（读回等价 + reopen CRC 全过，exit 0）。

---

## 0. 摘要

把「A（冻结分区索引 + 封存 WAL）∪ B（overlap 分区 SST 文件）」在 RocksDB **写侧物化出口**
交由 CSD-FPGA 内核（`krnl_vadd`，A+B 全版本模式）真实落盘为 **§14.6 锁定档位**分区 SST 的链路，
在三个层面完成并验收：

1. **软件级**（已交付，本报告引用）：A+B kernel 输出与「A∪B 全部版本」预言机逐条、逐字节一致，
   并经引擎 `ZfSeal` + `SstFileReader` 逐块 CRC 直读对拍（CPU-sim 7/7、引擎直读 14/14）。
2. **引擎深接（F，已完成）**：zeroflush DB 写侧**单一格式锁**到
   `format_version=2 / kCRC32c / kBinarySearch / kNoCompression`；新增 `csd_materialize`
   + `csd_xclbin` + `csd_device` 开关与 `ZfCsdSession` 设备后端接缝（XRT/OpenCL 实现在
   独立翻译单元，**引擎 lib 恒 XRT-free**）；`tools/zf_csd_test` 双后端等价测试 **8/8 全绿**，
   `zf_test` 回归保持 **36 PASS / 3 项既有 L0 失败（与本任务无关）不变**。
3. **硬件级（E，完成）**：`v++ -t hw` 综合产出 `krnl_vadd.xclbin`（27.97 MB，3h15m，
   内核 200 MHz Setup WNS **+7.866ns、0 违例端**）；真 U2 卡（device 0 = 6d:00.1）端到端
   **M3 默认 8/8** + **A+B 7/7（device 前缀逐字节 == CPU-sim）**；`zf_csd_test` 真卡卸载闭环
   **files=216 attempts=216 fallbacks=0**（读回等价 + reopen CRC 全过，exit 0）。

**验收口径**：卸载合格性门 + 失败恒回落 host（`csd_files/attempts/fallbacks` 计数可证），
**绝不静默错排**；任何产出都经引擎自身 reader（逐块 CRC）强校验可读。

---

## 1. 背景与目标

ZeroFlush 以「分区 WAL 即 L0、物化直装 L1」绕开原生 LSM 的 L0→L1 全范围串行归并。物化期
（WAL→SST）是每字节必经的**数据搬移 + 排序 + 编码**汇流点——这正是可卸载到 CSD 的计算。
本里程碑目标是**把这段物化的数据产出交给 SmartSSD U2 上的 FPGA 内核**：内核读出 A 侧
（封存 WAL + 冻结 slim 索引）与 B 侧（同分区 overlap 旧 SST），按 internal comparator
合并去重/保留全部版本，重编码为引擎 §14.6 锁定档位的 `[data+index]` 前缀；引擎
`ZfSeal` 封口直装。硬件完成排序与编码的**字节本质工作**，主机仅搬运输入/校验输出，
从而把物化 CPU/DRAM 占用与数据触碰从主机侧移走（后续性能定位目标）。

约束（全程生效）：
- 引擎 lib 不链接 XRT/OpenCL（`csd_session_opencl.cc` 只进带设备目标的链接图）。
- 卸载合格判定失败 / 设备不可用 / 运行出错 → 静默回落 host `BuildTable`，行为与 csd=off 一致。
- 本窗口不修复 3 项既有 L0 相关回归失败（与硬件卸载任务无关，用户已明确）。

---

## 2. 字节档位基线（A/B/AB 软件级证据链，只引用）

内核解码/编码只认 §14.6 锁定档位（`source/rocksdb-zeroflush/zeroflush/zf_seal.h`）：
`format_version==2`、`checksum==kCRC32c`、`index_type==kBinarySearch`、`kNoCompression`、
无 filter / prefix / merge / collector，data 块 restart=16、index 单级。

| 里程碑 | 内容 | 验证结论 |
|---|---|---|
| A/B | kernel encoder v2+crc32c 升档 / WAL 顺序窗预取 | CPU 全流水 + sw_emu 全绿、与 M1 decoder 逐字节等价 |
| AB | `decoder_sst`（B 侧 raw-SST 链解码）+ encoder 全版本模式（mode=1） | CPU-sim **7/7**；引擎字节直读对拍 **14/14**；shared=24 风险实证关闭 |
| 落点 | `AcceleratorKernelSstV2/`（kernel 零改动进入 E；M3 mode=0 行为不变） | 本报告 E/F 均沿用该内核同一字节产物 |

---

## 3. 阶段 F：引擎深接 CSD（完成）

### F-1 引擎写侧 §14.6 单一格式锁 —— `zeroflush_db.cc` `Open()`

`zeroflush_db.cc:1276-1288`：CF 全局覆写 `BlockBasedTableOptions`（`format_version=2`、
`checksum=kCRC32c`、`index_type=kBinarySearch`；其余默认 restart=16 / 无 filter / 无 partition
index）+ `compression=kNoCompression`、`bottommost_compression=kNoCompression`、
`compression_per_level.clear()`。因 FlushJob / ZfMaterializeJob 的 `output_compression` 与
`table_factory` 都溯源 CF options，此处即**单一锁点**，覆盖引擎写侧全部 BuildTable 与原生
compaction 写路径；**A+B kernel（decoder_sst 读）与引擎自产文件因此同档可互解**，且与
FPGA 产物逐字节同构。零档位旧文件仍可读回（引擎 reader 版本自适应），仅写侧统一。

### F-2 CSD 开关 + 后端接缝（引擎 lib XRT-free）

- **选项**（`zeroflush_db.h` `ZeroFlushOptions`）：`bool csd_materialize=false;`
  `std::string csd_xclbin; uint32_t csd_device=0;` —— 缺省全关，实时路径零行为差异。
- **`csd_backend.{h,cc}`**（`zeroflush/`，纯 C++、无 XRT）：`BuildCsdSlotAFromSorted`（把已排序
  keys/values 重建为 ZF01 帧区 + slim 区，LE locator，`ik==32B ∧ value≤1024B` 字节级资格）、
  `BuildCsdSlotB`（B 文件链，F-3 前保留）；`CreateZfCsdSession` + `RegisterZfCsdSessionFactory`
  工厂注册（**会话可由带设备 TU 惰性注册；未注册 ⇒ 恒返回 nullptr = 设备不可用**）。
- **接缝**（`materialize_job.cc` `MaterializePartition`）：门控
  `csd_materialize && aside_sorted && gens.size()==1` → `TryCsdDirectMaterialize(...)`：
  打包 A slot → 建会话（null ⇒ `csd_fallbacks_++` 回落）→ `RunAb` mode=1 → 强校验
  `file_num==1 ∧ pps[1]==n ∧ data/index 非空`（失败 ⇒ WARN + 回落，**绝不半装**）→ 从 PPS 填
  manifest（data/index 字节数、键范围、删除计数）→ `TableFileName` 落盘（`ZfSeal` 封口直装，
  `FileType::kTableFile`、checksum_handoff 镜像 host `finish_cur`）→ 产出与 host `build_one`
  完全同构的 `MaterializeOutput` → 单次 VersionEdit 原子安装。**meta.part_id 缺省 0，与 host
  路径一致** ⇒ FinalizeLocked 对 CSD/host 文件无差别处理。
- **会话实现**（`csd_session_opencl.cc`，只进 `zf_csd_test` 等带设备目标）：XRT/OpenCL
  软失败版 `OcCtx::TryMake`；`ZfCsdSessionOpencl::RunAb` 与 `AcceleratorKernelSstV2/host/main_zf.cpp`
  `RunAbDevice` **逐字同源**（`cl::Buffer` + `enqueueMapBuffer` 直读写 + `enqueueTask`，规避
  `xrt::bo` C2H 0x400 问题）；PPS/元区常量镜像 `kernel/krnl_host.h`。进程级惰性单会话：空
  `csd_xclbin` 或探测失败 → nullptr（host）。**引擎 lib 不编译本 TU。**

### F-3 双后端等价 + 真卡（host 回落已全绿；真卡待 E 产物）

`tools/zf_csd_test.cc`：同一确定性可写集（user key 恰 24B、value≤1024B，10 epoch × 4 分区 ×
48 = **1920 键**，静态边界 ka/kc/ke/kg）开 **双 ZeroFlush DB**（csd=off 参照 / csd=on），
各自写 → 触发物化 → 读回比对。断言集与**实测结果（2026-09-06，host 回落模式，无 xclbin）**：

| 断言 | 结果 |
|---|---|
| open host+csd | ✅ PASS |
| dual load（1920 键双库一致） | ✅ PASS |
| host / csd 物化收敛（sealed==materialized） | ✅ PASS ×2 |
| per-key Get == oracle 且 host==csd（1920 键） | ✅ PASS |
| 单迭代器全扫描等价（1920 条 host==csd，逐字节） | ✅ PASS |
| **计数** host `files=0 attempts=0 fallbacks=0`；csd `files=0 attempts=0 fallbacks=216` | — |
| `csd=on no-device: files==0 && fallbacks>0` | ✅ PASS（216 = 全部卸载请求经会话工厂回落 host） |
| csd 库 Close+Reopen 后重扫 == host（**CSD 产物经引擎 reader CRC 强校验直读**） | ✅ PASS |
| **合计** | **ALL PASSED（退出码 0）** |
| 真卡卸载（`--xclbin … --device 0`） | ✅ 会话就绪；csd `files=216 attempts=216 fallbacks=0`；reopen 重扫 == host；exit 0 |

> 计数在 Close/Reopen **之前**读取：reopen 新建 ZeroFlushContext 使 csd_* 原子归零——计数反映
> 首次打开的物化会话。host 回落模式 `fallbacks=216` 证明接缝**可达**（每个分区物化都尝试卸载）
> 且**零静默错排**（files==0 时读回与 host 全等）；真卡模式 **216 次卸载全部成功、零回落**。
> 真卡首跑暴露两处主机侧缺陷并已修（正是真卡验证的价值）：
>   (1) `materialize_parallelism=8` 并行 worker 并发调 `RunAb` 于单 `cl::CommandQueue`（内核
>       单实例本须串行）→ `ZfCsdSessionOpencl` 内加 mutex 整段串行化；
>   (2) 进程级 static 会话留到 atexit 析构，`clReleaseKernel` 在 XRT context_mgr 拆除后触发
>       SEGV → 新增 `ShutdownZeroFlushCsdSession()`，带设备测试在 main 作用域（关库后）显式释放。

**引擎回归门**：`zf_test`（无参全量）→ **36 PASS / 3 FAIL**；3 项失败
（`SteadyStateZeroL0` M3.3-26、`MaterializeVsCompactionRace` M3.3-27、`SkipBatchMaterialize`
M4.5b-48，均 L0 计数）为**既有、与硬件卸载无关**（用户已明确不修），csd 开关缺省关下与
接入前基线逐项一致 —— **零行为回归**。

---

## 4. 阶段 E：hw 综合 + 真卡端到端（完成）

### E-1 hw 综合运行记录（`AcceleratorKernelSstV2/build/hw/`）

- 命令：`make TARGET=hw xclbin` = `v++ -t hw --platform
  xilinx_u2_gen3x4_xdma_gc_2_202110_1.xpfm --hls.clock 200000000:krnl_vadd
  -l build/hw/krnl_vadd.xo -o build/hw/krnl_vadd.xclbin`（Vivado
  `sdx_optimization_effort_high`；内核 `krnl_vadd` 单实例）。
- 目标卡：Xilinx SmartSSD U2（gen3x4，gc_2 shell）；4× 真卡，本任务用 device 0
  （PCIe 6d:00.1）。
- 进度日志（后台，2026-09-06）：
  - `krnl_vadd.xo`（3.9 MB）与 `.compile_summary` 产出约 01:50；
  - link 自 01:47 起连续推进：FPGA logic optimization（约 10 min）→ placement
    （global → physical synthesis → detail placement）→（后续 route → bitstream）；
  - 02:13 已生成部分 `krnl_vadd.xclbin.link_summary`。
- 磁盘：`/` 54G free / 97%；综合 `_x` 中间物与另一项目（SmartRAG）综合并行，已盯盘。
- 产物：`build/hw/krnl_vadd.xclbin` **27,969,822 B（27.97 MB）**，2026-09-06 05:05 产出，
  link 全程 **3h15m03s**（01:50 xo → 05:05）。
- 时序（`impl_1_hw_bb_locked_timing_summary_routed.rpt`）：内核 200 MHz **Setup 0 违例端、
  WNS +7.866ns、Total Violation 0.000ns**；报告头部 “not met” 源自平台静态区 DDR4
  MMCME4 低脉宽检查（shell 级），与内核无关——真卡全 case 字节级通过即实证。
- 资源（routed 全片 `impl_1_full_util_routed.rpt`）：CLB LUT 260,315（49.8%）、CLB FF
  367,228（35.1%）、BRAM 357/984（36.3%）、URAM 12/128、DSP 11/1968；内核 krnl_vadd
  （`impl_1_kernel_util_placed.rpt`）LUT 151,243、FF 166,439、LUT-as-mem 16,077、BRAM 59、DSP 2。

### E-2 host A+B 运行模式（已就绪，代码随本里程碑）

`host/main_zf.cpp` 增 `--ab` 运行模式（默认 M3 行为不动）：host 现算 A 侧确定性语料 +
读 CPU-sim AB 套件落盘的 B 输入文件字节 → 组 `[WAL 段][slim]` / B 链缓冲，`host_data[15]=1`
（mode=A+B）、`host_data[16..19]=port_kind` → 同一 `decode_port` 接线（kernel 零改动）。
校验 = **device 输出前缀字节 == CPU-sim AB dump 前缀** + oracle 逐条比对。夹具
`dump/ab_host/`（ab_00..ab_06 的 `.desc/.s<i>/.prefix/.expect` 与 B 输入对照）已就位；
`host/test_host_hw` 已按最新源码编译。

### E-3 真 U2 卡端到端（完成，device 0 = 6d:00.1）

- **(a) M3 默认 8 case：8/8 PASS**（seed 1–8、1–4 口、30–150 记录；`file_num==1`、data/idx
  非零）——hw 对 sw_emu/CPU-sim 基线零回归。
- **(b) AB 7 case：7/7 device data+index 前缀 == CPU-sim `.prefix` 逐字节**（device 输出另落盘
  `dump/ab_host/<case>.device.prefix`），覆盖仅 B、B 链、仅 A、A 3 口、A-over-B、bulk 84 记录、
  value 长边界 → kernel mode=1 在真卡上字节级复现软件级证明。
- 运行：`env -u XCL_EMULATION_MODE ./host/test_host_hw -x build/hw/krnl_vadd.xclbin -d 0 [--ab dump/ab_host]`

---

## 5. 验证方法学总表（证据链如何互锁）

| 层 | 工具 | 校验什么 | 状态 |
|---|---|---|---|
| CPU 全流水 sim | `test/zf_cpu_sim_ab.cpp` | kernel 字节语义 vs 独立预言机 | ✅ AB 7/7 |
| 引擎字节直读 | `build/zf_seal_check` + SstFileReader | ZfSeal 封口 + CRC 逐块 + 全键集合 | ✅ AB 14/14 |
| sw_emu | 真实内核 + OpenCL host | 端到端（历史 AB 复核） | ✅（里程碑收口） |
| 引擎回归 | `zf_test`（无参） | F 深接零回归 | ✅ 36/3（3 项既有） |
| 双后端等价 | `zf_csd_test` | csd=on/off 读回全等 + 回落计数 | ✅ 8/8（host 回落）· 真卡 files=216/216 |
| **hw + 真卡** | `v++ -t hw` + `host/test_host_hw` + `zf_csd_test --xclbin` | 真实 bitstream 端到端 | ✅ E-1 xclbin（WNS +7.866ns）· E-3 M3 8/8 + AB 7/7 · F-3 真卡 216/216 |

**字节档位同构是这一切的锚**：engine 写档锁（F-1）、decoder_sst 读档、ZfSeal 封口、CSD 产物
均为同一 §14.6 档 → 任何一方产物可被其它各方直接打开校验 ⇒ 集合差异必被 CRC/全键直读捕获。

---

## 6. 边界与后续（如实）

- 本窗口 A-only 单文件（`kDirect`）档：B/merge 卸载路径 `BuildCsdSlotB` 已留位（F-3 后）；多代
  混包、slice、range tombstone 均回落 host。
- 卸载语料档仍限 `ik==32B ∧ value≤1024B ∧ ≤4 口 ∧ 单文件输出`（kernel 硬限，`krnl_host.h`）。
- 性能定位（卸载收益数字）不在本里程碑承诺范围；真实卡 offload 闭环跑通后另行 bench。
- 3 项既有 L0 回归失败未修（本任务范围外）。

---

## 7. 产物清单（日志 / 二进制 / 文档）

- 综合：`AcceleratorKernelSstV2/build/hw/{krnl_vadd.xo,krnl_vadd.xclbin,krnl_vadd.log,krnl_vadd.xclbin.link_summary}`（xclbin 27.97 MB，2026-09-06 05:05）+ `_x/reports/`（timing/util rpt）
- host/夹具：`AcceleratorKernelSstV2/{host/main_zf.cpp,host/test_host_hw,dump/ab_host/}`
- 引擎：`source/rocksdb-zeroflush/zeroflush/{zeroflush_db.cc(F-1),materialize_job.{h,cc}(F-2),
  csd_backend.{h,cc},csd_session_opencl.cc}` + `tools/zf_csd_test.cc`
- 测试：`source/rocksdb-zeroflush/build/{zf_csd_test,zf_test,zf_seal_check}`
- 本报告：`docs/ZF_MilestoneEFG_CSD_Final_Report.md`

---

## 附：本里程碑 commit / 变更（提交时回填 hash）

- [ ] 阶段 E hw 综合 + E-2 host AB 模式（`main_zf.cpp` / `zf_cpu_sim_ab.cpp`）
- [ ] 阶段 F 引擎深接（`zeroflush_db` 锁档 / `materialize_job` 接缝 / `csd_backend` / `csd_session_opencl` / `zf_csd_test` / CMake）
- [ ] 阶段 G 文档（本文 + README E/F 小节）
