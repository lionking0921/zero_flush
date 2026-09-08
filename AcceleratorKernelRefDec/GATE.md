# AcceleratorKernelRefDec — sw_emu 功能门禁记录

> 状态戳：2026-09-09。本文件夹 = AcceleratorKernelSstV2 fork，唯一改动在
> `kernel/krnl_vadd.cpp` 两个 decoder：**A 路 decoder 整帧 burst 进片上 value_buf**、
> **B 路 decoder_sst data block 整块 burst 进 blk[1300]（BRAM）再片上解**，
> 收敛到同一 `zf_burst_load` / `zfb_byte` / `zfb_varint32` / `zfb_rd16` 原语
> （reference `ap32to32_copy_datablock`→片上逐记录解 同构）。encoder / merge /
> decode_port / host_data 语义 / host 与 engine 零改动（fork host 与基线逐字节同）。
> 依据 memory `reference-not-cpu-competitive-bare-kernel`：差距在 decoder 的 DRAM
> 访问结构，不在 encoder。

## 门禁命令与结果

```bash
# 0) fork 后 kernel 手术（已完成）。CPU-sim 字节等价 oracle（改后真 kernel TU）：
#    baseline 与 refdec 两个 TU 独立编译，7 个确定性 AB 场景
#    （s0_single_mv / s1_chain / s2_a_only / s3_a_3ports / s4_a_over_b / s5_bulk / s6_vlen_bounds）
#    → 各自 7/7，全部 .bin 输出（data+index+PPS）逐字节一致。

# 1) sw_emu xclbin（HLS 合法性：新数组/uram/动态片上索引）
source /tools/Xilinx/Vitis/2022.2/settings64.sh
make TARGET=sw_emu xclbin        # => build/sw_emu/krnl_vadd.xclbin（4,691,538 B），v++ 无错

# 2) sw_emu host
make TARGET=sw_emu host          # => host/test_host_sw_emu

# 3) AB 设备门禁 —— 权威 A+B 功能门禁（CRC-free：设备 data+index 与 CPU-sim .prefix 逐字节 memcmp）
export XCL_EMULATION_MODE=sw_emu
./host/test_host_sw_emu -x build/sw_emu/krnl_vadd.xclbin -d 0 --ab dump/ab_host
#   => AB on-device: 7/7 cases == CPU-sim prefix   (exit 0)
#      覆盖 decoder A：s2_a_only_mv、s3_a_3ports、s4_a_over_b 混合
#      覆盖 decoder_sst B：s0_single_mv、s1_chain、s5_bulk、s6_vlen_bounds、s4 混合
#      每例落 dump/ab_host/<label>.device.prefix（== CPU-sim prefix 字节）
```

### 默认 8/8 RunCase 门禁 — 当前 repo 级红（预存 harness 陈旧，非本改动回归）

默认 `RunCase`（mode=0 M3 去重）经 `CompareWorkload` → `DecodeSST` 逐块 **无条件**
`VerifyBlockTrailer`（对每块重算 masked crc32c）。M1 起 encoder 按 kNoChecksum 写
**零 checksum trailer**（`ap8to128_encoder(0,…)`/`ap32to128_encoder(0,…)`，用户指令
「不要恢复CRC」维持该策略）→ host verify 恒抛。**baseline 与 refdec 抛错逐字节相同**：

```
what():  block checksum mismatch @off=0 size=483 stored=00000000 want=2fbf4d14
case seed=1 ports=4 base=60 dup=10 kv_sum=70 expect=60 file_num=1 data=52429 idx=488
   （AcceleratorKernelSstV2 与 AcceleratorKernelRefDec 各自跑默认门禁 RC=134，输出完全一致）
```

即：**8/8 门禁在 HEAD 红是 repo 级预存现象，与 burst decoder 改动无关**（refdec 无新增
失败；设备输出尺寸 data/idx 两边一致）。A/B 解码正确性由 CRC-free 的 AB 门禁 7/7 承担。

## 结论

- decoder A（整帧 burst）与 decoder_sst B（整块→BRAM）在 sw_emu 设备端 **7/7 功能
  PASS**（与 CPU-sim prefix 逐字节一致），CPU-sim 字节等价 oracle 亦 7/7 全场景一致。
- sw_emu xclbin / host 编译干净（新数组、动态片上索引 HLS 合法）。
- 本轮量不了周期（sw_emu 为 x86 功能仿真）；时钟归因兑现留待 HW build + 真卡。
- **遗留（另立项，非本 kernel 轮）**：host `zf_sst_decode.h` 的 `VerifyBlockTrailer`/
  `DecodeSST` 应读取 footer/块校验策略，在 kNoChecksum（stored==0）时跳过 masked crc32c
  重算 —— 该 host 侧修复可同时恢复 `zf_cpu_sim_ab` oracle、默认 RunCase 8/8 门禁与
  `zf_seal_check` 的 CompareWorkload 使用。本 fork 为保持「kernel-only diff」不改 host。
