# ZeroFlush FPGA 加速器 · 里程碑 2：WAL value 顺序化预取（AcceleratorKernelWalPrefetch）

在 `../AcceleratorKernel/`（里程碑 1，sw_emu + hw 真卡全绿，已交付）基础上实现的
**后续里程碑**：给 decoder 的 WAL 区 value 读取加**顺序窗预取**，把"按 slim 键序逐记录
随机窄读 WAL 帧"改为"以 1024B 固定窗为粒度把 WAL 段**整窗一次线性顺序读入**片上缓冲，
再从缓冲切帧/切字节"。源码放在**新 sibling 目录**，`../AcceleratorKernel/` 源码与
`build/*/krnl_vadd.xclbin` **原样保留、零改动**；本目录仅交付 **sw_emu** 验证产物
（hw 综合留给用户决定）。

对应字节格式文档
`../source/rocksdb-zeroflush/zeroflush/FPGA_SLIM_MEMTABLE_BYTEFORMAT.md` §16.2：
形态 B 已是"整段顺序搬入缓冲、`base+wal_offset` 缓冲内取帧"（host DMA 侧）；
本里程碑在 device decoder 侧落地 §16.2 直连变体的"按连续窗取回再本地切帧、保持读侧近顺序"
同构优化。

## 为什么"顺序化"只能落在物理读形态（设计前提，已核实）

- merge（`kernel/krnl_vadd.cpp`）逐条输出、不丢弃任何记录；encoder 每读一条 km 同步读
  胜者口的 2×512B value 片，**同 user key 遮蔽记录 key 置空但依然消费该口 2 片**。
- ⇒ 每口 value 流消费顺序 = 该口内部键序（全序、无洞），decoder 的物化**输出序被协议锁死，
  不能靠重排记录换取顺序读**；唯一可行的是把"随机细读"聚成"整窗线性顺序读 + 片上取字节"，
  输出逐字节不变。

## 改动面（相对 AcceleratorKernel）

```
kernel/krnl_vadd.cpp          唯一改动文件：decoder 主体物化段改写 + helpers 追加窗原语
tools/zf_decoder_helpers.inc  helpers 单一事实源：追加 zfwin_* 四原语（与 kernel 内嵌一致）
tools/zf_decoder_new.inc      decoder body 单一事实源：追加窗状态声明 + 新物化段
test/zf_cpu_sim.cpp           BytesToWords padding +1024 字（防整窗直读越出 vector，仅测试）
host/*  Makefile  kernel/krnl_host.h  .gitignore  原样复制，零改动
README.md                     ← 本文档
```

merge / encoder / PPS 封口 / 顶层接口 / 内核签名 / `host_data[15]` 布局 **全部不动**。
`slim` 区顺序读保持 `zf_ld_*` 直读（不进窗），避免误改顺序区读路径。

## 设计：顺序窗预取 decoder

### 片上窗缓冲（每口，decoder 局部非 static）

```
窗 WIN = 1024 B = 256 × ap_uint<32>
ap_uint<32> win[256];         // win[j] = 缓冲 (win_base + 4j) 处的 4B
ap_uint<64> win_base;         // 当前窗首字节地址（1024 对齐）
bool         win_valid;       // 是否已载过窗
```

窗状态在 decoder 主循环外声明、循环内使用；4×decoder 各持独立局部窗（非 static）。

### 四个原语（`tools/zf_decoder_helpers.inc`）

| 原语 | 行为 |
|---|---|
| `zfwin_load(buf, win, win_base, win_valid, aligned_base)` | `for j<256: win[j]=buf[(aligned_base>>2)+j]` —— 整窗**一次线性顺序读**（地址连续递增，HLS 合成宽线性 burst）；`win_base=aligned_base; win_valid=1` |
| `zfwin_ensure(buf, …, addr)` | `!win_valid \|\| addr∉[win_base,win_base+1024)` → `zfwin_load(addr & ~1023)` |
| `zfwin_u8 / zfwin_u32(…, addr)` | 经窗读 1/4 字节（LE，未对齐安全，固定位段拼装，可综合） |
| `zfwin_read16(…, addr)` | 经窗读 `[addr,addr+16)` 整 16B（填 2×512B 片用；4×`zfwin_u32` 固定位段拼 128b） |

### 物化段新流程（字节语义与里程碑 1 完全一致）

```
rec_addr = wal_offset                          // WAL 区缓冲内帧首（≤ file_size）
wkey_len = zfwin_u32(rec_addr+8); val_len = zfwin_u32(rec_addr+12)   // 24B header，经窗
vbase    = rec_addr + 24 + wkey_len
发 km({key, value_length=val_len})             // 顺序与旧 decoder 一致：km 先于 value 片
64×16B 循环 if(base<val_len): t = zfwin_read16(vbase+base); sl0/sl1.c[w]=t   // w<32→sl0
写 sl0、sl1                                    // 固定 2×512B，尾部零填充
```

- **命中**：相邻 slim 记录的帧落同一已载窗 → 零 DRAM，片上切字节。
- **miss**：恰一次整窗 256×4B 线性顺序读（物理读从逐记录随机细读聚成按窗顺序突发）。
- **骑窗**：帧跨 1024B 窗界。读按字节粒度做（`zfwin_u32` 由 4×`zfwin_u8` 组成，每字节
  独立 `zfwin_ensure`），**单字节读永不骑窗**，跨窗帧由连续字节读自然续载下一窗 ——
  不需专门慢路径，字节语义精确。

### 与里程碑 1 字节一致的三重保障

1. 填片仍**整 16B 字覆盖**（含 val_len 非 16 倍数时尾字越界字节照读进片，encoder 只认
   `value_length` 字节）——与旧 decoder 完全相同的 pad 语义。
2. header 字段位置/LE 不变；slim 区读（组键、locator、varint）不进窗、保持直读。
3. km/value 写顺序、SIGNAL/MAX_KEY 哨兵、2 片固定切片数量不变。

## 目录

```
kernel/krnl_vadd.cpp   内核整 TU（zf-decoder 含顺序窗预取；merge/encoder 为里程碑 1 原样）
kernel/krnl_host.h     常量（KEY 24B/USER、VALUE 1024B、restart、PPS 偏移…）—— 原样
host/zf_format.h       WAL ZfRecord 帧 + slim 条目编码 + 预言机（纯 C++）—— 原样
host/zf_sst_decode.h   host 侧 SST 解码器 + CompareWorkload —— 原样
host/main_zf.cpp       OpenCL(cl2.hpp) host 测试（sw_emu / hw 通用）—— 原样
test/zf_cpu_sim.cpp    CPU 全流水线仿真（decoder→merge→encoder；padding +1024 字）
Makefile               v++ 构建（TARGET 默认 sw_emu）+ host 编译 + run —— 原样
tools/apply_decoder.py + zf_decoder_helpers.inc + zf_decoder_new.inc  新 decoder 单一事实源
```

## 构建与运行

```bash
export XCL_EMULATION_MODE=sw_emu
make TARGET=sw_emu xclbin                   # .xclbin（sw_emu：HLS 前端综合检查 + 链接，分钟级）
make TARGET=sw_emu host                     # host/test_host_sw_emu
make TARGET=sw_emu run                      # 跑默认 8 case
```

产物 `build/sw_emu/krnl_vadd.xclbin`。单跑某组 workload：

```bash
XCL_EMULATION_MODE=sw_emu ./host/test_host_sw_emu -x build/sw_emu/krnl_vadd.xclbin \
   -d 0 101 4 800 120  106 4 1500 500
```

CPU 快速回归（不依赖 XRT，秒级）：

```bash
g++ -std=c++17 -O1 -pthread -DHLS_STREAM_THREAD_SAFE -I ../kernel -I ../host \
    -I /tools/Xilinx/Vitis_HLS/2022.2/include test/zf_cpu_sim.cpp -o /tmp/zf_cpu_sim
/tmp/zf_cpu_sim              # 默认 8 case；或 /tmp/zf_cpu_sim <seed> <ports> <base> <dup>
```

> sw_emu 说明：sw_emu xclbin 内内核以编译后的 C 目标码运行（非 RTL 模拟），HLS 仅做前端
> 综合检查（日志 `Running only source code synthesis checks, skipping scheduling and RTL`），
> 故 build 分钟级。win[] 落 BRAM、整窗 burst 的真实资源/时序验证须 hw 综合（后续）。

## 验证结果

- **CPU 全流水线仿真**（`test/zf_cpu_sim.cpp`，同一 TU 镜像内核，oracle 独立逐条比对）：
  默认 8 case + 随机 101–115（1~4 口、base 30~1000、dup 0~200）**全绿**；覆盖 1024B 满值跨窗界、
  删除标记(val_len=0)、跨口同键遮蔽、多口。
- **sw_emu（真实内核 + OpenCL host）**：
  - 默认 8 case（seed 1–8，1~4 口，45~150 记录）：**8/8 PASS**；
  - 随机 101–110（1~4 口、30~2000 记录、dup 0~500；最大 seed106 = 4 口 2000 记录）：
    **10/10 PASS**；
  - 全 case `file_num==1`，PPS/data/index 经 host 解码与预言机逐条吻合（键序、去重、值字节）。
- **新旧逐字节对比（无回归）**：同一 18 组 `(seed,ports,base,dup)` 显式参数，分别跑
  `../AcceleratorKernel/build/sw_emu/krnl_vadd.xclbin` + 其 host 二进制、与本目录新
  xclbin + 新 host 二进制；两实现各自均通过 oracle 逐条比对，且每 case 的
  `data=`/`idx=` **逐字节一致** ⇒ 顺序窗预取 decoder 输出与里程碑 1 decoder 完全等价。

## 本里程碑边界 / 后续

- 本目录仅 sw_emu 验证产物。**hw 综合留给用户决定**（在 `../AcceleratorKernel/` 流程上
  跑 `make TARGET=hw xclbin` 即可，源码已就绪；win[256]/口 的资源占用与 burst 效率须
  以 hw 报告为准）。
- 每输入口仍单个 (part,gen) 段；多段 / gen 目录表、SST v2+crc32c、接 zero_flush 引擎
  字节直读等沿用里程碑 1 边界，未在本里程碑扩展。
- 窗大小 1024B 为经验初值；真实 WAL 段命中率/窗口利用率取决于 slim 键序与 WAL 帧序的
  局部相关度，后续可在 hw 上以真实段测窗大小/预取深度调参。
- `zf_cpu_sim` 与 device decoder 各自独立实现字节格式，互为校验（沿用里程碑 1 约定）。

## 参考与格式文档

- `../AcceleratorKernel/README.md` —— 里程碑 1（本目录的基线与背景，含 host OpenCL 取舍、参考 bug 修复）
- `../source/rocksdb-zeroflush/zeroflush/FPGA_SLIM_MEMTABLE_BYTEFORMAT.md` ——
  §9 WAL 记录 / §10 slim memtable / §11 internal key / §16.2 顺序读变体
- `../AcceleratorKernel/reference/CoKVKey24Value1024_kernel/` —— 上游 CSD-CoKV 内核（改造来源）

---

# A+B 卸载（全版本保留）· 软件级数据通路验证（AB）

在 M3（A-only aux-sort，v2/kCRC32c/kBinarySearch 落盘 + ZfSeal + 引擎直读对拍）之上，
给 kernel 加 **B 侧 raw-SST 解码器 `decoder_sst`** 与 **encoder 全版本保留模式（mode=1）**，
用**纯 CPU-sim（sw_sim）**证明「A∪B 输入 → decode→merge→re-encode 全版本产物」的数据通路正确，
并经**引擎字节直读**对拍。本里程碑**不接硬件 / 不接 zeroflush_db**；范围 = 软件级代码正确性。

## 改动面（相对 M3，全部在 `AcceleratorKernelSstV2/`，引擎树/旧目录零改动）

```
kernel/krnl_vadd.cpp   (1) 新增 decoder_sst()：B 侧 §14.6 raw-SST 链解码
                       (2) 新增 zf_rd_varint64 / zf_rd16 / zf_ld_byte helper
                       (3) encoder() 增第 9 参 keep_all_versions：0=M3 去重（默认不变）
                           / 1=跳过 same_user_key 抑制，逐版本落盘
                       (4) 新增 decode_port()（kind==1 → decoder_sst / else → decoder）
                       (5) 顶层 krnl_vadd：host_data[15] 扩到 [24]（mode + 4×port_kind）
                           + 4×decode_port 接线；mode=0 + kind=0 时 = M3 行为
host/main_zf.cpp       host_data[24] + 布局注释；默认全 0 → 行为与 M3 一致
test/zf_cpu_sim_ab.cpp 新增 AB CPU-sim 套件（本文档「验证结果」）
README.md              ← 本文档（追加本节）
```

M3 回归（mode=0）不受扰：`zf_cpu_sim` 默认 8 case + 随机 101–110 全绿（同 M3）。

## B 侧输入协议（decoder_sst 读法）

- **链描述表 + 字节区**（1 个输入口 = 引擎 base/L0 一个分区内的 overlap 文件链）：
  `[u64 K][K × {u64 file_off, u64 file_sz}][文件 1 字节..文件 K 字节]`，同一口内文件键范围
  互不相交且有序，跨文件续解成同一条有序 KV 流。K ≤ 64（坏输入死循环保护）。
- **每个文件 = 已封口 §14.6 SST**：`[data 块][index 块][properties][metaindex][footer 53B]`，
  序解码校验 block-based magic / format_version==2 / checksum==kCRC32c → 解析单级 index
  BlockHandle → 顺序遍历每条 entry 取各 data-block handle → 逐 data 块按 block record +
  restart 前缀重建 32B 内部键（24B user + 8B seq/type），value ≤1024B，vlen=0 = 删除标记。
- **输出协议与 A `decoder` 逐字一致**：流头 SIGNAL → 每记录 `1×km`（keystring = 32B 内部键、
  自然字节序）+ 固定 `2×512B` value 切片 → 流尾 MAX_KEY。merge / encoder 不感知端口类别。
- 结构错误 → printf 诊断 + 提前结束（sim 侧以记录数 vs 预言机 FAIL，绝不静默错排）。

## encoder 全版本保留模式（keep_all_versions）

- 点：`:3468` 处 `if (keep_all_versions==0 && same_user_key(...)) set_empty()` 抑制块。
  mode=1 跳过 → 每条真实记录都写盘；同 user 连续版本（seq 降序）由 putKV 前缀共享
  （shared=24 / unshared=8）自然落块，是合法 v2 block record，**引擎直读门实证可读**。
- 字节档位与 M3 输出同构（v2 / kCRC32c / 每块 5B trailer / PPS）。M3 去重路径 mode=0 不变。

## CPU-sim AB 套件（`test/zf_cpu_sim_ab.cpp`）

线程模型照抄 `zf_cpu_sim.cpp`（decode 先喂满 → merge 子线程 kv_sum+1 次 → encoder 主线程），
另加 decoder_sst 走 `decode_port(kind=1)`。每个 case 在**子进程**内跑并设 60s 看门狗
（decoder 少发会令 merge/encoder 阻塞 → 超时杀进程标 FAIL，不整跑挂死）。case 矩阵：

| case | A 口 | B 链 | 覆盖点 |
|---|---|---|---|
| s0 | 0 | 1 文件 | 仅 B；同 user 3 版本 + 中间 tombstone；vlen 40/0/1024 |
| s1 | 0 | 2 文件 | 仅 B 多文件链（范围不相交），含删除 |
| s2 | 1 | 0 | 仅 A；1 口内同 user 多版本 + tombstone |
| s3 | 3 | 0 | 仅 A 3 口；跨口同 user 遮蔽 → mode1 保留全部版本 |
| s4 | 2 | 1 文件 | A 新版 + B 旧版 同 user 跨 A/B；A 新 user；双方 tombstone |
| s5 | 2 | 2 文件 | 大样本 84 记录；B 链 2 文件 + A 2 口；覆盖/新增/删除/值长短混合 |
| s6 | 1 | 1 文件 | 值长边界集 1/15/16/17/31/32/33/47/48/63/64/65/1024（含删除）|

预言机 = A∪B **全部版本**按 internal comparator（user 升 / seq 降）排序，**不去重**。
B 侧文件字节由「本 kernel 自己的 full-version encoder（mode=1）」预产 + 合成 §14.6 footer，
再作为 raw-SST 喂回 decoder_sst（round-trip）。逐 case 校验：`zfdecode::CompareWorkload`
（逐块 masked crc + 逐条 ik/vlen/value 位置比对 + PPS 抽查）。

编译运行：

```bash
g++ -std=c++17 -O1 -pthread -DHLS_STREAM_THREAD_SAFE -I ../kernel -I ../host \
    -I /tools/Xilinx/Vitis_HLS/2022.2/include test/zf_cpu_sim_ab.cpp -o /tmp/zf_cpu_sim_ab
/tmp/zf_cpu_sim_ab                    # 7/7；加 -o dump/ab_cpu 落盘 .prefix/.expect
```

## 验证结果（sw_sim + 引擎字节直读）

- **CPU-sim AB 套件：7/7 PASS** —— decoder_sst（仅 B / B 链 / A+B 混合）round-trip 记录多重集
  与全版本预言机逐条一致；值长边界、tombstone、同 user 多版本（含跨口、跨 A/B）全绿。
- **引擎字节直读对拍（AB-4，零引擎改动）**：7 个 case 输出前缀 + 7 个 B 输入前缀，共
  **14/14** 经 `../source/rocksdb-zeroflush/build/zf_seal_check` 封口（ZfSeal）+ SstFileReader
  Open / VerifyChecksum / VerifyNumEntries / 全键迭代，stdout `hex(ik)\thex(value)` 行集与
  各 `.expect` **逐字节 diff 一致**。
- **shared=24 风险实证关闭**：含连续同 user 版本的文件（s0:2、s4:10、s5:14 处跨版本边界）
  引擎 CRC + 直读干净通过 ⇒ 无需 restart 强制 shared=0 降级。
- **B 输入引擎可读锚定**：每个 B 输入文件前缀经引擎封口后读回 == 该 B 文件记录集 ⇒
  decoder_sst 解码的是真 §14.6 引擎可读格式，非自说自话。

## 本里程碑边界 / 后续

- 范围 = **软件级（CPU-sim + 引擎直读）**：未做 hw 综合 / 真卡、sw_emu 复核（AB-5）延后、
  未接 zeroflush_db 物化 / CSD driver / 引擎格式锁 —— 全部移入下个里程碑。
- 输入硬限（与 encoder 同构）：24B user / 32B ik / value ≤1024B / 单 data block ≤4096B /
  ≤4 口（A staged + B 链 ≤4）；B 文件键范围每口互不相交且有序；真库接入时的 eligibility
  判定留待下个里程碑。
- 引擎树零改动、旧目录（`../AcceleratorKernel/`、`../AcceleratorKernelWalPrefetch/`）零改动。

---

# E·F：hw 综合 + 引擎深接 CSD（真卡物化卸载）

> 把 AB 的软件级证明推上真卡并把引擎写侧物化接到该内核。全程详情见
> `../../docs/ZF_MilestoneEFG_CSD_Final_Report.md`。内核零改动。

## E：hw 综合 + 真卡端到端

- **E-1 综合**：`make TARGET=hw xclbin`（目标 `xilinx_u2_gen3x4_xdma_gc_2_202110_1`、
  200 MHz、`sdx_optimization_effort_high`）。产物 `build/hw/krnl_vadd.xclbin`。
- **E-2 host A+B 运行模式**（已就绪）：`main_zf` 增 `--ab <dump_dir>`——夹具 `dump/ab_host/`
  （7 case：`.desc` 槽表 + `.s<i>` 槽字节 + CPU-sim `.prefix/.expect` 对拍目标）自包含，
  `RunAbDevice` 现算 A staged、直喂 slot 字节，mode=1 逐 case 与 CPU-sim 前缀**逐字节对拍**。
- **E-3 真卡运行**（待 xclbin）：
  ```bash
  # (a) M3 默认 8 case 真卡回归（对 sw_emu 基线）       # (b) AB 7 case 前缀对拍
  ./host/test_host_hw -x build/hw/krnl_vadd.xclbin -d 0
  ./host/test_host_hw -x build/hw/krnl_vadd.xclbin -d 0 --ab dump/ab_host
  ```

## F：引擎深接 CSD（写档锁 + 物化接缝 + 双后端等价）

- **F-1 §14.6 写档锁**：`zeroflush_db.cc` `Open()` 单点锁 `format_version=2/kCRC32c/
  kBinarySearch/kNoCompression`（其余默认 restart=16、无 filter/partition index）——引擎自产
  分区 SST 与 decoder_sst 同档互解。
- **F-2 CSD 开关 + 接缝**：`ZeroFlushOptions{csd_materialize,csd_xclbin,csd_device}`（缺省关）；
  `zeroflush/csd_backend.{h,cc}`（打包/资格/会话工厂，引擎 lib XRT-free）；
  `zeroflush/csd_session_opencl.cc`（**只进带设备目标**，`cl2.hpp` map 直读写照 main_zf）。
  `materialize_job.cc` 输出口改走 `TryCsdDirectMaterialize`：A-only 单文件档、`pps[1]==n` 强校验、
  失败/设备不可用恒回落 host `BuildTable`（计数 `csd_files/attempts/fallbacks` 可证，零静默错排）。
- **F-3 `zf_csd_test`**（`tools/`，链 XRT）：csd=off/csd=on 双 DB 同一 1920 键可写集 → 物化 →
  全键 Get / 全扫描 / Close+Reopen 重扫（CRC 强校验直读）逐条等价。**host 回落模式 8/8 全绿**
  （csd 库 `files=0 fallbacks=216`，证明接缝可达且零错排）；`zf_test` 回归 36 PASS/3 项既有 L0
  失败（与本任务无关）不变。
  ```bash
  cd ../source/rocksdb-zeroflush
  ./build/zf_csd_test                          # host 回落等价（无设备）
  ./build/zf_csd_test --xclbin ../AcceleratorKernelSstV2/build/hw/krnl_vadd.xclbin --device 0  # 真卡卸载
  ```
