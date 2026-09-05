# ZeroFlush FPGA 加速器（AcceleratorKernel）

把 zero_flush L0→L1 辅助排序（aux-sort）物化卸载到 Samsung SmartSSD U2 的
FPGA 内核。本目录是基于 CSD-CoKV 参考内核（`reference/CoKVKey24Value1024_kernel/`）
改造的专用加速器：**只替换 decoder（RocksDB 字节解析器）**，merge / encoder /
PPS 封口保持参考原样，交付可下载的 `.xclbin` 与验证它的 host 测试。

- Key 规格：24B 用户键（内部键 32B = user 24 ‖ LE64((seq<<8)|type)）
- Value 规格：1024B（记录可短，内核按 2×512B 零填充切片传输）
- 工具链：Vitis 2022.2 + XRT 2.14（sw_emu / hw）
- 平台：`xilinx_u2_gen3x4_xdma_gc_2_202110_1`
- Host 语言：OpenCL C++（`cl2.hpp` + `-lxilinxopencl`）——U2 真卡验证过的数据通路，
  见下文「host device 层取舍」。

## 目录

```
kernel/krnl_vadd.cpp   内核整 TU（含 zf-decoder；merge/encoder 为参考原样 + 1 处 bug 修复）
kernel/krnl_host.h     常量（KEY 24B/USER、VALUE 1024B、restart、PPS 偏移…）
host/zf_format.h       字节格式事实标准：WAL ZfRecord 帧 + slim 条目编码 + 预言机（纯 C++）
host/zf_sst_decode.h   host 侧 SST 解码器 + CompareWorkload（比对 oracle）
host/main_zf.cpp       OpenCL(cl2.hpp) host 测试（sw_emu / hw 通用，数据层与 native 版共用）
test/zf_cpu_sim.cpp    CPU 全流水线仿真（decoder→merge→encoder，多种子回归，不依赖 XRT）
Makefile               v++ 构建（sw_emu / hw）+ host 编译 + run
```

## host device 层取舍（为什么是 OpenCL 而非 XRT native）

本机 XRT(2022.2/2.14) 只随 include 提供 `CL/cl2xrt.hpp`（cl↔xrt 桥接，非完整 CLHPP）；
真正可用的 OpenCL C++ 头是系统 `/usr/include/CL/cl2.hpp`，Xilinx 扩展枚举在
`/opt/xilinx/xrt/include/CL/cl_ext_xilinx.h`（`CL_MEM_EXT_PTR_XILINX` = 1<<31）。
host 编译加 `-DCL_HPP_TARGET_OPENCL_VERSION=120` 等以匹配 Khronos 1.2 头，链 `-lxilinxopencl`。

**为什么不用 `xrt::bo` native 路径（曾短暂采用又放弃）**：U2 卡无板载 DDR，xclbin 的
`bank0 (MEM_DDR4)` 实为 host-memory aperture。native `xrt::bo(dev,bytes,group)` 分配的缓冲
在真卡上：输入 `sync TO_DEVICE` 正常、`run.wait()` 正常返回，但内核产出的输出区既
`sync FROM_DEVICE` 失败（dmesg：`xdma_xfer_fastpath ... C2H0-MM status error 0x400`，
无板载存储可供 device→host DMA 读回），`bo.map()` 直读也看不到内核写（读回的是 host
shadow 页，仍为预置 pattern）。sw_emu 下 native 路径一切正常 —— 说明这是真卡上
native 默认缓冲与 U2 内存域的匹配问题，而非内核逻辑问题。

**参考 CoKV 的 OpenCL 路线规避了它**：`cl::Buffer(CL_MEM_READ_WRITE)` + 
`enqueueMapBuffer` 直读/直写 + `enqueueTask` 启动内核（`reference/CoKVKey24Value1024_kernel/
vadd.cpp` 即此模式，U2 真机验证）。同负载在 hw 实卡上 OpenCL 路径 pps 输出 521/2048 字变化
与 sw_emu 完全一致，端到端比对全绿。

## 构建

```bash
export XCL_EMULATION_MODE=sw_emu            # hw 真卡跑时不要设
make TARGET=sw_emu xclbin                   # sw_emu .xclbin（~0.5-1h，一次 C 综合）
make TARGET=sw_emu host                     # host/test_host_sw_emu
make TARGET=sw_emu run                      # sw_emu 跑默认 8 case
make TARGET=hw xclbin                       # 真比特流 .xclbin（place & route 数小时）
make TARGET=hw host                         # host/test_host_hw
make TARGET=hw run                          # 实卡跑
```

产物：`build/<target>/krnl_vadd.xclbin`。

单跑某个/某组 workload（`seed ports base_keys dup_keys`，可多组）：

```bash
XCL_EMULATION_MODE=sw_emu ./host/test_host_sw_emu -x build/sw_emu/krnl_vadd.xclbin \
   -d 0 101 4 200 60  102 3 180 90
```

## 内核数据流与接口

顶层流水不变（4×decoder → merge → encoder → sst + index + PPS）：

```
decoder(buf_i, kv_i, wal_bytes_i) → decoder_km_stream[i] + value_stream[i]
merge(4 流) → encoder_km_stream + merge_result(胜者 index)
encoder(...) → sst_buffer(data) + index_block_result + output_data(PPS)
```

内核签名（与参考完全一致，host_data[15] 布局零改动）：

```
krnl_vadd(ap_uint<32>* sst_input0..3, ap_uint<64> host_data[15],
          ap_uint<128>* sst_buffer, ap_uint<128>* index_block_result,
          uint64_t* output_data)
```

每个输入口设备缓冲 = 一段连续字节（单个 `m_axi` 口，无需加口）：

```
bytes[0 .. wal_bytes_i)        WAL 段原文：一个冻结代 (part,gen) 的 zf-wal 段，字节搬入
bytes[wal_bytes_i .. end)      slim 条目区：kv_i 条内部键有序的 RocksDB 版 slim 条目
```

`host_data`：`[0..3]=buf_offset(0)`，`[4]=sst 输出缓冲字节预算`（→ max_file_size），
`[5..8]=wal_bytes[i]`，`[9]=Σkv`，`[10..13]=kv[i]`，`[14]=init(0)`。

## 新 decoder：字节解析（本里程碑唯一代码改动面）

`slim` 条目（RocksDB block_based memtable 索引，doc §10）每条：

```
varint32 ik_len=32
internal_key   = user(24B) ‖ LE64((seq<<8)|type)         // 32B
varint32 loc_len=16
locator        = part u32 LE | gen u32 LE | wal_offset u64 LE
```

`WAL` ZfRecord 帧（doc §9，`wal_offset` 指帧首）：

```
[24B header][user key][value][crc32c 4B]
header LE: magic u32@0, cf_id u16@4, type u8@6, flags u8@7,
           key_len u32@8, val_len u32@12, seq u64@16
value 起点 = 24 + key_len，长 val_len（≤1024；删除标记 type=kTypeDeletion → val_len=0）
```

decoder 流程（每口一次）：

1. 写流头 `SIGNAL`；空 run（`file_size==0||kv_sum==0`）直接补 `MAX_KEY` 返回。
2. 游标 `p = wal_bytes_i`（slim 区起点）。循环 `kv_sum` 次：
   - 读 varint32 `ik_len` → 校验为 32；
   - 按 16B 块从 DRAM 读 32B 内部键，整块装入 `keystring.c[ci]`（自然字节序镜像，
     与参考 decoder 组键布局逐位一致），`p += ik_len`；
   - 读 varint32 `loc_len`(=16)，读 8B `wal_offset`（在载荷偏移 8），`p += loc_len`；
   - 从 `wal_offset` 读 24B header 得 `key_len`/`val_len`；
     `vbase = wal_offset + 24 + key_len`；
   - **先发 km**（`{key, value_length=val_len}`），再按 64×16B 遍历把
     `[vbase, vbase+val_len)` 拷入 **2 片 512B** 的 `fifo_value_slice`（尾部零填充），
     写入 value_stream（固定 2 片，与 merge/encoder 的 1km↔2片 对齐严格一致）。
3. 结尾写 `MAX_KEY`。

DRAM 读全部走 `zf_ld_byte/u32`、`zf_rd_u64`（字节偏移，未对齐安全，LE，HLS 可综合）。
value 物化路径当前是"每记录回读 WAL 对应帧"的正确性优先实现，未做跨记录顺序化
预取优化（留作后续）。

## 参考 bug 修复（encoder putvalue_data，1 行）

内核在 `reference/CoKVKey24Value1024_kernel/krnl_vadd.cpp` 基础上除 decoder 替换外，
**仅**对 encoder 的 `putvalue_data()` 做了一处手术式修复：

```cpp
// 原：
else {  ap_uint<8> max_offset=8*offset;  ...
// 改为（value_length==0 / copy_array_length==0 时跳过写；tombstone 记录值为空）：
else if (copy_array_length > 0) {  ap_uint<8> max_offset=8*offset;  ...
```

根因：空值记录（删除标记，val_len=0）会无条件执行 else 分支，
`copy_array_length - 1` 下溢到 255，把 `input.c[-1]`（前一 16B 字，即 key 尾字节）
读进来、掩码覆盖 value 起点所在字——**破坏上一条记录的 key 尾**。
本修复经诊断（bram 快照定位 encoder 写入当刻即错）确认为参考原始 bug，
非 zero_flush 引入，修复不影响非空值路径。merge/encoder/封口其余逻辑零改动。

## 测试与预言机语义

- 测试 workload 由 `host/zf_format.h::GenWorkload(seed, ports, base_keys, dup_keys)`
  生成：base 个互异 24B 键 → 摊到各口给随机 seq 基础版本；再挑 dup 个非最大键在
  另一口放更大 seq 版本（构造跨口遮蔽对）；**每口内部按键序排好后**按 slim 字节编码
  （WAL 帧序任意）；可选随机删除标记与短值。
- **预言机**：全部记录全局排序（user asc、同 user seq desc）后做 encoder 去重
  （保留首见 = 该 user 最大 seq 版本；key 完全相同即丢弃），得期望的有序 KV 流。
- `main_zf.cpp` 端到端：写 staged 缓冲 → host_data → 启动内核 → 读回
  `output_data`（PPS @0、file 长度 @512/516/520）+ sst_buffer + index_block_result →
  host 侧解码 data 块重放出有序 KV 流，与预言机逐条比对（键序、值字节、PPS 统计），
  `file_num != 1` 即 FAIL。
- `CompareWorkload` 还校验 PPS 的 min/max seq（用 `(seq<<8)|type` 极值口径）。

约束（encoder PPS 哨兵所致）：workload seq **必须 < 2^48**——encoder 的 PPS
`minSeqno` 初值是 `2^56-1` 哨兵，footer `(seq<<8|type) < 2^56-1` 才能被真实 seq
更新。`zf_format.h` 生成器已收窄 seq 到 `[1, 2^48)` 并注释说明。

## 验证结果

- CPU 全流水线仿真（`test/zf_cpu_sim.cpp`，不依赖 XRT）：默认 8 case + 12 随机种子全绿。
- `putkey_data` 穷举单测（`test/exp_putkey_data.cpp`）：2560 组合 0 失败（回归保留）。
- **sw_emu（真实内核 + OpenCL host）**：
  - 默认 8 case：**8/8 PASS**；
  - 随机种子 101–110（1~4 口、30~700 记录、dup 0~350）：**10/10 PASS**。
- **hw 实卡（`build/hw/krnl_vadd.xclbin`，27.6MB，真比特流）**，dev0（79:00.1）：
  - 默认 8 case：**8/8 PASS**（1~4 口，30~175 记录，dup 0~25）；
  - 随机种子 101–110：**10/10 PASS**（最大 seed103=1000 记录 / 2 口 dup 300、
    seed110=850 记录 / 3 口 dup 250，输出 data 区最大 545KB）。
- 实卡输出与 sw_emu 逐 case 完全一致（同 seed 同 `data=`/`idx=`），file_num 恒 1，
  每条比对条目与预言机吻合（键序、去重、值字节、PPS 统计）。
- 排查记录：实卡初遇 `sync BO EINVAL`，溯源为 native `xrt::bo` 在 U2 的回读路径
  （见上节），切 OpenCL 后解决；卡温检查发现 dev3（6d:00.1）邻近 NVMe 86°C 过热告警，
  验证跑在温度正常的 dev0。

## 本里程碑边界 / 后续

- 每输入口**单个 (part,gen) 段**；多段 / gen 目录描述表（doc §11/§16.2）留作后续。
- SST 输出为参考 encoder/封口的 format_v5 / kNoChecksum 风格，本里程碑只做 host 侧
  KV 级解码校验，未接 zero_flush 引擎字节直读（v2+crc32c SST 留作后续）。
- WAL 取 value 未做顺序化预取（正确性优先）。
- `zf_cpu_sim` 与 device decoder 各自独立实现字节格式，互为校验。

## 参考与格式文档

- `source/rocksdb-zeroflush/zeroflush/FPGA_SLIM_MEMTABLE_BYTEFORMAT.md` ——
  §9 WAL 记录 / §10 slim memtable / §11 internal key 字节格式
- `reference/CoKVKey24Value1024_kernel/` —— 上游 CSD-CoKV 内核（本次改造来源）
