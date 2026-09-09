# RefDec (decoder-burst) 内核真卡量测报告 — 2026-09-09

xclbin: `AcceleratorKernelRefDec/build/hw/krnl_vadd.DEC_111_0909_1130.xclbin`
达成时钟: **111 MHz**（scalable；300 布线布通，未触发 200 回退）
工具: `source/rocksdb-zeroflush/build/zf_runab_microbench`（A-only，kind=0 WAL 帧物化；host 零改动，`ZF_CSD_PROFILE=1`）
方法: 3700 键 ×6 量测(+2 warm)、50000 键 ×3(+1 warm)，逐 run 读七段埋点 kernel_ev；真卡全 exit 0，`pps[1]==keys` 契约通过（decode 正确）。

## 结果

| 指标 | M1 (97.7MHz) | RefDec (111MHz) | Δ |
|---|---|---|---|
| kernel_ev @3700 键 | 264 ms | **59.8 ms** (59705–59801µs, 极稳) | **4.4× 墙钟** |
| 周期/op | 6973 | **1794** | **3.9×** |
| kernel_ev @50000 键 | — | 813.2 ms → **1805 cyc/op** | cyc/op 与单元尺寸无关（线性） |
| seam tot @3700 | ~317 ms | ~105 ms | — |
| seam 份额 | ~17% (53ms) | **~43% (45ms)** | 分叉 |

输出产物 @3700: sst=3,870,385B + idx=37,675B；@50k: sst=52.3MB + idx=515KB。

## Seam 细分（@3700 键，tot≈105ms）

| 段 | 值 | 性质 |
|---|---|---|
| kernel_ev | 59.8ms | 设备内核（57%） |
| inbuf | ~5.5ms | 输入 staging map+memcpy（可池化复用） |
| outbuf | ~14ms | 输出 map+memcpy（可池化/直读） |
| datar/pps_r/final | ~5.6ms | 读回小段 |
| enqfin−kernel | ~20ms | launch + q.finish 同步/事件等待（异步可隐） |

## 归因

1. **decoder-burst 兑现大头**：6973→1794 cyc/op（3.9×），与预估 A 路区间(1500–2500)吻合；真卡 decode 正确（pps 契约）。
2. **kernel 现为线性 1800cyc/op**：加大 A 单元不摊薄（50k vs 3.7k 持平）；再降需逐记录微优化（递减）。
3. **瓶颈已分叉**：kernel 57% / seam 43%。M1 时代 seam 只占 17% 可忽略，现在 seam 与 kernel 同级 → **seam 优化窗口已开**（正是路线图#37 前置条件）。
4. 差距锚点：1794 cyc/op @111MHz = 16.2µs/op vs reference(SST→SST 800k) 420cyc/op@146.7 = 2.86µs/op → 周期 4.3× / 墙钟 ~5.7×；vs CPU 物化 2.01µs/op → kernel 8×、端到端 14×（@3.7k 单元形态）。
5. 战略读数：A-materialization @引擎 3.7k 单元形态 CSD 难赢 CPU（即便 kernel 打到 reference 量级仍受 seam+单元 floor 压制）；正收益杠杆 = ①seam 缓冲池+事件流水(藏 45ms) → ③/④ 批量多 flush + 大 B 归并单元(reference 800k 几何)，与既有路线一致。

## 原始日志

- /tmp/refdec_bench3700.log
- /tmp/refdec_bench50k.log
