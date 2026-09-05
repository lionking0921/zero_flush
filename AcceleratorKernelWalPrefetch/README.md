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
